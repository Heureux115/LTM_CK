#include "server/core/session.h"

#include <stdio.h>
#include <string.h>

void session_manager_init(SessionManager *mgr)
{
    if (mgr == NULL) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
}

SessionResult session_manager_bind(SessionManager *mgr, int64_t user_id,
                                   const char *connection_id, uint64_t generation,
                                   int64_t authenticated_at_ms)
{
    if (mgr == NULL || user_id <= 0 || connection_id == NULL ||
        connection_id[0] == '\0' || generation == 0) {
        return SESSION_INVALID_ARGUMENT;
    }

    /* 1. Kiểm tra tài khoản đã online trên kết nối khác chưa (chống dual-login) */
    for (size_t i = 0; i < mgr->count; ++i) {
        if (mgr->entries[i].user_id == user_id) {
            return SESSION_ALREADY_ONLINE;
        }
    }

    /* 2. Kiểm tra kết nối này đã đăng nhập tài khoản nào chưa */
    for (size_t i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->entries[i].connection_id, connection_id) == 0) {
            return SESSION_ALREADY_AUTHENTICATED;
        }
    }

    /* 3. Kiểm tra trần số lượng session đồng thời */
    if (mgr->count >= CHAT_MAX_CONNECTIONS) {
        return SESSION_LIMIT_EXCEEDED;
    }

    /* 4. Thêm phiên mới vào danh sách */
    SessionEntry *entry = &mgr->entries[mgr->count];
    entry->user_id = user_id;
    snprintf(entry->connection_id, sizeof(entry->connection_id), "%s", connection_id);
    entry->generation = generation;
    entry->authenticated_at_ms = authenticated_at_ms;

    mgr->count++;
    return SESSION_OK;
}

SessionResult session_manager_unbind_by_connection(SessionManager *mgr,
                                                   const char *connection_id,
                                                   uint64_t generation,
                                                   int64_t *out_user_id)
{
    if (mgr == NULL || connection_id == NULL || connection_id[0] == '\0' || generation == 0) {
        return SESSION_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->entries[i].connection_id, connection_id) == 0) {
            /* Kiểm tra generation để chống race condition khi fd reuse */
            if (mgr->entries[i].generation != generation) {
                return SESSION_GENERATION_MISMATCH;
            }

            if (out_user_id != NULL) {
                *out_user_id = mgr->entries[i].user_id;
            }

            /* Hoán đổi với phần tử cuối để xóa trong O(1) */
            if (i < mgr->count - 1) {
                mgr->entries[i] = mgr->entries[mgr->count - 1];
            }
            memset(&mgr->entries[mgr->count - 1], 0, sizeof(SessionEntry));
            mgr->count--;
            return SESSION_OK;
        }
    }

    return SESSION_NOT_FOUND;
}

SessionResult session_manager_unbind_by_user(SessionManager *mgr, int64_t user_id,
                                             char *out_connection_id, size_t conn_id_size)
{
    if (mgr == NULL || user_id <= 0) {
        return SESSION_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < mgr->count; ++i) {
        if (mgr->entries[i].user_id == user_id) {
            if (out_connection_id != NULL && conn_id_size > 0) {
                snprintf(out_connection_id, conn_id_size, "%s", mgr->entries[i].connection_id);
            }

            if (i < mgr->count - 1) {
                mgr->entries[i] = mgr->entries[mgr->count - 1];
            }
            memset(&mgr->entries[mgr->count - 1], 0, sizeof(SessionEntry));
            mgr->count--;
            return SESSION_OK;
        }
    }

    return SESSION_NOT_FOUND;
}

SessionResult session_manager_find_by_user(const SessionManager *mgr, int64_t user_id,
                                           SessionEntry *out_entry)
{
    if (mgr == NULL || user_id <= 0 || out_entry == NULL) {
        return SESSION_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < mgr->count; ++i) {
        if (mgr->entries[i].user_id == user_id) {
            *out_entry = mgr->entries[i];
            return SESSION_OK;
        }
    }

    return SESSION_NOT_FOUND;
}

SessionResult session_manager_find_by_connection(const SessionManager *mgr,
                                                 const char *connection_id,
                                                 SessionEntry *out_entry)
{
    if (mgr == NULL || connection_id == NULL || connection_id[0] == '\0' || out_entry == NULL) {
        return SESSION_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->entries[i].connection_id, connection_id) == 0) {
            *out_entry = mgr->entries[i];
            return SESSION_OK;
        }
    }

    return SESSION_NOT_FOUND;
}

bool session_manager_is_user_online(const SessionManager *mgr, int64_t user_id)
{
    if (mgr == NULL || user_id <= 0) {
        return false;
    }

    for (size_t i = 0; i < mgr->count; ++i) {
        if (mgr->entries[i].user_id == user_id) {
            return true;
        }
    }

    return false;
}

bool session_manager_is_connection_authenticated(const SessionManager *mgr,
                                                 const char *connection_id,
                                                 int64_t *out_user_id)
{
    if (mgr == NULL || connection_id == NULL || connection_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < mgr->count; ++i) {
        if (strcmp(mgr->entries[i].connection_id, connection_id) == 0) {
            if (out_user_id != NULL) {
                *out_user_id = mgr->entries[i].user_id;
            }
            return true;
        }
    }

    return false;
}

size_t session_manager_count(const SessionManager *mgr)
{
    if (mgr == NULL) {
        return 0;
    }
    return mgr->count;
}

void session_manager_clear(SessionManager *mgr)
{
    if (mgr == NULL) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
}

const char *session_result_to_string(SessionResult result)
{
    switch (result) {
    case SESSION_OK:
        return "Thành công.";
    case SESSION_INVALID_ARGUMENT:
        return "Tham số phiên không hợp lệ.";
    case SESSION_ALREADY_ONLINE:
        return "Tài khoản hiện đang online trên một kết nối khác.";
    case SESSION_ALREADY_AUTHENTICATED:
        return "Kết nối đã được xác thực trước đó.";
    case SESSION_NOT_FOUND:
        return "Không tìm thấy phiên.";
    case SESSION_GENERATION_MISMATCH:
        return "Thế hệ kết nối không khớp (phát hiện tái sử dụng fd).";
    case SESSION_LIMIT_EXCEEDED:
        return "Đã đạt giới hạn số lượng phiên tối đa.";
    }
    return "Lỗi phiên không xác định.";
}
