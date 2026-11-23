#ifndef TCP_CHAT_DB_H
#define TCP_CHAT_DB_H

#include <stdbool.h>
#include <stdint.h>
#include <sqlite3.h>

#include "shared/limits.h"

typedef enum {
    DB_OK = 0,
    DB_INVALID_ARGUMENT,
    DB_ERROR_OPEN,
    DB_ERROR_PRAGMA,
    DB_ERROR_MIGRATION,
    DB_UNSUPPORTED_VERSION,
    DB_BUSY,
    DB_CONSTRAINT_VIOLATION,
    DB_IO_ERROR,
    DB_INTERNAL_ERROR
} DbResult;

typedef struct {
    sqlite3 *handle;
    char *path;
    int schema_version;
} Database;

/* Mở hoặc tạo mới database SQLite tại path, cấu hình PRAGMAs và chạy migrations. */
DbResult db_open(const char *path, Database *db);

/* Đóng database và giải phóng tài nguyên. */
void db_close(Database *db);

/* Bắt đầu transaction ghi tức thời: BEGIN IMMEDIATE */
DbResult db_begin_immediate(Database *db);

/* Commit transaction */
DbResult db_commit(Database *db);

/* Rollback transaction */
DbResult db_rollback(Database *db);

/* Kiểm tra integrity và foreign key; trả true nếu database hoàn toàn hợp lệ. */
bool db_check_integrity(Database *db);

/* Tên mô tả lỗi DbResult */
const char *db_result_name(DbResult result);

#endif
