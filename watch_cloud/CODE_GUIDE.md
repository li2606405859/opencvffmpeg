# watch_cloud 代码说明

手表端相册云同步项目：本地相册上传到华为云 OBS、删除云端相册、用手机短信绑定/解绑账号。
手表不保存华为云 AK/SK，所有云端操作都经过自建后端代理。

## 1. 总体架构

```
┌──────────────┐   HTTP/1.1 + Bearer token   ┌──────────────┐
│  手表端固件   │ ──────────────────────────> │  后端代理     │
│  (本项目 C)  │ <────────────────────────── │ server.py    │
└──────────────┘        JSON / 照片二进制      └──────┬───────┘
       │                                             │
       │ 只保存 token                                 ├─ OBS 签名 V2（HMAC-SHA1）→ 华为云 OBS
       │ 不接触 AK/SK                                 └─ WSSE（HMAC-SHA256）→ 华为云 MSGSMS
       │
       └─ 本地相册目录 <root>/<album_id>/photo.jpg
```

- 手表端只依赖 HAL（socket / 文件 / 时间），业务代码可整体移植到嵌入式平台。
- 后端负责持有华为云凭据、生成签名、校验 token；无凭据时可切到 local 目录模式做联调。

## 2. 目录结构

```
watch_cloud/
├── include/
│   ├── cloud_hal.h        HAL 抽象：TCP、目录枚举、文件读写、时间
│   ├── cloud_client.h     云端 REST 客户端与错误码
│   ├── album_store.h      本地相册扫描 + 同步状态文件
│   ├── album_sync.h       同步状态机与上传/删除业务
│   └── account_bind.h     短信绑定/解绑业务
├── src/
│   ├── cloud_client.c     极简 HTTP/1.1 客户端、JSON 取值、FNV 哈希
│   ├── album_store.c      目录扫描、.sync_state 读写
│   ├── album_sync.c       上传（断点续传/重试）、删除、状态持久化
│   ├── account_bind.c     验证码流程、token 与绑定状态持久化
│   └── hal_pc.c           PC 版 HAL（Windows Winsock / POSIX）
├── examples/demo.c        命令行演示：绑定/上传/列表/删除/解绑
├── backend/
│   ├── server.py          华为云代理（OBS + MSGSMS）
│   └── config.ini.example 后端配置模板
├── mock_cloud/server.py   纯本地模拟云（无华为云时联调用）
└── build.bat              g++ 构建 demo.exe
```

## 3. 分层与模块职责

| 层 | 文件 | 职责 | 关键点 |
|---|---|---|---|
| HAL | `cloud_hal.h` / `hal_pc.c` | 屏蔽平台差异 | 上手表时只替换本层实现 |
| 传输 | `cloud_client.c` | HTTP、URL 编码、JSON 取值、哈希 | 短连接、Content-Length、流式 body |
| 存储 | `album_store.c` | 扫相册、读文件流、读写状态文件 | static 缓冲，避免占任务栈 |
| 业务 | `album_sync.c` | 上传/断点续传/删除/状态机 | 幂等、重试、逐张落盘 |
| 账号 | `account_bind.c` | 短信验证码、token、绑定状态 | 未绑定禁止上传 |
| 演示 | `demo.c` | 命令行入口 | `--host/--port` 可调后端地址 |
| 后端 | `backend/server.py` | 华为云 OBS/MSGSMS 代理 | 凭据隔离、token 鉴权 |

## 4. 关键数据结构

```c
/* 云端地址（cloud_client.h） */
typedef struct {
    char host[64];      // 后端 IP/域名
    int  port;          // 后端端口
    char base_path[64]; // 默认 /api/v1
    int  timeout_ms;    // 连接与收发超时
} cloud_endpoint_t;

/* 云端相册（cloud_client.h）：只存照片名哈希，64 张约 256B */
typedef struct {
    char album_id[64];
    unsigned int photo_hash[64];
    int  photo_count;
} cloud_album_t;

/* 同步状态（album_sync.h） */
typedef struct {
    char album_id[64];
    sync_state_t state;
    int total_photos;     // 本地张数
    int uploaded_photos;  // 云端已有张数
    int last_error;       // 最近一次错误码
} album_sync_info_t;
```

## 5. 业务流程

### 5.1 绑定（短信验证码）

```
用户输入手机号
  → account_bind_request_code
  → POST /api/v1/sms/code
  → 后端生成 6 位码（300 秒有效），调用华为云 MSGSMS
  → 用户收到短信，输入验证码
  → account_bind_confirm
  → POST /api/v1/sms/verify
  → 后端校验通过，签发 token，落盘 bind.json
  → 手表保存 phone / account / token 到 .bind_state，并调用 cloud_token_set()
```

### 5.2 上传（断点续传）

```
album_sync_upload(album_id)
  ├─ 未绑定 → 直接返回 CLOUD_E_STATE
  ├─ 本地无照片 → CLOUD_E_LOCAL
  ├─ 状态置 UPLOADING 并落盘
  ├─ PUT /albums/{id}                 幂等创建云端相册
  ├─ GET /albums/{id}                 取云端已有照片名哈希
  ├─ 对每张本地照片：
  │    哈希命中 → 跳过（续传）
  │    未命中   → 打开文件流 → PUT /albums/{id}/photos/{name}
  │             失败重试 3 次，每次重试前 rewind 文件指针
  │             成功后 uploaded_photos++，立即落盘状态
  └─ 全部完成 → 状态 UPLOADED
```

内存占用：照片按 2KB 块边读边发，与文件大小无关；照片名列表与云端结构放 static 区。

### 5.3 删除云端相册

```
album_sync_delete_cloud(album_id)
  ├─ 上传中 → CLOUD_E_BUSY
  ├─ 状态置 DELETING 并落盘
  ├─ DELETE /albums/{id}
  ├─ 404 → 视为已删除（幂等成功）
  └─ 状态 DELETED，本地文件不动
```

### 5.4 解绑

```
account_bind_unbind()
  ├─ DELETE /api/v1/bind （后端删除该账号全部云端对象）
  ├─ 清空本地 phone/account/token，写空 .bind_state
  ├─ cloud_token_clear()
  └─ album_sync_reset_all()：所有同步记录回到 IDLE
```

### 5.5 掉电恢复

- 每次状态变化都全量写 `.sync_state`。
- 启动时把残留的 `UPLOADING` / `DELETING` 统一改为 `FAILED`，由用户或上层重试。
- 重新上传时通过云端照片名哈希比对自动续传。

## 6. 同步状态机

```
         upload                 成功
IDLE ───────────> UPLOADING ──────────> UPLOADED
 ↑                   │ 失败                │
 │                   ↓                    │ delete
 └──── reset_all ── FAILED <──────┐       ↓
                     │ 重试上传     │   DELETING
                     └──────────────┘       │ 成功
                                            ↓
                                         DELETED
```

## 7. 后端 HTTP 接口

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| GET | `/api/v1/ping` | 否 | 探活 |
| GET | `/api/v1/albums` | 是 | 列出云端相册 |
| PUT | `/api/v1/albums/{album}` | 是 | 创建相册（幂等） |
| GET | `/api/v1/albums/{album}` | 是 | 相册照片列表，不存在 404 |
| DELETE | `/api/v1/albums/{album}` | 是 | 删除云端相册 |
| PUT | `/api/v1/albums/{album}/photos/{name}` | 是 | 上传照片原始二进制 |
| POST | `/api/v1/sms/code` | 否 | `{"phone":"..."}` 下发验证码 |
| POST | `/api/v1/sms/verify` | 否 | `{"phone","code"}`，返回 `account` + `token` |
| GET | `/api/v1/bind` | 视状态 | 查询绑定信息 |
| DELETE | `/api/v1/bind` | 是 | 解绑并删除全部云端照片 |

鉴权头：`Authorization: Bearer <token>`。

## 8. 错误码（cloud_client.h）

| 值 | 名称 | 含义 |
|---|---|---|
| 0 | `CLOUD_OK` | 成功 |
| -1 | `CLOUD_E_INVAL` | 参数非法 |
| -2 | `CLOUD_E_NET` | 网络不可达/超时 |
| -3 | `CLOUD_E_HTTP` | HTTP 状态异常（含 401） |
| -4 | `CLOUD_E_SERVER` | 云端返回数据异常 |
| -5 | `CLOUD_E_LOCAL` | 本地文件异常 |
| -6 | `CLOUD_E_STATE` | 未绑定等状态异常 |
| -7 | `CLOUD_E_BUSY` | 正在上传，禁止删除 |
| -8 | `CLOUD_E_NOTFOUND` | 云端不存在 |
| -9 | `CLOUD_E_NOMEM` | 数量/大小超限 |

## 9. 本地文件格式

| 文件 | 位置 | 格式 |
|---|---|---|
| `.bind_state` | `<local_root>/` | `phone \t account \t token` |
| `.sync_state` | `<local_root>/` | `album_id \t state \t total \t uploaded \t last_error`（每行一条） |
| `bind.json` | `backend/state/` | `{"phone","account","token","bound_at"}` |

`.bind_state` 解绑后写为空文件；`.sync_state` 每次变更全量重写。

## 10. 后端实现要点

- **OBS 签名 V2**：`StringToSign = VERB\nMD5\nContent-Type\nDate\nCanonicalizedResource`，
  `HMAC-SHA1` 后用 Base64，头为 `Authorization: OBS <AK>:<签名>`。
  本项目不发送 MD5/Content-Type/x-obs-*，对应字段留空。
- **对象键布局**：`albums/<album>/<photo>`；相册占位对象 `albums/<album>/`。
- **列目录**：`prefix` + `delimiter=/` 取 CommonPrefixes 作为相册列表，按
  `IsTruncated`/`NextMarker` 翻页（单页最多 1000）。
- **流式转发**：从手表 `rfile` 分块读，直接写 OBS 连接，不在后端落盘。
- **短信 WSSE**：`PasswordDigest = Base64(SHA256(nonce + created + app_secret))`，
  `Content-Type: application/x-www-form-urlencoded`，参数含 `from/to/templateId/templateParas/signature`。
- **token 校验**：`hmac.compare_digest` 定长比较，避免时序侧信道。
- **模式**：`mode=huawei|local|auto`；`auto` 在 AK/SK/桶齐全时走华为云，否则用本地目录。

## 11. 构建与运行

### 11.1 编译手表端演示

```powershell
cd watch_cloud
.\build.bat          # 产出 demo.exe
```

### 11.2 用模拟云（纯本地）

```powershell
python mock_cloud\server.py 8080 <存储目录>
.\demo.exe bind-send <root> 13800138000
# 在 mock 控制台看验证码
.\demo.exe bind-verify <root> 13800138000 <验证码>
.\demo.exe upload <root> <相册名>
```

### 11.3 用真实华为云

1. `copy backend\config.ini.example backend\config.ini`，填写 AK/SK、桶名、短信参数
   （也可用环境变量 `HUAWEI_AK`、`HUAWEI_SK`、`HUAWEI_OBS_BUCKET` 等覆盖）。
2. `python backend\server.py`（默认 `127.0.0.1:8080`）。
3. 手表/演示程序指向该地址，按绑定 → 上传 → 列表 → 删除/解绑流程操作。

### 11.4 常用 demo 命令

```
demo ping
demo bind-send  <root> <phone>
demo bind-verify <root> <phone> <code>
demo bind-status <root>
demo albums     <root>
demo status     <root>
demo upload     <root> <album>
demo upload-all <root>
demo cloud      <root>
demo delete     <root> <album>
demo unbind     <root>
```

注意：`<root>` 是包含相册目录的根，即 `<root>/<album_id>/photo.jpg`。

## 12. 移植到手表

只需替换 `hal_pc.c`，保持 `cloud_hal.h` 接口不变：

- 网络：lwIP / RW007 / ESP-AT 等，提供阻塞式 connect/send/recv。
- 文件：FATFS/LittleFS，提供目录枚举与文件流读取。
- 时间：毫秒延时与单调时钟。

注意：若设备走 HTTPS，需要在 HAL 内接入 TLS（如 mbedTLS），业务层无需改动。
当前实现是明文 HTTP，生产环境建议后端通过 HTTPS 暴露。

## 13. 安全模型

- AK/SK 只存在后端进程/配置中，手表端只有 `token`。
- token 由后端在短信验证通过后签发，解绑即失效。
- 所有相册/照片接口都校验 token；未绑定直接 401。
- 短信验证码 300 秒有效且一次性使用。
- 建议后端部署在内网或带 TLS 的网关后，避免 token 明文传输。

## 14. 已知限制

- 单相册最多 64 张照片、单张最大 64MB（`CLOUD_MAX_PHOTOS` / `PHOTO_MAX_BYTES`）。
- 照片名比对使用 32 位 FNV-1a 哈希，理论上存在碰撞（64 张规模下可忽略）。
- 业务层使用 static 缓冲区，不可重入，仅适合单任务调用。
- `.sync_state` 上限 8KB，最多记录几十个相册。
- `demo.exe` 与后端均为联调工具，未做多账号隔离（后端同一时间只维护一份绑定）。
