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

# ---- config fallback (dùng env nếu không có config.py) ----
try:
    import config  # type: ignore
except Exception:
    class config:
        HOST = os.getenv("FS_HOST", "127.0.0.1")
        PORT = int(os.getenv("FS_PORT", "5051"))
        MONGO_URI = os.getenv("FS_MONGO_URI", "mongodb://127.0.0.1:27017/?directConnection=true")
        DB_NAME = os.getenv("FS_DB_NAME", "fileshare_db")
        STORAGE_DIR = os.getenv("FS_STORAGE_DIR", "./storage")
        DEFAULT_QUOTA_BYTES = int(os.getenv("FS_DEFAULT_QUOTA", str(5 * 1024 * 1024 * 1024)))  # 5GB
        SESSION_ACTIVE_SECS = int(os.getenv("FS_SESSION_ACTIVE_SECS", "600"))

# ====================== Protocol helpers ======================

def send_message(conn, header: dict, data: bytes | None = None):
    """Send: 4-byte big-endian length + JSON header (utf-8) + optional raw bytes (file)."""
    header = dict(header)
    header["data_len"] = len(data) if data else 0
    payload = json.dumps(header, ensure_ascii=False, default=str).encode("utf-8")
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
    """Return (header: dict, data: bytes|None)."""
    raw_len = recv_exact(conn, 4)
    (length,) = struct.unpack("!I", raw_len)
    header_bytes = recv_exact(conn, length)
    header = json.loads(header_bytes.decode("utf-8"))
    data_len = int(header.get("data_len", 0) or 0)
    data = recv_exact(conn, data_len) if data_len > 0 else None
    return header, data

# ====================== Utilities ======================

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

# ====================== Database ======================

mongo = MongoClient(config.MONGO_URI, serverSelectionTimeoutMS=5000)
try:
    mongo.admin.command("ping")
except Exception as e:
    raise SystemExit(f"[MongoDB] Cannot connect: {e}\nCheck FS_MONGO_URI or start MongoDB first.")

db = mongo[config.DB_NAME]

def init_db():
    db.users.create_index([("username", ASCENDING)], unique=True)
    db.files.create_index([("owner", ASCENDING), ("path", ASCENDING)], unique=True)
    db.sessions.create_index([("session_id", ASCENDING)], unique=True)
    db.sessions.create_index([("last_seen", ASCENDING)])
    db.logs.create_index([("ts", ASCENDING)])
    db.logs.create_index([("user", ASCENDING)])

init_db()

# ====================== Quota helpers ======================

def user_used_bytes(username: str) -> int:
    agg = db.files.aggregate([
        {"$match": {"owner": username}},
        {"$group": {"_id": None, "total": {"$sum": "$size"}}}
    ])
    doc = next(agg, None)
    return int(doc["total"]) if doc and "total" in doc else 0

def get_quota(username: str) -> int:
    user = db.users.find_one({"username": username}, {"quota": 1})
    return int(user.get("quota", config.DEFAULT_QUOTA_BYTES)) if user else config.DEFAULT_QUOTA_BYTES

def can_store(username: str, add_bytes: int) -> tuple[bool, int, int]:
    used = user_used_bytes(username)
    quota = get_quota(username)
    return (used + add_bytes) <= quota, used, quota

# ====================== Auth & logs ======================

def register(username: str, password: str) -> tuple[bool, str]:
    if not username or not password:
        return False, "Missing username/password"
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
    if not user: return False
    pw_hash = user.get("password", "").encode("utf-8")
    return bcrypt.checkpw(password.encode("utf-8"), pw_hash)

def log_action(user: str | None, action: str, detail: dict):
    db.logs.insert_one({
        "ts": utc_now(),
        "user": user,
        "action": action,
        "detail": detail
    })

# ====================== Server stats ======================

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

# ====================== Folder & file helpers ======================

def list_dir(username: str, cwd: str = ""):
    """Return items directly under cwd: separate folders[] and files[]."""
    base = ensure_user_dirs(username)
    target = (base / (cwd or "")).resolve()
    if not is_under(base, target):
        return {"ok": False, "error": "Invalid path traversal"}
    target.mkdir(parents=True, exist_ok=True)

    folders, files = [], []
    try:
        with os.scandir(target) as it:
            for entry in it:
                rel = str(Path(entry.path).relative_to(base))
                st = entry.stat()
                if entry.is_dir():
                    folders.append({
                        "type": "folder",
                        "path": rel,
                        "size": 0,
                        "mtime": datetime.fromtimestamp(st.st_mtime, tz=timezone.utc).isoformat()
                    })
                elif entry.is_file():
                    meta = db.files.find_one({"owner": username, "path": rel}) or {}
                    files.append({
                        "type": "file",
                        "path": rel,
                        "size": meta.get("size", st.st_size),
                        "mtime": meta.get("mtime", datetime.fromtimestamp(st.st_mtime, tz=timezone.utc).isoformat())
                    })
    except FileNotFoundError:
        return {"ok": False, "error": "Directory not found"}

    return {"ok": True, "cwd": cwd or "", "folders": folders, "files": files}

def mkdir(username: str, path: str):
    base = ensure_user_dirs(username)
    dest = (base / path).resolve()
    if not is_under(base, dest):
        return {"ok": False, "error": "Invalid path traversal"}
    dest.mkdir(parents=True, exist_ok=True)
    log_action(username, "MKDIR", {"path": path})
    return {"ok": True}

def upload_file(username: str, rel_path: str, data: bytes | None, overwrite: bool = False):
    if not isinstance(rel_path, str) or data is None:
        return {"ok": False, "error": "Invalid parameters"}
    user_dir = ensure_user_dirs(username)
    dest = (user_dir / rel_path).resolve()
    if not is_under(user_dir, dest):
        return {"ok": False, "error": "Invalid path traversal"}
    dest.parent.mkdir(parents=True, exist_ok=True)
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

def download_file(username: str, rel_path: str):
    user_dir = ensure_user_dirs(username)
    src = (user_dir / rel_path).resolve()
    if not is_under(user_dir, src):
        return {"ok": False, "error": "Invalid path traversal"}, None
    if not src.exists() or not src.is_file():
        return {"ok": False, "error": "File not found"}, None
    data = src.read_bytes()
    return {"ok": True, "path": rel_path, "size": len(data)}, data

def delete_path(username: str, rel_path: str):
    user_dir = ensure_user_dirs(username)
    target = (user_dir / rel_path).resolve()
    if not is_under(user_dir, target):
        return {"ok": False, "error": "Invalid path traversal"}
    if not target.exists():
        return {"ok": False, "error": "Not found"}
    if target.is_dir():
        shutil.rmtree(target)
        prefix = str(target.relative_to(user_dir)).rstrip("/") + "/"
        db.files.delete_many({"owner": username, "path": {"$regex": f"^{prefix}"}})
    else:
        target.unlink()
        db.files.delete_one({"owner": username, "path": str(target.relative_to(user_dir))})
    log_action(username, "DELETE", {"path": rel_path})
    return {"ok": True}

def rename_path(username: str, old: str, new: str):
    user_dir = ensure_user_dirs(username)
    src = (user_dir / old).resolve()
    dst = (user_dir / new).resolve()
    if not is_under(user_dir, src) or not is_under(user_dir, dst):
        return {"ok": False, "error": "Invalid path traversal"}
    if not src.exists():
        return {"ok": False, "error": "Source not found"}
    dst.parent.mkdir(parents=True, exist_ok=True)
    os.replace(src, dst)
    old_rel = str(src.relative_to(user_dir))
    new_rel = str(dst.relative_to(user_dir))
    if dst.is_file():
        meta = db.files.find_one({"owner": username, "path": old_rel})
        if meta:
            db.files.update_one({"_id": meta["_id"]}, {"$set": {"path": new_rel}})
    else:
        prefix = old_rel.rstrip("/") + "/"
        for doc in db.files.find({"owner": username, "path": {"$regex": f"^{prefix}"}}):
            suffix = doc["path"][len(prefix):]
            db.files.update_one({"_id": doc["_id"]}, {"$set": {"path": new_rel.rstrip('/') + '/' + suffix}})
    log_action(username, "RENAME", {"old": old, "new": new})
    return {"ok": True}

def read_text(username: str, rel_path: str):
    if not (rel_path.endswith(".txt") or rel_path.endswith(".md")):
        return {"ok": False, "error": "Only .txt/.md allowed"}
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

def write_text(username: str, rel_path: str, content: str):
    if not (rel_path.endswith(".txt") or rel_path.endswith(".md")):
        return {"ok": False, "error": "Only .txt/.md allowed"}
    if not isinstance(content, str):
        return {"ok": False, "error": "Invalid content"}
    data = content.encode("utf-8")
    return upload_file(username, rel_path, data, overwrite=True)

def stats(username: str):
    used = user_used_bytes(username)
    quota = get_quota(username)
    active = active_user_count()
    uptime = int(time.time() - server_start)
    with total_lock:
        _in, _out = total_in, total_out
    return {"ok": True, "active_users": active, "server_uptime_secs": uptime,
            "server_bytes_in": _in, "server_bytes_out": _out,
            "user_used_bytes": used, "user_quota_bytes": quota}

# ====================== Client thread & dispatch ======================

def client_thread(conn: socket.socket, addr):
    session_id = str(uuid.uuid4())
    username = None
    try:
        update_session(session_id, None, 0, 0)
        while True:
            hdr, data = recv_message(conn)
            add_bytes(in_bytes=4 + len(json.dumps(hdr, default=str).encode("utf-8")) + (len(data) if data else 0))
            cmd = hdr.get("cmd")

            if cmd == "PING":
                send_message(conn, {"ok": True, "pong": True}); continue
            if cmd == "REGISTER":
                ok, msg = register(hdr.get("username",""), hdr.get("password",""))
                send_message(conn, {"ok": ok, "msg": msg}); continue
            if cmd == "LOGIN":
                if verify(hdr.get("username",""), hdr.get("password","")):
                    username = hdr["username"]
                    update_session(session_id, username)
                    send_message(conn, {"ok": True, "session_id": session_id})
                else:
                    send_message(conn, {"ok": False, "error": "Bad credentials"})
                continue
            if cmd == "QUIT":
                send_message(conn, {"ok": True, "bye": True}); break

            if not username:
                send_message(conn, {"ok": False, "error": "Not authenticated"})
                continue

            # Folder/File API
            if cmd == "LIST":
                cwd = hdr.get("cwd","")
                resp = list_dir(username, cwd)
                send_message(conn, resp)
            elif cmd == "MKDIR":
                resp = mkdir(username, hdr.get("path",""))
                send_message(conn, resp)
            elif cmd == "UPLOAD":
                resp = upload_file(username, hdr.get("path",""), data, overwrite=bool(hdr.get("overwrite", False)))
                send_message(conn, resp)
            elif cmd == "DOWNLOAD":
                meta, file_bytes = download_file(username, hdr.get("path",""))
                send_message(conn, meta, file_bytes if meta.get("ok") else None)
                if meta.get("ok"): add_bytes(out_bytes=len(file_bytes))
            elif cmd == "DELETE":
                resp = delete_path(username, hdr.get("path",""))
                send_message(conn, resp)
            elif cmd == "RENAME":
                resp = rename_path(username, hdr.get("old_path",""), hdr.get("new_path",""))
                send_message(conn, resp)
            elif cmd == "READ_TEXT":
                resp = read_text(username, hdr.get("path",""))
                send_message(conn, resp)
            elif cmd == "WRITE_TEXT":
                resp = write_text(username, hdr.get("path",""), hdr.get("content",""))
                send_message(conn, resp)
            elif cmd == "STATS":
                resp = stats(username)
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
    Path(config.STORAGE_DIR).mkdir(parents=True, exist_ok=True)
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
