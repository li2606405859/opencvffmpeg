#!/usr/bin/env python3
"""Local mock cloud service for the watch album sync demo.

Endpoints:
  GET    /api/v1/ping
  GET    /api/v1/albums
  PUT    /api/v1/albums/<album>
  GET    /api/v1/albums/<album>
  DELETE /api/v1/albums/<album>
  PUT    /api/v1/albums/<album>/photos/<name>
  POST   /api/v1/sms/code        {"phone": "..."}
  POST   /api/v1/sms/verify      {"phone": "...", "code": "..."}
  GET    /api/v1/bind
  DELETE /api/v1/bind            (unbind, deletes all cloud photos)

Usage:
  python server.py [port] [storage_dir]
"""
import json
import os
import random
import shutil
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

API = "/api/v1"
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STORAGE = os.path.join(BASE_DIR, "storage")

CODES = {}  # phone -> sms code


def album_dir(album_id):
    return os.path.join(STORAGE, album_id)


def bind_file():
    return os.path.join(STORAGE, "_bind.json")


def read_bind():
    bf = bind_file()
    if not os.path.exists(bf):
        return None
    try:
        with open(bf, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _json(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        return self.rfile.read(n) if n > 0 else b""

    def _parts(self):
        path = unquote(urlparse(self.path).path)
        if not path.startswith(API):
            return None
        return [p for p in path[len(API):].split("/") if p]

    def do_GET(self):
        parts = self._parts()
        if parts is None:
            return self._json(404, {"error": "not found"})
        if parts == ["ping"]:
            return self._json(200, {"pong": True})
        if parts == ["bind"]:
            b = read_bind()
            if b:
                return self._json(200, {"bound": True, "phone": b.get("phone", "")})
            return self._json(200, {"bound": False})
        if parts == ["albums"]:
            os.makedirs(STORAGE, exist_ok=True)
            ids = sorted(d for d in os.listdir(STORAGE)
                         if os.path.isdir(album_dir(d)))
            return self._json(200, {"albums": ids})
        if len(parts) == 2 and parts[0] == "albums":
            d = album_dir(parts[1])
            if not os.path.isdir(d):
                return self._json(404, {"error": "album not found"})
            photos = sorted(f for f in os.listdir(d)
                            if os.path.isfile(os.path.join(d, f)))
            return self._json(200, {
                "album_id": parts[1],
                "photos": photos,
                "count": len(photos),
            })
        return self._json(404, {"error": "not found"})

    def do_PUT(self):
        parts = self._parts()
        if parts is None:
            self._read_body()
            return self._json(404, {"error": "not found"})
        body = self._read_body()
        if len(parts) == 2 and parts[0] == "albums":
            os.makedirs(album_dir(parts[1]), exist_ok=True)
            return self._json(200, {"album_id": parts[1]})
        if len(parts) == 4 and parts[0] == "albums" and parts[2] == "photos":
            d = album_dir(parts[1])
            if not os.path.isdir(d):
                return self._json(404, {"error": "album not found"})
            with open(os.path.join(d, parts[3]), "wb") as f:
                f.write(body)
            return self._json(201, {"name": parts[3], "size": len(body)})
        return self._json(404, {"error": "not found"})

    def do_DELETE(self):
        parts = self._parts()
        if parts is None:
            return self._json(404, {"error": "not found"})
        if parts == ["bind"]:
            deleted = 0
            if os.path.isdir(STORAGE):
                for name in list(os.listdir(STORAGE)):
                    p = os.path.join(STORAGE, name)
                    if os.path.isdir(p):
                        shutil.rmtree(p)
                        deleted += 1
            if os.path.exists(bind_file()):
                os.remove(bind_file())
            return self._json(200, {"unbound": True, "deleted_albums": deleted})
        if len(parts) != 2 or parts[0] != "albums":
            return self._json(404, {"error": "not found"})
        d = album_dir(parts[1])
        if not os.path.isdir(d):
            return self._json(404, {"error": "album not found"})
        shutil.rmtree(d)
        return self._json(200, {"deleted": True, "album_id": parts[1]})

    def do_POST(self):
        parts = self._parts()
        body = self._read_body()
        if parts is None:
            return self._json(404, {"error": "not found"})
        try:
            data = json.loads(body.decode("utf-8")) if body else {}
        except Exception:
            return self._json(400, {"error": "bad json"})

        if parts == ["sms", "code"]:
            phone = str(data.get("phone", "")).strip()
            if not phone:
                return self._json(400, {"error": "phone required"})
            code = "%06d" % random.randint(0, 999999)
            CODES[phone] = code
            print("[mock-cloud] SMS to %s: code=%s" % (phone, code))
            sys.stdout.flush()
            return self._json(200, {"sent": True, "expires_in": 300, "code": code})

        if parts == ["sms", "verify"]:
            phone = str(data.get("phone", "")).strip()
            code = str(data.get("code", "")).strip()
            if CODES.get(phone) is None or code != CODES.get(phone):
                return self._json(400, {"error": "invalid code"})
            account = "huawei-cloud-" + phone[-4:]
            with open(bind_file(), "w", encoding="utf-8") as f:
                json.dump({"phone": phone, "account": account}, f)
            return self._json(200, {"bound": True, "phone": phone, "account": account})

        return self._json(404, {"error": "not found"})

    def log_message(self, fmt, *args):
        sys.stdout.write("[mock-cloud] %s\n" % (fmt % args))
        sys.stdout.flush()


def main():
    global STORAGE
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    if len(sys.argv) > 2:
        STORAGE = os.path.abspath(sys.argv[2])
    os.makedirs(STORAGE, exist_ok=True)

    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("[mock-cloud] listening on http://127.0.0.1:%d" % port)
    print("[mock-cloud] storage dir: %s" % STORAGE)
    sys.stdout.flush()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[mock-cloud] bye")
    finally:
        srv.server_close()


if __name__ == "__main__":
    main()
