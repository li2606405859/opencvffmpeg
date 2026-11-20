/*
 * account_bind.h - 华为云相册账号绑定业务
 *
 * 业务流程：
 *   1. 输入手机号 -> 云端下发短信验证码
 *   2. 输入验证码 -> 绑定成功，本地保存手机号、账号与访问令牌
 *   3. 解绑 -> 云端删除该账号下全部照片，本地同步记录作废
 *
 * 手表只保存后端下发的令牌，华为云 AK/SK 仅存在于后端。
 *
 * 未绑定时禁止上传相册（album_sync_upload 返回 CLOUD_E_STATE）。
 */
#ifndef ACCOUNT_BIND_H
#define ACCOUNT_BIND_H

#include "cloud_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  bound;
    char phone[24];
    char account[64];
} account_bind_status_t;

int account_bind_init(const cloud_endpoint_t* ep, const char* state_path);

/* 下发短信验证码，expires_in 为有效期秒数 */
int account_bind_request_code(const char* phone, int* expires_in);

/* 校验验证码并完成绑定 */
int account_bind_confirm(const char* phone, const char* code);

/* 解绑并删除云端照片；未绑定时也返回成功（幂等） */
int account_bind_unbind(void);

int account_bind_get(account_bind_status_t* out);
int account_bind_is_bound(void);

#ifdef __cplusplus
}
#endif

#endif /* ACCOUNT_BIND_H */
