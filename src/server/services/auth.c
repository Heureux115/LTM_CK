#include "server/services/auth.h"
#include "server/repos/activity_repo.h"
#include "server/services/password.h"
#include <stdio.h>
#include <string.h>

AuthServiceResult auth_service_register(Database *db,
                                        const char *username,
                                        const char *password,
                                        size_t password_len,
                                        const char *connection_id,
                                        int64_t now_ms,
                                        UserRecord *out_user)
{
    if (db == NULL || db->handle == NULL || username == NULL || password == NULL || now_ms < 0) {
        return AUTH_SERVICE_INVALID_ARGUMENT;
    }

    /* 1. Kiểm tra username */
    if (!user_repo_validate_username(username)) {
        return AUTH_SERVICE_VALIDATION_ERROR;
    }

    /* 2. Kiểm tra password */
    if (password_validate_input(password, password_len) != PASSWORD_OK) {
        return AUTH_SERVICE_VALIDATION_ERROR;
    }

    char canonical[CHAT_USERNAME_MAX_LEN + 1];
    if (!user_repo_canonicalize_username(username, canonical, sizeof(canonical))) {
        return AUTH_SERVICE_VALIDATION_ERROR;
    }

    /* 3. Pre-check username trùng (tránh hash tốn CPU nếu username đã có) */
    UserRecord existing;
    UserRepoResult find_res = user_repo_find_by_username(db, canonical, &existing);
    if (find_res == USER_REPO_OK) {
        /* Ghi log cảnh báo trùng username */
        activity_repo_log(db, now_ms, ACTIVITY_LEVEL_WARN, NULL, connection_id,
                          "auth.register", ACTIVITY_OUTCOME_FAILURE,
                          ACTIVITY_TARGET_USER, canonical, "USERNAME_TAKEN", NULL);
        return AUTH_SERVICE_USERNAME_TAKEN;
    }

    /* 4. Băm mật khẩu bằng Argon2id ngoài transaction DB */
    char hash[CHAT_PASSWORD_HASH_BYTES];
    PasswordResult hash_res = password_hash(password, password_len, hash, sizeof(hash));
    if (hash_res != PASSWORD_OK) {
        return AUTH_SERVICE_INTERNAL_ERROR;
    }

    /* 5. Mở transaction BEGIN IMMEDIATE */
    if (db_begin_immediate(db) != DB_OK) {
        password_wipe(hash, sizeof(hash));
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    /* 6. Chèn user vào bảng users */
    int64_t user_id = 0;
    UserRepoResult create_res = user_repo_create(db, canonical, hash, now_ms, &user_id);
    password_wipe(hash, sizeof(hash));

    if (create_res == USER_REPO_USERNAME_TAKEN) {
        db_rollback(db);
        activity_repo_log(db, now_ms, ACTIVITY_LEVEL_WARN, NULL, connection_id,
                          "auth.register", ACTIVITY_OUTCOME_FAILURE,
                          ACTIVITY_TARGET_USER, canonical, "USERNAME_TAKEN", NULL);
        return AUTH_SERVICE_USERNAME_TAKEN;
    }
    if (create_res != USER_REPO_OK) {
        db_rollback(db);
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    /* 7. Ghi audit log vào activity_logs */
    char user_id_str[32];
    snprintf(user_id_str, sizeof(user_id_str), "%lld", (long long)user_id);

    ActivityRepoResult log_res = activity_repo_log(
        db, now_ms, ACTIVITY_LEVEL_INFO, &user_id, connection_id,
        "auth.register", ACTIVITY_OUTCOME_SUCCESS,
        ACTIVITY_TARGET_USER, user_id_str, NULL, NULL);

    if (log_res != ACTIVITY_REPO_OK) {
        db_rollback(db);
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    /* 8. Commit transaction */
    if (db_commit(db) != DB_OK) {
        db_rollback(db);
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    /* 9. Trả về thông tin user */
    if (out_user != NULL) {
        memset(out_user, 0, sizeof(*out_user));
        out_user->user_id = user_id;
        snprintf(out_user->username, sizeof(out_user->username), "%s", canonical);
        out_user->created_at = now_ms;
        out_user->last_seen_at = -1;
        out_user->has_last_seen = false;
    }

    return AUTH_SERVICE_OK;
}

const char *auth_service_result_to_string(AuthServiceResult result)
{
    switch (result) {
    case AUTH_SERVICE_OK:
        return "Thao tác xác thực thành công";
    case AUTH_SERVICE_INVALID_ARGUMENT:
        return "Đối số dịch vụ xác thực không hợp lệ";
    case AUTH_SERVICE_VALIDATION_ERROR:
        return "Dữ liệu không đúng định dạng (username/password)";
    case AUTH_SERVICE_USERNAME_TAKEN:
        return "Tên người dùng đã tồn tại";
    case AUTH_SERVICE_STORAGE_ERROR:
        return "Lỗi cơ sở dữ liệu khi xử lý tài khoản";
    case AUTH_SERVICE_INTERNAL_ERROR:
        return "Lỗi máy chủ nội bộ trong dịch vụ xác thực";
    case AUTH_SERVICE_AUTH_FAILED:
        return "Tên người dùng hoặc mật khẩu không chính xác";
    case AUTH_SERVICE_ALREADY_ONLINE:
        return "Tài khoản hiện đang online trên một kết nối khác";
    case AUTH_SERVICE_ALREADY_AUTHENTICATED:
        return "Kết nối đã được xác thực trước đó";
    default:
        return "Lỗi xác thực không xác định";
    }
}

AuthServiceResult auth_service_login(Database *db,
                                     SessionManager *session_mgr,
                                     const char *username,
                                     const char *password,
                                     size_t password_len,
                                     const char *connection_id,
                                     uint64_t connection_generation,
                                     int64_t now_ms,
                                     UserRecord *out_user)
{
    if (db == NULL || db->handle == NULL || session_mgr == NULL ||
        username == NULL || password == NULL || connection_id == NULL ||
        connection_generation == 0 || now_ms < 0) {
        return AUTH_SERVICE_INVALID_ARGUMENT;
    }

    /* 1. Kiểm tra username */
    if (!user_repo_validate_username(username)) {
        return AUTH_SERVICE_VALIDATION_ERROR;
    }

    /* 2. Kiểm tra password */
    if (password_validate_input(password, password_len) != PASSWORD_OK) {
        return AUTH_SERVICE_VALIDATION_ERROR;
    }

    char canonical[CHAT_USERNAME_MAX_LEN + 1];
    if (!user_repo_canonicalize_username(username, canonical, sizeof(canonical))) {
        return AUTH_SERVICE_VALIDATION_ERROR;
    }

    /* 3. Kiểm tra xem connection này đã đăng nhập tài khoản nào chưa */
    if (session_manager_is_connection_authenticated(session_mgr, connection_id, NULL)) {
        return AUTH_SERVICE_ALREADY_AUTHENTICATED;
    }

    /* 4. Tìm kiếm tài khoản trong DB */
    UserRecord user = {0};
    UserRepoResult find_res = user_repo_find_by_username(db, canonical, &user);

    if (find_res == USER_REPO_NOT_FOUND) {
        /* Tài khoản không tồn tại: chạy verify với dummy hash để giữ thời gian xử lý đồng nhất (chống timing attack) */
        const char *dummy = password_get_dummy_hash();
        password_verify(dummy, password, password_len);

        activity_repo_log(db, now_ms, ACTIVITY_LEVEL_WARN, NULL, connection_id,
                          "auth.login", ACTIVITY_OUTCOME_FAILURE,
                          ACTIVITY_TARGET_USER, canonical, "AUTH_FAILED", NULL);
        return AUTH_SERVICE_AUTH_FAILED;
    }

    if (find_res != USER_REPO_OK) {
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    /* 5. Xác thực mật khẩu với chuỗi hash trong DB */
    PasswordResult verify_res = password_verify(user.password_hash, password, password_len);
    if (verify_res != PASSWORD_OK) {
        char user_id_str[32];
        snprintf(user_id_str, sizeof(user_id_str), "%lld", (long long)user.user_id);
        activity_repo_log(db, now_ms, ACTIVITY_LEVEL_WARN, &user.user_id, connection_id,
                          "auth.login", ACTIVITY_OUTCOME_FAILURE,
                          ACTIVITY_TARGET_USER, user_id_str, "AUTH_FAILED", NULL);
        return AUTH_SERVICE_AUTH_FAILED;
    }

    /* 6. Mật khẩu đúng: kiểm tra tài khoản đã online trên kết nối khác chưa (chống dual-login) */
    if (session_manager_is_user_online(session_mgr, user.user_id)) {
        char user_id_str[32];
        snprintf(user_id_str, sizeof(user_id_str), "%lld", (long long)user.user_id);
        activity_repo_log(db, now_ms, ACTIVITY_LEVEL_WARN, &user.user_id, connection_id,
                          "auth.login", ACTIVITY_OUTCOME_FAILURE,
                          ACTIVITY_TARGET_USER, user_id_str, "ALREADY_ONLINE", NULL);
        return AUTH_SERVICE_ALREADY_ONLINE;
    }

    /* 7. Mở transaction: cập nhật last_seen_at và ghi audit log thành công */
    if (db_begin_immediate(db) != DB_OK) {
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    if (user_repo_update_last_seen(db, user.user_id, now_ms) != USER_REPO_OK) {
        db_rollback(db);
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    char user_id_str[32];
    snprintf(user_id_str, sizeof(user_id_str), "%lld", (long long)user.user_id);
    ActivityRepoResult log_res = activity_repo_log(
        db, now_ms, ACTIVITY_LEVEL_INFO, &user.user_id, connection_id,
        "auth.login", ACTIVITY_OUTCOME_SUCCESS,
        ACTIVITY_TARGET_USER, user_id_str, NULL, NULL);

    if (log_res != ACTIVITY_REPO_OK) {
        db_rollback(db);
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    if (db_commit(db) != DB_OK) {
        db_rollback(db);
        return AUTH_SERVICE_STORAGE_ERROR;
    }

    /* 8. Gắn session vào SessionManager trong RAM */
    SessionResult sess_res = session_manager_bind(session_mgr, user.user_id,
                                                  connection_id, connection_generation, now_ms);
    if (sess_res != SESSION_OK) {
        return AUTH_SERVICE_INTERNAL_ERROR;
    }

    /* 9. Trả về thông tin user đã đăng nhập */
    if (out_user != NULL) {
        *out_user = user;
        out_user->last_seen_at = now_ms;
        out_user->has_last_seen = true;
    }

    return AUTH_SERVICE_OK;
}

AuthServiceResult auth_service_logout(Database *db,
                                      SessionManager *session_mgr,
                                      const char *connection_id,
                                      uint64_t connection_generation,
                                      int64_t now_ms,
                                      int64_t *out_user_id)
{
    if (session_mgr == NULL || connection_id == NULL) {
        return AUTH_SERVICE_INVALID_ARGUMENT;
    }

    SessionEntry entry = {0};
    if (session_manager_find_by_connection(session_mgr, connection_id, &entry) != SESSION_OK) {
        return AUTH_SERVICE_AUTH_FAILED;
    }

    int64_t uid = entry.user_id;
    if (out_user_id != NULL) {
        *out_user_id = uid;
    }

    /* Ghi audit log nếu DB có sẵn */
    if (db != NULL) {
        char user_id_str[32];
        snprintf(user_id_str, sizeof(user_id_str), "%lld", (long long)uid);
        activity_repo_log(db, now_ms, ACTIVITY_LEVEL_INFO, &uid, connection_id,
                          "auth.logout", ACTIVITY_OUTCOME_SUCCESS,
                          ACTIVITY_TARGET_USER, user_id_str, NULL, NULL);
    }

    /* Hủy session trong SessionManager */
    int64_t dummy_uid = 0;
    SessionResult unbind_res = session_manager_unbind_by_connection(
        session_mgr, connection_id, connection_generation, &dummy_uid);
    if (unbind_res != SESSION_OK && unbind_res != SESSION_NOT_FOUND) {
        return AUTH_SERVICE_INTERNAL_ERROR;
    }

    return AUTH_SERVICE_OK;
}
