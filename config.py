import os

# TCP server
HOST = os.getenv("FS_HOST", "127.0.0.1")
PORT = int(os.getenv("FS_PORT", "5050"))

# MongoDB (local mặc định). Nếu dùng Atlas, thay bằng URI của bạn.
# Với local, tham khảo: brew services start mongodb-community@7.0
MONGO_URI = os.getenv(
    "FS_MONGO_URI",
    "mongodb://127.0.0.1:27017/?directConnection=true"
)

# Tên database
DB_NAME = os.getenv("FS_DB_NAME", "fileshare_db")

# Thư mục data cho từng user
STORAGE_DIR = os.getenv("FS_STORAGE_DIR", "./storage")

# Hạn mức dung lượng (mặc định 5GB)
DEFAULT_QUOTA_BYTES = int(os.getenv("FS_DEFAULT_QUOTA", str(5 * 1024 * 1024 * 1024)))

# Khoảng thời gian tính "active session" (giây)
SESSION_ACTIVE_SECS = int(os.getenv("FS_SESSION_ACTIVE_SECS", "600"))

