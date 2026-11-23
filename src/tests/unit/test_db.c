#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server/db/db.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_db_open_memory_and_pragmas(void)
{
    Database db = {0};
    check(db_open(":memory:", &db) == DB_OK, "open in-memory database");
    check(db.handle != NULL, "db handle is valid");
    check(db.schema_version == CHAT_SCHEMA_VERSION, "schema version matches v1");
    check(db_check_integrity(&db), "db integrity check passes");

    /* Kiểm tra PRAGMA foreign_keys = ON */
    sqlite3_stmt *stmt = NULL;
    check(sqlite3_prepare_v2(db.handle, "PRAGMA foreign_keys;", -1, &stmt, NULL) == SQLITE_OK,
          "prepare foreign_keys query");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step foreign_keys query");
    int fk_enabled = sqlite3_column_int(stmt, 0);
    check(fk_enabled == 1, "foreign_keys is enabled (1)");
    sqlite3_finalize(stmt);

    /* Kiểm tra bảng users đã tồn tại và có thể truy vấn */
    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM users;", -1, &stmt, NULL) == SQLITE_OK,
          "prepare count users query");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step count users query");
    check(sqlite3_column_int(stmt, 0) == 0, "users table is initially empty");
    sqlite3_finalize(stmt);

    db_close(&db);
    check(db.handle == NULL && db.path == NULL, "db_close cleans up state");
}

static void test_db_users_table_constraints(void)
{
    Database db = {0};
    check(db_open(":memory:", &db) == DB_OK, "open in-memory database");

    /* 1. Chèn user hợp lệ */
    sqlite3_stmt *stmt = NULL;
    const char *insert_sql =
        "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?);";
    check(sqlite3_prepare_v2(db.handle, insert_sql, -1, &stmt, NULL) == SQLITE_OK,
          "prepare insert user");
    sqlite3_bind_text(stmt, 1, "alice", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "$argon2id$fakehash1", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, 1792054800000LL);
    check(sqlite3_step(stmt) == SQLITE_DONE, "insert alice successfully");
    check(sqlite3_last_insert_rowid(db.handle) == 1, "alice rowid is 1");
    sqlite3_finalize(stmt);

    /* 2. Chèn user thứ hai hợp lệ */
    check(sqlite3_prepare_v2(db.handle, insert_sql, -1, &stmt, NULL) == SQLITE_OK,
          "prepare insert bob");
    sqlite3_bind_text(stmt, 1, "bob", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "$argon2id$fakehash2", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, 1792054801000LL);
    check(sqlite3_step(stmt) == SQLITE_DONE, "insert bob successfully");
    check(sqlite3_last_insert_rowid(db.handle) == 2, "bob rowid is 2");
    sqlite3_finalize(stmt);

    /* 3. Chèn user trùng username "alice": phải vi phạm UNIQUE constraint */
    check(sqlite3_prepare_v2(db.handle, insert_sql, -1, &stmt, NULL) == SQLITE_OK,
          "prepare duplicate alice insert");
    sqlite3_bind_text(stmt, 1, "alice", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "$argon2id$otherhash", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, 1792054802000LL);
    int rc = sqlite3_step(stmt);
    check(rc == SQLITE_CONSTRAINT, "duplicate username is rejected by UNIQUE constraint");
    sqlite3_finalize(stmt);

    /* 4. Chèn user có created_at < 0: phải vi phạm CHECK constraint */
    check(sqlite3_prepare_v2(db.handle, insert_sql, -1, &stmt, NULL) == SQLITE_OK,
          "prepare invalid created_at insert");
    sqlite3_bind_text(stmt, 1, "carol", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "$argon2id$fakehash3", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, -1);
    rc = sqlite3_step(stmt);
    check(rc == SQLITE_CONSTRAINT, "negative created_at is rejected by CHECK constraint");
    sqlite3_finalize(stmt);

    db_close(&db);
}

static void test_db_persistence_and_reopen(void)
{
    const char *db_file = "/tmp/test_chat_persistence.db";
    unlink(db_file);

    /* Lần 1: Mở DB trên disk, chèn user */
    Database db1 = {0};
    check(db_open(db_file, &db1) == DB_OK, "open disk database");
    check(db1.schema_version == CHAT_SCHEMA_VERSION, "schema version is current on disk");

    sqlite3_stmt *stmt = NULL;
    check(sqlite3_prepare_v2(db1.handle,
                            "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?);",
                            -1, &stmt, NULL) == SQLITE_OK,
          "prepare insert charlie");
    sqlite3_bind_text(stmt, 1, "charlie", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, "$argon2id$charliehash", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, 1792054803000LL);
    check(sqlite3_step(stmt) == SQLITE_DONE, "insert charlie on disk");
    sqlite3_finalize(stmt);
    db_close(&db1);

    /* Lần 2: Mở lại DB từ disk, xác nhận user "charlie" vẫn tồn tại nguyên vẹn */
    Database db2 = {0};
    check(db_open(db_file, &db2) == DB_OK, "reopen disk database");
    check(sqlite3_prepare_v2(db2.handle,
                            "SELECT user_id, password_hash, created_at FROM users WHERE username = ?;",
                            -1, &stmt, NULL) == SQLITE_OK,
          "prepare select charlie");
    sqlite3_bind_text(stmt, 1, "charlie", -1, SQLITE_STATIC);
    check(sqlite3_step(stmt) == SQLITE_ROW, "find charlie row");
    check(sqlite3_column_int64(stmt, 0) == 1, "charlie user_id is 1");
    check(strcmp((const char *)sqlite3_column_text(stmt, 1), "$argon2id$charliehash") == 0,
          "password_hash preserved");
    check(sqlite3_column_int64(stmt, 2) == 1792054803000LL, "created_at preserved");
    sqlite3_finalize(stmt);
    db_close(&db2);

    unlink(db_file);
}

static void test_db_unsupported_version_guard(void)
{
    const char *db_file = "/tmp/test_chat_future_ver.db";
    unlink(db_file);

    /* Tạo DB và sửa user_version lên 999 (version tương lai) */
    sqlite3 *raw_handle = NULL;
    check(sqlite3_open(db_file, &raw_handle) == SQLITE_OK, "create raw db");
    check(sqlite3_exec(raw_handle, "PRAGMA user_version = 999;", NULL, NULL, NULL) == SQLITE_OK,
          "set user_version to 999");
    sqlite3_close(raw_handle);

    /* db_open phải từ chối mở vì version lớn hơn CHAT_SCHEMA_VERSION */
    Database db = {0};
    check(db_open(db_file, &db) == DB_UNSUPPORTED_VERSION,
          "db_open rejects unsupported newer schema version");
    check(db.handle == NULL, "db handle remains NULL");

    unlink(db_file);
}

static void test_db_transactions(void)
{
    Database db = {0};
    check(db_open(":memory:", &db) == DB_OK, "open in-memory database");

    check(db_begin_immediate(&db) == DB_OK, "begin immediate transaction");
    sqlite3_stmt *stmt = NULL;
    check(sqlite3_prepare_v2(db.handle,
                            "INSERT INTO users (username, password_hash, created_at) VALUES ('temp', 'h', 1);",
                            -1, &stmt, NULL) == SQLITE_OK,
          "prepare temp insert");
    check(sqlite3_step(stmt) == SQLITE_DONE, "step temp insert");
    sqlite3_finalize(stmt);

    check(db_rollback(&db) == DB_OK, "rollback transaction");

    /* Xác nhận user 'temp' không tồn tại sau rollback */
    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM users;", -1, &stmt, NULL) == SQLITE_OK,
          "prepare count users");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step count");
    check(sqlite3_column_int(stmt, 0) == 0, "users table has 0 rows after rollback");
    sqlite3_finalize(stmt);

    /* Test commit */
    check(db_begin_immediate(&db) == DB_OK, "begin second transaction");
    check(sqlite3_prepare_v2(db.handle,
                            "INSERT INTO users (username, password_hash, created_at) VALUES ('committed', 'h', 2);",
                            -1, &stmt, NULL) == SQLITE_OK,
          "prepare committed insert");
    check(sqlite3_step(stmt) == SQLITE_DONE, "step committed insert");
    sqlite3_finalize(stmt);
    check(db_commit(&db) == DB_OK, "commit transaction");

    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM users;", -1, &stmt, NULL) == SQLITE_OK,
          "prepare count users");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step count");
    check(sqlite3_column_int(stmt, 0) == 1, "users table has 1 row after commit");
    sqlite3_finalize(stmt);

    db_close(&db);
}

static void test_db_migration_v1_to_v2(void)
{
    const char *db_file = "/tmp/test_chat_mig_v1_v2.db";
    unlink(db_file);

    /* 1. Tạo database version 1 với chỉ bảng users */
    sqlite3 *raw = NULL;
    check(sqlite3_open(db_file, &raw) == SQLITE_OK, "create v1 raw db");
    const char *ddl_v1 =
        "CREATE TABLE users ("
        "    user_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    username TEXT NOT NULL UNIQUE,"
        "    password_hash TEXT NOT NULL,"
        "    created_at INTEGER NOT NULL CHECK (created_at >= 0),"
        "    last_seen_at INTEGER CHECK (last_seen_at IS NULL OR last_seen_at >= 0)"
        ");"
        "INSERT INTO users (username, password_hash, created_at) VALUES ('v1_user', 'hash', 100);"
        "PRAGMA user_version = 1;";
    check(sqlite3_exec(raw, ddl_v1, NULL, NULL, NULL) == SQLITE_OK, "init v1 schema");
    sqlite3_close(raw);

    /* 2. Mở bằng db_open: phải tự động chạy migration lên version mới nhất */
    Database db = {0};
    check(db_open(db_file, &db) == DB_OK, "open and auto-migrate v1 db");
    check(db.schema_version == CHAT_SCHEMA_VERSION, "schema version upgraded to current");

    /* Dữ liệu users cũ còn nguyên */
    sqlite3_stmt *stmt = NULL;
    check(sqlite3_prepare_v2(db.handle, "SELECT username FROM users WHERE user_id = 1;", -1, &stmt, NULL) == SQLITE_OK,
          "query old user");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step old user");
    check(strcmp((const char *)sqlite3_column_text(stmt, 0), "v1_user") == 0, "old username preserved");
    sqlite3_finalize(stmt);

    /* Bảng activity_logs mới đã được tạo */
    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM activity_logs;", -1, &stmt, NULL) == SQLITE_OK,
          "query activity_logs table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step activity_logs count");
    check(sqlite3_column_int(stmt, 0) == 0, "activity_logs starts empty");
    sqlite3_finalize(stmt);

    /* Bảng friend_requests và friendships mới đã được tạo */
    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM friend_requests;", -1, &stmt, NULL) == SQLITE_OK,
          "query friend_requests table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step friend_requests count");
    check(sqlite3_column_int(stmt, 0) == 0, "friend_requests starts empty");
    sqlite3_finalize(stmt);

    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM friendships;", -1, &stmt, NULL) == SQLITE_OK,
          "query friendships table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step friendships count");
    check(sqlite3_column_int(stmt, 0) == 0, "friendships starts empty");
    sqlite3_finalize(stmt);

    /* Bảng direct_messages mới (v4) đã được tạo */
    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM direct_messages;", -1, &stmt, NULL) == SQLITE_OK,
          "query direct_messages table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step direct_messages count");
    check(sqlite3_column_int(stmt, 0) == 0, "direct_messages starts empty");
    sqlite3_finalize(stmt);

    /* Các bảng chat_groups, group_members, group_invitations, group_messages, group_message_recipients (v5) đã được tạo */
    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM chat_groups;", -1, &stmt, NULL) == SQLITE_OK,
          "query chat_groups table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step chat_groups count");
    check(sqlite3_column_int(stmt, 0) == 0, "chat_groups starts empty");
    sqlite3_finalize(stmt);

    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM group_members;", -1, &stmt, NULL) == SQLITE_OK,
          "query group_members table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step group_members count");
    check(sqlite3_column_int(stmt, 0) == 0, "group_members starts empty");
    sqlite3_finalize(stmt);

    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM group_invitations;", -1, &stmt, NULL) == SQLITE_OK,
          "query group_invitations table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step group_invitations count");
    check(sqlite3_column_int(stmt, 0) == 0, "group_invitations starts empty");
    sqlite3_finalize(stmt);

    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM group_messages;", -1, &stmt, NULL) == SQLITE_OK,
          "query group_messages table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step group_messages count");
    check(sqlite3_column_int(stmt, 0) == 0, "group_messages starts empty");
    sqlite3_finalize(stmt);

    check(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM group_message_recipients;", -1, &stmt, NULL) == SQLITE_OK,
          "query group_message_recipients table");
    check(sqlite3_step(stmt) == SQLITE_ROW, "step group_message_recipients count");
    check(sqlite3_column_int(stmt, 0) == 0, "group_message_recipients starts empty");
    sqlite3_finalize(stmt);

    check(db_check_integrity(&db), "migrated db passes integrity check");
    db_close(&db);
    unlink(db_file);
}

int main(void)
{
    test_db_open_memory_and_pragmas();
    test_db_users_table_constraints();
    test_db_persistence_and_reopen();
    test_db_unsupported_version_guard();
    test_db_transactions();
    test_db_migration_v1_to_v2();
    puts("6 database tests passed.");
    return EXIT_SUCCESS;
}
