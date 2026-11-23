#include "server/repos/user_repo.h"
#include "server/services/password.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_user_repo_validate_and_canonicalize(void) {
    printf("1. test_user_repo_validate_and_canonicalize: kiểm tra định dạng và chuẩn hóa username... ");

    assert(!user_repo_validate_username(NULL));
    assert(!user_repo_validate_username(""));
    assert(!user_repo_validate_username("ab"));
    assert(!user_repo_validate_username("this_username_is_way_too_long_exceeding_32_characters_limit"));

    /* Ký tự không hợp lệ */
    assert(!user_repo_validate_username("alice!"));
    assert(!user_repo_validate_username("bob name"));
    assert(!user_repo_validate_username("user-hyphen"));
    assert(!user_repo_validate_username("admin@domain"));

    /* Hợp lệ */
    assert(user_repo_validate_username("a_1"));
    assert(user_repo_validate_username("Alice_123"));
    assert(user_repo_validate_username("bob_the_builder"));

    /* Chuẩn hóa canonical lowercase */
    char canonical[CHAT_USERNAME_MAX_LEN + 1];
    assert(user_repo_canonicalize_username("Alice_123", canonical, sizeof(canonical)));
    assert(strcmp(canonical, "alice_123") == 0);

    assert(user_repo_canonicalize_username("BOB", canonical, sizeof(canonical)));
    assert(strcmp(canonical, "bob") == 0);

    assert(user_repo_canonicalize_username("charlie", canonical, sizeof(canonical)));
    assert(strcmp(canonical, "charlie") == 0);

    /* Buffer nhỏ */
    char small[3];
    assert(!user_repo_canonicalize_username("Alice", small, sizeof(small)));

    printf("PASS\n");
}

static void test_user_repo_create_and_find(void) {
    printf("2. test_user_repo_create_and_find: tạo user và tìm kiếm theo username / id... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    int64_t user_id = 0;
    const char *raw_pw = "Password123!";
    char pw_hash[CHAT_PASSWORD_HASH_BYTES];
    assert(password_hash(raw_pw, strlen(raw_pw), pw_hash, sizeof(pw_hash)) == PASSWORD_OK);

    int64_t now_ms = 1700000000000LL;
    UserRepoResult res = user_repo_create(&db, "Alice_Dev", pw_hash, now_ms, &user_id);
    assert(res == USER_REPO_OK);
    assert(user_id == 1);

    /* Tìm theo username (bất kể hoa thường) */
    UserRecord user;
    assert(user_repo_find_by_username(&db, "alice_dev", &user) == USER_REPO_OK);
    assert(user.user_id == 1);
    assert(strcmp(user.username, "alice_dev") == 0);
    assert(strcmp(user.password_hash, pw_hash) == 0);
    assert(user.created_at == now_ms);
    assert(!user.has_last_seen);
    assert(user.last_seen_at == -1);

    /* Tìm theo biến thể chữ hoa */
    memset(&user, 0, sizeof(user));
    assert(user_repo_find_by_username(&db, "ALICE_DEV", &user) == USER_REPO_OK);
    assert(user.user_id == 1);
    assert(strcmp(user.username, "alice_dev") == 0);

    /* Tìm theo id */
    memset(&user, 0, sizeof(user));
    assert(user_repo_find_by_id(&db, 1, &user) == USER_REPO_OK);
    assert(user.user_id == 1);
    assert(strcmp(user.username, "alice_dev") == 0);

    /* Kiểm tra xác thực mật khẩu từ bản ghi lấy từ repo */
    assert(password_verify(user.password_hash, raw_pw, strlen(raw_pw)) == PASSWORD_OK);

    /* Đếm số lượng user */
    int64_t count = 0;
    assert(user_repo_count(&db, &count) == USER_REPO_OK);
    assert(count == 1);

    db_close(&db);
    printf("PASS\n");
}

static void test_user_repo_unique_constraint(void) {
    printf("3. test_user_repo_unique_constraint: chặn đăng ký trùng username case-insensitive... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    int64_t uid1 = 0;
    assert(user_repo_create(&db, "bob_ross", "$argon2id$fakehash1", 1000, &uid1) == USER_REPO_OK);
    assert(uid1 == 1);

    /* Cùng username chữ thường */
    int64_t uid2 = 0;
    assert(user_repo_create(&db, "bob_ross", "$argon2id$fakehash2", 1001, &uid2) == USER_REPO_USERNAME_TAKEN);

    /* Cùng username nhưng viết hoa */
    assert(user_repo_create(&db, "BOB_ROSS", "$argon2id$fakehash3", 1002, &uid2) == USER_REPO_USERNAME_TAKEN);
    assert(user_repo_create(&db, "Bob_Ross", "$argon2id$fakehash4", 1003, &uid2) == USER_REPO_USERNAME_TAKEN);

    /* Tổng số lượng vẫn là 1 */
    int64_t count = 0;
    assert(user_repo_count(&db, &count) == USER_REPO_OK);
    assert(count == 1);

    db_close(&db);
    printf("PASS\n");
}

static void test_user_repo_update_last_seen(void) {
    printf("4. test_user_repo_update_last_seen: cập nhật thời điểm last_seen_at... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    int64_t uid = 0;
    assert(user_repo_create(&db, "charlie", "$argon2id$hash", 1000, &uid) == USER_REPO_OK);

    /* Trước update: has_last_seen = false */
    UserRecord record;
    assert(user_repo_find_by_id(&db, uid, &record) == USER_REPO_OK);
    assert(!record.has_last_seen);

    /* Update last_seen_at */
    int64_t seen_time = 1700000050000LL;
    assert(user_repo_update_last_seen(&db, uid, seen_time) == USER_REPO_OK);

    /* Sau update: has_last_seen = true, last_seen_at = seen_time */
    assert(user_repo_find_by_id(&db, uid, &record) == USER_REPO_OK);
    assert(record.has_last_seen);
    assert(record.last_seen_at == seen_time);

    /* Cập nhật cho user_id không tồn tại */
    assert(user_repo_update_last_seen(&db, 9999, seen_time) == USER_REPO_NOT_FOUND);

    db_close(&db);
    printf("PASS\n");
}

static void test_user_repo_not_found_and_errors(void) {
    printf("5. test_user_repo_not_found_and_errors: xử lý user không tồn tại và tham số lỗi... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    UserRecord user;
    assert(user_repo_find_by_username(&db, "nonexistent", &user) == USER_REPO_NOT_FOUND);
    assert(user_repo_find_by_id(&db, 404, &user) == USER_REPO_NOT_FOUND);

    /* Đối số không hợp lệ */
    assert(user_repo_create(NULL, "alice", "hash", 100, NULL) == USER_REPO_INVALID_ARGUMENT);
    assert(user_repo_create(&db, "al", "hash", 100, NULL) == USER_REPO_INVALID_USERNAME);
    assert(user_repo_create(&db, "alice", "", 100, NULL) == USER_REPO_INVALID_ARGUMENT);
    assert(user_repo_create(&db, "alice", "hash", -1, NULL) == USER_REPO_INVALID_ARGUMENT);

    assert(user_repo_find_by_username(NULL, "alice", &user) == USER_REPO_INVALID_ARGUMENT);
    assert(user_repo_find_by_username(&db, NULL, &user) == USER_REPO_INVALID_ARGUMENT);
    assert(user_repo_find_by_username(&db, "al", &user) == USER_REPO_INVALID_USERNAME);

    assert(user_repo_find_by_id(NULL, 1, &user) == USER_REPO_INVALID_ARGUMENT);
    assert(user_repo_find_by_id(&db, 0, &user) == USER_REPO_INVALID_ARGUMENT);
    assert(user_repo_find_by_id(&db, -1, &user) == USER_REPO_INVALID_ARGUMENT);

    db_close(&db);
    printf("PASS\n");
}

static void test_user_repo_sql_injection_defense(void) {
    printf("6. test_user_repo_sql_injection_defense: chống injection qua prepared statements... ");

    Database db;
    assert(db_open(":memory:", &db) == DB_OK);

    /* 1. Payload SQL injection trong username: bị validator từ chối ngay */
    const char *inj_user1 = "admin' OR '1'='1";
    assert(user_repo_create(&db, inj_user1, "hash", 100, NULL) == USER_REPO_INVALID_USERNAME);

    const char *inj_user2 = "admin; DROP TABLE users; --";
    assert(user_repo_create(&db, inj_user2, "hash", 100, NULL) == USER_REPO_INVALID_USERNAME);

    /* 2. Payload độc hại trong password_hash: lưu trữ literal text qua prepared statement */
    const char *inj_hash = "fake' OR 1=1; DROP TABLE users; --";
    int64_t uid = 0;
    assert(user_repo_create(&db, "sec_user", inj_hash, 100, &uid) == USER_REPO_OK);
    assert(uid == 1);

    /* Bảng users không hề bị DROP, dữ liệu đọc lại nguyên vẹn */
    UserRecord user;
    assert(user_repo_find_by_id(&db, uid, &user) == USER_REPO_OK);
    assert(strcmp(user.password_hash, inj_hash) == 0);

    /* Integrity check toàn bộ cơ sở dữ liệu vẫn hoàn hảo */
    assert(db_check_integrity(&db));

    db_close(&db);
    printf("PASS\n");
}

int main(void) {
    printf("=== Chạy kiểm thử Module User Repository (SQLite Prepared Statements) ===\n");
    test_user_repo_validate_and_canonicalize();
    test_user_repo_create_and_find();
    test_user_repo_unique_constraint();
    test_user_repo_update_last_seen();
    test_user_repo_not_found_and_errors();
    test_user_repo_sql_injection_defense();
    printf("=== Hoàn tất: 6/6 test cases của Module User Repository thành công! ===\n");
    return 0;
}
