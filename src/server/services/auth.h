#ifndef TCP_CHAT_AUTH_SERVICE_H
#define TCP_CHAT_AUTH_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "server/core/session.h"
#include "server/db/db.h"
#include "server/repos/user_repo.h"

typedef enum {
    AUTH_SERVICE_OK = 0,
    AUTH_SERVICE_INVALID_ARGUMENT,
    AUTH_SERVICE_VALIDATION_ERROR,
    AUTH_SERVICE_USERNAME_TAKEN,
    AUTH_SERVICE_STORAGE_ERROR,
    AUTH_SERVICE_INTERNAL_ERROR,
    AUTH_SERVICE_AUTH_FAILED,
    AUTH_SERVICE_ALREADY_ONLINE,
    AUTH_SERVICE_ALREADY_AUTHENTICATED
} AuthServiceResult;

/**
 * Đăng ký tài khoản người dùng mới theo Workflow W02:
 * 1. Validate username (3-32 ký tự, [a-zA-Z0-9_]) và password (8-128 byte UTF-8, không NUL).
 * 2. Pre-check username đã tồn tại chưa (để tránh băm Argon2id tốn CPU nếu trùng).
 * 3. Băm mật khẩu bằng Argon2id qua libsodium ngoài transaction DB.
 * 4. Bắt đầu transaction BEGIN IMMEDIATE.
 * 5. Tạo bản ghi trong bảng users qua user_repo_create (UNIQUE username là chốt chặn cuối).
 * 6. Ghi audit log vào bảng activity_logs (action="auth.register", outcome="success").
 * 7. Commit transaction.
 * 8. Điền kết quả vào out_user (user_id, canonical lowercase username).
 * 9. Xóa an toàn bộ nhớ nhạy cảm chứa password/hash.
 */
AuthServiceResult auth_service_register(Database *db,
                                        const char *username,
                                        const char *password,
                                        size_t password_len,
                                        const char *connection_id,
                                        int64_t now_ms,
                                        UserRecord *out_user);

/**
 * Đăng nhập tài khoản người dùng theo Workflow W03:
 * 1. Validate username và password cơ bản.
 * 2. Tra cứu user trong database; nếu không thấy, chạy dummy hash verify để chống timing attack.
 * 3. Nếu mật khẩu sai, ghi audit log failure (WARN) và trả AUTH_SERVICE_AUTH_FAILED.
 * 4. Nếu mật khẩu đúng, kiểm tra SessionManager: nếu user đã online -> ghi log WARN, trả AUTH_SERVICE_ALREADY_ONLINE.
 * 5. Mở transaction: cập nhật last_seen_at trong bảng users, ghi audit log success (INFO), commit.
 * 6. Gắn session vào SessionManager (user_id <-> connection_id với connection_generation).
 * 7. Điền thông tin vào out_user và xóa an toàn RAM.
 */
AuthServiceResult auth_service_login(Database *db,
                                     SessionManager *session_mgr,
                                     const char *username,
                                     const char *password,
                                     size_t password_len,
                                     const char *connection_id,
                                     uint64_t connection_generation,
                                     int64_t now_ms,
                                     UserRecord *out_user);

/**
 * Đăng xuất tài khoản người dùng theo Workflow W14:
 * 1. Tra cứu session hiện tại của connection_id trong SessionManager.
 * 2. Nếu tìm thấy session:
 *    a. Ghi audit log vào activity_logs: action="auth.logout", outcome=SUCCESS, level=INFO.
 *    b. Hủy session trong SessionManager bằng session_manager_unbind_by_connection.
 * 3. Điền out_user_id (nếu không NULL).
 * 4. Trả về AUTH_SERVICE_OK.
 */
AuthServiceResult auth_service_logout(Database *db,
                                      SessionManager *session_mgr,
                                      const char *connection_id,
                                      uint64_t connection_generation,
                                      int64_t now_ms,
                                      int64_t *out_user_id);

/**
 * Chuyển đổi mã lỗi AuthServiceResult thành chuỗi mô tả thân thiện tiếng Việt.
 */
const char *auth_service_result_to_string(AuthServiceResult result);

#endif /* TCP_CHAT_AUTH_SERVICE_H */
