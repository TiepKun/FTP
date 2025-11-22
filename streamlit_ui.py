import os
import socket
import struct
import json
import streamlit as st

HOST = os.getenv("FS_HOST", "127.0.0.1")
PORT = int(os.getenv("FS_PORT", "5050"))

def send_message(conn, header: dict, data: bytes | None = None):
    header = dict(header)
    header["data_len"] = len(data) if data else 0
    payload = json.dumps(header, ensure_ascii=False).encode("utf-8")
    conn.sendall(struct.pack("!I", len(payload)))
    conn.sendall(payload)
    if data:
        conn.sendall(data)

def recv_exact(conn, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf.extend(chunk)
    return bytes(buf)

def recv_message(conn):
    raw_len = recv_exact(conn, 4)
    (length,) = struct.unpack("!I", raw_len)
    header_bytes = recv_exact(conn, length)
    header = json.loads(header_bytes.decode("utf-8"))
    data_len = int(header.get("data_len", 0) or 0)
    data = recv_exact(conn, data_len) if data_len > 0 else None
    return header, data

st.set_page_config(page_title="FileShare UI", page_icon="📁", layout="centered")
st.title("📁 FileShare (Checkpoint 2)")

if "conn" not in st.session_state:
    st.session_state.conn = None
    st.session_state.logged_in = False

def connect():
    st.session_state.conn = socket.create_connection((HOST, PORT))

def disconnect():
    if st.session_state.conn:
        try:
            send_message(st.session_state.conn, {"cmd": "QUIT"})
        except Exception:
            pass
        try:
            st.session_state.conn.close()
        except Exception:
            pass
    st.session_state.conn = None
    st.session_state.logged_in = False

with st.sidebar:
    st.subheader("Connection")
    if st.session_state.conn is None:
        if st.button("Connect to server"):
            try:
                connect()
                st.success("Connected")
            except Exception as e:
                st.error(f"Failed to connect: {e}")
    else:
        st.success("Connected")
        if st.button("Disconnect"):
            disconnect()

if st.session_state.conn:
    if not st.session_state.logged_in:
        st.subheader("Login / Register")
        tab1, tab2 = st.tabs(["Login","Register"])
        with tab1:
            user = st.text_input("Username", key="login_user")
            pw = st.text_input("Password", type="password", key="login_pw")
            if st.button("Login"):
                send_message(st.session_state.conn, {"cmd": "LOGIN", "username": user, "password": pw})
                h, _ = recv_message(st.session_state.conn)
                if h.get("ok"):
                    st.session_state.logged_in = True
                    st.session_state.username = user
                    st.success("Logged in")
                else:
                    st.error(h.get("error","login failed"))
        with tab2:
            user_r = st.text_input("New username", key="reg_user")
            pw_r = st.text_input("New password", type="password", key="reg_pw")
            if st.button("Register"):
                send_message(st.session_state.conn, {"cmd": "REGISTER", "username": user_r, "password": pw_r})
                h, _ = recv_message(st.session_state.conn)
                if h.get("ok"):
                    st.success("Registered. Now login.")
                else:
                    st.error(h.get("msg") or h.get("error"))
    else:
        st.write(f"Hello, **{st.session_state.username}**")
        # Upload
        st.subheader("Upload")
        up = st.file_uploader("Choose a file")
        rp = st.text_input("Remote path (optional)")
        if up and st.button("Upload now"):
            data = up.read()
            remote = rp or up.name
            send_message(st.session_state.conn, {"cmd": "UPLOAD", "path": remote, "overwrite": True}, data)
            h, _ = recv_message(st.session_state.conn)
            if h.get("ok"):
                st.success(f"Uploaded {remote} ({h.get('size',0)} bytes)")
            else:
                st.error(h.get("error","upload failed"))
        # List
        st.subheader("My files")
        if st.button("Refresh list"):
            send_message(st.session_state.conn, {"cmd":"LIST"})
            h, _ = recv_message(st.session_state.conn)
            if h.get("ok"):
                st.session_state.list_items = h["items"]
            else:
                st.error(h.get("error","list failed"))
        for it in st.session_state.get("list_items", []):
            col1, col2, col3, col4 = st.columns([4,2,2,2])
            col1.write(it["path"])
            col2.write(f'{it["size"]} bytes')
            col3.write(it["mtime"])
            dwn = col4.button("Download", key=f"dl_{it['path']}")
            if dwn:
                send_message(st.session_state.conn, {"cmd":"DOWNLOAD","path":it["path"]})
                h, data = recv_message(st.session_state.conn)
                if h.get("ok"):
                    st.download_button("Save file", data=data, file_name=os.path.basename(it["path"]), key=f"save_{it['path']}")
                else:
                    st.error(h.get("error","download failed"))
        # Edit .txt
        st.subheader("Edit .txt")
        edit_path = st.text_input("Remote .txt path")
        colr, cols = st.columns(2)
        if colr.button("Read text"):
            send_message(st.session_state.conn, {"cmd":"READ_TEXT","path":edit_path})
            h, _ = recv_message(st.session_state.conn)
            if h.get("ok"):
                st.session_state.edit_content = h["content"]
            else:
                st.error(h.get("error","read failed"))
        content = st.text_area("Content", value=st.session_state.get("edit_content",""), height=200)
        if cols.button("Save text"):
            send_message(st.session_state.conn, {"cmd":"WRITE_TEXT","path":edit_path,"content":content})
            h, _ = recv_message(st.session_state.conn)
            if h.get("ok"):
                st.success("Saved")
            else:
                st.error(h.get("error","write failed"))
        # Stats
        st.subheader("Stats")
        if st.button("Get stats"):
            send_message(st.session_state.conn, {"cmd":"STATS"})
            h, _ = recv_message(st.session_state.conn)
            if h.get("ok"):
                st.json(h)
            else:
                st.error(h.get("error","stats failed"))
else:
    st.info("Connect to the server to begin.")
