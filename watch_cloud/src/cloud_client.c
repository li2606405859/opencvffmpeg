/*
 * cloud_client.c - 基于 HAL socket 的极简 HTTP/1.1 客户端
 *
 * 只实现本项目需要的能力：短连接、Content-Length、明文 JSON 收发。
 * 手表端如需 HTTPS，替换传输层或在 HAL 中加 TLS 即可，业务层不用改。
 */
#include "cloud_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESP_CAP        8192
#define DEFAULT_TIMEOUT 5000
#define TOKEN_MAX       128

/* 绑定后由后端下发的访问令牌；手表不保存华为云 AK/SK */
static char g_token[TOKEN_MAX];

void cloud_token_set(const char* token)
{
    if (token == NULL) {
        g_token[0] = '\0';
        return;
    }
    snprintf(g_token, sizeof(g_token), "%s", token);
}

void cloud_token_clear(void)
{
    g_token[0] = '\0';
}

void cloud_endpoint_init(cloud_endpoint_t* ep, const char* host, int port)
{
    if (ep == NULL) return;
    memset(ep, 0, sizeof(*ep));
    snprintf(ep->host, sizeof(ep->host), "%s",
             (host != NULL && *host != '\0') ? host : "127.0.0.1");
    ep->port = (port > 0) ? port : 8080;
    snprintf(ep->base_path, sizeof(ep->base_path), "%s", "/api/v1");
    ep->timeout_ms = DEFAULT_TIMEOUT;
}

/*
 * 百分号编码 URL 片段（相册名/照片名可能含中文或空格）。
 * 只保留 RFC 3986 的 unreserved 字符，其余按 UTF-8 字节转 %XX。
 */
static void url_encode(const char* in, char* out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)in;

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (in == NULL) return;

    while (*p != '\0') {
        unsigned char c = *p++;
        int keep = isalnum((int)c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (keep) {
            if (o + 1 >= out_size) break;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= out_size) break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/*
 * 一次短连接 HTTP 请求（阻塞式）：
 *   1. 建立 TCP 连接，拼请求头（Content-Length 必填，连接用完即关）
 *   2. body 可直接给内存指针，也可给 cloud_body_source 回调分块发送
 *   3. 读完全部响应，解析状态行并去掉头部，只把 JSON 正文留给上层
 * 返回值为传输层错误码；HTTP 状态码通过 status 带回，由各业务函数判断。
 */
static int http_request(const cloud_endpoint_t* ep,
                        const char* method,
                        const char* path,
                        const void* body,
                        cloud_body_source source, void* source_ctx,
                        int body_len,
                        const char* content_type,
                        int* status,
                        char* resp, int resp_cap, int* resp_len)
{
    hal_socket_t* sock;
    char head[1024];
    char auth_line[192];
    int head_len;
    int rc = CLOUD_OK;
    int total;

    if (status != NULL) *status = 0;
    if (resp_len != NULL) *resp_len = 0;
    if (ep == NULL || method == NULL || path == NULL || resp == NULL || resp_cap < 64)
        return CLOUD_E_INVAL;

    sock = hal_tcp_connect(ep->host, ep->port, ep->timeout_ms);
    if (sock == NULL) return CLOUD_E_NET;

    if (g_token[0] != '\0')
        snprintf(auth_line, sizeof(auth_line), "Authorization: Bearer %s\r\n", g_token);
    else
        auth_line[0] = '\0';

    head_len = snprintf(head, sizeof(head),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "%s"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "User-Agent: watch-cloud/1.0\r\n"
        "\r\n",
        method, path, ep->host, ep->port, auth_line,
        (content_type != NULL) ? content_type : "application/octet-stream",
        body_len);

    if (head_len <= 0 || head_len >= (int)sizeof(head)) {
        rc = CLOUD_E_INVAL;
        goto done;
    }
    if (hal_tcp_send(sock, head, head_len) != head_len) {
        rc = CLOUD_E_NET;
        goto done;
    }
    if (body != NULL && body_len > 0) {
        if (hal_tcp_send(sock, body, body_len) != body_len) {
            rc = CLOUD_E_NET;
            goto done;
        }
    } else if (source != NULL && body_len > 0) {
        /* 分块读取 + 分块发送，内存占用与照片大小无关 */
        static unsigned char chunk[2048];
        int sent_total = 0;

        while (sent_total < body_len) {
            int want = body_len - sent_total;
            int n;
            if (want > (int)sizeof(chunk)) want = (int)sizeof(chunk);
            n = source(source_ctx, chunk, want);
            if (n <= 0) {
                rc = CLOUD_E_LOCAL; /* 本地文件读失败 */
                goto done;
            }
            if (hal_tcp_send(sock, chunk, n) != n) {
                rc = CLOUD_E_NET;
                goto done;
            }
            sent_total += n;
        }
    }

    total = 0;
    for (;;) {
        int n;
        if (total >= resp_cap - 1) break;
        n = hal_tcp_recv(sock, resp + total, resp_cap - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    if (total <= 0) {
        rc = CLOUD_E_NET;
        goto done;
    }

    {
        int code = 0;
        char* sep;

        if (sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1) {
            rc = CLOUD_E_HTTP;
            goto done;
        }
        if (status != NULL) *status = code;

        sep = strstr(resp, "\r\n\r\n");
        if (sep != NULL) {
            char* b = sep + 4;
            int blen = (int)strlen(b);
            memmove(resp, b, (size_t)blen + 1);
            if (resp_len != NULL) *resp_len = blen;
        } else {
            resp[0] = '\0';
        }
    }

done:
    hal_tcp_close(sock);
    return rc;
}

/* ---------------------- 极简 JSON 取值（够用即可） ---------------------- */

/* 跳过空白，便于宽松解析 "key" : "value" 之间的空格 */
static const char* skip_ws(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* 取出 "key":"value" 的字符串值；找不到或类型不符返回 0 */
static int json_get_str(const char* json, const char* key, char* out, int out_size)
{
    char needle[96];
    const char* p;
    const char* q;
    int len;

    if (json == NULL || key == NULL || out == NULL || out_size <= 0) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (p == NULL) return 0;
    p = skip_ws(p + strlen(needle));
    if (*p != ':') return 0;
    p = skip_ws(p + 1);
    if (*p != '"') return 0;
    p++;
    q = strchr(p, '"');
    if (q == NULL) return 0;
    len = (int)(q - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return 1;
}

/*
 * 解析 "key":["a.jpg","b.png"] 形式的字符串数组。
 * out 是二维数组的扁平地址，stride 为每个元素占用字节数。
 */
static int json_get_str_array(const char* json, const char* key, char* out, int stride,
                              int max, int* count)
{
    char needle[96];
    const char* p;
    const char* end;
    int n = 0;

    if (count != NULL) *count = 0;
    if (json == NULL || key == NULL || out == NULL || stride <= 0 || max <= 0) return 0;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (p == NULL) return 0;
    p = strchr(p + strlen(needle), '[');
    if (p == NULL) return 0;
    end = strchr(p, ']');
    if (end == NULL) return 0;

    p++;
    while (p < end && n < max) {
        const char* q = strchr(p, '"');
        const char* r;
        int len;
        if (q == NULL || q >= end) break;
        r = strchr(q + 1, '"');
        if (r == NULL || r > end) break;
        len = (int)(r - q - 1);
        if (len > 0 && len < stride) {
            char* dst = out + (size_t)n * (size_t)stride;
            memcpy(dst, q + 1, (size_t)len);
            dst[len] = '\0';
            n++;
        }
        p = r + 1;
    }

    if (count != NULL) *count = n;
    return n;
}

/*
 * FNV-1a 32 位哈希：把照片名压缩成一个 4 字节整数。
 * 断点续传时用哈希判断"云端是否已有这张照片"，单相册 64 张时碰撞概率可忽略。
 */
unsigned int cloud_name_hash(const char* name)
{
    unsigned int h = 2166136261u; /* FNV-1a offset basis */
    const unsigned char* p = (const unsigned char*)name;

    if (name == NULL) return 0;
    while (*p != '\0') {
        h ^= (unsigned int)*p++;
        h *= 16777619u;
    }
    return h;
}

/* 解析 "key":["a.jpg","b.png"] 的字符串数组，只保留每项的哈希（省内存） */
static int json_get_hash_array(const char* json, const char* key, unsigned int* out,
                               int max, int* count)
{
    char needle[96];
    const char* p;
    const char* end;
    int n = 0;

    if (count != NULL) *count = 0;
    if (json == NULL || key == NULL || out == NULL || max <= 0) return 0;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (p == NULL) return 0;
    p = strchr(p + strlen(needle), '[');
    if (p == NULL) return 0;
    end = strchr(p, ']');
    if (end == NULL) return 0;

    p++;
    while (p < end && n < max) {
        const char* q = strchr(p, '"');
        const char* r;
        if (q == NULL || q >= end) break;
        r = strchr(q + 1, '"');
        if (r == NULL || r > end) break;
        if (r > q + 1) {
            char name[HAL_NAME_MAX];
            int len = (int)(r - q - 1);
            if (len >= HAL_NAME_MAX) len = HAL_NAME_MAX - 1;
            memcpy(name, q + 1, (size_t)len);
            name[len] = '\0';
            out[n++] = cloud_name_hash(name);
        }
        p = r + 1;
    }

    if (count != NULL) *count = n;
    return n;
}

/* ------------------------------ 云接口 ------------------------------ */

/* 探活：GET /ping，返回 200 即认为云端可达 */
int cloud_ping(const cloud_endpoint_t* ep)
{
    char path[256];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (ep == NULL) return CLOUD_E_INVAL;
    snprintf(path, sizeof(path), "%s/ping", ep->base_path);
    rc = http_request(ep, "GET", path, NULL, NULL, NULL, 0, NULL,
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    return (st == 200) ? CLOUD_OK : CLOUD_E_HTTP;
}

/* 创建相册：PUT 同一路径即可幂等创建（已存在也不会报错） */
int cloud_album_create(const cloud_endpoint_t* ep, const char* album_id)
{
    char enc[256];
    char path[384];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (ep == NULL || album_id == NULL || *album_id == '\0') return CLOUD_E_INVAL;
    url_encode(album_id, enc, sizeof(enc));
    snprintf(path, sizeof(path), "%s/albums/%s", ep->base_path, enc);

    rc = http_request(ep, "PUT", path, "{}", NULL, NULL, 2, "application/json",
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    return (st >= 200 && st < 300) ? CLOUD_OK : CLOUD_E_HTTP;
}

/* 查询相册及照片列表；404 映射为 CLOUD_E_NOTFOUND，便于上层区分"空相册" */
int cloud_album_get(const cloud_endpoint_t* ep, const char* album_id, cloud_album_t* out)
{
    char enc[256];
    char path[384];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (ep == NULL || album_id == NULL || out == NULL) return CLOUD_E_INVAL;
    url_encode(album_id, enc, sizeof(enc));
    snprintf(path, sizeof(path), "%s/albums/%s", ep->base_path, enc);

    rc = http_request(ep, "GET", path, NULL, NULL, NULL, 0, NULL,
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    if (st == 404) return CLOUD_E_NOTFOUND;
    if (st < 200 || st >= 300) return CLOUD_E_HTTP;

    memset(out, 0, sizeof(*out));
    if (!json_get_str(resp, "album_id", out->album_id, sizeof(out->album_id)))
        snprintf(out->album_id, sizeof(out->album_id), "%s", album_id);
    json_get_hash_array(resp, "photos", out->photo_hash, CLOUD_MAX_PHOTOS,
                        &out->photo_count);
    return CLOUD_OK;
}

/* 删除云端相册；云端不存在时返回 CLOUD_E_NOTFOUND，由业务层按幂等处理 */
int cloud_album_delete(const cloud_endpoint_t* ep, const char* album_id)
{
    char enc[256];
    char path[384];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (ep == NULL || album_id == NULL || *album_id == '\0') return CLOUD_E_INVAL;
    url_encode(album_id, enc, sizeof(enc));
    snprintf(path, sizeof(path), "%s/albums/%s", ep->base_path, enc);

    rc = http_request(ep, "DELETE", path, NULL, NULL, NULL, 0, NULL,
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;      /* 网络/超时：本次删除未完成 */
    if (st == 404) return CLOUD_E_NOTFOUND; /* 云端没有这个相册（可能已删过） */
    /* DELETE 是幂等的：后端返回 2xx 即视为删除成功 */
    return (st >= 200 && st < 300) ? CLOUD_OK : CLOUD_E_HTTP;
}

/*
 * 流式上传单张照片：total_len 必须先由本地文件大小得到，
 * source 回调按需返回数据块，2KB 一块边读边发，内存占用与照片大小无关。
 */
int cloud_photo_put_stream(const cloud_endpoint_t* ep, const char* album_id,
                           const char* photo_name, int total_len,
                           cloud_body_source source, void* ctx)
{
    char album_enc[256];
    char name_enc[512];
    char path[768];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (ep == NULL || album_id == NULL || photo_name == NULL ||
        total_len <= 0 || source == NULL)
        return CLOUD_E_INVAL;
    url_encode(album_id, album_enc, sizeof(album_enc));
    url_encode(photo_name, name_enc, sizeof(name_enc));
    snprintf(path, sizeof(path), "%s/albums/%s/photos/%s",
             ep->base_path, album_enc, name_enc);

    rc = http_request(ep, "PUT", path, NULL, source, ctx, total_len,
                      "application/octet-stream", &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    return (st >= 200 && st < 300) ? CLOUD_OK : CLOUD_E_HTTP;
}

/* 列出云端已有相册 ID，用于和本地相册比对 */
int cloud_album_list(const cloud_endpoint_t* ep, char ids[][CLOUD_ALBUM_ID_MAX],
                     int max, int* count)
{
    char path[256];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (count != NULL) *count = 0;
    if (ep == NULL || ids == NULL || max <= 0) return CLOUD_E_INVAL;

    snprintf(path, sizeof(path), "%s/albums", ep->base_path);
    rc = http_request(ep, "GET", path, NULL, NULL, NULL, 0, NULL,
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    if (st < 200 || st >= 300) return CLOUD_E_HTTP;

    json_get_str_array(resp, "albums", (char*)ids, CLOUD_ALBUM_ID_MAX, max, count);
    return CLOUD_OK;
}

/* ------------------------ 账号绑定（短信验证码） ------------------------ */

/* 取出 "key":数字 的整数值（如 expires_in / deleted_albums） */
static int json_get_int(const char* json, const char* key, int* out)
{
    char needle[96];
    const char* p;

    if (json == NULL || key == NULL || out == NULL) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (p == NULL) return 0;
    p = skip_ws(p + strlen(needle));
    if (*p != ':') return 0;
    p = skip_ws(p + 1);
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

/* 取出 "key":true/false 的布尔值（如 bound） */
static int json_get_bool(const char* json, const char* key, int* out)
{
    char needle[96];
    const char* p;

    if (json == NULL || key == NULL || out == NULL) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (p == NULL) return 0;
    p = skip_ws(p + strlen(needle));
    if (*p != ':') return 0;
    p = skip_ws(p + 1);
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* 请求下发短信验证码；expires_in 返回有效期秒数（后端/华为云决定） */
int cloud_sms_send_code(const cloud_endpoint_t* ep, const char* phone, int* expires_in)
{
    char path[256];
    char body[160];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (ep == NULL || phone == NULL || *phone == '\0') return CLOUD_E_INVAL;
    if (expires_in != NULL) *expires_in = 0;

    snprintf(path, sizeof(path), "%s/sms/code", ep->base_path);
    snprintf(body, sizeof(body), "{\"phone\":\"%s\"}", phone);

    rc = http_request(ep, "POST", path, body, NULL, NULL, (int)strlen(body),
                      "application/json", &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    if (st < 200 || st >= 300) return CLOUD_E_HTTP;

    if (expires_in != NULL) json_get_int(resp, "expires_in", expires_in);
    return CLOUD_OK;
}

/* 校验验证码，成功后拿到账号名与后续请求用的访问令牌 */
int cloud_bind_verify(const cloud_endpoint_t* ep, const char* phone, const char* code,
                      char* account_out, int account_size,
                      char* token_out, int token_size)
{
    char path[256];
    char body[224];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (account_out != NULL && account_size > 0) account_out[0] = '\0';
    if (token_out != NULL && token_size > 0) token_out[0] = '\0';
    if (ep == NULL || phone == NULL || code == NULL ||
        account_out == NULL || account_size <= 0)
        return CLOUD_E_INVAL;

    snprintf(path, sizeof(path), "%s/sms/verify", ep->base_path);
    snprintf(body, sizeof(body), "{\"phone\":\"%s\",\"code\":\"%s\"}", phone, code);

    rc = http_request(ep, "POST", path, body, NULL, NULL, (int)strlen(body),
                      "application/json", &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    if (st < 200 || st >= 300) return CLOUD_E_HTTP;

    if (!json_get_str(resp, "account", account_out, account_size))
        return CLOUD_E_SERVER;
    if (token_out != NULL && token_size > 0 &&
        !json_get_str(resp, "token", token_out, token_size))
        return CLOUD_E_SERVER;
    return CLOUD_OK;
}

/* 查询后端当前绑定状态（供界面展示用，业务层以本地状态为准） */
int cloud_bind_query(const cloud_endpoint_t* ep, int* bound,
                     char* phone_out, int phone_size)
{
    char path[256];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (bound != NULL) *bound = 0;
    if (phone_out != NULL && phone_size > 0) phone_out[0] = '\0';
    if (ep == NULL || bound == NULL) return CLOUD_E_INVAL;

    snprintf(path, sizeof(path), "%s/bind", ep->base_path);
    rc = http_request(ep, "GET", path, NULL, NULL, NULL, 0, NULL,
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    if (st < 200 || st >= 300) return CLOUD_E_HTTP;

    json_get_bool(resp, "bound", bound);
    if (*bound && phone_out != NULL)
        json_get_str(resp, "phone", phone_out, phone_size);
    return CLOUD_OK;
}

/* 解绑：请求后端删除该账号下全部云端照片，deleted_albums 返回删除的相册数 */
int cloud_bind_unbind(const cloud_endpoint_t* ep, int* deleted_albums)
{
    char path[256];
    char resp[RESP_CAP];
    int st = 0;
    int rl = 0;
    int rc;

    if (deleted_albums != NULL) *deleted_albums = 0;
    if (ep == NULL) return CLOUD_E_INVAL;

    snprintf(path, sizeof(path), "%s/bind", ep->base_path);
    rc = http_request(ep, "DELETE", path, NULL, NULL, NULL, 0, NULL,
                      &st, resp, sizeof(resp), &rl);
    if (rc != CLOUD_OK) return rc;
    if (st < 200 || st >= 300) return CLOUD_E_HTTP;

    if (deleted_albums != NULL) json_get_int(resp, "deleted_albums", deleted_albums);
    return CLOUD_OK;
}
