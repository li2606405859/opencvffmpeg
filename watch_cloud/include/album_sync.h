/*
 * album_sync.h - 相册云同步业务层
 *
 * 业务能力：
 *   1. 上传：把本地相册（目录）整体上传到云端，逐张上传、支持断点续传
 *   2. 删除：删除云端相册，本地照片保留
 *   3. 状态持久化：掉电/重启后继续未完成的上传
 */
#ifndef ALBUM_SYNC_H
#define ALBUM_SYNC_H

#include "cloud_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNC_IDLE = 0,   /* 本地有相册，从未同步 */
    SYNC_UPLOADING,  /* 上传中 */
    SYNC_UPLOADED,   /* 已全部上传到云端 */
    SYNC_DELETING,   /* 正在删除云端相册 */
    SYNC_DELETED,    /* 云端相册已删除（本地保留） */
    SYNC_FAILED      /* 上一次同步失败，可重试 */
} sync_state_t;

typedef struct {
    char album_id[CLOUD_ALBUM_ID_MAX];
    sync_state_t state;
    int total_photos;
    int uploaded_photos;
    int last_error;
} album_sync_info_t;

/* 上传进度回调：uploaded 为云端已有张数，total 为本地总张数 */
typedef void (*sync_progress_cb)(const char* album_id, int uploaded, int total, void* user);

int album_sync_init(const char* local_root, const char* state_path,
                    const cloud_endpoint_t* ep);
void album_sync_deinit(void);

/* 上传整个本地相册；可重复调用，已上传的照片会跳过 */
int album_sync_upload(const char* album_id, sync_progress_cb cb, void* user);

/* 删除云端相册，本地文件不动；云端本就不存在时视为成功 */
int album_sync_delete_cloud(const char* album_id);

/* 查询单个相册的同步状态，未记录时返回 CLOUD_E_NOTFOUND */
int album_sync_get(const char* album_id, album_sync_info_t* out);
int album_sync_list(album_sync_info_t* out, int max, int* count);

/* 解绑后云端照片已清空，把所有同步记录重置为 IDLE */
void album_sync_reset_all(void);

const char* album_sync_state_name(sync_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* ALBUM_SYNC_H */
