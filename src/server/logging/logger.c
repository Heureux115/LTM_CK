#define _POSIX_C_SOURCE 200809L

#include "server/logging/logger.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static Logger g_default_logger = {0};
static bool g_default_logger_initialized = false;

static void ensure_parent_dir_exists(const char *filepath)
{
    char tmp[512];
    strncpy(tmp, filepath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *slash = strrchr(tmp, '/');
    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        mkdir(tmp, 0755);
    }
}

static size_t get_file_size(const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) == 0) {
        return (size_t)st.st_size;
    }
    return 0;
}

bool logger_init(Logger *logger, const char *filepath, size_t max_bytes, size_t max_backup_files)
{
    if (logger == NULL) {
        return false;
    }
    memset(logger, 0, sizeof(*logger));

    if (filepath == NULL || filepath[0] == '\0') {
        logger->use_stderr_fallback = true;
        return true;
    }

    strncpy(logger->filepath, filepath, sizeof(logger->filepath) - 1);
    logger->max_bytes = max_bytes > 0 ? max_bytes : CHAT_LOG_MAX_BYTES;
    logger->max_backup_files = max_backup_files > 0 ? max_backup_files : CHAT_LOG_MAX_BACKUP_FILES;

    ensure_parent_dir_exists(logger->filepath);

    logger->fp = fopen(logger->filepath, "a");
    if (logger->fp == NULL) {
        logger->use_stderr_fallback = true;
        return false;
    }

    logger->current_size = get_file_size(logger->filepath);
    return true;
}

void logger_close(Logger *logger)
{
    if (logger == NULL) {
        return;
    }
    if (logger->fp != NULL) {
        fflush(logger->fp);
        fclose(logger->fp);
        logger->fp = NULL;
    }
    logger->current_size = 0;
    logger->use_stderr_fallback = false;
}

bool logger_rotate(Logger *logger)
{
    if (logger == NULL || logger->filepath[0] == '\0') {
        return false;
    }

    if (logger->fp != NULL) {
        fflush(logger->fp);
        fclose(logger->fp);
        logger->fp = NULL;
    }

    /* Xoay từ file có index cao nhất xuống */
    for (size_t i = logger->max_backup_files; i >= 1; --i) {
        char old_path[560];
        char new_path[560];

        if (i == 1) {
            snprintf(old_path, sizeof(old_path), "%s", logger->filepath);
            snprintf(new_path, sizeof(new_path), "%s.1", logger->filepath);
        } else {
            snprintf(old_path, sizeof(old_path), "%s.%zu", logger->filepath, i - 1);
            snprintf(new_path, sizeof(new_path), "%s.%zu", logger->filepath, i);
        }

        /* Nếu đích là max_backup_files, xóa file cũ trước khi rename */
        if (i == logger->max_backup_files) {
            unlink(new_path);
        }

        if (access(old_path, F_OK) == 0) {
            rename(old_path, new_path);
        }
    }

    /* Mở lại file log chính ở trạng thái mới */
    logger->fp = fopen(logger->filepath, "w+");
    if (logger->fp == NULL) {
        logger->use_stderr_fallback = true;
        logger->current_size = 0;
        return false;
    }

    logger->current_size = 0;
    logger->use_stderr_fallback = false;
    return true;
}

static const char *level_to_string(LogLevel level)
{
    switch (level) {
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_WARN:
        return "WARN";
    case LOG_LEVEL_ERROR:
        return "ERROR";
    }
    return "INFO";
}

void logger_log(Logger *logger,
                LogLevel level,
                int64_t occurred_at_ms,
                const int64_t *actor_user_id,
                const char *connection_id,
                const char *action,
                const char *outcome,
                const char *target_type,
                const char *target_id,
                const char *error_code)
{
    /* Fallback an toàn nếu logger NULL */
    FILE *out = stderr;
    bool writing_to_file = false;

    if (logger != NULL && !logger->use_stderr_fallback && logger->fp != NULL) {
        /* Kiểm tra xoay file khi dung lượng vượt ngưỡng max_bytes */
        if (logger->current_size >= logger->max_bytes) {
            logger_rotate(logger);
        }
        if (logger->fp != NULL) {
            out = logger->fp;
            writing_to_file = true;
        }
    }

    /* Tính toán thời gian ISO-8601 */
    time_t sec;
    int msec = 0;
    if (occurred_at_ms > 0) {
        sec = (time_t)(occurred_at_ms / 1000);
        msec = (int)(occurred_at_ms % 1000);
    } else {
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            sec = ts.tv_sec;
            msec = (int)(ts.tv_nsec / 1000000);
        } else {
            sec = time(NULL);
            msec = 0;
        }
    }

    struct tm tm_buf;
    gmtime_r(&sec, &tm_buf);
    char time_str[64];
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, msec);

    /* Chuẩn bị các trường metadata theo Allowlist */
    char actor_buf[32] = "none";
    if (actor_user_id != NULL) {
        snprintf(actor_buf, sizeof(actor_buf), "%lld", (long long)*actor_user_id);
    }

    const char *conn_str = (connection_id != NULL && connection_id[0] != '\0') ? connection_id : "none";
    const char *act_str = (action != NULL && action[0] != '\0') ? action : "unknown";
    const char *out_str = (outcome != NULL && outcome[0] != '\0') ? outcome : "SUCCESS";
    const char *tgt_type = (target_type != NULL && target_type[0] != '\0') ? target_type : "none";
    const char *tgt_id = (target_id != NULL && target_id[0] != '\0') ? target_id : "none";
    const char *err_code = (error_code != NULL && error_code[0] != '\0') ? error_code : "none";

    int written = fprintf(out,
                          "[%s] level=%s action=%s outcome=%s actor_user_id=%s conn_id=%s target_type=%s target_id=%s error_code=%s\n",
                          time_str,
                          level_to_string(level),
                          act_str,
                          out_str,
                          actor_buf,
                          conn_str,
                          tgt_type,
                          tgt_id,
                          err_code);

    if (written > 0) {
        fflush(out);
        if (writing_to_file && logger != NULL) {
            logger->current_size += (size_t)written;
        }
    } else if (writing_to_file && logger != NULL) {
        /* I/O error trên file: fallback an toàn ra stderr, không abort và không gọi lại DB */
        logger->use_stderr_fallback = true;
        fprintf(stderr,
                "[%s] level=%s action=%s outcome=%s actor_user_id=%s conn_id=%s target_type=%s target_id=%s error_code=%s (file write error fallback)\n",
                time_str,
                level_to_string(level),
                act_str,
                out_str,
                actor_buf,
                conn_str,
                tgt_type,
                tgt_id,
                err_code);
    }
}

bool logger_global_init(const char *filepath)
{
    if (g_default_logger_initialized) {
        logger_global_close();
    }
    bool ok = logger_init(&g_default_logger, filepath, CHAT_LOG_MAX_BYTES, CHAT_LOG_MAX_BACKUP_FILES);
    g_default_logger_initialized = true;
    return ok;
}

void logger_global_close(void)
{
    if (g_default_logger_initialized) {
        logger_close(&g_default_logger);
        g_default_logger_initialized = false;
    }
}

Logger *logger_global_get(void)
{
    if (!g_default_logger_initialized) {
        return NULL;
    }
    return &g_default_logger;
}

void log_info(const char *action, const char *outcome, const int64_t *actor, const char *conn_id, const char *target_type, const char *target_id)
{
    logger_log(logger_global_get(), LOG_LEVEL_INFO, 0, actor, conn_id, action, outcome, target_type, target_id, NULL);
}

void log_warn(const char *action, const char *outcome, const int64_t *actor, const char *conn_id, const char *target_type, const char *target_id, const char *error_code)
{
    logger_log(logger_global_get(), LOG_LEVEL_WARN, 0, actor, conn_id, action, outcome, target_type, target_id, error_code);
}

void log_error(const char *action, const char *outcome, const int64_t *actor, const char *conn_id, const char *target_type, const char *target_id, const char *error_code)
{
    logger_log(logger_global_get(), LOG_LEVEL_ERROR, 0, actor, conn_id, action, outcome, target_type, target_id, error_code);
}
