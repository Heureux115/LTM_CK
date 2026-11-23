#define _POSIX_C_SOURCE 200809L

#include "server/db/db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static bool ensure_parent_directory(const char *path)
{
    if (path == NULL || strcmp(path, ":memory:") == 0) {
        return true;
    }
    const char *last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        return true;
    }
    size_t dir_len = (size_t)(last_slash - path);
    if (dir_len == 0) {
        return true;
    }
    char dir[512];
    if (dir_len >= sizeof(dir)) {
        return false;
    }
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

    /* Tạo các thư mục cha theo từng cấp */
    for (char *p = dir + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static DbResult exec_sql(sqlite3 *handle, const char *sql)
{
    char *err_msg = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg != NULL) {
            sqlite3_free(err_msg);
        }
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            return DB_BUSY;
        }
        if (rc == SQLITE_CONSTRAINT) {
            return DB_CONSTRAINT_VIOLATION;
        }
        return DB_IO_ERROR;
    }
    return DB_OK;
}

static DbResult get_user_version(sqlite3 *handle, int *version_out)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(handle, "PRAGMA user_version;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return DB_ERROR_PRAGMA;
    }
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return DB_ERROR_PRAGMA;
    }
    *version_out = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return DB_OK;
}

static DbResult migrate_to_v1(sqlite3 *handle)
{
    DbResult res = exec_sql(handle, "BEGIN IMMEDIATE;");
    if (res != DB_OK) {
        return res;
    }

    const char *ddl_v1 =
        "CREATE TABLE IF NOT EXISTS users ("
        "    user_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    username TEXT NOT NULL UNIQUE,"
        "    password_hash TEXT NOT NULL,"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    last_seen_at INTEGER CHECK (last_seen_at IS NULL OR last_seen_at >= 0)"
        ");";

    res = exec_sql(handle, ddl_v1);
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    res = exec_sql(handle, "PRAGMA user_version = 1;");
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    return exec_sql(handle, "COMMIT;");
}

static DbResult migrate_to_v2(sqlite3 *handle)
{
    DbResult res = exec_sql(handle, "BEGIN IMMEDIATE;");
    if (res != DB_OK) {
        return res;
    }

    const char *ddl_v2 =
        "CREATE TABLE IF NOT EXISTS activity_logs ("
        "    log_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    occurred_at INTEGER NOT NULL CHECK (occurred_at >= 0),"
        "    level TEXT NOT NULL CHECK (level IN ('INFO', 'WARN', 'ERROR')),"
        "    actor_user_id INTEGER REFERENCES users(user_id) ON DELETE SET NULL,"
        "    connection_id TEXT,"
        "    action TEXT NOT NULL,"
        "    outcome TEXT NOT NULL CHECK (outcome IN ('success', 'failure')),"
        "    target_type TEXT CHECK (target_type IS NULL OR target_type IN ('user', 'friend_request', 'group', 'message', 'connection')),"
        "    target_id TEXT,"
        "    error_code TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_activity_logs_occurred_at ON activity_logs (occurred_at, log_id);"
        "CREATE INDEX IF NOT EXISTS idx_activity_logs_actor ON activity_logs (actor_user_id, occurred_at);";

    res = exec_sql(handle, ddl_v2);
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    res = exec_sql(handle, "PRAGMA user_version = 2;");
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    return exec_sql(handle, "COMMIT;");
}

static DbResult migrate_to_v3(sqlite3 *handle)
{
    DbResult res = exec_sql(handle, "BEGIN IMMEDIATE;");
    if (res != DB_OK) {
        return res;
    }

    const char *ddl_v3 =
        "CREATE TABLE IF NOT EXISTS friend_requests ("
        "    friend_request_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    sender_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
        "    recipient_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
        "    user_low_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
        "    user_high_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
        "    status TEXT NOT NULL CHECK (status IN ('pending', 'accepted', 'rejected')),"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    resolved_at INTEGER CHECK (resolved_at IS NULL OR resolved_at >= created_at),"
        "    CHECK (sender_id != recipient_id),"
        "    CHECK (user_low_id < user_high_id),"
        "    CHECK ((sender_id < recipient_id AND user_low_id = sender_id AND user_high_id = recipient_id) OR "
        "           (sender_id > recipient_id AND user_low_id = recipient_id AND user_high_id = sender_id))"
        ");"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_friend_requests_pending_pair "
        "ON friend_requests (user_low_id, user_high_id) WHERE status = 'pending';"
        "CREATE INDEX IF NOT EXISTS idx_friend_requests_recipient "
        "ON friend_requests (recipient_id, status, friend_request_id);"
        "CREATE INDEX IF NOT EXISTS idx_friend_requests_sender "
        "ON friend_requests (sender_id, status, friend_request_id);"
        "CREATE TABLE IF NOT EXISTS friendships ("
        "    user_low_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
        "    user_high_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    PRIMARY KEY (user_low_id, user_high_id),"
        "    CHECK (user_low_id < user_high_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_friendships_high_low "
        "ON friendships (user_high_id, user_low_id);";

    res = exec_sql(handle, ddl_v3);
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    res = exec_sql(handle, "PRAGMA user_version = 3;");
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    return exec_sql(handle, "COMMIT;");
}

static DbResult migrate_to_v4(sqlite3 *handle)
{
    DbResult res = exec_sql(handle, "BEGIN IMMEDIATE;");
    if (res != DB_OK) {
        return res;
    }

    const char *ddl_v4 =
        "CREATE TABLE IF NOT EXISTS direct_messages ("
        "    message_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    sender_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    recipient_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    client_msg_id TEXT NOT NULL,"
        "    body TEXT NOT NULL,"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    delivered_at INTEGER CHECK (delivered_at IS NULL OR delivered_at >= created_at),"
        "    CHECK (sender_id != recipient_id),"
        "    CHECK (length(client_msg_id) = 32),"
        "    CHECK (length(body) >= 1 AND length(body) <= 4096),"
        "    UNIQUE (sender_id, client_msg_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_direct_messages_pending "
        "ON direct_messages (recipient_id, message_id) WHERE delivered_at IS NULL;"
        "CREATE INDEX IF NOT EXISTS idx_direct_messages_conversation "
        "ON direct_messages (sender_id, recipient_id, message_id);"
        "CREATE INDEX IF NOT EXISTS idx_direct_messages_recipient_sender "
        "ON direct_messages (recipient_id, sender_id, message_id);";

    res = exec_sql(handle, ddl_v4);
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    res = exec_sql(handle, "PRAGMA user_version = 4;");
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    return exec_sql(handle, "COMMIT;");
}

static DbResult migrate_to_v5(sqlite3 *handle)
{
    DbResult res = exec_sql(handle, "BEGIN IMMEDIATE;");
    if (res != DB_OK) {
        return res;
    }

    const char *ddl_v5 =
        "CREATE TABLE IF NOT EXISTS chat_groups ("
        "    group_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    name TEXT NOT NULL,"
        "    visibility TEXT NOT NULL CHECK (visibility IN ('public', 'private')),"
        "    owner_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    closed_at INTEGER CHECK (closed_at IS NULL OR closed_at >= created_at),"
        "    CHECK (length(name) >= 1 AND length(name) <= 128)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_chat_groups_owner ON chat_groups (owner_id, group_id);"
        "CREATE TABLE IF NOT EXISTS group_members ("
        "    group_id INTEGER NOT NULL REFERENCES chat_groups(group_id) ON DELETE RESTRICT,"
        "    user_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    joined_at INTEGER NOT NULL CHECK (joined_at >= 0),"
        "    PRIMARY KEY (group_id, user_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_group_members_user ON group_members (user_id, group_id);"
        "CREATE TABLE IF NOT EXISTS group_invitations ("
        "    invitation_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    group_id INTEGER NOT NULL REFERENCES chat_groups(group_id) ON DELETE RESTRICT,"
        "    inviter_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    invitee_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    status TEXT NOT NULL CHECK (status IN ('pending', 'accepted', 'rejected', 'canceled')),"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    resolved_at INTEGER CHECK (resolved_at IS NULL OR resolved_at >= created_at)"
        ");"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_group_invitations_pending "
        "ON group_invitations (group_id, invitee_id) WHERE status = 'pending';"
        "CREATE INDEX IF NOT EXISTS idx_group_invitations_invitee "
        "ON group_invitations (invitee_id, status, invitation_id);"
        "CREATE INDEX IF NOT EXISTS idx_group_invitations_group "
        "ON group_invitations (group_id, status, invitation_id);"
        "CREATE TABLE IF NOT EXISTS group_messages ("
        "    message_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    group_id INTEGER NOT NULL REFERENCES chat_groups(group_id) ON DELETE RESTRICT,"
        "    sender_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    client_msg_id TEXT NOT NULL,"
        "    body TEXT NOT NULL,"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    CHECK (length(client_msg_id) = 32),"
        "    CHECK (length(body) >= 1 AND length(body) <= 4096),"
        "    UNIQUE (sender_id, client_msg_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_group_messages_group ON group_messages (group_id, message_id);"
        "CREATE TABLE IF NOT EXISTS group_message_recipients ("
        "    message_id INTEGER NOT NULL REFERENCES group_messages(message_id) ON DELETE RESTRICT,"
        "    recipient_id INTEGER NOT NULL REFERENCES users(user_id) ON DELETE RESTRICT,"
        "    delivered_at INTEGER CHECK (delivered_at IS NULL OR delivered_at >= 0),"
        "    PRIMARY KEY (message_id, recipient_id)"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_group_message_recipients_pending "
        "ON group_message_recipients (recipient_id, message_id) WHERE delivered_at IS NULL;";

    res = exec_sql(handle, ddl_v5);
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    res = exec_sql(handle, "PRAGMA user_version = 5;");
    if (res != DB_OK) {
        exec_sql(handle, "ROLLBACK;");
        return DB_ERROR_MIGRATION;
    }

    return exec_sql(handle, "COMMIT;");
}

DbResult db_open(const char *path, Database *db)
{
    if (path == NULL || db == NULL) {
        return DB_INVALID_ARGUMENT;
    }
    memset(db, 0, sizeof(*db));

    if (!ensure_parent_directory(path)) {
        return DB_IO_ERROR;
    }

    sqlite3 *handle = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(path, &handle, flags, NULL);
    if (rc != SQLITE_OK || handle == NULL) {
        if (handle != NULL) {
            sqlite3_close(handle);
        }
        return DB_ERROR_OPEN;
    }

    /* Bật foreign keys và cấu hình timeout */
    if (exec_sql(handle, "PRAGMA foreign_keys = ON;") != DB_OK) {
        sqlite3_close(handle);
        return DB_ERROR_PRAGMA;
    }
    sqlite3_busy_timeout(handle, CHAT_DB_BUSY_TIMEOUT_MS);

    /* Cấu hình WAL mode và synchronous nếu là file disk */
    if (strcmp(path, ":memory:") != 0) {
        exec_sql(handle, "PRAGMA journal_mode = WAL;");
        exec_sql(handle, "PRAGMA synchronous = FULL;");
    }

    /* Kiểm tra version schema */
    int current_version = 0;
    DbResult ver_res = get_user_version(handle, &current_version);
    if (ver_res != DB_OK) {
        sqlite3_close(handle);
        return ver_res;
    }

    if (current_version > CHAT_SCHEMA_VERSION) {
        sqlite3_close(handle);
        return DB_UNSUPPORTED_VERSION;
    }

    /* Chạy migration tuần tự */
    if (current_version < 1) {
        DbResult mig_res = migrate_to_v1(handle);
        if (mig_res != DB_OK) {
            sqlite3_close(handle);
            return mig_res;
        }
        current_version = 1;
    }

    if (current_version < 2) {
        DbResult mig_res = migrate_to_v2(handle);
        if (mig_res != DB_OK) {
            sqlite3_close(handle);
            return mig_res;
        }
        current_version = 2;
    }

    if (current_version < 3) {
        DbResult mig_res = migrate_to_v3(handle);
        if (mig_res != DB_OK) {
            sqlite3_close(handle);
            return mig_res;
        }
        current_version = 3;
    }

    if (current_version < 4) {
        DbResult mig_res = migrate_to_v4(handle);
        if (mig_res != DB_OK) {
            sqlite3_close(handle);
            return mig_res;
        }
        current_version = 4;
    }

    if (current_version < 5) {
        DbResult mig_res = migrate_to_v5(handle);
        if (mig_res != DB_OK) {
            sqlite3_close(handle);
            return mig_res;
        }
        current_version = 5;
    }

    char *path_dup = strdup(path);
    if (path_dup == NULL) {
        sqlite3_close(handle);
        return DB_INTERNAL_ERROR;
    }

    db->handle = handle;
    db->path = path_dup;
    db->schema_version = current_version;
    return DB_OK;
}

void db_close(Database *db)
{
    if (db == NULL || db->handle == NULL) {
        return;
    }
    sqlite3_close(db->handle);
    free(db->path);
    memset(db, 0, sizeof(*db));
}

DbResult db_begin_immediate(Database *db)
{
    if (db == NULL || db->handle == NULL) {
        return DB_INVALID_ARGUMENT;
    }
    return exec_sql(db->handle, "BEGIN IMMEDIATE;");
}

DbResult db_commit(Database *db)
{
    if (db == NULL || db->handle == NULL) {
        return DB_INVALID_ARGUMENT;
    }
    return exec_sql(db->handle, "COMMIT;");
}

DbResult db_rollback(Database *db)
{
    if (db == NULL || db->handle == NULL) {
        return DB_INVALID_ARGUMENT;
    }
    return exec_sql(db->handle, "ROLLBACK;");
}

bool db_check_integrity(Database *db)
{
    if (db == NULL || db->handle == NULL) {
        return false;
    }

    /* 1. integrity_check */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, "PRAGMA integrity_check;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return false;
    }
    bool integrity_ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text != NULL && strcmp((const char *)text, "ok") == 0) {
            integrity_ok = true;
        }
    }
    sqlite3_finalize(stmt);
    if (!integrity_ok) {
        return false;
    }

    /* 2. foreign_key_check */
    rc = sqlite3_prepare_v2(db->handle, "PRAGMA foreign_key_check;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return false;
    }
    /* Nếu không có lỗi FK, step trả về SQLITE_DONE ngay (0 dòng) */
    bool fk_ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    return fk_ok;
}

const char *db_result_name(DbResult result)
{
    switch (result) {
    case DB_OK:
        return "success";
    case DB_INVALID_ARGUMENT:
        return "invalid argument";
    case DB_ERROR_OPEN:
        return "cannot open database";
    case DB_ERROR_PRAGMA:
        return "pragma execution failed";
    case DB_ERROR_MIGRATION:
        return "migration failed";
    case DB_UNSUPPORTED_VERSION:
        return "unsupported newer schema version";
    case DB_BUSY:
        return "database is busy";
    case DB_CONSTRAINT_VIOLATION:
        return "constraint violation";
    case DB_IO_ERROR:
        return "database I/O error";
    case DB_INTERNAL_ERROR:
        return "internal database error";
    }
    return "unknown database error";
}
