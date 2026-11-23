#ifndef TCP_CHAT_SESSION_H
#define TCP_CHAT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/limits.h"

/* Kết quả trả về của các thao tác quản lý phiên */
typedef enum {
    SESSION_OK = 0,
    SESSION_INVALID_ARGUMENT,
    SESSION_ALREADY_ONLINE,          /* User này đang online trên một kết nối khác */
    SESSION_ALREADY_AUTHENTICATED,   /* Kết nối này đã được gán cho một user */
    SESSION_NOT_FOUND,               /* Không tìm thấy session */
    SESSION_GENERATION_MISMATCH,     /* Generation của kết nối không khớp (chống fd reuse) */
    SESSION_LIMIT_EXCEEDED           /* Đạt giới hạn số lượng session tối đa của server */
} SessionResult;

/* Thông tin chi tiết của một phiên hoạt động trong RAM */
typedef struct {
    int64_t user_id;
    char connection_id[64];
    uint64_t generation;
    int64_t authenticated_at_ms;
} SessionEntry;

/* Bảng quản lý phiên ánh xạ hai chiều trong RAM */
typedef struct {
    SessionEntry entries[CHAT_MAX_CONNECTIONS];
    size_t count;
} SessionManager;

/* Khởi tạo SessionManager ở trạng thái rỗng */
void session_manager_init(SessionManager *mgr);

/* Gắn một user vào connection (yêu cầu user chưa online và connection chưa có session) */
SessionResult session_manager_bind(SessionManager *mgr, int64_t user_id,
                                   const char *connection_id, uint64_t generation,
                                   int64_t authenticated_at_ms);

/* Hủy session theo connection_id và generation (bảo vệ chống fd reuse) */
SessionResult session_manager_unbind_by_connection(SessionManager *mgr,
                                                   const char *connection_id,
                                                   uint64_t generation,
                                                   int64_t *out_user_id);

/* Hủy session theo user_id */
SessionResult session_manager_unbind_by_user(SessionManager *mgr, int64_t user_id,
                                             char *out_connection_id, size_t conn_id_size);

/* Tra cứu session theo user_id */
SessionResult session_manager_find_by_user(const SessionManager *mgr, int64_t user_id,
                                           SessionEntry *out_entry);

/* Tra cứu session theo connection_id */
SessionResult session_manager_find_by_connection(const SessionManager *mgr,
                                                 const char *connection_id,
                                                 SessionEntry *out_entry);

/* Kiểm tra xem user có đang online không */
bool session_manager_is_user_online(const SessionManager *mgr, int64_t user_id);

/* Kiểm tra xem connection đã được xác thực chưa; nếu có trả về user_id */
bool session_manager_is_connection_authenticated(const SessionManager *mgr,
                                                 const char *connection_id,
                                                 int64_t *out_user_id);

/* Lấy tổng số lượng session đang hoạt động */
size_t session_manager_count(const SessionManager *mgr);

/* Xóa toàn bộ session (dùng khi shutdown hoặc test) */
void session_manager_clear(SessionManager *mgr);

/* Chuyển đổi mã SessionResult sang chuỗi thông báo lỗi */
const char *session_result_to_string(SessionResult result);

#endif
