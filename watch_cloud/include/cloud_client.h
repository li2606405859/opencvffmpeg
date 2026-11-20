/*
 * cloud_client.h - 云端相册 REST 客户端
 *
 * 约定的云接口（与 mock_cloud/server.py 一致）：
 *   GET    /api/v1/ping                         探活
 *   PUT    /api/v1/albums/{album}               创建相册（幂等）
 *   GET    /api/v1/albums/{album}               查询相册及照片列表
 *   PUT    /api/v1/albums/{album}/photos/{name} 上传单张照片（原始二进制）
 *   DELETE /api/v1/albums/{album}               删除整个云端相册
 *   GET    /api/v1/albums                       列出云端相册
 */
#ifndef CLOUD_CLIENT_H
#define CLOUD_CLIENT_H

#include "cloud_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 统一错误码 */
enum {
    CLOUD_OK       = 0,
    CLOUD_E_INVAL  = -1,   /* 参数非法 */
    CLOUD_E_NET    = -2,   /* 网络不可达 */
    CLOUD_E_HTTP   = -3,   /* HTTP 状态异常 */
    CLOUD_E_SERVER = -4,   /* 云端数据异常 */
    CLOUD_E_LOCAL  = -5,   /* 本地文件异常 */
    CLOUD_E_STATE  = -6,   /* 状态异常 */
    CLOUD_E_BUSY   = -7,   /* 正在上传，不能删除 */
    CLOUD_E_NOTFOUND = -8, /* 云端不存在 */
    CLOUD_E_NOMEM  = -9    /* 数量超限或内存不足 */
};

#define CLOUD_ALBUM_ID_MAX   64
#define CLOUD_MAX_PHOTOS     64

/* 待上传数据的按块读取回调：返回 >0 读到的字节数，0 表示结束，<0 表示本地读失败 */
typedef int (*cloud_body_source)(void* ctx, void* buf, int max);

typedef struct {
    char host[64];
    int  port;
    char base_path[64];
    int  timeout_ms;
} cloud_endpoint_t;

typedef struct {
    char album_id[CLOUD_ALBUM_ID_MAX];
    /* 只存照片名的 32 位哈希：64 张约 256B，避免存完整文件名占用大量静态内存 */
    unsigned int photo_hash[CLOUD_MAX_PHOTOS];
    int  photo_count;
} cloud_album_t;

/* FNV-1a 32 位哈希，用于云端照片名比对 */
unsigned int cloud_name_hash(const char* name);

void cloud_endpoint_init(cloud_endpoint_t* ep, const char* host, int port);

/*
 * 访问令牌：绑定成功后由后端下发，手表只保存 token，不保存任何华为云 AK/SK。
 * 设置后，所有云接口请求都会带 Authorization: Bearer <token>。
 */
void cloud_token_set(const char* token);
void cloud_token_clear(void);

int cloud_ping(const cloud_endpoint_t* ep);
int cloud_album_create(const cloud_endpoint_t* ep, const char* album_id);
int cloud_album_get(const cloud_endpoint_t* ep, const char* album_id, cloud_album_t* out);
int cloud_album_delete(const cloud_endpoint_t* ep, const char* album_id);

/* 流式上传：边读边发，照片大小不再受内存限制 */
int cloud_photo_put_stream(const cloud_endpoint_t* ep, const char* album_id,
                           const char* photo_name, int total_len,
                           cloud_body_source source, void* ctx);

int cloud_album_list(const cloud_endpoint_t* ep, char ids[][CLOUD_ALBUM_ID_MAX],
                     int max, int* count);

/* 华为云相册账号绑定（短信验证码） */
int cloud_sms_send_code(const cloud_endpoint_t* ep, const char* phone, int* expires_in);
/* 校验成功时带回账号与访问令牌（token 用于后续 Bearer 鉴权） */
int cloud_bind_verify(const cloud_endpoint_t* ep, const char* phone, const char* code,
                      char* account_out, int account_size,
                      char* token_out, int token_size);
int cloud_bind_query(const cloud_endpoint_t* ep, int* bound,
                     char* phone_out, int phone_size);
/* 解绑：云端会删除该账号下的全部照片 */
int cloud_bind_unbind(const cloud_endpoint_t* ep, int* deleted_albums);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_CLIENT_H */
