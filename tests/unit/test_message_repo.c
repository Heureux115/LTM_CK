#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server/db/db.h"
#include "server/repos/user_repo.h"
#include "server/repos/friend_repo.h"
#include "server/repos/message_repo.h"
#include "shared/limits.h"

static Database g_db;
static int64_t g_alice_id = 0;
static int64_t g_bob_id = 0;
static int64_t g_carol_id = 0;

static void setup_test_db(void)
{
    int rc = db_open(":memory:", &g_db);
    assert(rc == DB_OK);

    /* Tạo 3 người dùng: Alice (1), Bob (2), Carol (3) */
    int u_rc = user_repo_create(&g_db, "alice", "$argon2id$fake_alice_hash", 1700000000, &g_alice_id);
    assert(u_rc == USER_REPO_OK);

    u_rc = user_repo_create(&g_db, "bob", "$argon2id$fake_bob_hash", 1700000001, &g_bob_id);
    assert(u_rc == USER_REPO_OK);

    u_rc = user_repo_create(&g_db, "carol", "$argon2id$fake_carol_hash", 1700000002, &g_carol_id);
    assert(u_rc == USER_REPO_OK);

    /* Thiết lập quan hệ bạn bè giữa Alice và Bob */
    FriendRepoResult fr = friend_repo_create_friendship(&g_db, g_alice_id, g_bob_id, 1700000010);
    assert(fr == FRIEND_REPO_OK);
}

static void teardown_test_db(void)
{
    db_close(&g_db);
}

static void test_message_repo_validation(void)
{
    /* 1. Kiểm tra validator client_msg_id */
    assert(!message_repo_validate_client_msg_id(NULL));
    assert(!message_repo_validate_client_msg_id(""));
    assert(!message_repo_validate_client_msg_id("0123456789abcdef0123456789abcde")); /* 31 chars */
    assert(!message_repo_validate_client_msg_id("0123456789abcdef0123456789abcdef0")); /* 33 chars */
    assert(!message_repo_validate_client_msg_id("0123456789ABCDEF0123456789abcdef")); /* Uppercase bị từ chối */
    assert(!message_repo_validate_client_msg_id("0123456789abcdef0123456789abcdeg")); /* 'g' không phải hex */
    assert(message_repo_validate_client_msg_id("0123456789abcdef0123456789abcdef")); /* 32 chars hex lowercase hợp lệ */

    /* 2. Kiểm tra validator body */
    size_t len = 0;
    assert(!message_repo_validate_body(NULL, &len));
    assert(!message_repo_validate_body("", &len)); /* Rỗng 0 byte */

    char too_large[4098];
    memset(too_large, 'a', sizeof(too_large) - 1);
    too_large[sizeof(too_large) - 1] = '\0';
    assert(!message_repo_validate_body(too_large, &len)); /* 4097 bytes vượt giới hạn 4096 */

    const char *vietnamese_text = "Chào bạn Bob, chúc bạn một ngày tốt lành! 🚀\n\tTab và xuống dòng.";
    assert(message_repo_validate_body(vietnamese_text, &len));
    assert(len == strlen(vietnamese_text));

    /* 3. Tự gửi tin nhắn cho chính mình */
    DirectMessageRecord msg = {0};
    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_alice_id,
                                                      "0123456789abcdef0123456789abcdef",
                                                      "Self send", 1700000100, &msg);
    assert(res == MESSAGE_REPO_ERR_SELF_SEND);

    /* 4. Tham số sai lệch */
    res = message_repo_insert_direct(NULL, g_alice_id, g_bob_id, "0123456789abcdef0123456789abcdef", "Hi", 100, &msg);
    assert(res == MESSAGE_REPO_ERR_INVALID_ARGUMENT);

    res = message_repo_insert_direct(&g_db, -1, g_bob_id, "0123456789abcdef0123456789abcdef", "Hi", 100, &msg);
    assert(res == MESSAGE_REPO_ERR_INVALID_ARGUMENT);

    res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id, "invalid_client_id", "Hi", 100, &msg);
    assert(res == MESSAGE_REPO_ERR_INVALID_ARGUMENT);
}

static void test_message_repo_insert_non_friend_forbidden(void)
{
    /* Alice gửi tin cho Carol (chưa là bạn bè) -> Bị từ chối FORBIDDEN */
    DirectMessageRecord msg = {0};
    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_carol_id,
                                                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                                      "Xin chào Carol!", 1700000200, &msg);
    assert(res == MESSAGE_REPO_ERR_FORBIDDEN);

    /* Carol gửi tin cho Alice -> Bị từ chối FORBIDDEN */
    res = message_repo_insert_direct(&g_db, g_carol_id, g_alice_id,
                                     "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                                     "Xin chào Alice!", 1700000201, &msg);
    assert(res == MESSAGE_REPO_ERR_FORBIDDEN);
}

static void test_message_repo_insert_success(void)
{
    /* Alice gửi tin cho Bob (đã là bạn bè) */
    DirectMessageRecord msg = {0};
    const char *client_msg_id = "00000000000000000000000000000001";
    const char *text = "Chào Bob, tối nay có rảnh không?";

    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                                      client_msg_id, text, 1700000300, &msg);
    assert(res == MESSAGE_REPO_OK);
    assert(msg.message_id > 0);
    assert(msg.sender_id == g_alice_id);
    assert(strcmp(msg.sender_username, "alice") == 0);
    assert(msg.recipient_id == g_bob_id);
    assert(strcmp(msg.recipient_username, "bob") == 0);
    assert(strcmp(msg.client_msg_id, client_msg_id) == 0);
    assert(strcmp(msg.body, text) == 0);
    assert(msg.created_at == 1700000300);
    assert(msg.delivered_at == 0);
    assert(!msg.is_delivered);

    /* Tra cứu lại theo ID */
    DirectMessageRecord found = {0};
    res = message_repo_find_by_id(&g_db, msg.message_id, &found);
    assert(res == MESSAGE_REPO_OK);
    assert(found.message_id == msg.message_id);
    assert(found.sender_id == g_alice_id);
    assert(strcmp(found.sender_username, "alice") == 0);
    assert(found.recipient_id == g_bob_id);
    assert(strcmp(found.recipient_username, "bob") == 0);
    assert(strcmp(found.body, text) == 0);

    /* Tra cứu lại theo client_msg_id */
    DirectMessageRecord found_by_cid = {0};
    res = message_repo_find_by_client_msg_id(&g_db, g_alice_id, client_msg_id, &found_by_cid);
    assert(res == MESSAGE_REPO_OK);
    assert(found_by_cid.message_id == msg.message_id);
}

static void test_message_repo_idempotent_hit(void)
{
    /* Alice gửi lại tin nhắn cùng client_msg_id, cùng người nhận Bob, cùng nội dung */
    const char *client_msg_id = "00000000000000000000000000000001";
    const char *text = "Chào Bob, tối nay có rảnh không?";

    DirectMessageRecord retry_msg = {0};
    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                                      client_msg_id, text, 1700000400, &retry_msg);
    assert(res == MESSAGE_REPO_IDEMPOTENT_HIT);
    assert(retry_msg.message_id > 0);
    assert(retry_msg.sender_id == g_alice_id);
    assert(retry_msg.recipient_id == g_bob_id);
    assert(strcmp(retry_msg.body, text) == 0);
    assert(retry_msg.created_at == 1700000300); /* Giữ nguyên created_at ban đầu */
}

static void test_message_repo_idempotency_conflict(void)
{
    const char *client_msg_id = "00000000000000000000000000000001";

    /* Cùng client_msg_id nhưng đổi nội dung khác */
    DirectMessageRecord conflict_msg = {0};
    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                                      client_msg_id, "Nội dung khác hoàn toàn!",
                                                      1700000500, &conflict_msg);
    assert(res == MESSAGE_REPO_ERR_IDEMPOTENCY_CONFLICT);

    /* Cùng client_msg_id nhưng đổi recipient_id sang Carol */
    res = message_repo_insert_direct(&g_db, g_alice_id, g_carol_id,
                                     client_msg_id, "Chào Bob, tối nay có rảnh không?",
                                     1700000500, &conflict_msg);
    assert(res == MESSAGE_REPO_ERR_IDEMPOTENCY_CONFLICT);
}

static void test_message_repo_unfriend_idempotent_resend(void)
{
    const char *client_msg_id = "00000000000000000000000000000001";
    const char *text = "Chào Bob, tối nay có rảnh không?";

    /* Hủy quan hệ bạn bè giữa Alice và Bob */
    FriendRepoResult fr = friend_repo_remove_friendship(&g_db, g_alice_id, g_bob_id);
    assert(fr == FRIEND_REPO_OK);

    /* 1. Alice gửi lại tin CŨ (đã commit trước lúc unfriend) với cùng client_msg_id:
     * Theo spec Q07 và wire protocol, server trả về IDEMPOTENT_HIT thành công mà không báo lỗi */
    DirectMessageRecord retry_msg = {0};
    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                                      client_msg_id, text, 1700000600, &retry_msg);
    assert(res == MESSAGE_REPO_IDEMPOTENT_HIT);
    assert(strcmp(retry_msg.body, text) == 0);

    /* 2. Alice cố gửi tin MỚI (client_msg_id mới) khi đã unfriend -> Phải bị từ chối FORBIDDEN */
    DirectMessageRecord new_msg = {0};
    res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                     "00000000000000000000000000000002",
                                     "Tin mới sau unfriend", 1700000601, &new_msg);
    assert(res == MESSAGE_REPO_ERR_FORBIDDEN);

    /* Phục hồi lại tình bạn để chạy các test tiếp theo */
    fr = friend_repo_create_friendship(&g_db, g_alice_id, g_bob_id, 1700000610);
    assert(fr == FRIEND_REPO_OK);
}

static void test_message_repo_mark_delivered(void)
{
    /* Tìm tin nhắn đã tạo có message_id = 1 */
    DirectMessageRecord msg1 = {0};
    MessageRepoResult res = message_repo_find_by_id(&g_db, 1, &msg1);
    assert(res == MESSAGE_REPO_OK);
    assert(!msg1.is_delivered);

    /* 1. Tin nhắn không tồn tại */
    res = message_repo_mark_delivered(&g_db, 99999, g_bob_id, 1700000700);
    assert(res == MESSAGE_REPO_ERR_NOT_FOUND);

    /* 2. Carol (người thứ ba) cố tình ACK tin nhắn của Bob -> FORBIDDEN */
    res = message_repo_mark_delivered(&g_db, 1, g_carol_id, 1700000700);
    assert(res == MESSAGE_REPO_ERR_FORBIDDEN);

    /* 3. Bob (đúng recipient) ACK tin nhắn -> Thành công */
    res = message_repo_mark_delivered(&g_db, 1, g_bob_id, 1700000750);
    assert(res == MESSAGE_REPO_OK);

    /* Xác nhận trong DB đã được cập nhật delivered_at */
    res = message_repo_find_by_id(&g_db, 1, &msg1);
    assert(res == MESSAGE_REPO_OK);
    assert(msg1.is_delivered);
    assert(msg1.delivered_at == 1700000750);

    /* 4. Bob ACK lại lần 2 (Duplicate ACK) -> Thành công và giữ nguyên delivered_at cũ */
    res = message_repo_mark_delivered(&g_db, 1, g_bob_id, 1700000999);
    assert(res == MESSAGE_REPO_OK);

    res = message_repo_find_by_id(&g_db, 1, &msg1);
    assert(res == MESSAGE_REPO_OK);
    assert(msg1.delivered_at == 1700000750); /* Giữ nguyên 1700000750, không bị ghi đè thành 1700000999 */
}

static void test_message_repo_pending_and_history(void)
{
    /* Tạo thêm 3 tin nhắn:
     * - Alice gửi Bob tin 2
     * - Alice gửi Bob tin 3
     * - Bob gửi Alice tin 4
     */
    DirectMessageRecord m2 = {0};
    MessageRepoResult res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                                      "00000000000000000000000000000002",
                                                      "Bob ơi đi ăn trưa không?", 1700000800, &m2);
    assert(res == MESSAGE_REPO_OK);

    DirectMessageRecord m3 = {0};
    res = message_repo_insert_direct(&g_db, g_alice_id, g_bob_id,
                                     "00000000000000000000000000000003",
                                     "Quán cơm gà nhé!", 1700000801, &m3);
    assert(res == MESSAGE_REPO_OK);

    DirectMessageRecord m4 = {0};
    res = message_repo_insert_direct(&g_db, g_bob_id, g_alice_id,
                                     "00000000000000000000000000000004",
                                     "Ok Alice, 12h nhé!", 1700000802, &m4);
    assert(res == MESSAGE_REPO_OK);

    /* 1. Kiểm tra list pending cho Bob:
     * Tin 1 đã ACK. Tin 2 và Tin 3 đang pending (chưa ACK) */
    DirectMessageRecord pending_bob[10] = {0};
    size_t pending_count = 0;
    res = message_repo_list_pending_for_recipient(&g_db, g_bob_id, 10, pending_bob, &pending_count);
    assert(res == MESSAGE_REPO_OK);
    assert(pending_count == 2);
    assert(pending_bob[0].message_id == m2.message_id);
    assert(pending_bob[1].message_id == m3.message_id);

    /* Bob ACK tin 2 */
    res = message_repo_mark_delivered(&g_db, m2.message_id, g_bob_id, 1700000810);
    assert(res == MESSAGE_REPO_OK);

    /* List lại pending của Bob -> chỉ còn tin 3 */
    res = message_repo_list_pending_for_recipient(&g_db, g_bob_id, 10, pending_bob, &pending_count);
    assert(res == MESSAGE_REPO_OK);
    assert(pending_count == 1);
    assert(pending_bob[0].message_id == m3.message_id);

    /* 2. Kiểm tra list history giữa Alice và Bob:
     * Tổng cộng 4 tin (1, 2, 3, 4) */
    DirectMessageRecord history[10] = {0};
    size_t history_count = 0;
    bool has_more = false;

    /* Lấy trang 1: limit = 2 */
    res = message_repo_list_history(&g_db, g_alice_id, g_bob_id, 0, 2, history, &history_count, &has_more);
    assert(res == MESSAGE_REPO_OK);
    assert(history_count == 2);
    assert(has_more == true);
    assert(history[0].message_id == 1);
    assert(history[1].message_id == 2);

    /* Lấy trang 2: after_message_id = 2, limit = 2 */
    res = message_repo_list_history(&g_db, g_bob_id, g_alice_id, 2, 2, history, &history_count, &has_more);
    assert(res == MESSAGE_REPO_OK);
    assert(history_count == 2);
    assert(has_more == false);
    assert(history[0].message_id == 3);
    assert(history[1].message_id == 4);
}

int main(void)
{
    printf("=== Chạy kiểm thử Module Message Repository (M5.1) ===\n");

    setup_test_db();

    printf("1. test_message_repo_validation: kiểm tra định dạng client_msg_id, body và self-send... ");
    test_message_repo_validation();
    printf("PASS\n");

    printf("2. test_message_repo_insert_non_friend_forbidden: từ chối gửi tin nhắn khi chưa là bạn bè... ");
    test_message_repo_insert_non_friend_forbidden();
    printf("PASS\n");

    printf("3. test_message_repo_insert_success: gửi tin nhắn thành công giữa hai bạn bè... ");
    test_message_repo_insert_success();
    printf("PASS\n");

    printf("4. test_message_repo_idempotent_hit: retry cùng client_msg_id trả bản ghi cũ... ");
    test_message_repo_idempotent_hit();
    printf("PASS\n");

    printf("5. test_message_repo_idempotency_conflict: cùng client_msg_id khác payload bị từ chối... ");
    test_message_repo_idempotency_conflict();
    printf("PASS\n");

    printf("6. test_message_repo_unfriend_idempotent_resend: unfriend vẫn cho retry tin cũ, chặn tin mới... ");
    test_message_repo_unfriend_idempotent_resend();
    printf("PASS\n");

    printf("7. test_message_repo_mark_delivered: xác nhận nhận tin (ACK), chặn người thứ ba và duplicate ACK... ");
    test_message_repo_mark_delivered();
    printf("PASS\n");

    printf("8. test_message_repo_pending_and_history: truy vấn danh sách pending và lịch sử phân trang... ");
    test_message_repo_pending_and_history();
    printf("PASS\n");

    teardown_test_db();

    printf("=== Hoàn tất: 8/8 test cases của Module Message Repository (M5.1) thành công! ===\n");
    return 0;
}
