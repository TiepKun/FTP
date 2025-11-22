#!/usr/bin/env python3
import os
import socket
import struct
import json
from io import BytesIO
from datetime import datetime
import streamlit as st

HOST = os.getenv("FS_HOST", "127.0.0.1")
PORT = int(os.getenv("FS_PORT", "5051"))

# ===== Protocol helpers =====
def _pack_and_send(conn, header: dict, data: bytes | None = None):
    header = dict(header)
    header["data_len"] = len(data) if data else 0
    payload = json.dumps(header, ensure_ascii=False).encode("utf-8")
    conn.sendall(struct.pack("!I", len(payload)))
    conn.sendall(payload)
    if data:
        conn.sendall(data)

def _recv_exact(conn, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf.extend(chunk)
    return bytes(buf)

def _recv_any(conn):
    raw_len = _recv_exact(conn, 4)
    (length,) = struct.unpack("!I", raw_len)
    header_bytes = _recv_exact(conn, length)
    header = json.loads(header_bytes.decode("utf-8"))
    data_len = int(header.get("data_len", 0) or 0)
    data = _recv_exact(conn, data_len) if data_len > 0 else None
    return header, data

# Safe wrappers: tự kiểm tra kết nối + bắt lỗi
def require_conn() -> bool:
    if st.session_state.conn is None:
        st.error("Chưa kết nối server. Nhấn **Connect** trước khi thao tác.")
        return False
    return True

def safe_request(header: dict, data: bytes | None = None):
    """Gửi 1 request; nếu lỗi kết nối thì reset conn và báo lỗi."""
    if not require_conn():
        return {"ok": False, "error": "no-conn"}, None
    try:
        _pack_and_send(st.session_state.conn, header, data)
        return _recv_any(st.session_state.conn)
    except Exception as e:
        # socket đã chết / server đóng / network issue
        try:
            st.session_state.conn.close()
        except Exception:
            pass
        st.session_state.conn = None
        st.session_state.logged_in = False
        st.error(f"Kết nối tới server bị gián đoạn: {e}. Hãy **Connect** lại.")
        return {"ok": False, "error": str(e)}, None

# ===== UI helpers =====
def fmt_size(n: int) -> str:
    u = ["B","KB","MB","GB","TB"]; i = 0; f = float(n)
    while f >= 1024 and i < len(u)-1:
        f /= 1024; i += 1
    return f"{f:.1f} {u[i]}"

def ext_of(path: str) -> str:
    return (path.rsplit(".", 1)[-1].lower() if "." in path else "")

def connect():
    try:
        st.session_state.conn = socket.create_connection((HOST, PORT), timeout=5)
        # ping thử
        h, _ = safe_request({"cmd": "PING"})
        if not h or not h.get("ok"):
            raise RuntimeError("PING fail")
        st.success("Connected (PING ok)")
    except Exception as e:
        st.session_state.conn = None
        st.error(f"Connect failed: {e}")

def disconnect():
    if st.session_state.conn:
        try: safe_request({"cmd": "QUIT"})
        except Exception: pass
        try: st.session_state.conn.close()
        except Exception: pass
    st.session_state.conn = None
    st.session_state.logged_in = False
    st.session_state.cwd = ""

def refresh_list():
    h, _ = safe_request({"cmd":"LIST", "cwd": st.session_state.cwd})
    if h.get("ok"):
        st.session_state.folders = h.get("folders", [])
        st.session_state.files = h.get("files", [])
    else:
        st.error(h.get("error","list failed"))

# ===== Streamlit page =====
st.set_page_config(page_title="Drive X (Streamlit)", page_icon="📁", layout="wide")

# --- Custom CSS (modern look) ---
st.markdown("""
<style>
:root { --glass: rgba(255,255,255,0.06); --glass-2: rgba(255,255,255,0.10); }
.main { background: radial-gradient(1200px 600px at 0% 0%, rgba(120,119,198,.25), transparent),
                 radial-gradient(1200px 600px at 100% 0%, rgba(0,212,255,.20), transparent); }
.block-container { padding-top: 1.2rem; }
.glass { background: var(--glass); border: 1px solid rgba(255,255,255,0.12); border-radius: 16px; }
.card { background: var(--glass); border: 1px solid rgba(255,255,255,0.12); border-radius: 18px; padding: 12px; }
.badge { padding: 2px 8px; border-radius: 999px; border: 1px solid rgba(255,255,255,0.15); background: rgba(255,255,255,0.06); font-size: 12px; }
.item { padding: 10px; border-radius: 14px; border: 1px solid rgba(255,255,255,0.10); transition: background .15s ease; }
.item:hover { background: rgba(255,255,255,0.06); }
</style>
""", unsafe_allow_html=True)

if "conn" not in st.session_state:
    st.session_state.conn = None
    st.session_state.logged_in = False
    st.session_state.cwd = ""
    st.session_state.folders = []
    st.session_state.files = []
    st.session_state.preview = None
    st.session_state.search = ""
    st.session_state.sort_key = "name"
    st.session_state.sort_dir = "asc"

# --- Header ---
c1, c2, c3 = st.columns([1, 2, 1])
with c1:
    st.markdown("### 📁 Drive X")
with c3:
    if st.session_state.conn is None:
        if st.button("🔌 Connect", use_container_width=True):
            connect()
    else:
        st.success("Connected", icon="✅")
        if st.button("⏏ Disconnect", use_container_width=True):
            disconnect()

# --- Auth ---
with st.expander("Authentication", expanded=not st.session_state.logged_in):
    if st.session_state.conn is None:
        st.info("Connect to server trước.")
    else:
        t1, t2 = st.tabs(["Login", "Register"])
        with t1:
            user = st.text_input("Username", key="login_user")
            pw = st.text_input("Password", type="password", key="login_pw")
            if st.button("Login"):
                if not user or not pw:
                    st.warning("Nhập username/password.")
                else:
                    h, _ = safe_request({"cmd":"LOGIN", "username": user, "password": pw})
                    if h.get("ok"):
                        st.session_state.logged_in = True
                        st.session_state.username = user
                        st.success("Logged in")
                        refresh_list()
                    else:
                        st.error(h.get("error","login failed"))
        with t2:
            user2 = st.text_input("New username", key="reg_user")
            pw2 = st.text_input("New password", type="password", key="reg_pw")
            if st.button("Register"):
                if not user2 or not pw2:
                    st.warning("Nhập username/password.")
                else:
                    h, _ = safe_request({"cmd":"REGISTER", "username": user2, "password": pw2})
                    if h.get("ok"):
                        st.success("Registered. Please login.")
                    else:
                        st.error(h.get("msg") or h.get("error","register failed"))

if st.session_state.conn and st.session_state.logged_in:
    # --- Toolbar ---
    with st.container():
        t1, t2, t3, t4, t5 = st.columns([2, 2, 2, 3, 3])
        if t1.button("🗂 New folder"):
            with st.form("mk_folder", clear_on_submit=True):
                p = st.text_input("Folder name")
                submitted = st.form_submit_button("Create")
                if submitted and p.strip():
                    rel = f"{st.session_state.cwd}/{p.strip()}" if st.session_state.cwd else p.strip()
                    h, _ = safe_request({"cmd":"MKDIR", "path": rel})
                    if h.get("ok"):
                        st.success("Folder created")
                        refresh_list()
                    else:
                        st.error(h.get("error","mkdir failed"))
        with t2:
            up = st.file_uploader("Upload files", accept_multiple_files=True, label_visibility="collapsed")
            if up and st.button("Upload now"):
                for f in up:
                    rp = f"{st.session_state.cwd}/{f.name}" if st.session_state.cwd else f.name
                    h, _ = safe_request({"cmd":"UPLOAD", "path": rp, "overwrite": True}, f.getvalue())
                    if not h.get("ok"):
                        st.error(h.get("error","upload failed")); break
                else:
                    st.success("Uploaded")
                    refresh_list()
        with t3:
            if st.button("🔄 Refresh", use_container_width=True):
                refresh_list()
        with t4:
            st.session_state.search = st.text_input("Search", value=st.session_state.search, label_visibility="collapsed", placeholder="Search in folder")
        with t5:
            sk = st.selectbox("Sort by", ["name","mtime","size"], index=["name","mtime","size"].index(st.session_state.sort_key), label_visibility="collapsed")
            sd = st.selectbox("Dir", ["asc","desc"], index=0 if st.session_state.sort_dir=="asc" else 1, label_visibility="collapsed")
            st.session_state.sort_key, st.session_state.sort_dir = sk, sd

    # --- Breadcrumbs ---
    parts = [p for p in st.session_state.cwd.split("/") if p]
    breadcrumb_cols = st.columns(max(1, len(parts)*2+1))
    if breadcrumb_cols[0].button("My Drive", use_container_width=True):
        st.session_state.cwd = ""
        refresh_list()
    acc = ""
    idx = 1
    for p in parts:
        breadcrumb_cols[idx].markdown("➡️"); idx += 1
        acc = f"{acc}/{p}" if acc else p
        if breadcrumb_cols[idx].button(p, use_container_width=True):
            st.session_state.cwd = acc
            refresh_list()
        idx += 1

    # --- Compose items (filter + sort) ---
    items = [{"type":"folder", **f} for f in st.session_state.folders] + [{"type":"file", **f} for f in st.session_state.files]
    term = st.session_state.search.strip().lower()
    if term:
        items = [x for x in items if term in x["path"].lower()]
    def keyfn(x):
        if st.session_state.sort_key == "size":
            return (0 if x["type"]=="folder" else x.get("size",0))
        if st.session_state.sort_key == "mtime":
            try: return datetime.fromisoformat(x.get("mtime"))
            except: return datetime.min
        return x["path"].split("/")[-1].lower()
    items.sort(key=keyfn, reverse=(st.session_state.sort_dir=="desc"))

    # --- Layout: left (grid/list) + right (preview) ---
    left, right = st.columns([3, 2], gap="large")
    with left:
        view = st.radio("View", ["Grid","List"], horizontal=True, label_visibility="collapsed")
        st.markdown("<div class='glass' style='padding:10px;'>", unsafe_allow_html=True)
        if not items:
            st.info("Folder is empty.")
        elif view == "Grid":
            cols = st.columns(4)
            for i, it in enumerate(items):
                with cols[i % 4]:
                    with st.container():
                        st.markdown(f"<div class='item'>", unsafe_allow_html=True)
                        icon = "📁" if it["type"]=="folder" else "📄"
                        name = it["path"].split("/")[-1]
                        st.write(f"{icon} **{name}**")
                        st.caption(("Folder" if it["type"]=="folder" else fmt_size(it.get("size",0))) + f" • {it.get('mtime','')}")
                        c1, c2, c3 = st.columns(3)
                        if it["type"]=="folder":
                            if c1.button("Open", key=f"open_{it['path']}"):
                                st.session_state.cwd = it["path"]
                                refresh_list()
                        else:
                            if c1.button("Download", key=f"dl_{it['path']}"):
                                h, data = safe_request({"cmd":"DOWNLOAD", "path": it["path"]})
                                if h.get("ok") and data is not None:
                                    st.download_button("Save file", data=data, file_name=name, key=f"save_{it['path']}")
                                else:
                                    st.error(h.get("error","download failed"))
                        if c2.button("Rename", key=f"rn_{it['path']}"):
                            st.session_state.preview = ("rename", it)
                        if c3.button("Delete", key=f"rm_{it['path']}"):
                            h, _ = safe_request({"cmd":"DELETE", "path": it["path"]})
                            if h.get("ok"): refresh_list()
                            else: st.error(h.get("error","delete failed"))
                        st.markdown("</div>", unsafe_allow_html=True)
        else:
            st.markdown("| Name | Type | Size | Modified | Actions |\n|---|---:|---:|---:|---|\n", unsafe_allow_html=True)
            for it in items:
                name = it["path"].split("/")[-1]
                size = "-" if it["type"]=="folder" else fmt_size(it.get("size",0))
                row = f"| **{name}** | {it['type']} | {size} | {it.get('mtime','')} | "
                st.markdown(row)
                with st.container():
                    a,b,c,d = st.columns(4)
                    if it["type"]=="folder":
                        if a.button("Open", key=f"open2_{it['path']}"):
                            st.session_state.cwd = it["path"]; refresh_list()
                    else:
                        if a.button("Download", key=f"dl2_{it['path']}"):
                            h, data = safe_request({"cmd":"DOWNLOAD", "path": it["path"]})
                            if h.get("ok") and data is not None:
                                st.download_button("Save file", data=data, file_name=name, key=f"save2_{it['path']}")
                            else:
                                st.error(h.get("error","download failed"))
                    if b.button("Rename", key=f"rn2_{it['path']}"):
                        st.session_state.preview = ("rename", it)
                    if c.button("Delete", key=f"rm2_{it['path']}"):
                        h, _ = safe_request({"cmd":"DELETE", "path": it["path"]})
                        if h.get("ok"): refresh_list()
                        else: st.error(h.get("error","delete failed"))
                    if it["type"]=="file" and d.button("Preview", key=f"pv_{it['path']}"):
                        st.session_state.preview = ("preview", it)

        st.markdown("</div>", unsafe_allow_html=True)

    with right:
        st.markdown("#### Details / Preview")
        if st.session_state.preview:
            mode, it = st.session_state.preview
            name = it["path"].split("/")[-1]
            if mode == "rename":
                with st.form("rename_form", clear_on_submit=True):
                    new_name = st.text_input("New name", value=name)
                    submitted = st.form_submit_button("Save")
                    if submitted and new_name.strip():
                        parent = "/".join(it["path"].split("/")[:-1])
                        new_path = f"{parent}/{new_name}".strip("/")
                        h, _ = safe_request({"cmd":"RENAME", "old_path": it["path"], "new_path": new_path})
                        if h.get("ok"):
                            st.session_state.preview = None
                            refresh_list()
                        else:
                            st.error(h.get("error","rename failed"))
            elif mode == "preview":
                st.write(f"**{name}**")
                st.caption(f"{it['type']} • {fmt_size(it.get('size',0))} • {it.get('mtime','')}")
                ext = ext_of(name)
                if ext in ("png","jpg","jpeg","webp","gif"):
                    h, data = safe_request({"cmd":"DOWNLOAD", "path": it["path"]})
                    if h.get("ok") and data is not None:
                        st.image(BytesIO(data), caption=name, use_column_width=True)
                    else:
                        st.error(h.get("error","preview failed"))
                elif ext in ("txt", "md"):
                    h, _ = safe_request({"cmd":"READ_TEXT", "path": it["path"]})
                    if h.get("ok"):
                        if ext == "md":
                            st.markdown(h["content"])
                        else:
                            st.code(h["content"])
                    else:
                        st.error(h.get("error","read failed"))
                else:
                    st.info("No inline preview for this file type. Use Download.")
        else:
            st.caption("Select a file to preview or choose Rename to rename.")

    # Stats
    with st.expander("Server / Account Stats"):
        h, _ = safe_request({"cmd":"STATS"})
        if h.get("ok"):
            st.json(h)
        else:
            st.error(h.get("error","stats failed"))
