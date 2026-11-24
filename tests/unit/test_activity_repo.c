#include "server/repos/activity_repo.h"
#include "server/repos/user_repo.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_activity_repo_enum_conversions(void) {
    printf("1. test_activity_repo_enum_conversions: kiểm tra chuyển đổi chuỗi enum... ");

    ActivityLevel level;
    assert(activity_level_from_string("INFO", &level) && level == ACTIVITY_LEVEL_INFO);
    assert(activity_level_from_string("WARN", &level) && level == ACTIVITY_LEVEL_WARN);
    assert(activity_level_from_string("ERROR", &level) && level == ACTIVITY_LEVEL_ERROR);
    assert(!activity_level_from_string("UNKNOWN", &level));
    assert(strcmp(activity_level_to_string(ACTIVITY_LEVEL_INFO), "INFO") == 0);
    assert(strcmp(activity_level_to_string(ACTIVITY_LEVEL_WARN), "WARN") == 0);
    assert(strcmp(activity_level_to_string(ACTIVITY_LEVEL_ERROR), "ERROR") == 0);

    ActivityOutcome outcome;
    assert(activity_outcome_from_string("success", &outcome) && outcome == ACTIVITY_OUTCOME_SUCCESS);
    assert(activity_outcome_from_string("failure", &outcome) && outcome == ACTIVITY_OUTCOME_FAILURE);
    assert(!activity_outcome_from_string("invalid", &outcome));

    ActivityTargetType tt;
    assert(activity_target_type_from_string("user", &tt) && tt == ACTIVITY_TARGET_USER);
    assert(activity_target_type_from_string("friend_request", &tt) && tt == ACTIVITY_TARGET_FRIEND_REQUEST);
    assert(activity_target_type_from_string("group", &tt) && tt == ACTIVITY_TARGET_GROUP);
    assert(activity_target_type_from_string("message", &tt) && tt == ACTIVITY_TARGET_MESSAGE);
    assert(activity_target_type_from_string("connection", &tt) && tt == ACTIVITY_TARGET_CONNECTION);
    assert(activity_target_type_from_string(NULL, &tt) && tt == ACTIVITY_TARGET_NONE);
    assert(activity_target_type_from_string("", &tt) && tt == ACTIVITY_TARGET_NONE);
    assert(!activity_target_type_from_string("non_existent", &tt));

    printf("PASS\n");
}

static void test_activity_repo_log_unauthenticated(void) {
    printf("2. test_activity_repo_log_unauthenticated: ghi log khi chưa có user_id (NULL actor)... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    int64_t log_id = 0;
    int64_t now_ms = 1700000001000LL;

    ActivityRepoResult res = activity_repo_log(
        &db,
        now_ms,
        ACTIVITY_LEVEL_WARN,
        NULL,
        "conn-10",
        "protocol.error",
        ACTIVITY_OUTCOME_FAILURE,
        ACTIVITY_TARGET_CONNECTION,
        "conn-10",
        "INVALID_JSON",
        &log_id
    );
    assert(res == ACTIVITY_REPO_OK);
    assert(log_id == 1);

    ActivityRecord record;
    assert(activity_repo_find_by_id(&db, log_id, &record) == ACTIVITY_REPO_OK);
    assert(record.log_id == 1);
    assert(record.occurred_at == now_ms);
    assert(record.level == ACTIVITY_LEVEL_WARN);
    assert(!record.has_actor);
    assert(record.actor_user_id == -1);
    assert(strcmp(record.connection_id, "conn-10") == 0);
    assert(strcmp(record.action, "protocol.error") == 0);
    assert(record.outcome == ACTIVITY_OUTCOME_FAILURE);
    assert(record.target_type == ACTIVITY_TARGET_CONNECTION);
    assert(strcmp(record.target_id, "conn-10") == 0);
    assert(strcmp(record.error_code, "INVALID_JSON") == 0);

    db_close(&db);
    printf("PASS\n");
}

static void test_activity_repo_log_authenticated(void) {
    printf("3. test_activity_repo_log_authenticated: ghi log với user hợp lệ và đếm theo user... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    /* Tạo 1 user trước */
    int64_t user_id = 0;
    assert(user_repo_create(&db, "alice", "$argon2id$fakehash", 1000, &user_id) == USER_REPO_OK);
    assert(user_id == 1);

    int64_t log_id1 = 0;
    ActivityRepoResult res1 = activity_repo_log(
        &db,
        2000,
        ACTIVITY_LEVEL_INFO,
        &user_id,
        "conn-42",
        "auth.login",
        ACTIVITY_OUTCOME_SUCCESS,
        ACTIVITY_TARGET_USER,
        "1",
        NULL,
        &log_id1
    );
    assert(res1 == ACTIVITY_REPO_OK);
    assert(log_id1 == 1);

    int64_t log_id2 = 0;
    ActivityRepoResult res2 = activity_repo_log(
        &db,
        3000,
        ACTIVITY_LEVEL_INFO,
        &user_id,
        "conn-42",
        "auth.logout",
        ACTIVITY_OUTCOME_SUCCESS,
        ACTIVITY_TARGET_USER,
        "1",
        NULL,
        &log_id2
    );
    assert(res2 == ACTIVITY_REPO_OK);
    assert(log_id2 == 2);

    /* Kiểm tra bản ghi */
    ActivityRecord rec;
    assert(activity_repo_find_by_id(&db, log_id1, &rec) == ACTIVITY_REPO_OK);
    assert(rec.has_actor);
    assert(rec.actor_user_id == 1);
    assert(strcmp(rec.action, "auth.login") == 0);
    assert(rec.error_code[0] == '\0');

    /* Đếm theo user */
    int64_t count = 0;
    assert(activity_repo_count_by_user(&db, 1, &count) == ACTIVITY_REPO_OK);
    assert(count == 2);

    db_close(&db);
    printf("PASS\n");
}

static void test_activity_repo_foreign_key_violation(void) {
    printf("4. test_activity_repo_foreign_key_violation: chặn actor_user_id không tồn tại... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    int64_t nonexistent_user_id = 9999;
    int64_t log_id = 0;

    /* Thử ghi log với actor_user_id không có trong bảng users -> phải bị FOREIGN KEY constraint chặn */
    ActivityRepoResult res = activity_repo_log(
        &db,
        1000,
        ACTIVITY_LEVEL_INFO,
        &nonexistent_user_id,
        "conn-1",
        "auth.register",
        ACTIVITY_OUTCOME_SUCCESS,
        ACTIVITY_TARGET_USER,
        "9999",
        NULL,
        &log_id
    );
    assert(res == ACTIVITY_REPO_DB_ERROR);

    /* Bảng activity_logs vẫn rỗng */
    int64_t count = 0;
    assert(activity_repo_count(&db, &count) == ACTIVITY_REPO_OK);
    assert(count == 0);

    db_close(&db);
    printf("PASS\n");
}

static void test_activity_repo_not_found_and_errors(void) {
    printf("5. test_activity_repo_not_found_and_errors: xử lý log không tồn tại và tham số biên... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    ActivityRecord rec;
    assert(activity_repo_find_by_id(&db, 404, &rec) == ACTIVITY_REPO_NOT_FOUND);

    /* Tham số NULL / rỗng */
    assert(activity_repo_log(NULL, 100, ACTIVITY_LEVEL_INFO, NULL, NULL, "act", ACTIVITY_OUTCOME_SUCCESS, ACTIVITY_TARGET_NONE, NULL, NULL, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_log(&db, -1, ACTIVITY_LEVEL_INFO, NULL, NULL, "act", ACTIVITY_OUTCOME_SUCCESS, ACTIVITY_TARGET_NONE, NULL, NULL, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_log(&db, 100, ACTIVITY_LEVEL_INFO, NULL, NULL, NULL, ACTIVITY_OUTCOME_SUCCESS, ACTIVITY_TARGET_NONE, NULL, NULL, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_log(&db, 100, ACTIVITY_LEVEL_INFO, NULL, NULL, "", ACTIVITY_OUTCOME_SUCCESS, ACTIVITY_TARGET_NONE, NULL, NULL, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);

    assert(activity_repo_find_by_id(NULL, 1, &rec) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_find_by_id(&db, 0, &rec) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_find_by_id(&db, -1, &rec) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_find_by_id(&db, 1, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);

    assert(activity_repo_count(NULL, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);
    assert(activity_repo_count_by_user(NULL, 1, NULL) == ACTIVITY_REPO_INVALID_ARGUMENT);

    db_close(&db);
    printf("PASS\n");
}

static void test_activity_repo_persistence(void) {
    printf("6. test_activity_repo_persistence: lưu trữ bền vững trên đĩa sau khi reopen... ");

    const char *db_file = "/tmp/test_activity_persistence.db";
    unlink(db_file);

    /* 1. Mở DB trên đĩa, chèn user và ghi log */
    Database db1;
    assert(db_open(db_file, &db1) == DB_OK);

    int64_t uid = 0;
    assert(user_repo_create(&db1, "bob", "$argon2id$bobhash", 1000, &uid) == USER_REPO_OK);

    int64_t log_id = 0;
    assert(activity_repo_log(&db1, 2000, ACTIVITY_LEVEL_INFO, &uid, "conn-99", "auth.register", ACTIVITY_OUTCOME_SUCCESS, ACTIVITY_TARGET_USER, "1", NULL, &log_id) == ACTIVITY_REPO_OK);
    assert(log_id == 1);
    db_close(&db1);

    /* 2. Mở lại DB, đọc lại bản ghi log */
    Database db2;
    assert(db_open(db_file, &db2) == DB_OK);

    ActivityRecord rec;
    assert(activity_repo_find_by_id(&db2, 1, &rec) == ACTIVITY_REPO_OK);
    assert(rec.occurred_at == 2000);
    assert(rec.has_actor && rec.actor_user_id == uid);
    assert(strcmp(rec.action, "auth.register") == 0);
    assert(rec.outcome == ACTIVITY_OUTCOME_SUCCESS);

    int64_t count = 0;
    assert(activity_repo_count(&db2, &count) == ACTIVITY_REPO_OK);
    assert(count == 1);

    db_close(&db2);
    unlink(db_file);
    printf("PASS\n");
}

int main(void) {
    printf("=== Chạy kiểm thử Module Activity Log Repository (SQLite Prepared Statements) ===\n");
    test_activity_repo_enum_conversions();
    test_activity_repo_log_unauthenticated();
    test_activity_repo_log_authenticated();
    test_activity_repo_foreign_key_violation();
    test_activity_repo_not_found_and_errors();
    test_activity_repo_persistence();
    printf("=== Hoàn tất: 6/6 test cases của Module Activity Log Repository thành công! ===\n");
    return 0;
}
