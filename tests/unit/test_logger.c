#define _POSIX_C_SOURCE 200809L

#include "server/logging/logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void cleanup_test_files(const char *prefix)
{
    char path[512];
    snprintf(path, sizeof(path), "%s", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s.1", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s.2", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s.3", prefix);
    unlink(path);
    snprintf(path, sizeof(path), "%s.4", prefix);
    unlink(path);
}

static void test_logger_init_and_close(void)
{
    const char *test_path = "/tmp/test_chat_logger_init.log";
    cleanup_test_files(test_path);

    Logger logger;
    bool ok = logger_init(&logger, test_path, 1024, 3);
    assert(ok);
    assert(logger.fp != NULL);
    assert(!logger.use_stderr_fallback);
    assert(logger.current_size == 0);
    assert(logger.max_bytes == 1024);
    assert(logger.max_backup_files == 3);

    logger_close(&logger);
    assert(logger.fp == NULL);

    cleanup_test_files(test_path);
    puts("1. test_logger_init_and_close... PASS");
}

static void test_logger_format_allowlist(void)
{
    const char *test_path = "/tmp/test_chat_logger_format.log";
    cleanup_test_files(test_path);

    Logger logger;
    assert(logger_init(&logger, test_path, 10240, 3));

    int64_t actor = 1001;
    logger_log(&logger, LOG_LEVEL_INFO, 1793712600123LL, &actor, "conn-42", "auth.login", "SUCCESS", "user", "1001", NULL);
    logger_log(&logger, LOG_LEVEL_WARN, 1793712601000LL, &actor, "conn-42", "friend.request.send", "FAILURE", "user", "2002", "ALREADY_FRIENDS");
    logger_close(&logger);

    FILE *f = fopen(test_path, "r");
    assert(f != NULL);
    char line1[512] = {0};
    char line2[512] = {0};
    assert(fgets(line1, sizeof(line1), f) != NULL);
    assert(fgets(line2, sizeof(line2), f) != NULL);
    fclose(f);

    /* Kiểm tra trường dòng 1 */
    assert(strstr(line1, "level=INFO") != NULL);
    assert(strstr(line1, "action=auth.login") != NULL);
    assert(strstr(line1, "outcome=SUCCESS") != NULL);
    assert(strstr(line1, "actor_user_id=1001") != NULL);
    assert(strstr(line1, "conn_id=conn-42") != NULL);
    assert(strstr(line1, "target_type=user") != NULL);
    assert(strstr(line1, "target_id=1001") != NULL);
    assert(strstr(line1, "error_code=none") != NULL);

    /* Kiểm tra trường dòng 2 */
    assert(strstr(line2, "level=WARN") != NULL);
    assert(strstr(line2, "action=friend.request.send") != NULL);
    assert(strstr(line2, "outcome=FAILURE") != NULL);
    assert(strstr(line2, "target_id=2002") != NULL);
    assert(strstr(line2, "error_code=ALREADY_FRIENDS") != NULL);

    cleanup_test_files(test_path);
    puts("2. test_logger_format_allowlist... PASS");
}

static void test_logger_redact_secrets(void)
{
    const char *test_path = "/tmp/test_chat_logger_redact.log";
    cleanup_test_files(test_path);

    Logger logger;
    assert(logger_init(&logger, test_path, 10240, 3));

    int64_t actor = 999;
    /* Ghi các sự kiện nhạy cảm: register, login, message.send */
    logger_log(&logger, LOG_LEVEL_INFO, 0, &actor, "conn-1", "auth.register", "SUCCESS", "user", "999", NULL);
    logger_log(&logger, LOG_LEVEL_INFO, 0, &actor, "conn-1", "direct.send", "SUCCESS", "message", "msg-123", NULL);
    logger_log(&logger, LOG_LEVEL_WARN, 0, NULL, "conn-2", "auth.login", "FAILURE", "none", "none", "INVALID_CREDENTIALS");
    logger_close(&logger);

    FILE *f = fopen(test_path, "r");
    assert(f != NULL);
    char buf[2048] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    assert(n > 0);

    /* Đảm bảo tuyệt đối không lộ password, secrets, message body trong allowlist metadata */
    assert(strstr(buf, "password") == NULL);
    assert(strstr(buf, "secret_password_123") == NULL);
    assert(strstr(buf, "body=") == NULL);
    assert(strstr(buf, "Xin chao") == NULL);

    cleanup_test_files(test_path);
    puts("3. test_logger_redact_secrets... PASS");
}

static void test_logger_rotation_size_threshold(void)
{
    const char *test_path = "/tmp/test_chat_logger_rotation.log";
    cleanup_test_files(test_path);

    Logger logger;
    /* Giới hạn nhỏ 250 byte, tối đa 3 file backup */
    assert(logger_init(&logger, test_path, 250, 3));

    int64_t actor = 1;
    /* Mỗi log line xấp xỉ 100-120 byte. Sau 3 dòng sẽ vượt 250 byte -> kích hoạt rotate */
    for (int i = 0; i < 15; ++i) {
        char act[32];
        snprintf(act, sizeof(act), "action_%d", i);
        logger_log(&logger, LOG_LEVEL_INFO, 0, &actor, "conn-1", act, "SUCCESS", "item", "1", NULL);
    }
    logger_close(&logger);

    char path1[512], path2[512], path3[512], path4[512];
    snprintf(path1, sizeof(path1), "%s.1", test_path);
    snprintf(path2, sizeof(path2), "%s.2", test_path);
    snprintf(path3, sizeof(path3), "%s.3", test_path);
    snprintf(path4, sizeof(path4), "%s.4", test_path);

    /* File chính test_path và các backup .1, .2, .3 phải tồn tại */
    assert(access(test_path, F_OK) == 0);
    assert(access(path1, F_OK) == 0);
    assert(access(path2, F_OK) == 0);
    assert(access(path3, F_OK) == 0);
    /* .4 không được vượt quá max_backup_files=3 */
    assert(access(path4, F_OK) != 0);

    cleanup_test_files(test_path);
    puts("4. test_logger_rotation_size_threshold... PASS");
}

static void test_logger_stderr_fallback(void)
{
    /* Thử đường dẫn không hợp lệ hoặc không có quyền ghi */
    const char *bad_path = "/sys/kernel/test_forbidden_log.log";

    Logger logger;
    bool ok = logger_init(&logger, bad_path, 1024, 3);
    /* Không được crash hay abort, fallback bật cờ use_stderr_fallback */
    assert(!ok);
    assert(logger.use_stderr_fallback);
    assert(logger.fp == NULL);

    /* Ghi log khi fallback vẫn chạy an toàn, không segmentation fault */
    int64_t actor = 88;
    logger_log(&logger, LOG_LEVEL_ERROR, 0, &actor, "conn-bad", "system.fault", "FAILURE", "none", "none", "IO_ERROR");

    logger_close(&logger);
    puts("5. test_logger_stderr_fallback... PASS");
}

static void test_logger_global_singleton(void)
{
    const char *test_path = "/tmp/test_chat_logger_global.log";
    cleanup_test_files(test_path);

    assert(logger_global_init(test_path));
    assert(logger_global_get() != NULL);

    int64_t actor = 55;
    log_info("user.connect", "SUCCESS", &actor, "conn-9", "connection", "9");
    log_warn("auth.login", "FAILURE", &actor, "conn-9", "user", "55", "BAD_PASSWORD");
    log_error("socket.poll", "FAILURE", NULL, "none", "none", "none", "EADDRINUSE");

    logger_global_close();
    assert(logger_global_get() == NULL);

    FILE *f = fopen(test_path, "r");
    assert(f != NULL);
    char buf[2048] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    assert(strstr(buf, "action=user.connect") != NULL);
    assert(strstr(buf, "action=auth.login") != NULL);
    assert(strstr(buf, "action=socket.poll") != NULL);

    cleanup_test_files(test_path);
    puts("6. test_logger_global_singleton... PASS");
}

int main(void)
{
    puts("=== Chạy kiểm thử Module File Logger & Log Rotation (M8.1 / T53) ===");
    test_logger_init_and_close();
    test_logger_format_allowlist();
    test_logger_redact_secrets();
    test_logger_rotation_size_threshold();
    test_logger_stderr_fallback();
    test_logger_global_singleton();
    puts("=== Hoàn tất: 6/6 test cases của File Logger (M8.1) thành công! ===");
    return 0;
}
