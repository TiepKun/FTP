import socket
import struct
import json
import os
import sys

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

def prompt():
    return input("fs> ").strip()

def main():
    with socket.create_connection((HOST, PORT)) as conn:
        print("Connected.")
        while True:
            try:
                line = prompt()
            except EOFError:
                break
            if not line:
                continue
            parts = line.split()
            cmd = parts[0].lower()
            if cmd == "register":
                user = input("username: ")
                pw = input("password: ")
                send_message(conn, {"cmd": "REGISTER", "username": user, "password": pw})
                h, _ = recv_message(conn)
                print(h)
            elif cmd == "login":
                user = input("username: ")
                pw = input("password: ")
                send_message(conn, {"cmd": "LOGIN", "username": user, "password": pw})
                h, _ = recv_message(conn)
                print(h)
            elif cmd == "list":
                send_message(conn, {"cmd": "LIST"})
                h, _ = recv_message(conn)
                if h.get("ok"):
                    for it in h["items"]:
                        print(f'{it["path"]}\t{it["size"]} bytes\t{it["mtime"]}')
                else:
                    print(h)
            elif cmd == "upload":
                if len(parts) < 2:
                    print("Usage: upload <local_path> [remote_path]")
                    continue
                lp = parts[1]
                rp = parts[2] if len(parts) > 2 else os.path.basename(lp)
                data = open(lp, "rb").read()
                send_message(conn, {"cmd": "UPLOAD", "path": rp, "overwrite": True}, data)
                h, _ = recv_message(conn)
                print(h)
            elif cmd == "download":
                if len(parts) < 3:
                    print("Usage: download <remote_path> <local_path>")
                    continue
                rp, lp = parts[1], parts[2]
                send_message(conn, {"cmd": "DOWNLOAD", "path": rp})
                h, data = recv_message(conn)
                if h.get("ok"):
                    with open(lp, "wb") as f:
                        f.write(data or b"")
                    print(f"Saved to {lp} ({h.get('size',0)} bytes)")
                else:
                    print(h)
            elif cmd == "delete":
                if len(parts) < 2:
                    print("Usage: delete <remote_path>")
                    continue
                send_message(conn, {"cmd": "DELETE", "path": parts[1]})
                h, _ = recv_message(conn)
                print(h)
            elif cmd == "rename":
                if len(parts) < 3:
                    print("Usage: rename <old_path> <new_path>")
                    continue
                send_message(conn, {"cmd": "RENAME", "old_path": parts[1], "new_path": parts[2]})
                h, _ = recv_message(conn)
                print(h)
            elif cmd == "readtxt":
                if len(parts) < 2:
                    print("Usage: readtxt <remote_txt_path>")
                    continue
                send_message(conn, {"cmd": "READ_TEXT", "path": parts[1]})
                h, _ = recv_message(conn)
                if h.get("ok"):
                    print("--- FILE CONTENT BEGIN ---")
                    print(h.get("content",""))
                    print("--- FILE CONTENT END ---")
                else:
                    print(h)
            elif cmd == "writetxt":
                if len(parts) < 3:
                    print("Usage: writetxt <remote_txt_path> <text>")
                    continue
                path = parts[1]
                text = " ".join(parts[2:])
                send_message(conn, {"cmd": "WRITE_TEXT", "path": path, "content": text})
                h, _ = recv_message(conn)
                print(h)
            elif cmd == "stats":
                send_message(conn, {"cmd": "STATS"})
                h, _ = recv_message(conn)
                print(h)
            elif cmd in ("quit","exit"):
                send_message(conn, {"cmd": "QUIT"})
                break
            else:
                print("Commands: register, login, list, upload, download, delete, rename, readtxt, writetxt, stats, quit")
    print("Disconnected.")

if __name__ == "__main__":
    main()
