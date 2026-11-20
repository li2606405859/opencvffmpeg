/*
 * demo.c - 手表相册云同步业务演示（PC 上跑，验证业务逻辑）
 *
 * 用法：
 *   demo ping                               探活云端
 *   demo albums  <local_root>               查看本地相册及同步状态
 *   demo cloud   <local_root>               查看云端相册列表
 *   demo upload  <local_root> <album_id>    上传指定相册
 *   demo upload-all <local_root>            上传全部本地相册
 *   demo delete  <local_root> <album_id>    删除云端相册（本地保留）
 *   demo status  <local_root>               查看已记录的同步状态
 *
 * 可选参数：--host <ip>  --port <port>，默认 127.0.0.1:8080
 */
#include "album_sync.h"
#include "album_store.h"
#include "account_bind.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define DEMO_MAX_ALBUMS 32
#define DEMO_MAX_PHOTOS CLOUD_MAX_PHOTOS

static cloud_endpoint_t g_ep;

/* 把内部错误码翻译成可读文本，方便在串口/控制台定位问题 */
static const char* err_name(int rc)
{
    switch (rc) {
    case CLOUD_OK:       return "OK";
    case CLOUD_E_INVAL:  return "E_INVAL";
    case CLOUD_E_NET:    return "E_NET (cloud unreachable?)";
    case CLOUD_E_HTTP:   return "E_HTTP";
    case CLOUD_E_SERVER: return "E_SERVER";
    case CLOUD_E_LOCAL:  return "E_LOCAL";
    case CLOUD_E_STATE:  return "E_STATE";
    case CLOUD_E_BUSY:   return "E_BUSY (uploading)";
    case CLOUD_E_NOTFOUND: return "E_NOTFOUND";
    case CLOUD_E_NOMEM:  return "E_NOMEM";
    default:             return "E_UNKNOWN";
    }
}

/* 上传进度回调：每次上传成功一张就会触发，便于界面刷新进度条 */
static void on_progress(const char* album_id, int uploaded, int total, void* user)
{
    (void)user;
    printf("  [progress] %s : %d/%d\n", album_id, uploaded, total);
    fflush(stdout);
}

static void usage(void)
{
    printf("Usage:\n");
    printf("  demo ping\n");
    printf("  demo albums <local_root>\n");
    printf("  demo cloud <local_root>\n");
    printf("  demo upload <local_root> <album_id>\n");
    printf("  demo upload-all <local_root>\n");
    printf("  demo delete <local_root> <album_id>\n");
    printf("  demo bind-send <local_root> <phone>\n");
    printf("  demo bind-verify <local_root> <phone> <code>\n");
    printf("  demo bind-status <local_root>\n");
    printf("  demo unbind <local_root>\n");
    printf("  demo status <local_root>\n");
    printf("Options: --host <ip> --port <port>\n");
}

static void print_info(const album_sync_info_t* it)
{
    printf("  %-20s state=%-9s photos=%d/%d",
           it->album_id, album_sync_state_name(it->state),
           it->uploaded_photos, it->total_photos);
    if (it->last_error != 0)
        printf(" last_error=%s", err_name(it->last_error));
    printf("\n");
}

static int cmd_albums(const char* root)
{
    char ids[DEMO_MAX_ALBUMS][CLOUD_ALBUM_ID_MAX];
    int n = 0;
    int i;
    int rc = album_store_list_albums(ids, DEMO_MAX_ALBUMS, &n);

    if (rc != CLOUD_OK) {
        printf("list local albums failed: %s\n", err_name(rc));
        return rc;
    }

    printf("local root: %s\n", root);
    printf("local albums: %d\n", n);
    for (i = 0; i < n; ++i) {
        static char photos[DEMO_MAX_PHOTOS][HAL_NAME_MAX];
        album_sync_info_t info;
        const char* state = "NOT-SYNCED";
        int pc = 0;

        if (album_store_list_photos(ids[i], photos, DEMO_MAX_PHOTOS, &pc) != CLOUD_OK)
            pc = -1;
        if (album_sync_get(ids[i], &info) == CLOUD_OK)
            state = album_sync_state_name(info.state);
        printf("  %-20s photos=%d  state=%s\n", ids[i], pc, state);
    }
    return CLOUD_OK;
}

static int cmd_cloud(const char* root)
{
    char ids[DEMO_MAX_ALBUMS][CLOUD_ALBUM_ID_MAX];
    int n = 0;
    int i;
    int rc;

    (void)root;
    rc = cloud_album_list(&g_ep, ids, DEMO_MAX_ALBUMS, &n);
    if (rc != CLOUD_OK) {
        printf("list cloud albums failed: %s\n", err_name(rc));
        return rc;
    }

    printf("cloud albums: %d\n", n);
    for (i = 0; i < n; ++i) {
        cloud_album_t album;
        if (cloud_album_get(&g_ep, ids[i], &album) == CLOUD_OK)
            printf("  %-20s photos=%d\n", ids[i], album.photo_count);
        else
            printf("  %s\n", ids[i]);
    }
    return CLOUD_OK;
}

static int cmd_upload(const char* album_id)
{
    int rc;

    printf("upload album '%s' -> cloud ...\n", album_id);
    rc = album_sync_upload(album_id, on_progress, NULL);
    if (rc == CLOUD_OK)
        printf("upload done: %s\n", album_id);
    else
        printf("upload failed: %s (%s)\n", album_id, err_name(rc));
    return rc;
}

static int cmd_upload_all(const char* root)
{
    char ids[DEMO_MAX_ALBUMS][CLOUD_ALBUM_ID_MAX];
    int n = 0;
    int i;
    int rc = album_store_list_albums(ids, DEMO_MAX_ALBUMS, &n);

    (void)root;
    if (rc != CLOUD_OK) {
        printf("list local albums failed: %s\n", err_name(rc));
        return rc;
    }

    printf("uploading %d album(s) ...\n", n);
    for (i = 0; i < n; ++i) {
        rc = cmd_upload(ids[i]);
        if (rc != CLOUD_OK) return rc;
    }
    return CLOUD_OK;
}

static int cmd_delete(const char* album_id)
{
    int rc;

    printf("delete album '%s' from cloud (local files kept) ...\n", album_id);
    rc = album_sync_delete_cloud(album_id);
    if (rc == CLOUD_OK)
        printf("cloud album deleted: %s\n", album_id);
    else
        printf("delete failed: %s (%s)\n", album_id, err_name(rc));
    return rc;
}

static int cmd_bind_send(const char* phone)
{
    int expires = 0;
    int rc = account_bind_request_code(phone, &expires);

    if (rc == CLOUD_OK)
        printf("sms code sent to %s (valid %ds), check mock cloud log\n", phone, expires);
    else
        printf("send sms code failed: %s\n", err_name(rc));
    return rc;
}

static int cmd_bind_verify(const char* phone, const char* code)
{
    int rc = account_bind_confirm(phone, code);

    if (rc == CLOUD_OK) {
        account_bind_status_t st;
        account_bind_get(&st);
        printf("bound to Huawei Cloud Photos: phone=%s account=%s\n", st.phone, st.account);
    } else {
        printf("bind failed: %s\n", err_name(rc));
    }
    return rc;
}

static int cmd_bind_status(void)
{
    account_bind_status_t st;
    int rc = account_bind_get(&st);

    if (rc != CLOUD_OK) return rc;
    if (st.bound)
        printf("bound: phone=%s account=%s\n", st.phone, st.account);
    else
        printf("not bound\n");
    return CLOUD_OK;
}

static int cmd_unbind(void)
{
    int rc;

    printf("unbind Huawei Cloud Photos; cloud photos will be deleted ...\n");
    rc = account_bind_unbind();
    if (rc == CLOUD_OK)
        printf("unbound, cloud photos deleted, local files kept\n");
    else
        printf("unbind failed: %s\n", err_name(rc));
    return rc;
}

static int cmd_status(void)
{
    album_sync_info_t list[DEMO_MAX_ALBUMS];
    int n = 0;
    int i;
    int rc = album_sync_list(list, DEMO_MAX_ALBUMS, &n);

    if (rc != CLOUD_OK) {
        printf("read sync status failed: %s\n", err_name(rc));
        return rc;
    }

    printf("sync records: %d\n", n);
    for (i = 0; i < n; ++i) print_info(&list[i]);
    return CLOUD_OK;
}

/*
 * 参数解析：
 *   --host/--port 可与子命令任意顺序出现，子命令及其参数最多 4 个。
 * 注意 albums/cloud/upload 等命令的 <local_root> 是"包含相册目录的根"，
 * 即 <local_root>/<album_id>/photo.jpg。
 */
int main(int argc, char** argv)
{
    const char* pos[4];
    int np = 0;
    const char* host = "127.0.0.1";
    int port = 8080;
    const char* cmd;
    const char* root;
    char state_path[HAL_PATH_MAX];
    int i;
    int rc;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (np < 4) {
            pos[np++] = argv[i];
        }
    }

    if (np < 1) {
        usage();
        return 1;
    }
    cmd = pos[0];

    if (hal_net_init() != 0) {
        printf("network init failed\n");
        return 1;
    }
    cloud_endpoint_init(&g_ep, host, port);

    if (strcmp(cmd, "ping") == 0) {
        rc = cloud_ping(&g_ep);
        printf("ping %s:%d -> %s\n", g_ep.host, g_ep.port, err_name(rc));
        hal_net_deinit();
        return rc == CLOUD_OK ? 0 : 1;
    }

    if (np < 2) {
        usage();
        hal_net_deinit();
        return 1;
    }
    root = pos[1];
    snprintf(state_path, sizeof(state_path), "%s/.sync_state", root);
    {
        char bind_path[HAL_PATH_MAX];
        snprintf(bind_path, sizeof(bind_path), "%s/.bind_state", root);
        rc = account_bind_init(&g_ep, bind_path);
        if (rc != CLOUD_OK) {
            printf("bind init failed: %s\n", err_name(rc));
            hal_net_deinit();
            return 1;
        }
    }

    rc = album_sync_init(root, state_path, &g_ep);
    if (rc != CLOUD_OK) {
        printf("sync init failed: %s\n", err_name(rc));
        hal_net_deinit();
        return 1;
    }

    if (strcmp(cmd, "albums") == 0) {
        rc = cmd_albums(root);
    } else if (strcmp(cmd, "cloud") == 0) {
        rc = cmd_cloud(root);
    } else if (strcmp(cmd, "upload") == 0) {
        rc = (np >= 3) ? cmd_upload(pos[2]) : (usage(), CLOUD_E_INVAL);
    } else if (strcmp(cmd, "upload-all") == 0) {
        rc = cmd_upload_all(root);
    } else if (strcmp(cmd, "delete") == 0) {
        rc = (np >= 3) ? cmd_delete(pos[2]) : (usage(), CLOUD_E_INVAL);
    } else if (strcmp(cmd, "bind-send") == 0) {
        rc = (np >= 3) ? cmd_bind_send(pos[2]) : (usage(), CLOUD_E_INVAL);
    } else if (strcmp(cmd, "bind-verify") == 0) {
        rc = (np >= 4) ? cmd_bind_verify(pos[2], pos[3]) : (usage(), CLOUD_E_INVAL);
    } else if (strcmp(cmd, "bind-status") == 0) {
        rc = cmd_bind_status();
    } else if (strcmp(cmd, "unbind") == 0) {
        rc = cmd_unbind();
    } else if (strcmp(cmd, "status") == 0) {
        rc = cmd_status();
    } else {
        usage();
        rc = CLOUD_E_INVAL;
    }

    album_sync_deinit();
    hal_net_deinit();
    return rc == CLOUD_OK ? 0 : 1;
}
