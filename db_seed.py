from pymongo import MongoClient
import bcrypt
import config

mongo = MongoClient(config.MONGO_URI)
db = mongo[config.DB_NAME]

def create_user(username: str, password: str, quota_bytes: int | None = None):
    if db.users.find_one({"username": username}):
        print("User exists")
        return
    pw_hash = bcrypt.hashpw(password.encode("utf-8"), bcrypt.gensalt()).decode("utf-8")
    doc = {"username": username, "password": pw_hash}
    if quota_bytes is not None:
        doc["quota"] = int(quota_bytes)
    db.users.insert_one(doc)
    print("Created user:", username)

if __name__ == "__main__":
    create_user("admin", "admin123")
