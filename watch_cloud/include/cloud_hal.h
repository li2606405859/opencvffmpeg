/*
 * cloud_hal.h - 手表平台硬件抽象层（HAL）
 *
 * 业务层只依赖这些接口：
 *   - 网络：TCP 连接收发（设备上用 lwIP / RW007 / ESP-AT 等实现）
 *   - 文件：目录枚举、文件读写、创建目录
 *   - 时间：延时与毫秒计时
 *
 * PC 上由 src/hal_pc.c 提供实现，方便在电脑上联调；
 * 移植到手表固件时，只需替换成对应平台的实现。
 */
#ifndef CLOUD_HAL_H
#define CLOUD_HAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_NAME_MAX 128
#define HAL_PATH_MAX 512

typedef struct hal_socket hal_socket_t;

/* 文件流：大照片按块读取上传，避免整张读进内存 */
typedef struct hal_file hal_file_t;

/* 网络 */
int  hal_net_init(void);
void hal_net_deinit(void);
hal_socket_t* hal_tcp_connect(const char* host, int port, int timeout_ms);
int  hal_tcp_send(hal_socket_t* s, const void* buf, int len);
int  hal_tcp_recv(hal_socket_t* s, void* buf, int len);
void hal_tcp_close(hal_socket_t* s);

/* 文件系统 */
int  hal_fs_list_dirs(const char* dir, char names[][HAL_NAME_MAX], int max, int* count);
int  hal_fs_list_files(const char* dir, char names[][HAL_NAME_MAX], int max, int* count);
long hal_fs_size(const char* path);
int  hal_fs_read(const char* path, void* buf, int max, int* out_len);
int  hal_fs_write(const char* path, const void* buf, int len);
int  hal_fs_mkdir_p(const char* path);

/* 文件流读写 */
hal_file_t* hal_fs_open(const char* path);
int  hal_fs_read_some(hal_file_t* f, void* buf, int max); /* >0 实际字节, 0 读完, -1 出错 */
int  hal_fs_rewind(hal_file_t* f);
void hal_fs_close(hal_file_t* f);

/* 时间 */
void hal_sleep_ms(int ms);
unsigned int hal_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_HAL_H */
