#define _GNU_SOURCE
#include "db.h"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <sys/random.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static sqlite3 *db;

static int random_hex(char *out, size_t bytes)
{
    unsigned char raw[32];
    if (bytes > sizeof(raw)) return -1;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = getrandom(raw + done, bytes - done, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)n;
    }
    for (size_t i = 0; i < bytes; ++i)
        sprintf(out + i * 2, "%02x", raw[i]);
    out[bytes * 2] = '\0';
    return 0;
}

static void password_digest(const char *salt, const char *password, char out[65])
{
    unsigned char digest[32];
    PKCS5_PBKDF2_HMAC(password, (int)strlen(password), (const unsigned char *)salt, (int)strlen(salt), 120000, EVP_sha256(), sizeof(digest), digest);
    for (int i = 0; i < (int)sizeof(digest); ++i)
        sprintf(out + i * 2, "%02x", digest[i]);
    out[64] = '\0';
}

int db_init(const char *path)
{
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); db = NULL; return -1; }
    sqlite3_busy_timeout(db, 3000);
    const char *schema =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, salt TEXT NOT NULL, password_hash TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions (token TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, expires_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sensors (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, location TEXT NOT NULL, temperature REAL NOT NULL, humidity REAL NOT NULL, updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "INSERT OR IGNORE INTO sensors(name,location,temperature,humidity) VALUES"
        "('温度传感器 A1','一号实验室',23.5,46.2),('温湿度传感器 B2','二号实验室',25.1,51.8),('温度传感器 C3','设备间',21.8,43.6);";
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) { db_close(); return -1; }
    return 0;
}

static int user_id_for(const char *name, const char *password, int create)
{
    sqlite3_stmt *stmt = NULL;
    char salt[33], hash[65], stored_salt[40], stored_hash[65];
    int id = 0;
    if (!name || !password || !*name || (create && strlen(password) < 6) || strlen(name) > 64 || strlen(password) > 128)
        return -1;
    if (create) {
        if (random_hex(salt, 16) < 0) return -1;
        password_digest(salt, password, hash);
        if (sqlite3_prepare_v2(db, "INSERT INTO users(username,salt,password_hash) VALUES(?,?,?)", -1, &stmt, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, salt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, hash, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(db) : 0;
    }
    if (sqlite3_prepare_v2(db, "SELECT id,salt,password_hash FROM users WHERE username=?", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int(stmt, 0);
        snprintf(stored_salt, sizeof(stored_salt), "%s", sqlite3_column_text(stmt, 1));
        snprintf(stored_hash, sizeof(stored_hash), "%s", sqlite3_column_text(stmt, 2));
    }
    sqlite3_finalize(stmt);
    if (!id) return 0;
    password_digest(stored_salt, password, hash);
    return strcmp(hash, stored_hash) == 0 ? id : 0;
}

int db_register_user(const char *name, const char *password) { return user_id_for(name, password, 1); }
int db_login_user(const char *name, const char *password) { return user_id_for(name, password, 0); }

int db_create_session(int user_id, char token[65])
{
    sqlite3_stmt *stmt = NULL;
    if (random_hex(token, 32) < 0 || sqlite3_prepare_v2(db, "INSERT INTO sessions(token,user_id,expires_at) VALUES(?,?,?)", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL) + 86400);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_session_user(const char *token)
{
    sqlite3_stmt *stmt = NULL;
    int id = 0;
    if (!token || strlen(token) != 64 || sqlite3_prepare_v2(db, "SELECT user_id FROM sessions WHERE token=? AND expires_at>?", -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

void db_delete_session(const char *token)
{
    sqlite3_stmt *stmt = NULL;
    if (token && sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token=?", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, token, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

static int json_string(char *out, int cap, int pos, const unsigned char *s)
{
    if (pos >= cap) return -1;
    out[pos++] = '"';
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') {
            if (pos + 2 >= cap) return -1;
            out[pos++] = '\\'; out[pos++] = (char)*s;
        } else if (*s < 32) {
            if (pos + 7 >= cap) return -1;
            pos += snprintf(out + pos, cap - pos, "\\u%04x", *s);
        } else {
            if (pos + 2 >= cap) return -1;
            out[pos++] = (char)*s;
        }
    }
    if (pos >= cap) return -1;
    out[pos++] = '"';
    return pos;
}

int db_list_sensors(const char *filter, char *json, int capacity)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id,name,location,temperature,humidity,updated_at FROM sensors WHERE name LIKE ? OR location LIKE ? ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    char pattern[260]; snprintf(pattern, sizeof(pattern), "%%%s%%", filter ? filter : "");
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
    int pos = 0, first = 1;
    if (capacity < 3) { sqlite3_finalize(stmt); return -1; }
    json[pos++] = '[';
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) { if (pos + 1 >= capacity) goto fail; json[pos++] = ','; }
        first = 0;
        int n = snprintf(json + pos, capacity - pos, "{\"id\":%d,\"name\":", sqlite3_column_int(stmt, 0));
        if (n < 0 || n >= capacity - pos) goto fail; pos += n;
        pos = json_string(json, capacity, pos, sqlite3_column_text(stmt, 1)); if (pos < 0) goto fail;
        n = snprintf(json + pos, capacity - pos, ",\"location\":"); if (n < 0 || n >= capacity - pos) goto fail; pos += n;
        pos = json_string(json, capacity, pos, sqlite3_column_text(stmt, 2)); if (pos < 0) goto fail;
        n = snprintf(json + pos, capacity - pos, ",\"temperature\":%.2f,\"humidity\":%.2f,\"updated_at\":", sqlite3_column_double(stmt, 3), sqlite3_column_double(stmt, 4));
        if (n < 0 || n >= capacity - pos) goto fail; pos += n;
        pos = json_string(json, capacity, pos, sqlite3_column_text(stmt, 5)); if (pos < 0 || pos + 2 >= capacity) goto fail;
        json[pos++] = '}';
    }
    sqlite3_finalize(stmt);
    if (pos + 2 > capacity) return -1;
    json[pos++] = ']'; json[pos] = '\0';
    return pos;
fail:
    sqlite3_finalize(stmt);
    return -1;
}

int db_save_sensor(int id, const char *name, const char *location, double temperature, double humidity)
{
    sqlite3_stmt *stmt = NULL;
    const char *sql = id ? "UPDATE sensors SET name=?,location=?,temperature=?,humidity=?,updated_at=CURRENT_TIMESTAMP WHERE id=?" :
                           "INSERT INTO sensors(name,location,temperature,humidity) VALUES(?,?,?,?)";
    if (!name || !location || !*name || !*location || strlen(name) > 80 || strlen(location) > 120 || temperature < -100 || temperature > 200 || humidity < 0 || humidity > 100)
        return -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, location, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, temperature);
    sqlite3_bind_double(stmt, 4, humidity);
    if (id) sqlite3_bind_int(stmt, 5, id);
    int rc = sqlite3_step(stmt);
    int result = rc == SQLITE_DONE ? (id ? sqlite3_changes(db) : (int)sqlite3_last_insert_rowid(db)) : -1;
    sqlite3_finalize(stmt);
    return result;
}

int db_delete_sensor(int id)
{
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM sensors WHERE id=?", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt), changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? changes : -1;
}

void db_close(void) { if (db) sqlite3_close(db); db = NULL; }
