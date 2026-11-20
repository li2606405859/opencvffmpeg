/*
 * hal_pc.c - HAL 的 PC 移植实现（Windows Winsock / POSIX socket）
 *
 * 上手表时保留同样的接口，把本文件替换为 lwIP 等平台的实现即可。
 */
#include "cloud_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <direct.h>
#  define HAL_CLOSE_SOCK(fd) closesocket(fd)
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <time.h>
#  define HAL_CLOSE_SOCK(fd) close(fd)
#endif

struct hal_socket {
#ifdef _WIN32
    SOCKET fd;
#else
    int fd;
#endif
};

struct hal_file {
    FILE* fp;
};

/* ------------------------------ 网络 ------------------------------ */

/* Windows 需要 WSAStartup；POSIX 直接返回成功 */
int hal_net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void hal_net_deinit(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

/*
 * 建立 TCP 连接。
 * timeout_ms 同时用于连接后的收发超时（SO_RCVTIMEO/SO_SNDTIMEO），
 * 这样上层不需要再单独设置 socket 超时。
 * 支持域名（getaddrinfo 可能返回多个地址，逐个尝试直到成功）。
 */
hal_socket_t* hal_tcp_connect(const char* host, int port, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    hal_socket_t* out = NULL;
    char port_str[16];

    if (!host || port <= 0) return NULL;
    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return NULL;

    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
#ifdef _WIN32
        SOCKET fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        if (timeout_ms > 0) {
            DWORD tv = (DWORD)timeout_ms;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        }
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
#else
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
#endif
            out = (hal_socket_t*)malloc(sizeof(*out));
            if (out != NULL) {
                out->fd = fd;
            } else {
                HAL_CLOSE_SOCK(fd);
            }
            break;
        }
        HAL_CLOSE_SOCK(fd);
    }

    freeaddrinfo(res);
    return out;
}

/* 循环发送直到 len 字节全部发出；任何一次失败都返回 -1（上层按断网处理） */
int hal_tcp_send(hal_socket_t* s, const void* buf, int len)
{
    int sent = 0;
    const char* p = (const char*)buf;

    if (s == NULL || buf == NULL || len < 0) return -1;
    while (sent < len) {
        int n = send(s->fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

/* 单次收取，返回实际字节数；<=0 表示对端关闭或超时，由上层决定重试/失败 */
int hal_tcp_recv(hal_socket_t* s, void* buf, int len)
{
    if (s == NULL || buf == NULL || len <= 0) return -1;
    return recv(s->fd, (char*)buf, len, 0);
}

void hal_tcp_close(hal_socket_t* s)
{
    if (s == NULL) return;
    HAL_CLOSE_SOCK(s->fd);
    free(s);
}

/* ---------------------------- 文件系统 ---------------------------- */

/*
 * 目录枚举的公共实现：want_dir=1 只返回子目录，0 只返回普通文件。
 * 返回 0 表示成功（即使目录为空）；目录打不开返回 -1。
 * 注意：max 满后直接停止读取，剩余条目被忽略。
 */
static int hal_fs_list(const char* dir, char names[][HAL_NAME_MAX],
                       int max, int* count, int want_dir)
{
    int n = 0;

    if (count != NULL) *count = 0;
    if (dir == NULL || names == NULL || max <= 0) return -1;

#ifdef _WIN32
    {
        char pattern[HAL_PATH_MAX];
        WIN32_FIND_DATAA fd;
        HANDLE h;

        snprintf(pattern, sizeof(pattern), "%s/*", dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return -1;

        do {
            int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
            if (is_dir != want_dir) continue;
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            if (n >= max) break;
            snprintf(names[n], HAL_NAME_MAX, "%s", fd.cFileName);
            n++;
        } while (FindNextFileA(h, &fd));

        FindClose(h);
    }
#else
    {
        DIR* d = opendir(dir);
        struct dirent* e;

        if (d == NULL) return -1;
        while ((e = readdir(d)) != NULL) {
            char path[HAL_PATH_MAX];
            struct stat st;
            int is_dir = 0;

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (stat(path, &st) == 0) is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            if (is_dir != want_dir) continue;
            if (n >= max) break;
            snprintf(names[n], HAL_NAME_MAX, "%s", e->d_name);
            n++;
        }
        closedir(d);
    }
#endif

    if (count != NULL) *count = n;
    return 0;
}

int hal_fs_list_dirs(const char* dir, char names[][HAL_NAME_MAX], int max, int* count)
{
    return hal_fs_list(dir, names, max, count, 1);
}

int hal_fs_list_files(const char* dir, char names[][HAL_NAME_MAX], int max, int* count)
{
    return hal_fs_list(dir, names, max, count, 0);
}

/* 取文件字节数；文件不存在或不可读返回 -1（注意：空文件返回 0） */
long hal_fs_size(const char* path)
{
    FILE* f;
    long size;

    if (path == NULL) return -1;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    fclose(f);
    return size;
}

/* 一次性读文件（最多 max 字节），用于读取小型状态文件 */
int hal_fs_read(const char* path, void* buf, int max, int* out_len)
{
    FILE* f;
    size_t got;

    if (out_len != NULL) *out_len = 0;
    if (path == NULL || buf == NULL || max <= 0) return -1;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    got = fread(buf, 1, (size_t)max, f);
    fclose(f);
    if (out_len != NULL) *out_len = (int)got;
    return 0;
}

/* 覆盖写整个文件；len=0 时生成空文件（用于清空绑定状态） */
int hal_fs_write(const char* path, const void* buf, int len)
{
    FILE* f;
    size_t put;

    if (path == NULL || (buf == NULL && len > 0)) return -1;
    f = fopen(path, "wb");
    if (f == NULL) return -1;
    put = (len > 0) ? fwrite(buf, 1, (size_t)len, f) : 0;
    fclose(f);
    return ((int)put == len) ? 0 : -1;
}

/* 逐级创建目录（类似 mkdir -p），已存在时忽略错误 */
int hal_fs_mkdir_p(const char* path)
{
    char tmp[HAL_PATH_MAX];
    size_t len;
    size_t i;

    if (path == NULL) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = saved;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
    return 0;
}

/* ---------------------------- 文件流读写 ---------------------------- */

/* 以只读方式打开照片，返回的文件流由调用方负责 hal_fs_close */
hal_file_t* hal_fs_open(const char* path)
{
    FILE* fp;
    hal_file_t* f;

    if (path == NULL) return NULL;
    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;

    f = (hal_file_t*)malloc(sizeof(*f));
    if (f == NULL) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    return f;
}

/* 流式读取一块数据：>0 实际字节数，0 已读完，-1 读错误 */
int hal_fs_read_some(hal_file_t* f, void* buf, int max)
{
    size_t n;

    if (f == NULL || buf == NULL || max <= 0) return -1;
    n = fread(buf, 1, (size_t)max, f->fp);
    if (n == 0 && ferror(f->fp)) return -1;
    return (int)n;
}

/* 回到文件头：上传失败重试时使用，保证重新发送完整内容 */
int hal_fs_rewind(hal_file_t* f)
{
    if (f == NULL) return -1;
    return (fseek(f->fp, 0, SEEK_SET) == 0) ? 0 : -1;
}

void hal_fs_close(hal_file_t* f)
{
    if (f == NULL) return;
    fclose(f->fp);
    free(f);
}

/* ------------------------------ 时间 ------------------------------ */

/* 毫秒级阻塞延时；<=0 直接返回 */
void hal_sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
#endif
}

/* 单调递增的毫秒计数（仅用于计算耗时，不做日历时间） */
unsigned int hal_now_ms(void)
{
#ifdef _WIN32
    return (unsigned int)GetTickCount64();
#else
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
    }
#endif
}
