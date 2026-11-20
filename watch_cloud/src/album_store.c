/*
 * album_store.c - 本地相册扫描 + 同步状态文件持久化
 *
 * 职责：
 *   1. 扫描 <local_root> 下的相册目录与照片文件（按扩展名过滤）
 *   2. 读写 .sync_state，用于掉电/重启后恢复每个相册的同步进度
 *
 * 实现约束（面向手表的小内存环境）：
 *   - 所有目录扫描结果都放在 static 缓冲区里，不占任务栈
 *   - 因此本模块不可重入，且同一时刻只支持一个任务调用
 */
#include "album_store.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_MAX_ENTRIES 128                                  /* 单次扫描最多返回的条目数 */
#define STATE_BUF_SIZE    8192                                 /* .sync_state 读写缓冲区大小 */
#define PHOTO_MAX_BYTES   (64L * 1024L * 1024L)                /* 单张照片上限 64MB */

static char g_root[HAL_PATH_MAX];       /* 本地相册根目录 */
static char g_state_path[HAL_PATH_MAX]; /* 同步状态文件路径，如 <root>/.sync_state */

/*
 * 判断文件名是否为支持的照片类型（按最后一个 '.' 后的扩展名，忽略大小写）。
 * 手表相册目录里可能混有缩略图/说明文件，上传前需要过滤掉。
 */
static int is_photo_name(const char* name)
{
    static const char* exts[] = { "jpg", "jpeg", "png", "bmp", "gif", "heic", "webp" };
    const char* dot = strrchr(name, '.');
    char ext[8];
    int i = 0;
    size_t k;

    if (dot == NULL) return 0;
    for (const char* p = dot + 1; *p != '\0' && i < (int)sizeof(ext) - 1; ++p)
        ext[i++] = (char)tolower((int)(unsigned char)*p);
    ext[i] = '\0';

    for (k = 0; k < sizeof(exts) / sizeof(exts[0]); ++k)
        if (strcmp(ext, exts[k]) == 0) return 1;
    return 0;
}

/*
 * qsort 比较函数。
 * 传入的是二维数组元素的首地址（char[64] / char[128]），
 * 因此可以按普通 C 字符串比较；固定 stride 保证排序后元素仍是完整文件名。
 */
static int cmp_name(const void* a, const void* b)
{
    return strcmp((const char*)a, (const char*)b);
}

/* 保存根目录与状态文件路径；只记录指针内容，不做 IO */
int album_store_init(const char* root, const char* state_path)
{
    if (root == NULL || state_path == NULL) return CLOUD_E_INVAL;
    snprintf(g_root, sizeof(g_root), "%s", root);
    snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
    return CLOUD_OK;
}

/*
 * 列出根目录下的所有相册（子目录名）。
 * 失败返回 CLOUD_E_LOCAL；条目数超过调用方容量返回 CLOUD_E_NOMEM，
 * 由业务层决定是提示用户还是分批处理。
 */
int album_store_list_albums(char ids[][CLOUD_ALBUM_ID_MAX], int max, int* count)
{
    static char raw[STORE_MAX_ENTRIES][HAL_NAME_MAX]; /* 原始目录名，避开任务栈 */
    int n = 0;
    int i;
    int out_n = 0;

    if (count != NULL) *count = 0;
    if (ids == NULL || max <= 0) return CLOUD_E_INVAL;
    if (hal_fs_list_dirs(g_root, raw, STORE_MAX_ENTRIES, &n) != 0) return CLOUD_E_LOCAL;

    for (i = 0; i < n; ++i) {
        if (out_n >= max) return CLOUD_E_NOMEM;
        /* 目录名可能超过相册 ID 上限，snprintf 会安全截断 */
        snprintf(ids[out_n], CLOUD_ALBUM_ID_MAX, "%s", raw[i]);
        out_n++;
    }
    /* 排序保证多次运行列出的顺序一致，便于界面展示和断点续传比较 */
    qsort(ids, (size_t)out_n, CLOUD_ALBUM_ID_MAX, cmp_name);
    if (count != NULL) *count = out_n;
    return CLOUD_OK;
}

/*
 * 列出指定相册内的照片文件名（仅保留图片扩展名），并排序。
 * 相册目录不存在时 HAL 返回 -1，这里统一映射为 CLOUD_E_LOCAL。
 */
int album_store_list_photos(const char* album_id, char names[][HAL_NAME_MAX],
                            int max, int* count)
{
    static char raw[STORE_MAX_ENTRIES][HAL_NAME_MAX];
    char dir[HAL_PATH_MAX];
    int n = 0;
    int i;
    int out_n = 0;

    if (count != NULL) *count = 0;
    if (album_id == NULL || names == NULL || max <= 0) return CLOUD_E_INVAL;

    snprintf(dir, sizeof(dir), "%s/%s", g_root, album_id);
    if (hal_fs_list_files(dir, raw, STORE_MAX_ENTRIES, &n) != 0) return CLOUD_E_LOCAL;

    for (i = 0; i < n; ++i) {
        if (!is_photo_name(raw[i])) continue; /* 过滤非图片文件 */
        if (out_n >= max) return CLOUD_E_NOMEM; /* 超出业务层单相册上限 */
        snprintf(names[out_n], HAL_NAME_MAX, "%s", raw[i]);
        out_n++;
    }
    qsort(names, (size_t)out_n, HAL_NAME_MAX, cmp_name);
    if (count != NULL) *count = out_n;
    return CLOUD_OK;
}

/*
 * 以文件流方式打开照片，供流式上传边读边发。
 * 先取文件大小：0 字节视为无效照片，超过 64MB 返回 CLOUD_E_NOMEM，
 * 这样上层无需再关心内存与大小校验。
 */
int album_store_open_photo(const char* album_id, const char* photo,
                           hal_file_t** out, int* out_size)
{
    char path[HAL_PATH_MAX];
    long size;

    if (out != NULL) *out = NULL;
    if (out_size != NULL) *out_size = 0;
    if (album_id == NULL || photo == NULL || out == NULL || out_size == NULL)
        return CLOUD_E_INVAL;

    snprintf(path, sizeof(path), "%s/%s/%s", g_root, album_id, photo);
    size = hal_fs_size(path);
    if (size <= 0) return CLOUD_E_LOCAL;
    if (size > PHOTO_MAX_BYTES) return CLOUD_E_NOMEM;

    *out = hal_fs_open(path);
    if (*out == NULL) return CLOUD_E_LOCAL;
    *out_size = (int)size;
    return CLOUD_OK;
}

/*
 * 读取 .sync_state。
 * 文件格式：album_id \t state \t total \t uploaded \t last_error
 * 说明：
 *   - 文件不存在视为首次运行，返回成功且 count=0
 *   - 超过缓冲区的大文件按字节截断，最后一行不完整会被丢弃
 *   - 至少解析出 4 个字段才认这一行（last_error 可缺省）
 */
int album_store_load_state(album_sync_info_t* items, int max, int* count)
{
    static char buf[STATE_BUF_SIZE];
    long size;
    int got = 0;
    char* line;
    int n = 0;

    if (count != NULL) *count = 0;
    if (items == NULL || max <= 0) return CLOUD_E_INVAL;

    size = hal_fs_size(g_state_path);
    if (size <= 0) return CLOUD_OK; /* 首次运行，没有状态文件 */
    if (size > (long)sizeof(buf) - 1) size = (long)sizeof(buf) - 1;
    if (hal_fs_read(g_state_path, buf, (int)size, &got) != 0) return CLOUD_E_LOCAL;
    buf[got] = '\0';

    line = buf;
    while (line != NULL && *line != '\0' && n < max) {
        char* nl = strchr(line, '\n');
        int state = 0;
        album_sync_info_t* it = &items[n];

        if (nl != NULL) *nl = '\0';
        memset(it, 0, sizeof(*it));
        if (sscanf(line, "%63[^\t]\t%d\t%d\t%d\t%d",
                   it->album_id, &state, &it->total_photos,
                   &it->uploaded_photos, &it->last_error) >= 4) {
            it->state = (sync_state_t)state;
            n++;
        }
        line = (nl != NULL) ? nl + 1 : NULL;
    }

    if (count != NULL) *count = n;
    return CLOUD_OK;
}

/*
 * 全量重写 .sync_state。
 * 记录数量受 8KB 缓冲区限制，写不下时保留已拼接的部分，避免产生半行脏数据。
 */
int album_store_save_state(const album_sync_info_t* items, int count)
{
    static char buf[STATE_BUF_SIZE];
    int off = 0;
    int i;

    if (items == NULL && count > 0) return CLOUD_E_INVAL;

    for (i = 0; i < count; ++i) {
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off,
                         "%s\t%d\t%d\t%d\t%d\n",
                         items[i].album_id, (int)items[i].state,
                         items[i].total_photos, items[i].uploaded_photos,
                         items[i].last_error);
        if (n <= 0 || off + n >= (int)sizeof(buf)) break;
        off += n;
    }

    if (hal_fs_write(g_state_path, buf, off) != 0) return CLOUD_E_LOCAL;
    return CLOUD_OK;
}
