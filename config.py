import os

# Server
HOST = os.getenv("FS_HOST", "127.0.0.1")
PORT = int(os.getenv("FS_PORT", "5050"))

# Storage
STORAGE_DIR = os.getenv("FS_STORAGE_DIR", os.path.join(os.path.dirname(__file__), "storage"))
os.makedirs(STORAGE_DIR, exist_ok=True)

# MongoDB
MONGO_URI = os.getenv("FS_MONGO_URI", "mongodb://localhost:27017")
DB_NAME = os.getenv("FS_DB_NAME", "fileshare_db")

# Quota (bytes) per user default (e.g., 200 MB)
DEFAULT_QUOTA_BYTES = int(os.getenv("FS_DEFAULT_QUOTA", str(200 * 1024 * 1024)))

# Session activity window (seconds)
SESSION_ACTIVE_SECS = int(os.getenv("FS_SESSION_ACTIVE_SECS", "300"))
