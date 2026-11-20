/*
 * album_sync.c - 相册云同步业务逻辑（状态机 + 断点续传 + 删除）
 *
 * 上传流程：
 *   IDLE/FAILED -> UPLOADING -> (创建云端相册) -> (查询已上传照片)
 *        -> 逐张上传缺失照片（失败重试）-> UPLOADED
 *
 * 删除流程：
 *   UPLOADED/FAILED/IDLE -> DELETING -> (删除云端相册) -> DELETED
 *
 * 说明：
 *   - 删除只作用于云端，本地相册文件原样保留
 *   - 每上传成功一张就落盘一次状态，掉电重启后可续传
 *   - 上传中的相册不允许删除（返回 CLOUD_E_BUSY），避免状态打架
 *   - 未绑定账号时直接拒绝上传（CLOUD_E_STATE）
 *
 * 内存策略：照片名列表与云端相册结构都放在 static 区，不占任务栈；
 * 本模块同样不可重入（手表端由单一同步任务调用）。
 */
#include "album_sync.h"
#include "album_store.h"
#include "account_bind.h"

#include <stdio.h>
#include <string.h>

#define MAX_ALBUMS       32
#define MAX_PHOTOS       CLOUD_MAX_PHOTOS
#define UPLOAD_RETRY     3
#define RETRY_BACKOFF_MS 300

static album_sync_info_t g_albums[MAX_ALBUMS];
static int g_album_count;
static cloud_endpoint_t g_ep;
static int g_ready;

static album_sync_info_t* find_album(const char* album_id)
{
    int i;
    for (i = 0; i < g_album_count; ++i)
        if (strcmp(g_albums[i].album_id, album_id) == 0) return &g_albums[i];
    return NULL;
}

/* 按相册 ID 查找同步记录，没有则追加一条（上限 MAX_ALBUMS） */
static album_sync_info_t* find_or_add(const char* album_id)
{
    album_sync_info_t* it = find_album(album_id);
    if (it != NULL) return it;
    if (g_album_count >= MAX_ALBUMS) return NULL;
    it = &g_albums[g_album_count++];
    memset(it, 0, sizeof(*it));
    snprintf(it->album_id, sizeof(it->album_id), "%s", album_id);
    it->state = SYNC_IDLE;
    return it;
}

/* 把当前内存中的同步记录整体写回 .sync_state（调用频繁，但数据量很小） */
static void persist(void)
{
    if (g_ready)
        album_store_save_state(g_albums, g_album_count);
}

/* 云端照片名哈希是否已存在：用于跳过已上传的照片 */
static int contains_hash(const unsigned int* list, int n, unsigned int hash)
{
    int i;
    for (i = 0; i < n; ++i)
        if (list[i] == hash) return 1;
    return 0;
}

/* 交给云客户端按块读取照片，每次只占一个 chunk 的内存 */
static int photo_reader(void* ctx, void* buf, int max)
{
    return hal_fs_read_some((hal_file_t*)ctx, buf, max);
}

const char* album_sync_state_name(sync_state_t s)
{
    switch (s) {
    case SYNC_IDLE:      return "IDLE";
    case SYNC_UPLOADING: return "UPLOADING";
    case SYNC_UPLOADED:  return "UPLOADED";
    case SYNC_DELETING:  return "DELETING";
    case SYNC_DELETED:   return "DELETED";
    case SYNC_FAILED:    return "FAILED";
    default:             return "UNKNOWN";
    }
}

int album_sync_init(const char* local_root, const char* state_path,
                    const cloud_endpoint_t* ep)
{
    int n = 0;
    int i;

    if (local_root == NULL || state_path == NULL || ep == NULL) return CLOUD_E_INVAL;

    g_ep = *ep;
    g_album_count = 0;
    album_store_init(local_root, state_path);
    album_store_load_state(g_albums, MAX_ALBUMS, &n);
    g_album_count = n;

    /* 上次掉电时停在“上传中/删除中”的，一律标记失败以便重试 */
    for (i = 0; i < g_album_count; ++i) {
        if (g_albums[i].state == SYNC_UPLOADING || g_albums[i].state == SYNC_DELETING) {
            g_albums[i].state = SYNC_FAILED;
            g_albums[i].last_error = CLOUD_E_STATE;
        }
    }

    g_ready = 1;
    persist();
    return CLOUD_OK;
}

void album_sync_deinit(void)
{
    persist();
    g_ready = 0;
    g_album_count = 0;
}

int album_sync_upload(const char* album_id, sync_progress_cb cb, void* user)
{
    static char photos[MAX_PHOTOS][HAL_NAME_MAX];
    static cloud_album_t remote; /* 结构体较大，避免占用业务任务栈 */
    album_sync_info_t* it;
    int total = 0;
    int i;
    int rc;

    if (!g_ready || album_id == NULL || *album_id == '\0') return CLOUD_E_INVAL;
    if (!account_bind_is_bound()) return CLOUD_E_STATE; /* 未绑定华为云相册，禁止上传 */

    rc = album_store_list_photos(album_id, photos, MAX_PHOTOS, &total);
    if (rc != CLOUD_OK) return rc;
    if (total <= 0) return CLOUD_E_LOCAL; /* 本地相册为空或不存在 */

    it = find_or_add(album_id);
    if (it == NULL) return CLOUD_E_NOMEM;
    if (it->state == SYNC_UPLOADING) return CLOUD_E_BUSY;

    it->state = SYNC_UPLOADING;
    it->total_photos = total;
    it->uploaded_photos = 0;
    it->last_error = 0;
    persist();

    /* 1. 幂等创建云端相册 */
    rc = cloud_album_create(&g_ep, album_id);
    if (rc != CLOUD_OK) goto fail;

    /* 2. 拉取云端已有照片，用于断点续传 */
    memset(&remote, 0, sizeof(remote));
    rc = cloud_album_get(&g_ep, album_id, &remote);
    if (rc != CLOUD_OK) goto fail;

    it->uploaded_photos = remote.photo_count;
    if (it->uploaded_photos > total) it->uploaded_photos = total;
    if (cb != NULL) cb(album_id, it->uploaded_photos, total, user);

    /* 3. 逐张流式上传缺失的照片 */
    for (i = 0; i < total; ++i) {
        hal_file_t* file = NULL;
        int file_size = 0;
        int attempt_rc = CLOUD_E_NET;
        int a;

        /* 云端已有同名照片则跳过，实现断点续传；哈希碰撞概率可忽略 */
        if (contains_hash(remote.photo_hash, remote.photo_count,
                          cloud_name_hash(photos[i])))
            continue; /* 云端已存在，直接跳过 */

        rc = album_store_open_photo(album_id, photos[i], &file, &file_size);
        if (rc != CLOUD_OK) goto fail;

        /* 网络抖动时重试：每次重试前把文件指针复位，避免只发剩余一半 */
        for (a = 0; a < UPLOAD_RETRY; ++a) {
            if (a > 0) hal_fs_rewind(file); /* 重试前回到文件头重发 */
            attempt_rc = cloud_photo_put_stream(&g_ep, album_id, photos[i],
                                                file_size, photo_reader, file);
            if (attempt_rc == CLOUD_OK) break;
            hal_sleep_ms(RETRY_BACKOFF_MS * (a + 1));
        }
        hal_fs_close(file);

        if (attempt_rc != CLOUD_OK) {
            rc = attempt_rc;
            goto fail;
        }

        it->uploaded_photos++;
        persist();
        if (cb != NULL) cb(album_id, it->uploaded_photos, total, user);
    }

    it->state = SYNC_UPLOADED;
    it->last_error = 0;
    persist();
    if (cb != NULL) cb(album_id, total, total, user);
    return CLOUD_OK;

fail:
    it->state = SYNC_FAILED;
    it->last_error = rc;
    persist();
    return rc;
}

/*
 * 删除云端相册（本地照片始终保留）。
 *
 * 处理顺序：
 *   1. 检查参数与状态：上传中的相册禁止删除，避免上传/删除互相覆盖状态
 *   2. 先把状态落盘为 DELETING：万一掉电，重启时会归一化成 FAILED，可重试
 *   3. 请求云端删除：
 *        - 2xx        删除成功
 *        - 404        云端本来就没有，按幂等成功处理（重复删除不报错）
 *        - 其他错误   回滚到删除前状态并记录错误码，本次返回失败
 *   4. 成功后置 DELETED，并清零上传进度，避免界面继续显示"已上传 N 张"
 *
 * 注意：本函数只影响云端与同步记录，不会删除本地任何照片文件。
 */
int album_sync_delete_cloud(const char* album_id)
{
    album_sync_info_t* it;
    sync_state_t prev_state; /* 删除失败时用于回滚 */
    int rc;

    /* 1. 基本检查 */
    if (!g_ready || album_id == NULL || *album_id == '\0') return CLOUD_E_INVAL;

    it = find_or_add(album_id);
    if (it == NULL) return CLOUD_E_NOMEM;

    if (it->state == SYNC_UPLOADING)
        return CLOUD_E_BUSY; /* 上传与删除不能同时进行 */

    /* 2. 先落盘"删除中"，让掉电重启后能识别出这次操作被中断 */
    prev_state = it->state;
    it->state = SYNC_DELETING;
    persist();

    /* 3. 调用云端删除接口（不关心返回体，只看错误码） */
    rc = cloud_album_delete(&g_ep, album_id);

    /* 404 表示云端已经没有这个相册，等价于删除成功（幂等） */
    if (rc == CLOUD_E_NOTFOUND) rc = CLOUD_OK;

    if (rc != CLOUD_OK) {
        /* 失败回滚：恢复删除前状态，只更新错误码，上层可再次调用重试 */
        it->state = prev_state;
        it->last_error = rc;
        persist();
        return rc;
    }

    /* 4. 删除成功：标记 DELETED 并清零上传进度 */
    it->state = SYNC_DELETED;
    it->uploaded_photos = 0;
    it->last_error = 0;
    persist();
    return CLOUD_OK;
}

int album_sync_get(const char* album_id, album_sync_info_t* out)
{
    album_sync_info_t* it;

    if (album_id == NULL || out == NULL) return CLOUD_E_INVAL;
    it = find_album(album_id);
    if (it == NULL) return CLOUD_E_NOTFOUND;
    *out = *it;
    return CLOUD_OK;
}

int album_sync_list(album_sync_info_t* out, int max, int* count)
{
    int n;

    if (count != NULL) *count = 0;
    if (out == NULL || max <= 0) return CLOUD_E_INVAL;

    n = (g_album_count < max) ? g_album_count : max;
    memcpy(out, g_albums, (size_t)n * sizeof(album_sync_info_t));
    if (count != NULL) *count = n;
    return CLOUD_OK;
}

/* 解绑后调用：云端照片已清空，本地同步记录全部回到 IDLE */
void album_sync_reset_all(void)
{
    int i;

    for (i = 0; i < g_album_count; ++i) {
        g_albums[i].state = SYNC_IDLE;
        g_albums[i].total_photos = 0;
        g_albums[i].uploaded_photos = 0;
        g_albums[i].last_error = 0;
    }
    persist();
}
