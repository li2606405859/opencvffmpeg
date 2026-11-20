/*
 * account_bind.c - 华为云相册账号绑定业务
 *
 * 状态文件（制表符分隔）：phone \t account \t token
 * token 为后端下发的访问令牌，手表不保存华为云 AK/SK。
 * 解绑后：云端删除全部照片，本地绑定信息清空，相册同步状态重置为 IDLE。
 */
#include "account_bind.h"
#include "album_sync.h"

#include <stdio.h>
#include <string.h>

#define BIND_TOKEN_MAX 128

static cloud_endpoint_t g_ep;
static char g_state_path[HAL_PATH_MAX];
static account_bind_status_t g_status;
static char g_token[BIND_TOKEN_MAX];

/* 手机号允许 '+'/'-'/空格，但真正参与业务的数字须在 6~15 位之间 */
static int valid_phone(const char* s)
{
    int digits = 0;

    if (s == NULL || *s == '\0') return 0;
    for (; *s != '\0'; ++s) {
        if (*s >= '0' && *s <= '9') {
            digits++;
        } else if (*s != '+' && *s != '-' && *s != ' ') {
            return 0;
        }
    }
    return (digits >= 6 && digits <= 15);
}

/* 验证码只接受 4~8 位纯数字，避免把用户误输入的空格/字母发给后端 */
static int valid_code(const char* s)
{
    int n = 0;

    if (s == NULL) return 0;
    for (; *s != '\0'; ++s, ++n)
        if (*s < '0' || *s > '9') return 0;
    return (n >= 4 && n <= 8);
}

/*
 * 落盘绑定信息（制表符分隔，单行）。
 * 解绑时写空文件，而不是删除文件：避免某些 FAT 文件系统上的删除开销与失败分支。
 */
static void persist(void)
{
    char buf[256];
    int len;

    if (g_status.bound) {
        len = snprintf(buf, sizeof(buf), "%s\t%s\t%s\n",
                       g_status.phone, g_status.account, g_token);
    } else {
        buf[0] = '\0';
        len = 0;
    }
    hal_fs_write(g_state_path, buf, len);
}

/*
 * 初始化：载入 .bind_state。
 * 只有手机号与账号都解析成功才认为已绑定；有 token 时同步设置到云客户端，
 * 这样后续上传请求无需业务层重复传参。
 */
int account_bind_init(const cloud_endpoint_t* ep, const char* state_path)
{
    char buf[256];
    int got = 0;
    long size;

    if (ep == NULL || state_path == NULL) return CLOUD_E_INVAL;

    g_ep = *ep;
    snprintf(g_state_path, sizeof(g_state_path), "%s", state_path);
    memset(&g_status, 0, sizeof(g_status));
    g_token[0] = '\0';
    cloud_token_clear();

    size = hal_fs_size(g_state_path);
    if (size <= 0) return CLOUD_OK;
    if (hal_fs_read(g_state_path, buf, (int)sizeof(buf) - 1, &got) != 0 || got <= 0)
        return CLOUD_OK;
    buf[got] = '\0';

    if (sscanf(buf, "%23[^\t]\t%63s\t%127s",
               g_status.phone, g_status.account, g_token) >= 2) {
        g_status.bound = (g_status.phone[0] != '\0');
    }
    if (g_status.bound && g_token[0] != '\0')
        cloud_token_set(g_token);
    return CLOUD_OK;
}

/* 第一步：请求后端（华为云短信）下发验证码 */
int account_bind_request_code(const char* phone, int* expires_in)
{
    if (expires_in != NULL) *expires_in = 0;
    if (!valid_phone(phone)) return CLOUD_E_INVAL;

    return cloud_sms_send_code(&g_ep, phone, expires_in);
}

/*
 * 第二步：校验验证码并完成绑定。
 * 先清空旧状态再写入，避免旧账号残留；写盘失败不影响本次会话可用性，
 * 但重启后会回到未绑定状态。
 */
int account_bind_confirm(const char* phone, const char* code)
{
    char account[64];
    char token[BIND_TOKEN_MAX];
    int rc;

    if (!valid_phone(phone) || !valid_code(code)) return CLOUD_E_INVAL;

    rc = cloud_bind_verify(&g_ep, phone, code, account, sizeof(account),
                           token, sizeof(token));
    if (rc != CLOUD_OK) return rc;

    memset(&g_status, 0, sizeof(g_status));
    g_status.bound = 1;
    snprintf(g_status.phone, sizeof(g_status.phone), "%s", phone);
    snprintf(g_status.account, sizeof(g_status.account), "%s", account);
    snprintf(g_token, sizeof(g_token), "%s", token);
    cloud_token_set(g_token);
    persist();
    return CLOUD_OK;
}

/*
 * 解绑：
 *   1. 先请求后端删除云端全部照片（失败则本地状态保持不变，可重试）
 *   2. 清空本地绑定信息与 token
 *   3. 把所有相册同步记录重置为 IDLE，避免出现"云端有/本地无"的脏状态
 */
int account_bind_unbind(void)
{
    int deleted = 0;
    int rc;

    /* 云端解绑会删除该账号下的全部照片 */
    rc = cloud_bind_unbind(&g_ep, &deleted);
    if (rc != CLOUD_OK) return rc;

    memset(&g_status, 0, sizeof(g_status));
    g_token[0] = '\0';
    cloud_token_clear();
    persist();

    /* 云端照片已删除，本地同步记录全部作废 */
    album_sync_reset_all();
    return CLOUD_OK;
}

int account_bind_get(account_bind_status_t* out)
{
    if (out == NULL) return CLOUD_E_INVAL;
    *out = g_status;
    return CLOUD_OK;
}

int account_bind_is_bound(void)
{
    return g_status.bound;
}
