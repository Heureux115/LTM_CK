#include "server/repos/activity_repo.h"
#include <stdio.h>
#include <string.h>

const char *activity_level_to_string(ActivityLevel level) {
    switch (level) {
        case ACTIVITY_LEVEL_INFO:
            return "INFO";
        case ACTIVITY_LEVEL_WARN:
            return "WARN";
        case ACTIVITY_LEVEL_ERROR:
            return "ERROR";
        default:
            return "INFO";
    }
}

bool activity_level_from_string(const char *str, ActivityLevel *out_level) {
    if (str == NULL || out_level == NULL) {
        return false;
    }
    if (strcmp(str, "INFO") == 0) {
        *out_level = ACTIVITY_LEVEL_INFO;
        return true;
    }
    if (strcmp(str, "WARN") == 0) {
        *out_level = ACTIVITY_LEVEL_WARN;
        return true;
    }
    if (strcmp(str, "ERROR") == 0) {
        *out_level = ACTIVITY_LEVEL_ERROR;
        return true;
    }
    return false;
}

const char *activity_outcome_to_string(ActivityOutcome outcome) {
    switch (outcome) {
        case ACTIVITY_OUTCOME_SUCCESS:
            return "success";
        case ACTIVITY_OUTCOME_FAILURE:
            return "failure";
        default:
            return "failure";
    }
}

bool activity_outcome_from_string(const char *str, ActivityOutcome *out_outcome) {
    if (str == NULL || out_outcome == NULL) {
        return false;
    }
    if (strcmp(str, "success") == 0) {
        *out_outcome = ACTIVITY_OUTCOME_SUCCESS;
        return true;
    }
    if (strcmp(str, "failure") == 0) {
        *out_outcome = ACTIVITY_OUTCOME_FAILURE;
        return true;
    }
    return false;
}

const char *activity_target_type_to_string(ActivityTargetType target_type) {
    switch (target_type) {
        case ACTIVITY_TARGET_USER:
            return "user";
        case ACTIVITY_TARGET_FRIEND_REQUEST:
            return "friend_request";
        case ACTIVITY_TARGET_GROUP:
            return "group";
        case ACTIVITY_TARGET_MESSAGE:
            return "message";
        case ACTIVITY_TARGET_CONNECTION:
            return "connection";
        case ACTIVITY_TARGET_NONE:
        default:
            return NULL;
    }
}

bool activity_target_type_from_string(const char *str, ActivityTargetType *out_target_type) {
    if (out_target_type == NULL) {
        return false;
    }
    if (str == NULL || strlen(str) == 0) {
        *out_target_type = ACTIVITY_TARGET_NONE;
        return true;
    }
    if (strcmp(str, "user") == 0) {
        *out_target_type = ACTIVITY_TARGET_USER;
        return true;
    }
    if (strcmp(str, "friend_request") == 0) {
        *out_target_type = ACTIVITY_TARGET_FRIEND_REQUEST;
        return true;
    }
    if (strcmp(str, "group") == 0) {
        *out_target_type = ACTIVITY_TARGET_GROUP;
        return true;
    }
    if (strcmp(str, "message") == 0) {
        *out_target_type = ACTIVITY_TARGET_MESSAGE;
        return true;
    }
    if (strcmp(str, "connection") == 0) {
        *out_target_type = ACTIVITY_TARGET_CONNECTION;
        return true;
    }
    return false;
}

ActivityRepoResult activity_repo_log(Database *db,
                                     int64_t occurred_at,
                                     ActivityLevel level,
                                     const int64_t *actor_user_id,
                                     const char *connection_id,
                                     const char *action,
                                     ActivityOutcome outcome,
                                     ActivityTargetType target_type,
                                     const char *target_id,
                                     const char *error_code,
                                     int64_t *out_log_id) {
    if (db == NULL || db->handle == NULL || occurred_at < 0 || action == NULL) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }
    if (strlen(action) == 0) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }
    if (level < ACTIVITY_LEVEL_INFO || level > ACTIVITY_LEVEL_ERROR) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }
    if (outcome < ACTIVITY_OUTCOME_SUCCESS || outcome > ACTIVITY_OUTCOME_FAILURE) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }

    const char *sql =
        "INSERT INTO activity_logs ("
        "    occurred_at, level, actor_user_id, connection_id, action,"
        "    outcome, target_type, target_id, error_code"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return ACTIVITY_REPO_DB_ERROR;
    }

    sqlite3_bind_int64(stmt, 1, occurred_at);
    sqlite3_bind_text(stmt, 2, activity_level_to_string(level), -1, SQLITE_STATIC);

    if (actor_user_id != NULL) {
        sqlite3_bind_int64(stmt, 3, *actor_user_id);
    } else {
        sqlite3_bind_null(stmt, 3);
    }

    if (connection_id != NULL && strlen(connection_id) > 0) {
        sqlite3_bind_text(stmt, 4, connection_id, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, 4);
    }

    sqlite3_bind_text(stmt, 5, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, activity_outcome_to_string(outcome), -1, SQLITE_STATIC);

    const char *tt_str = activity_target_type_to_string(target_type);
    if (tt_str != NULL) {
        sqlite3_bind_text(stmt, 7, tt_str, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, 7);
    }

    if (target_id != NULL && strlen(target_id) > 0) {
        sqlite3_bind_text(stmt, 8, target_id, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, 8);
    }

    if (error_code != NULL && strlen(error_code) > 0) {
        sqlite3_bind_text(stmt, 9, error_code, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_null(stmt, 9);
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        if (out_log_id != NULL) {
            *out_log_id = sqlite3_last_insert_rowid(db->handle);
        }
        sqlite3_finalize(stmt);
        return ACTIVITY_REPO_OK;
    }

    sqlite3_finalize(stmt);
    return ACTIVITY_REPO_DB_ERROR;
}

static void populate_activity_record(sqlite3_stmt *stmt, ActivityRecord *out_record) {
    if (stmt == NULL || out_record == NULL) {
        return;
    }
    memset(out_record, 0, sizeof(*out_record));

    out_record->log_id = sqlite3_column_int64(stmt, 0);
    out_record->occurred_at = sqlite3_column_int64(stmt, 1);

    const char *level_str = (const char *)sqlite3_column_text(stmt, 2);
    activity_level_from_string(level_str, &out_record->level);

    if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) {
        out_record->has_actor = false;
        out_record->actor_user_id = -1;
    } else {
        out_record->has_actor = true;
        out_record->actor_user_id = sqlite3_column_int64(stmt, 3);
    }

    const unsigned char *conn_str = sqlite3_column_text(stmt, 4);
    if (conn_str != NULL) {
        snprintf(out_record->connection_id, sizeof(out_record->connection_id), "%s", (const char *)conn_str);
    }

    const unsigned char *action_str = sqlite3_column_text(stmt, 5);
    if (action_str != NULL) {
        snprintf(out_record->action, sizeof(out_record->action), "%s", (const char *)action_str);
    }

    const char *outcome_str = (const char *)sqlite3_column_text(stmt, 6);
    activity_outcome_from_string(outcome_str, &out_record->outcome);

    const char *target_type_str = (const char *)sqlite3_column_text(stmt, 7);
    activity_target_type_from_string(target_type_str, &out_record->target_type);

    const unsigned char *target_id_str = sqlite3_column_text(stmt, 8);
    if (target_id_str != NULL) {
        snprintf(out_record->target_id, sizeof(out_record->target_id), "%s", (const char *)target_id_str);
    }

    const unsigned char *error_code_str = sqlite3_column_text(stmt, 9);
    if (error_code_str != NULL) {
        snprintf(out_record->error_code, sizeof(out_record->error_code), "%s", (const char *)error_code_str);
    }
}

ActivityRepoResult activity_repo_find_by_id(Database *db,
                                            int64_t log_id,
                                            ActivityRecord *out_record) {
    if (db == NULL || db->handle == NULL || log_id <= 0 || out_record == NULL) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }

    const char *sql =
        "SELECT log_id, occurred_at, level, actor_user_id, connection_id,"
        "       action, outcome, target_type, target_id, error_code "
        "FROM activity_logs WHERE log_id = ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return ACTIVITY_REPO_DB_ERROR;
    }

    sqlite3_bind_int64(stmt, 1, log_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        populate_activity_record(stmt, out_record);
        sqlite3_finalize(stmt);
        return ACTIVITY_REPO_OK;
    } else if (rc == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return ACTIVITY_REPO_NOT_FOUND;
    }

    sqlite3_finalize(stmt);
    return ACTIVITY_REPO_DB_ERROR;
}

ActivityRepoResult activity_repo_count(Database *db, int64_t *out_count) {
    if (db == NULL || db->handle == NULL || out_count == NULL) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }

    const char *sql = "SELECT COUNT(*) FROM activity_logs;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return ACTIVITY_REPO_DB_ERROR;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return ACTIVITY_REPO_OK;
    }

    sqlite3_finalize(stmt);
    return ACTIVITY_REPO_DB_ERROR;
}

ActivityRepoResult activity_repo_count_by_user(Database *db, int64_t user_id, int64_t *out_count) {
    if (db == NULL || db->handle == NULL || user_id <= 0 || out_count == NULL) {
        return ACTIVITY_REPO_INVALID_ARGUMENT;
    }

    const char *sql = "SELECT COUNT(*) FROM activity_logs WHERE actor_user_id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return ACTIVITY_REPO_DB_ERROR;
    }

    sqlite3_bind_int64(stmt, 1, user_id);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return ACTIVITY_REPO_OK;
    }

    sqlite3_finalize(stmt);
    return ACTIVITY_REPO_DB_ERROR;
}

const char *activity_repo_result_to_string(ActivityRepoResult result) {
    switch (result) {
        case ACTIVITY_REPO_OK:
            return "Thao tác repository nhật ký hoạt động thành công";
        case ACTIVITY_REPO_INVALID_ARGUMENT:
            return "Đối số repository nhật ký hoạt động không hợp lệ";
        case ACTIVITY_REPO_NOT_FOUND:
            return "Không tìm thấy bản ghi nhật ký hoạt động";
        case ACTIVITY_REPO_DB_ERROR:
            return "Lỗi cơ sở dữ liệu khi thao tác nhật ký hoạt động";
        default:
            return "Lỗi repository nhật ký hoạt động không xác định";
    }
}
