#!/usr/bin/env python3
"""华为云相册后端：手表端代理。

手表只访问本服务，不接触华为云 AK/SK：
  - 照片存到华为云 OBS（请求签名 V2：HMAC-SHA1 + Base64）
  - 验证码通过华为云消息&短信（MSGSMS，WSSE 鉴权）下发

对外接口与 mock_cloud/server.py 完全一致：
  GET    /api/v1/ping
  GET    /api/v1/albums
  PUT    /api/v1/albums/<album>
  GET    /api/v1/albums/<album>
  DELETE /api/v1/albums/<album>
  PUT    /api/v1/albums/<album>/photos/<name>
  POST   /api/v1/sms/code        {"phone": "..."}
  POST   /api/v1/sms/verify      {"phone": "...", "code": "..."}
  GET    /api/v1/bind
  DELETE /api/v1/bind            (解绑，删除该账号下全部云端照片)

绑定成功后除 ping 与短信接口外的请求都需要：
  Authorization: Bearer <token>

用法：
  python server.py [config.ini]
  python server.py --selftest      # 用占位凭据打印签名样例，便于对照官方文档
"""
import base64
import configparser
import hashlib
import hmac
import json
import os
import secrets
import shutil
import ssl
import sys
import threading
import time
import urllib.parse
import uuid
import xml.etree.ElementTree as ET
from email.utils import formatdate
from http.client import HTTPConnection, HTTPSConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API = "/api/v1"
OBS_PREFIX = "albums/"
CODE_TTL = 300
CHUNK = 65536

BASE_DIR = os.path.dirname(os.path.abspath(__file__))


# ----------------------------- 通用工具 -----------------------------

def _json_bytes(payload):
    return json.dumps(payload, ensure_ascii=False).encode("utf-8")


def _local_tag(tag):
    return tag.rsplit("}", 1)[-1]


def _safe_part(name):
    """拒绝目录穿越，防止本地模式写出存储目录。"""
    return bool(name) and name not in (".", "..") and "/" not in name and "\\" not in name


def _digits(phone):
    return "".join(ch for ch in phone if ch.isdigit())


# ----------------------------- 本地存储 -----------------------------

class LocalStorage:
    """无凭据时的本地目录实现，接口与华为云 OBS 实现一致。"""

    def __init__(self, root):
        self.root = os.path.abspath(root)
        os.makedirs(self.root, exist_ok=True)

    def _album_dir(self, album):
        return os.path.join(self.root, album)

    def create_album(self, album):
        os.makedirs(self._album_dir(album), exist_ok=True)

    def list_albums(self):
        return sorted(d for d in os.listdir(self.root)
                      if os.path.isdir(os.path.join(self.root, d)))

    def list_photos(self, album):
        d = self._album_dir(album)
        if not os.path.isdir(d):
            return None
        return sorted(f for f in os.listdir(d) if os.path.isfile(os.path.join(d, f)))

    def put_photo(self, album, name, length, read_chunk):
        """先写 .part 再原子改名，避免掉电留下半张照片。"""
        d = self._album_dir(album)
        os.makedirs(d, exist_ok=True)
        tmp = os.path.join(d, name + ".part")
        remaining = length
        with open(tmp, "wb") as f:
            while remaining > 0:
                chunk = read_chunk(min(CHUNK, remaining))
                if not chunk:
                    raise IOError("请求体提前结束")
                f.write(chunk)
                remaining -= len(chunk)
        os.replace(tmp, os.path.join(d, name))

    def delete_album(self, album):
        d = self._album_dir(album)
        if not os.path.isdir(d):
            return False
        shutil.rmtree(d)
        return True

    def delete_all(self):
        """解绑时使用：删除全部相册目录，返回删除的相册数。"""
        count = 0
        for album in self.list_albums():
            shutil.rmtree(self._album_dir(album), ignore_errors=True)
            count += 1
        return count


# ----------------------------- 华为云 OBS -----------------------------

class ObsStorage:
    """对象键布局：albums/<album>/<photo>；相册占位对象为 albums/<album>/。"""

    def __init__(self, ak, sk, bucket, endpoint):
        self.ak = ak
        self.sk = sk
        self.bucket = bucket
        self.endpoint = endpoint

    def _connect(self):
        return HTTPSConnection(self.endpoint, 443, timeout=30,
                               context=ssl.create_default_context())

    @staticmethod
    def _url_path(key):
        return "/" + urllib.parse.quote(key, safe="/")

    def _headers(self, method, key):
        """OBS 签名 V2：

        StringToSign = VERB \\n Content-MD5 \\n Content-Type \\n Date \\n
                       CanonicalizedHeaders + CanonicalizedResource
        本项目不发送 MD5/Content-Type/x-obs-* 头，因此中间两项为空；
        CanonicalizedResource 固定为 /<bucket>/<key>（不含查询参数）。
        """
        date = formatdate(usegmt=True)
        resource = "/%s/%s" % (self.bucket, key)
        string_to_sign = "%s\n\n\n%s\n%s" % (method, date, resource)
        sig = base64.b64encode(
            hmac.new(self.sk.encode("utf-8"), string_to_sign.encode("utf-8"),
                     hashlib.sha1).digest()).decode("ascii")
        return {"Date": date, "Authorization": "OBS %s:%s" % (self.ak, sig)}

    def _list(self, prefix, delimiter=""):
        """列出对象；delimiter="/" 时只返回 CommonPrefixes（相册目录）。

        OBS 单次最多 1000 条，需要按 IsTruncated/NextMarker 翻页。
        """
        keys, prefixes = [], []
        marker = ""
        while True:
            params = {"prefix": prefix, "max-keys": "1000"}
            if delimiter:
                params["delimiter"] = delimiter
            if marker:
                params["marker"] = marker
            query = urllib.parse.urlencode(params, quote_via=urllib.parse.quote, safe="")

            conn = self._connect()
            try:
                conn.request("GET", "/?" + query, headers=self._headers("GET", ""))
                resp = conn.getresponse()
                data = resp.read()
                if resp.status != 200:
                    raise RuntimeError("OBS 列目录失败: HTTP %d %s" % (
                        resp.status, data[:200].decode("utf-8", "replace")))
            finally:
                conn.close()

            root = ET.fromstring(data)
            truncated = False
            next_marker = ""
            last_key = ""
            for el in root:
                tag = _local_tag(el.tag)
                if tag == "Contents":
                    key = ""
                    for c in el:
                        if _local_tag(c.tag) == "Key":
                            key = c.text or ""
                    if key:
                        keys.append(key)
                        last_key = key
                elif tag == "CommonPrefixes":
                    for c in el:
                        if _local_tag(c.tag) == "Prefix":
                            prefixes.append(c.text or "")
                elif tag == "IsTruncated":
                    truncated = (el.text or "").strip().lower() == "true"
                elif tag == "NextMarker":
                    next_marker = el.text or ""

            if not truncated:
                break
            marker = next_marker or last_key
            if not marker:
                break
        return keys, prefixes

    def _delete_key(self, key):
        conn = self._connect()
        try:
            conn.request("DELETE", self._url_path(key), headers=self._headers("DELETE", key))
            resp = conn.getresponse()
            resp.read()
            if resp.status not in (200, 204, 404):
                raise RuntimeError("OBS 删除对象失败: HTTP %d" % resp.status)
            return resp.status
        finally:
            conn.close()

    def _album_prefix(self, album):
        return OBS_PREFIX + album + "/"

    def create_album(self, album):
        key = self._album_prefix(album)
        conn = self._connect()
        try:
            headers = self._headers("PUT", key)
            headers["Content-Length"] = "0"
            conn.request("PUT", self._url_path(key), body=b"", headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            if resp.status not in (200, 201):
                raise RuntimeError("OBS 创建相册失败: HTTP %d %s" % (
                    resp.status, data[:200].decode("utf-8", "replace")))
        finally:
            conn.close()

    def list_albums(self):
        _, prefixes = self._list(OBS_PREFIX, delimiter="/")
        out = []
        for p in prefixes:
            name = p[len(OBS_PREFIX):].rstrip("/")
            if name:
                out.append(name)
        return sorted(out)

    def list_photos(self, album):
        keys, _ = self._list(self._album_prefix(album))
        if not keys:
            return None
        prefix = self._album_prefix(album)
        names = []
        for k in keys:
            rel = k[len(prefix):]
            if rel and not rel.endswith("/"):
                names.append(rel)
        return sorted(names)

    def put_photo(self, album, name, length, read_chunk):
        """流式转发：从手表请求体逐块读、逐块写 OBS，不在后端落盘。"""
        key = self._album_prefix(album) + name
        conn = self._connect()
        try:
            headers = self._headers("PUT", key)
            headers["Content-Length"] = str(length)
            conn.putrequest("PUT", self._url_path(key), skip_accept_encoding=True)
            for k, v in headers.items():
                conn.putheader(k, v)
            conn.endheaders()

            remaining = length
            while remaining > 0:
                chunk = read_chunk(min(CHUNK, remaining))
                if not chunk:
                    raise IOError("请求体提前结束")
                conn.send(chunk)
                remaining -= len(chunk)

            resp = conn.getresponse()
            data = resp.read()
            if resp.status not in (200, 201):
                raise RuntimeError("OBS 上传失败: HTTP %d %s" % (
                    resp.status, data[:200].decode("utf-8", "replace")))
        finally:
            conn.close()

    def delete_album(self, album):
        """删除整本相册：OBS 没有目录概念，需列出前缀下所有对象逐个删除。"""
        keys, _ = self._list(self._album_prefix(album))
        if not keys:
            return False
        for k in keys:
            self._delete_key(k)
        return True

    def delete_all(self):
        """解绑时使用：删除 albums/ 下全部对象，返回涉及的相册数。"""
        keys, _ = self._list(OBS_PREFIX)
        albums = set()
        for k in keys:
            rel = k[len(OBS_PREFIX):]
            albums.add(rel.split("/", 1)[0])
            self._delete_key(k)
        return len(albums)


# ----------------------------- 华为云短信 -----------------------------

def send_sms_code(cfg, phone, code):
    """按 MSGSMS 文档组装 WSSE 头并调用 /sms/batchSendSms/v1。

    WSSE: PasswordDigest = Base64(SHA256(nonce + created + app_secret))，
    created 为 UTC 的 ISO8601（秒级）。模板变量通过 templateParas 传 JSON 数组。
    """
    nonce = uuid.uuid4().hex
    created = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    digest = base64.b64encode(
        hashlib.sha256((nonce + created + cfg["sms_app_secret"]).encode("utf-8")).digest()
    ).decode("ascii")
    wsse = ('UsernameToken Username="%s",PasswordDigest="%s",Nonce="%s",Created="%s"'
            % (cfg["sms_app_key"], digest, nonce, created))

    params = {
        "from": cfg["sms_sender"],
        "to": _digits(phone),
        "templateId": cfg["sms_template_id"],
        "templateParas": cfg["sms_template_paras"].replace("{code}", code),
        "signature": cfg["sms_signature"],
    }
    body = urllib.parse.urlencode(params).encode("utf-8")
    headers = {
        "Content-Type": "application/x-www-form-urlencoded",
        "Authorization": 'WSSE realm="SDP",profile="UsernameToken",type="Appkey"',
        "X-WSSE": wsse,
    }

    conn = HTTPSConnection(cfg["sms_endpoint"], 443, timeout=15,
                           context=ssl.create_default_context())
    try:
        conn.request("POST", "/sms/batchSendSms/v1", body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
    finally:
        conn.close()

    try:
        payload = json.loads(data.decode("utf-8"))
    except Exception:
        raise RuntimeError("短信接口返回非 JSON: HTTP %d" % resp.status)
    if resp.status != 200 or payload.get("code") != "000000":
        raise RuntimeError("短信发送失败: HTTP %d code=%s %s" % (
            resp.status, payload.get("code"), payload.get("description", "")))


# ----------------------------- 配置 -----------------------------

def load_config(path):
    cp = configparser.ConfigParser(interpolation=None)
    if not cp.read(path, encoding="utf-8"):
        print("[backend] 未找到配置文件 %s，使用默认值（local 模式）" % path)

    def get(section, key, env, default=""):
        val = os.environ.get(env)
        if val is not None and val.strip() != "":
            return val.strip()
        return (cp.get(section, key, fallback=default) or "").strip()

    cfg = {
        "host": get("backend", "host", "BACKEND_HOST", "127.0.0.1"),
        "port": int(get("backend", "port", "BACKEND_PORT", "8080")),
        "mode": get("backend", "mode", "BACKEND_MODE", "auto").lower(),
        "state_dir": get("backend", "state_dir", "BACKEND_STATE_DIR", "state"),
        "ak": get("huawei", "ak", "HUAWEI_AK"),
        "sk": get("huawei", "sk", "HUAWEI_SK"),
        "region": get("huawei", "region", "HUAWEI_REGION", "cn-north-4"),
        "obs_endpoint": get("huawei", "obs_endpoint", "HUAWEI_OBS_ENDPOINT",
                            "obs.cn-north-4.myhuaweicloud.com"),
        "obs_bucket": get("huawei", "obs_bucket", "HUAWEI_OBS_BUCKET"),
        "sms_endpoint": get("huawei", "sms_endpoint", "HUAWEI_SMS_ENDPOINT",
                            "smsapi.cn-north-4.myhuaweicloud.com"),
        "sms_app_key": get("huawei", "sms_app_key", "HUAWEI_SMS_APP_KEY"),
        "sms_app_secret": get("huawei", "sms_app_secret", "HUAWEI_SMS_APP_SECRET"),
        "sms_sender": get("huawei", "sms_sender", "HUAWEI_SMS_SENDER"),
        "sms_template_id": get("huawei", "sms_template_id", "HUAWEI_SMS_TEMPLATE_ID"),
        "sms_signature": get("huawei", "sms_signature", "HUAWEI_SMS_SIGNATURE"),
        "sms_template_paras": get("huawei", "sms_template_paras",
                                  "HUAWEI_SMS_TEMPLATE_PARAS", '["{code}"]'),
        "local_storage": get("local", "storage_dir", "BACKEND_STORAGE_DIR", "storage"),
    }

    huawei_ready = bool(cfg["ak"] and cfg["sk"] and cfg["obs_bucket"])
    if cfg["mode"] == "auto":
        cfg["mode"] = "huawei" if huawei_ready else "local"
    if cfg["mode"] == "huawei":
        if not huawei_ready:
            raise SystemExit("[backend] mode=huawei 但 ak/sk/obs_bucket 未配置完整")
        missing = [k for k in ("sms_app_key", "sms_app_secret", "sms_sender",
                               "sms_template_id", "sms_signature") if not cfg[k]]
        if missing:
            raise SystemExit("[backend] 缺少短信参数: %s" % ", ".join(missing))
    elif cfg["mode"] != "local":
        raise SystemExit("[backend] 未知 mode: %s（只支持 huawei/local/auto）" % cfg["mode"])
    return cfg


# ----------------------------- 业务状态 -----------------------------

class Backend:
    def __init__(self, storage, state_dir, sms_sender):
        self.storage = storage
        self.state_dir = state_dir
        self.sms_sender = sms_sender  # 为 None 表示 local 模式，只打印验证码
        self.lock = threading.Lock()
        self.codes = {}
        self.bind = self._load_bind()

    def _bind_path(self):
        return os.path.join(self.state_dir, "bind.json")

    def _load_bind(self):
        try:
            with open(self._bind_path(), "r", encoding="utf-8") as f:
                data = json.load(f)
            return data if data.get("token") else None
        except Exception:
            return None

    def _save_bind(self):
        os.makedirs(self.state_dir, exist_ok=True)
        path = self._bind_path()
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.bind, f, ensure_ascii=False)
        os.replace(tmp, path)

    def issue_code(self, phone):
        """生成 6 位验证码并下发；local 模式只打印到控制台。"""
        code = "%06d" % secrets.randbelow(1000000)
        with self.lock:
            self.codes[phone] = (code, time.time() + CODE_TTL)
        if self.sms_sender is not None:
            self.sms_sender(phone, code)
            print("[backend] 短信已下发: %s" % phone)
        else:
            print("[backend] local 模式验证码 %s -> %s" % (phone, code))
        sys.stdout.flush()
        return CODE_TTL

    def verify_code(self, phone, code):
        """校验并消费验证码；成功后签发 token 并落盘绑定信息。"""
        with self.lock:
            item = self.codes.get(phone)
            if item is None or item[1] < time.time() or item[0] != code:
                return None
            del self.codes[phone]
            token = secrets.token_hex(24)
            self.bind = {
                "phone": phone,
                "account": "huawei-cloud-" + _digits(phone)[-4:],
                "token": token,
                "bound_at": int(time.time()),
            }
            self._save_bind()
            return dict(self.bind)

    def token_ok(self, authorization):
        """定长比较 Authorization 头，避免时序侧信道。"""
        if not self.bind:
            return False
        return hmac.compare_digest(authorization or "", "Bearer " + self.bind["token"])

    def unbind(self):
        """解绑：先删云端全部照片，再清空绑定与 token（照片删失败则保留绑定）。"""
        deleted = self.storage.delete_all()
        with self.lock:
            self.bind = None
            path = self._bind_path()
            if os.path.exists(path):
                os.remove(path)
        return deleted


# ----------------------------- HTTP 服务 -----------------------------

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    backend = None

    def _json(self, code, payload):
        body = _json_bytes(payload)
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _fail(self, code, message, close=False):
        if close:
            self.close_connection = True
        self._json(code, {"error": message})

    def _read_body(self, limit=None):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0:
            return b""
        if limit is not None and n > limit:
            self._fail(413, "body too large", close=True)
            return None
        data = self.rfile.read(n)
        if len(data) != n:
            return None
        return data

    def _parts(self):
        path = urllib.parse.unquote(urllib.parse.urlparse(self.path).path)
        if not path.startswith(API):
            return None
        return [p for p in path[len(API):].split("/") if p]

    def _require_token(self):
        """除 ping/短信外的接口都要求 Bearer token；失败时关闭连接防止请求体残留。"""
        if self.backend.token_ok(self.headers.get("Authorization")):
            return True
        self._fail(401, "unauthorized", close=True)
        return False

    def do_GET(self):
        parts = self._parts()
        if parts is None:
            return self._fail(404, "not found")
        if parts == ["ping"]:
            return self._json(200, {"pong": True})
        if parts == ["bind"]:
            if self.backend.bind and not self._require_token():
                return
            if self.backend.bind:
                return self._json(200, {"bound": True,
                                        "phone": self.backend.bind.get("phone", "")})
            return self._json(200, {"bound": False})
        if parts == ["albums"]:
            if not self._require_token():
                return
            try:
                albums = self.backend.storage.list_albums()
            except Exception as exc:
                return self._fail(502, str(exc), close=True)
            return self._json(200, {"albums": albums})
        if len(parts) == 2 and parts[0] == "albums":
            if not self._require_token():
                return
            if not _safe_part(parts[1]):
                return self._fail(400, "bad album id")
            try:
                photos = self.backend.storage.list_photos(parts[1])
            except Exception as exc:
                return self._fail(502, str(exc), close=True)
            if photos is None:
                return self._fail(404, "album not found")
            return self._json(200, {"album_id": parts[1], "photos": photos,
                                    "count": len(photos)})
        return self._fail(404, "not found")

    def do_PUT(self):
        parts = self._parts()
        if parts is None:
            return self._fail(404, "not found")
        if len(parts) == 2 and parts[0] == "albums":
            if self._read_body(limit=4096) is None:
                return
            if not self._require_token():
                return
            if not _safe_part(parts[1]):
                return self._fail(400, "bad album id")
            try:
                self.backend.storage.create_album(parts[1])
            except Exception as exc:
                return self._fail(502, str(exc), close=True)
            return self._json(200, {"album_id": parts[1]})

        if len(parts) == 4 and parts[0] == "albums" and parts[2] == "photos":
            if not self._require_token():
                return
            album, name = parts[1], parts[3]
            if not _safe_part(album) or not _safe_part(name):
                return self._fail(400, "bad name", close=True)
            try:
                body_len = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                body_len = 0
            if body_len <= 0:
                return self._fail(400, "empty body", close=True)
            if self.backend.storage.list_photos(album) is None:
                return self._fail(404, "album not found", close=True)
            # 不整体读入内存：把 rfile 的分块读取能力交给存储层直接转发
            try:
                self.backend.storage.put_photo(
                    album, name, body_len,
                    lambda n: self.rfile.read(n))
            except Exception as exc:
                return self._fail(502, str(exc), close=True)
            return self._json(201, {"name": name, "size": body_len})

        return self._fail(404, "not found")

    def do_DELETE(self):
        parts = self._parts()
        if parts is None:
            return self._fail(404, "not found")
        if parts == ["bind"]:
            if self.backend.bind and not self._require_token():
                return
            try:
                deleted = self.backend.unbind()
            except Exception as exc:
                return self._fail(502, str(exc), close=True)
            return self._json(200, {"unbound": True, "deleted_albums": deleted})
        if len(parts) == 2 and parts[0] == "albums":
            if not self._require_token():
                return
            if not _safe_part(parts[1]):
                return self._fail(400, "bad album id")
            # 存储层返回 False 表示云端本来就没有该相册
            try:
                ok = self.backend.storage.delete_album(parts[1])
            except Exception as exc:
                return self._fail(502, str(exc), close=True)
            if not ok:
                return self._fail(404, "album not found")
            # 与手表端约定：删除成功返回 200，重复删除由手表按幂等处理
            return self._json(200, {"deleted": True, "album_id": parts[1]})
        return self._fail(404, "not found")

    def do_POST(self):
        parts = self._parts()
        if parts is None:
            self._read_body()
            return self._fail(404, "not found")
        raw = self._read_body(limit=8192)
        if raw is None:
            return
        try:
            data = json.loads(raw.decode("utf-8")) if raw else {}
        except Exception:
            return self._fail(400, "bad json")

        if parts == ["sms", "code"]:
            phone = _digits(str(data.get("phone", "")))
            if len(phone) < 6 or len(phone) > 15:
                return self._fail(400, "phone required")
            try:
                ttl = self.backend.issue_code(phone)
            except Exception as exc:
                return self._fail(502, str(exc))
            return self._json(200, {"sent": True, "expires_in": ttl})

        if parts == ["sms", "verify"]:
            phone = _digits(str(data.get("phone", "")))
            code = str(data.get("code", "")).strip()
            bind = self.backend.verify_code(phone, code)
            if bind is None:
                return self._fail(400, "invalid code")
            return self._json(200, {"bound": True, "phone": bind["phone"],
                                    "account": bind["account"], "token": bind["token"]})

        return self._fail(404, "not found")

    def log_message(self, fmt, *args):
        print("[backend] %s" % (fmt % args))


# ----------------------------- 入口 -----------------------------

def selftest():
    """打印签名样例（占位凭据），用于人工对照华为云官方文档。"""
    date = "Sun, 16 Nov 2026 12:00:00 GMT"
    key = "albums/demo/1.jpg"
    resource = "/demo-bucket/" + key
    sts = "PUT\n\n\n%s\n%s" % (date, resource)
    sig = base64.b64encode(hmac.new(b"demo-sk", sts.encode(), hashlib.sha1).digest()).decode()
    print("OBS StringToSign:")
    print(sts)
    print("OBS Authorization:")
    print("OBS demo-ak:%s" % sig)
    print()
    nonce, created = "abc123", "2026-11-16T12:00:00Z"
    digest = base64.b64encode(
        hashlib.sha256((nonce + created + "demo-app-secret").encode()).digest()).decode()
    print("SMS X-WSSE:")
    print('UsernameToken Username="demo-app-key",PasswordDigest="%s",Nonce="%s",Created="%s"'
          % (digest, nonce, created))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if "--selftest" in sys.argv:
        selftest()
        return

    cfg_path = os.path.abspath(args[0]) if args else os.path.join(BASE_DIR, "config.ini")
    cfg = load_config(cfg_path)

    # mode=auto 时：AK/SK/桶齐全就走真实华为云，否则退回本地目录，便于无凭据联调
    if cfg["mode"] == "huawei":
        storage = ObsStorage(cfg["ak"], cfg["sk"], cfg["obs_bucket"], cfg["obs_endpoint"])
        sms_sender = lambda phone, code: send_sms_code(cfg, phone, code)
        storage_desc = "OBS bucket=%s endpoint=%s" % (cfg["obs_bucket"], cfg["obs_endpoint"])
    else:
        local_root = cfg["local_storage"]
        if not os.path.isabs(local_root):
            local_root = os.path.join(os.path.dirname(cfg_path), local_root)
        storage = LocalStorage(local_root)
        sms_sender = None
        storage_desc = "local dir=%s" % local_root

    state_dir = cfg["state_dir"]
    if not os.path.isabs(state_dir):
        state_dir = os.path.join(os.path.dirname(cfg_path), state_dir)

    Handler.backend = Backend(storage, state_dir, sms_sender)

    srv = ThreadingHTTPServer((cfg["host"], cfg["port"]), Handler)
    print("[backend] mode=%s storage=%s" % (cfg["mode"], storage_desc))
    print("[backend] listening on http://%s:%d" % (cfg["host"], cfg["port"]))
    sys.stdout.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[backend] bye")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
