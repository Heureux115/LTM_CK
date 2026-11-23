#ifndef TCP_CHAT_ACTIVITY_REPO_H
#define TCP_CHAT_ACTIVITY_REPO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "server/db/db.h"

typedef enum {
    ACTIVITY_REPO_OK = 0,
    ACTIVITY_REPO_INVALID_ARGUMENT = 1,
    ACTIVITY_REPO_NOT_FOUND = 2,
    ACTIVITY_REPO_DB_ERROR = 3
} ActivityRepoResult;

typedef enum {
    ACTIVITY_LEVEL_INFO = 0,
    ACTIVITY_LEVEL_WARN,
    ACTIVITY_LEVEL_ERROR
} ActivityLevel;

typedef enum {
    ACTIVITY_OUTCOME_SUCCESS = 0,
    ACTIVITY_OUTCOME_FAILURE
} ActivityOutcome;

typedef enum {
    ACTIVITY_TARGET_NONE = 0,
    ACTIVITY_TARGET_USER,
    ACTIVITY_TARGET_FRIEND_REQUEST,
    ACTIVITY_TARGET_GROUP,
    ACTIVITY_TARGET_MESSAGE,
    ACTIVITY_TARGET_CONNECTION
} ActivityTargetType;

typedef struct {
    int64_t log_id;
    int64_t occurred_at;
    ActivityLevel level;
    int64_t actor_user_id;
    bool has_actor;
    char connection_id[64];
    char action[64];
    ActivityOutcome outcome;
    ActivityTargetType target_type;
    char target_id[64];
    char error_code[64];
} ActivityRecord;

/**
 * Ghi một bản ghi nhật ký hoạt động / audit vào bảng activity_logs.
 * Sử dụng Prepared Statement và kiểm tra toàn vẹn kiểu dữ liệu.
 *
 * - occurred_at: Unix milliseconds (>= 0).
 * - level: ACTIVITY_LEVEL_INFO / WARN / ERROR.
 * - actor_user_id: con trỏ int64_t trỏ tới user_id (NULL nếu chưa xác thực).
 * - connection_id: ID kết nối tạm thời (có thể NULL).
 * - action: tên hành động (bắt buộc, không được NULL hoặc rỗng).
 * - outcome: ACTIVITY_OUTCOME_SUCCESS hoặc ACTIVITY_OUTCOME_FAILURE.
 * - target_type: kiểu đối tượng tác động (ACTIVITY_TARGET_NONE nếu không có).
 * - target_id: ID đối tượng tác động dạng chuỗi (có thể NULL).
 * - error_code: mã lỗi nghiệp vụ nếu failure (có thể NULL).
 * - out_log_id: con trỏ nhận log_id mới sinh (có thể NULL).
 */
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
                                     int64_t *out_log_id);

/**
 * Tìm kiếm bản ghi audit log theo log_id.
 */
ActivityRepoResult activity_repo_find_by_id(Database *db,
                                            int64_t log_id,
                                            ActivityRecord *out_record);

/**
 * Đếm tổng số bản ghi trong bảng activity_logs.
 */
ActivityRepoResult activity_repo_count(Database *db, int64_t *out_count);

/**
 * Đếm số bản ghi của một user cụ thể (dựa theo actor_user_id).
 */
ActivityRepoResult activity_repo_count_by_user(Database *db, int64_t user_id, int64_t *out_count);

/**
 * Helper chuyển đổi enum sang chuỗi và ngược lại.
 */
const char *activity_level_to_string(ActivityLevel level);
bool activity_level_from_string(const char *str, ActivityLevel *out_level);

const char *activity_outcome_to_string(ActivityOutcome outcome);
bool activity_outcome_from_string(const char *str, ActivityOutcome *out_outcome);

const char *activity_target_type_to_string(ActivityTargetType target_type);
bool activity_target_type_from_string(const char *str, ActivityTargetType *out_target_type);

const char *activity_repo_result_to_string(ActivityRepoResult result);

#endif /* TCP_CHAT_ACTIVITY_REPO_H */
