#ifndef TCP_CHAT_USER_REPO_H
#define TCP_CHAT_USER_REPO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "server/db/db.h"
#include "shared/limits.h"

typedef enum {
    USER_REPO_OK = 0,
    USER_REPO_INVALID_ARGUMENT = 1,
    USER_REPO_INVALID_USERNAME = 2,
    USER_REPO_USERNAME_TAKEN = 3,
    USER_REPO_NOT_FOUND = 4,
    USER_REPO_DB_ERROR = 5
} UserRepoResult;

typedef struct {
    int64_t user_id;
    char username[CHAT_USERNAME_MAX_LEN + 1];
    char password_hash[CHAT_PASSWORD_HASH_BYTES + 1];
    int64_t created_at;
    int64_t last_seen_at;
    bool has_last_seen;
} UserRecord;

/**
 * Kiểm tra username hợp lệ theo quy tắc:
 * - Không NULL
 * - Độ dài từ CHAT_USERNAME_MIN_LEN (3) đến CHAT_USERNAME_MAX_LEN (32)
 * - Chỉ gồm các ký tự ASCII [a-zA-Z0-9_]
 */
bool user_repo_validate_username(const char *username);

/**
 * Chuẩn hóa username: kiểm tra hợp lệ và chuyển các ký tự hoa về chữ thường (canonical lowercase).
 * Buffer out_canonical phải có kích thước tối thiểu CHAT_USERNAME_MAX_LEN + 1.
 */
bool user_repo_canonicalize_username(const char *input, char *out_canonical, size_t out_size);

/**
 * Tạo mới user với username và password_hash đã băm.
 * Username được tự động chuẩn hóa lowercase trước khi lưu.
 * Gán user_id mới sinh vào out_user_id (nếu không NULL).
 * Trả về USER_REPO_USERNAME_TAKEN nếu trùng tên đăng ký (ràng buộc UNIQUE).
 */
UserRepoResult user_repo_create(Database *db,
                                const char *raw_username,
                                const char *password_hash,
                                int64_t created_at,
                                int64_t *out_user_id);

/**
 * Tìm kiếm user theo username (tự động chuẩn hóa lowercase).
 */
UserRepoResult user_repo_find_by_username(Database *db,
                                          const char *raw_username,
                                          UserRecord *out_user);

/**
 * Tìm kiếm user theo user_id.
 */
UserRepoResult user_repo_find_by_id(Database *db,
                                    int64_t user_id,
                                    UserRecord *out_user);

/**
 * Cập nhật thời điểm hoạt động gần nhất (last_seen_at) cho user_id.
 * Trả về USER_REPO_NOT_FOUND nếu không tồn tại user_id này.
 */
UserRepoResult user_repo_update_last_seen(Database *db,
                                          int64_t user_id,
                                          int64_t last_seen_at);

/**
 * Đếm tổng số user trong bảng users.
 */
UserRepoResult user_repo_count(Database *db, int64_t *out_count);

/**
 * Chuyển mã kết quả UserRepoResult thành chuỗi mô tả thân thiện.
 */
const char *user_repo_result_to_string(UserRepoResult result);

#endif /* TCP_CHAT_USER_REPO_H */
