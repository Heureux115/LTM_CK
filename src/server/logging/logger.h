#ifndef TCP_CHAT_LOGGER_H
#define TCP_CHAT_LOGGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "shared/limits.h"

typedef enum {
    LOG_LEVEL_INFO = 0,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

typedef struct {
    char filepath[512];
    FILE *fp;
    size_t current_size;
    size_t max_bytes;
    size_t max_backup_files;
    bool use_stderr_fallback;
} Logger;

/**
 * Khởi tạo logger ghi file có giới hạn dung lượng và xoay file (log rotation).
 * Nếu không mở được file, tự động fallback ra stderr.
 */
bool logger_init(Logger *logger, const char *filepath, size_t max_bytes, size_t max_backup_files);

/**
 * Đóng logger và xả buffer.
 */
void logger_close(Logger *logger);

/**
 * Ghi một bản ghi nhật ký hoạt động / sự kiện hệ thống theo chuẩn Allowlist Metadata.
 * TUYỆT ĐỐI KHÔNG GHI: password, session secrets, hoặc nội dung tin nhắn chat (body).
 *
 * - occurred_at_ms: Unix epoch tính bằng mili-giây (<= 0 sẽ tự lấy thời gian hiện tại).
 * - level: LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_ERROR.
 * - actor_user_id: con trỏ int64_t tới user thực hiện (NULL nếu chưa auth hoặc hệ thống).
 * - connection_id: định danh kết nối TCP (ví dụ "conn-5", có thể NULL).
 * - action: tên hành động (ví dụ "auth.login", "transport.accept", "group.create").
 * - outcome: "SUCCESS", "FAILURE", hoặc chuỗi kết quả tương ứng.
 * - target_type: loại đối tượng chịu tác động (ví dụ "user", "group", "message", "connection", có thể NULL).
 * - target_id: ID đối tượng (ví dụ "42", "grp-1", có thể NULL).
 * - error_code: mã lỗi nếu outcome=FAILURE (ví dụ "UNAUTHORIZED", "FORBIDDEN", có thể NULL).
 */
void logger_log(Logger *logger,
                LogLevel level,
                int64_t occurred_at_ms,
                const int64_t *actor_user_id,
                const char *connection_id,
                const char *action,
                const char *outcome,
                const char *target_type,
                const char *target_id,
                const char *error_code);

/**
 * Thực hiện xoay file log thủ công hoặc khi dung lượng vượt quá max_bytes.
 * Thuật toán:
 *   filepath.3 bị xóa nếu tồn tại.
 *   filepath.2 -> filepath.3
 *   filepath.1 -> filepath.2
 *   filepath   -> filepath.1
 *   Mở mới file filepath rỗng với size = 0.
 */
bool logger_rotate(Logger *logger);

/**
 * Quản lý Logger Singleton toàn cục cho Server.
 */
bool logger_global_init(const char *filepath);
void logger_global_close(void);
Logger *logger_global_get(void);

/**
 * Tiện ích ghi log nhanh ra Logger Singleton toàn cục.
 */
void log_info(const char *action, const char *outcome, const int64_t *actor, const char *conn_id, const char *target_type, const char *target_id);
void log_warn(const char *action, const char *outcome, const int64_t *actor, const char *conn_id, const char *target_type, const char *target_id, const char *error_code);
void log_error(const char *action, const char *outcome, const int64_t *actor, const char *conn_id, const char *target_type, const char *target_id, const char *error_code);

#endif /* TCP_CHAT_LOGGER_H */
