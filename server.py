import os
import socket
import threading
import struct
import json
import time
import uuid
import shutil
from datetime import datetime, timezone
from pathlib import Path

from pymongo import MongoClient, ASCENDING
import bcrypt

import config

# ---------------- Protocol helpers (length-prefixed JSON + optional raw payload) ----------------

def send_message(conn, header: dict, data: bytes | None = None):
    """Send a message: 4-byte big-endian length + JSON header (utf-8). If header has 'data_len' > 0,
    raw data bytes follow immediately.
    """
    header = dict(header)  # copy
    if data is not None:
        header["data_len"] = len(data)
    else:
        header["data_len"] = 0
    payload = json.dumps(header, ensure_ascii=False).encode("utf-8")
    conn.sendall(struct.pack("!I", len(payload)))
    conn.sendall(payload)
    if header["data_len"]:
        conn.sendall(data)

def recv_exact(conn, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed during recv_exact")
        buf.extend(chunk)
    return bytes(buf)

def recv_message(conn):
    """Return (header: dict, data: bytes | None)"""
    raw_len = recv_exact(conn, 4)
    (length,) = struct.unpack("!I", raw_len)
    header_bytes = recv_exact(conn, length)
    header = json.loads(header_bytes.decode("utf-8"))
    data_len = int(header.get("data_len", 0) or 0)
    data = recv_exact(conn, data_len) if data_len > 0 else None
    return header, data

# ---------------- Utilities ----------------

def utc_now() -> datetime:
    return datetime.now(timezone.utc)

def ensure_user_dirs(username: str) -> Path:
    user_dir = Path(config.STORAGE_DIR) / username
    user_dir.mkdir(parents=True, exist_ok=True)
    return user_dir

def is_under(base: Path, child: Path) -> bool:
    """Ensure child path is inside base path after resolve()."""
    base = base.resolve()
    child = child.resolve()
    return base == child or base in child.parents

def atomic_write_bytes(path: Path, data: bytes):
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)

def human_size(n: int) -> str:
    for unit in ["B","KB","MB","GB","TB"]:
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} PB"

# ---------------- Database ----------------

mongo = MongoClient(config.MONGO_URI)
db = mongo[config.DB_NAME]

def init_db():
    db.users.create_index([("username", ASCENDING)], unique=True)
    db.files.create_index([("owner", ASCENDING), ("path", ASCENDING)], unique=True)
    db.sessions.create_index([("session_id", ASCENDING)], unique=True)
    db.sessions.create_index([("last_seen", ASCENDING)])
    db.logs.create_index([("ts", ASCENDING)])
    db.logs.create_index([("user", ASCENDING)])

init_db()

# ---------------- Quota helpers ----------------

def user_used_bytes(username: str) -> int:
    agg = db.files.aggregate([
        {"$match": {"owner": username}},
        {"$group": {"_id": None, "total": {"$sum": "$size"}}}
    ])
    doc = next(agg, None)
    return int(doc["total"]) if doc and "total" in doc else 0

def get_quota(username: str) -> int:
    user = db.users.find_one({"username": username}, {"quota": 1})
    return int(user.get("quota", config.DEFAULT_QUOTA_BYTES))

def can_store(username: str, add_bytes: int) -> tuple[bool, int, int]:
    used = user_used_bytes(username)
    quota = get_quota(username)
    return (used + add_bytes) <= quota, used, quota

# ---------------- Authentication ----------------

def register(username: str, password: str) -> tuple[bool, str]:
    if db.users.find_one({"username": username}):
        return False, "User already exists"
    pw_hash = bcrypt.hashpw(password.encode("utf-8"), bcrypt.gensalt()).decode("utf-8")
    db.users.insert_one({
        "username": username,
        "password": pw_hash,
        "quota": config.DEFAULT_QUOTA_BYTES,
        "created_at": utc_now()
    })
    return True, "Registered"

def verify(username: str, password: str) -> bool:
    user = db.users.find_one({"username": username})
    if not user:
        return False
    pw_hash = user.get("password", "").encode("utf-8")
    return bcrypt.checkpw(password.encode("utf-8"), pw_hash)

# ---------------- Logging ----------------

def log_action(user: str | None, action: str, detail: dict):
    db.logs.insert_one({
        "ts": utc_now(),
        "user": user,
        "action": action,
        "detail": detail
    })

# ---------------- Server stats ----------------

server_start = time.time()
total_in = 0
total_out = 0
total_lock = threading.Lock()

def add_bytes(in_bytes=0, out_bytes=0):
    global total_in, total_out
    with total_lock:
        total_in += in_bytes
        total_out += out_bytes

def update_session(session_id: str, username: str | None, inc_in=0, inc_out=0):
    db.sessions.update_one(
        {"session_id": session_id},
        {"$set": {"username": username, "last_seen": utc_now()},
         "$inc": {"bytes_in": inc_in, "bytes_out": inc_out}},
        upsert=True
    )

def active_user_count() -> int:
    cutoff = utc_now().timestamp() - config.SESSION_ACTIVE_SECS
    return db.sessions.count_documents({"last_seen": {"$gte": datetime.fromtimestamp(cutoff, tz=timezone.utc)}})

# ---------------- Command handlers ----------------

def handle_list(username: str, req: dict):
    base = ensure_user_dirs(username)
    items = []
    for p in sorted(base.rglob("*")):
        if p.is_file():
            rel = str(p.relative_to(base))
            meta = db.files.find_one({"owner": username, "path": rel}) or {}
            items.append({
                "path": rel,
                "size": meta.get("size", p.stat().st_size),
                "mtime": meta.get("mtime", datetime.fromtimestamp(p.stat().st_mtime, tz=timezone.utc).isoformat())
            })
    return {"ok": True, "items": items}

def handle_upload(username: str, req: dict, data: bytes | None):
    rel_path = req.get("path")
    if not isinstance(rel_path, str):
        return {"ok": False, "error": "Invalid path"}
    if data is None:
        return {"ok": False, "error": "Missing file data"}
    overwrite = bool(req.get("overwrite", False))
    user_dir = ensure_user_dirs(username)
    dest = (user_dir / rel_path).resolve()
    if not is_under(user_dir, dest):
        return {"ok": False, "error": "Invalid path traversal"}
    dest.parent.mkdir(parents=True, exist_ok=True)
    # quota check (consider overwrite delta)
    prev = dest.stat().st_size if dest.exists() else 0
    can, used, quota = can_store(username, len(data) - prev)
    if not can and not overwrite:
        return {"ok": False, "error": f"Quota exceeded: used {human_size(used)} / {human_size(quota)}"}
    atomic_write_bytes(dest, data)
    st = dest.stat()
    db.files.update_one(
        {"owner": username, "path": str(dest.relative_to(user_dir))},
        {"$set": {"size": st.st_size, "mtime": datetime.fromtimestamp(st.st_mtime, tz=timezone.utc)},
         "$setOnInsert": {"owner": username}},
        upsert=True
    )
    log_action(username, "UPLOAD", {"path": rel_path, "size": len(data)})
    return {"ok": True, "size": st.st_size}

def handle_download(username: str, req: dict):
    rel_path = req.get("path")
    user_dir = ensure_user_dirs(username)
    src = (user_dir / rel_path).resolve()
    if not is_under(user_dir, src):
        return {"ok": False, "error": "Invalid path traversal"}, None
    if not src.exists() or not src.is_file():
        return {"ok": False, "error": "File not found"}, None
    data = src.read_bytes()
    return {"ok": True, "path": rel_path, "size": len(data)}, data

def handle_delete(username: str, req: dict):
    rel_path = req.get("path")
    user_dir = ensure_user_dirs(username)
    target = (user_dir / rel_path).resolve()
    if not is_under(user_dir, target):
        return {"ok": False, "error": "Invalid path traversal"}
    if not target.exists():
        return {"ok": False, "error": "Not found"}
    if target.is_dir():
        shutil.rmtree(target)
        # remove all metadata under that dir
        prefix = str(target.relative_to(user_dir)).rstrip("/") + "/"
        db.files.delete_many({"owner": username, "path": {"$regex": f"^{prefix}"}})
    else:
        target.unlink()
        db.files.delete_one({"owner": username, "path": str(target.relative_to(user_dir))})
    log_action(username, "DELETE", {"path": rel_path})
    return {"ok": True}

def handle_rename(username: str, req: dict):
    old = req.get("old_path")
    new = req.get("new_path")
    user_dir = ensure_user_dirs(username)
    src = (user_dir / old).resolve()
    dst = (user_dir / new).resolve()
    if not is_under(user_dir, src) or not is_under(user_dir, dst):
        return {"ok": False, "error": "Invalid path traversal"}
    if not src.exists():
        return {"ok": False, "error": "Source not found"}
    dst.parent.mkdir(parents=True, exist_ok=True)
    os.replace(src, dst)
    # update metadata (single file case)
    old_rel = str(src.relative_to(user_dir))
    new_rel = str(dst.relative_to(user_dir))
    meta = db.files.find_one({"owner": username, "path": old_rel})
    if meta:
        db.files.update_one({"_id": meta["_id"]}, {"$set": {"path": new_rel}})
    log_action(username, "RENAME", {"old": old, "new": new})
    return {"ok": True}

def handle_read_text(username: str, req: dict):
    rel_path = req.get("path")
    if not str(rel_path).endswith(".txt"):
        return {"ok": False, "error": "Only .txt allowed"}
    user_dir = ensure_user_dirs(username)
    f = (user_dir / rel_path).resolve()
    if not is_under(user_dir, f):
        return {"ok": False, "error": "Invalid path traversal"}
    if not f.exists():
        return {"ok": False, "error": "Not found"}
    try:
        content = f.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return {"ok": False, "error": "File not utf-8 text"}
    return {"ok": True, "content": content}

def handle_write_text(username: str, req: dict):
    rel_path = req.get("path")
    content = req.get("content", "")
    if not str(rel_path).endswith(".txt"):
        return {"ok": False, "error": "Only .txt allowed"}
    if not isinstance(content, str):
        return {"ok": False, "error": "Invalid content"}
    data = content.encode("utf-8")
    return handle_upload(username, {"path": rel_path, "overwrite": True}, data)

def handle_stats(username: str, req: dict):
    used = user_used_bytes(username)
    quota = get_quota(username)
    active = active_user_count()
    uptime = int(time.time() - server_start)
    with total_lock:
        _in, _out = total_in, total_out
    return {"ok": True, "active_users": active, "server_uptime_secs": uptime,
            "server_bytes_in": _in, "server_bytes_out": _out,
            "user_used_bytes": used, "user_quota_bytes": quota}

# ---------------- Client thread ----------------

def client_thread(conn: socket.socket, addr):
    session_id = str(uuid.uuid4())
    username = None
    try:
        update_session(session_id, None, 0, 0)
        while True:
            hdr, data = recv_message(conn)
            add_bytes(in_bytes=4 + len(json.dumps(hdr).encode("utf-8")) + (len(data) if data else 0))
            cmd = hdr.get("cmd")
            if cmd == "PING":
                send_message(conn, {"ok": True, "pong": True})
                continue
            if cmd == "REGISTER":
                ok, msg = register(hdr.get("username",""), hdr.get("password",""))
                send_message(conn, {"ok": ok, "msg": msg})
                continue
            if cmd == "LOGIN":
                if verify(hdr.get("username",""), hdr.get("password","")):
                    username = hdr["username"]
                    update_session(session_id, username)
                    send_message(conn, {"ok": True, "session_id": session_id})
                else:
                    send_message(conn, {"ok": False, "error": "Bad credentials"})
                continue
            if cmd == "QUIT":
                send_message(conn, {"ok": True, "bye": True})
                break

            # Require login for the rest
            if not username:
                send_message(conn, {"ok": False, "error": "Not authenticated"})
                continue

            if cmd == "LIST":
                resp = handle_list(username, hdr)
                send_message(conn, resp)
            elif cmd == "UPLOAD":
                resp = handle_upload(username, hdr, data)
                send_message(conn, resp)
            elif cmd == "DOWNLOAD":
                meta, file_bytes = handle_download(username, hdr)
                send_message(conn, meta, file_bytes if meta.get("ok") else None)
                if meta.get("ok"):
                    add_bytes(out_bytes=len(file_bytes))
            elif cmd == "DELETE":
                resp = handle_delete(username, hdr)
                send_message(conn, resp)
            elif cmd == "RENAME":
                resp = handle_rename(username, hdr)
                send_message(conn, resp)
            elif cmd == "READ_TEXT":
                resp = handle_read_text(username, hdr)
                send_message(conn, resp)
            elif cmd == "WRITE_TEXT":
                resp = handle_write_text(username, hdr)
                send_message(conn, resp)
            elif cmd == "STATS":
                resp = handle_stats(username, hdr)
                send_message(conn, resp)
            else:
                send_message(conn, {"ok": False, "error": f"Unknown cmd {cmd}"})
            update_session(session_id, username)
    except (ConnectionError, OSError):
        pass
    finally:
        conn.close()
        db.sessions.update_one({"session_id": session_id}, {"$set": {"last_seen": utc_now()}})

def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((config.HOST, config.PORT))
        s.listen(128)
        print(f"Server listening on {config.HOST}:{config.PORT}")
        while True:
            conn, addr = s.accept()
            t = threading.Thread(target=client_thread, args=(conn, addr), daemon=True)
            t.start()

if __name__ == "__main__":
    serve()
