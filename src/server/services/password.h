#ifndef TCP_CHAT_PASSWORD_H
#define TCP_CHAT_PASSWORD_H

#include <stdbool.h>
#include <stddef.h>
#include "shared/limits.h"

typedef enum {
    PASSWORD_OK = 0,
    PASSWORD_INVALID_ARGUMENT = 1,
    PASSWORD_TOO_SHORT = 2,
    PASSWORD_TOO_LONG = 3,
    PASSWORD_CONTAINS_NUL = 4,
    PASSWORD_VERIFY_FAILED = 5,
    PASSWORD_INTERNAL_ERROR = 6
} PasswordResult;

/**
 * Khởi tạo thư viện libsodium (an toàn khi gọi nhiều lần).
 * Trả về true nếu thành công, false nếu khởi tạo thất bại.
 */
bool password_init(void);

/**
 * Kiểm tra mật khẩu thô theo quy tắc nghiệp vụ:
 * - Không rỗng / NULL.
 * - Độ dài từ CHAT_PASSWORD_MIN_BYTES (8) đến CHAT_PASSWORD_MAX_BYTES (128).
 * - Không chứa ký tự NUL ('\0') bên trong phạm vi length.
 */
PasswordResult password_validate_input(const char *password, size_t length);

/**
 * Băm mật khẩu bằng thuật toán Argon2id (libsodium crypto_pwhash_str).
 * Chuỗi hash sinh ra có dạng "$argon2id$v=19$m=65536,t=2,p=1$..." kết thúc bằng '\0'.
 *
 * hash_out: buffer đầu ra, dung lượng tối thiểu CHAT_PASSWORD_HASH_BYTES (128 byte).
 */
PasswordResult password_hash(const char *password, size_t length, char *hash_out, size_t hash_out_size);

/**
 * Xác thực mật khẩu plaintext đối chiếu với chuỗi hash đã lưu trong database.
 * Trả về PASSWORD_OK nếu khớp, PASSWORD_VERIFY_FAILED nếu sai mật khẩu hoặc hash hỏng.
 */
PasswordResult password_verify(const char *hash, const char *password, size_t length);

/**
 * Xóa an toàn vùng nhớ nhạy cảm bằng sodium_memzero (chống bị trình biên dịch tối ưu hóa bỏ qua).
 */
void password_wipe(void *buffer, size_t length);

/**
 * Trả về chuỗi hash Argon2id hợp lệ cố định dùng để verify khi user không tồn tại,
 * nhằm loại bỏ khác biệt thời gian xử lý (chống user enumeration qua timing attack).
 */
const char *password_get_dummy_hash(void);

/**
 * Chuyển đổi mã kết quả PasswordResult thành chuỗi mô tả lỗi.
 */
const char *password_result_to_string(PasswordResult result);

#endif /* TCP_CHAT_PASSWORD_H */
