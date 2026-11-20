/*
 * album_store.h - 本地相册扫描与同步状态文件读写
 *
 * 本地目录结构：
 *   <local_root>/<album_id>/xxx.jpg
 *   <local_root>/<album_id>/yyy.png
 *
 * 同步状态文件为制表符分隔的文本：
 *   album_id \t state \t total \t uploaded \t last_error
 */
#ifndef ALBUM_STORE_H
#define ALBUM_STORE_H

#include "cloud_hal.h"
#include "album_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

int album_store_init(const char* root, const char* state_path);

int album_store_list_albums(char ids[][CLOUD_ALBUM_ID_MAX], int max, int* count);
int album_store_list_photos(const char* album_id, char names[][HAL_NAME_MAX],
                            int max, int* count);

/* 以文件流打开一张照片（用于流式上传），*out_size 为文件字节数 */
int album_store_open_photo(const char* album_id, const char* photo,
                           hal_file_t** out, int* out_size);

int album_store_load_state(album_sync_info_t* items, int max, int* count);
int album_store_save_state(const album_sync_info_t* items, int count);

#ifdef __cplusplus
}
#endif

#endif /* ALBUM_STORE_H */
