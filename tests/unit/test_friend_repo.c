#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server/db/db.h"
#include "server/repos/friend_repo.h"
#include "server/repos/user_repo.h"

static void check(bool condition, const char *msg)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(EXIT_FAILURE);
    }
}

static void create_test_user(Database *db, const char *username, int64_t *out_user_id)
{
    UserRepoResult res = user_repo_create(db, username, "$argon2id$testhash", 1000, out_user_id);
    check(res == USER_REPO_OK, "create test user");
}

/* 1. Kiểm tra canonical pair và enum conversions */
static void test_friend_repo_canonical_pair_and_validation(void)
{
    printf("1. test_friend_repo_canonical_pair_and_validation: canonical pair và enum... ");
    int64_t low = 0;
    int64_t high = 0;

    check(friend_repo_canonical_pair(10, 20, &low, &high), "pair 10, 20");
    check(low == 10 && high == 20, "low 10, high 20");

    check(friend_repo_canonical_pair(50, 5, &low, &high), "pair 50, 5");
    check(low == 5 && high == 50, "low 5, high 50");

    /* Không thể tạo pair cho cùng một người */
    check(!friend_repo_canonical_pair(10, 10, &low, &high), "self pair rejected");

    /* Không thể tạo pair cho id âm hoặc 0 */
    check(!friend_repo_canonical_pair(0, 10, &low, &high), "zero id rejected");
    check(!friend_repo_canonical_pair(-5, 10, &low, &high), "negative id rejected");

    /* Kiểm tra enum string */
    check(strcmp(friend_request_status_to_string(FRIEND_REQUEST_STATUS_PENDING), "pending") == 0, "status pending string");
    check(strcmp(friend_request_status_to_string(FRIEND_REQUEST_STATUS_ACCEPTED), "accepted") == 0, "status accepted string");
    check(strcmp(friend_request_status_to_string(FRIEND_REQUEST_STATUS_REJECTED), "rejected") == 0, "status rejected string");

    FriendRequestStatus st;
    check(friend_request_status_from_string("pending", &st) && st == FRIEND_REQUEST_STATUS_PENDING, "from string pending");
    check(friend_request_status_from_string("accepted", &st) && st == FRIEND_REQUEST_STATUS_ACCEPTED, "from string accepted");
    check(friend_request_status_from_string("rejected", &st) && st == FRIEND_REQUEST_STATUS_REJECTED, "from string rejected");
    check(!friend_request_status_from_string("unknown", &st), "from string unknown rejected");

    printf("PASS\n");
}

/* 2. Tạo lời mời kết bạn và tìm kiếm theo ID / pair */
static void test_friend_repo_create_and_find_request(void)
{
    printf("2. test_friend_repo_create_and_find_request: tạo và tra cứu friend request... ");
    Database db;
    check(db_open(":memory:", &db) == DB_OK, "open in-memory db");

    int64_t alice_id = 0;
    int64_t bob_id = 0;
    create_test_user(&db, "alice", &alice_id);
    create_test_user(&db, "bob", &bob_id);

    /* Alice không thể tự mời chính mình */
    int64_t req_id = 0;
    FriendRepoResult res = friend_repo_create_request(&db, alice_id, alice_id, 1000, &req_id);
    check(res == FRIEND_REPO_ERR_SELF_RELATION, "self invitation rejected");

    /* Alice mời Bob */
    res = friend_repo_create_request(&db, alice_id, bob_id, 1000, &req_id);
    check(res == FRIEND_REPO_OK, "create request Alice -> Bob succeeds");
    check(req_id > 0, "valid req_id");

    /* Tìm theo friend_request_id */
    FriendRequestRecord req = {0};
    check(friend_repo_find_request_by_id(&db, req_id, &req) == FRIEND_REPO_OK, "find by id");
    check(req.friend_request_id == req_id, "id matches");
    check(req.sender_id == alice_id, "sender is Alice");
    check(req.recipient_id == bob_id, "recipient is Bob");
    check(req.user_low_id == alice_id && req.user_high_id == bob_id, "canonical pair matches");
    check(req.status == FRIEND_REQUEST_STATUS_PENDING, "status is pending");
    check(req.created_at == 1000, "created_at is 1000");
    check(!req.has_resolved_at, "has_resolved_at is false");

    /* Tìm theo pending pair (cả 2 chiều tra cứu đều tìm ra cùng request) */
    FriendRequestRecord pair_req1 = {0};
    check(friend_repo_find_pending_pair(&db, alice_id, bob_id, &pair_req1) == FRIEND_REPO_OK, "find pair Alice, Bob");
    check(pair_req1.friend_request_id == req_id, "pair matches req_id");

    FriendRequestRecord pair_req2 = {0};
    check(friend_repo_find_pending_pair(&db, bob_id, alice_id, &pair_req2) == FRIEND_REPO_OK, "find pair Bob, Alice");
    check(pair_req2.friend_request_id == req_id, "reverse pair matches req_id");

    db_close(&db);
    printf("PASS\n");
}

/* 3. Chống duplicate và cross-invite bằng Partial Unique Index */
static void test_friend_repo_partial_unique_pending(void)
{
    printf("3. test_friend_repo_partial_unique_pending: chặn lời mời trùng và chéo... ");
    Database db;
    check(db_open(":memory:", &db) == DB_OK, "open in-memory db");

    int64_t alice_id = 0;
    int64_t bob_id = 0;
    create_test_user(&db, "alice", &alice_id);
    create_test_user(&db, "bob", &bob_id);

    /* 1. Alice mời Bob */
    int64_t req1_id = 0;
    check(friend_repo_create_request(&db, alice_id, bob_id, 1000, &req1_id) == FRIEND_REPO_OK, "create request 1");

    /* 2. Alice gửi lại lần nữa -> bị từ chối PENDING_EXISTS */
    int64_t req2_id = 0;
    FriendRepoResult dup_res = friend_repo_create_request(&db, alice_id, bob_id, 1001, &req2_id);
    check(dup_res == FRIEND_REPO_ERR_PENDING_EXISTS, "duplicate invite rejected");

    /* 3. Bob gửi ngược lại cho Alice (cross-invite) -> bị từ chối PENDING_EXISTS */
    int64_t req3_id = 0;
    FriendRepoResult cross_res = friend_repo_create_request(&db, bob_id, alice_id, 1002, &req3_id);
    check(cross_res == FRIEND_REPO_ERR_PENDING_EXISTS, "cross invite rejected");

    db_close(&db);
    printf("PASS\n");
}

/* 4. Resolve request (reject) và cho phép mời lại sau khi đã resolve */
static void test_friend_repo_resolve_and_reinvite(void)
{
    printf("4. test_friend_repo_resolve_and_reinvite: resolve reject và mời lại sau đó... ");
    Database db;
    check(db_open(":memory:", &db) == DB_OK, "open in-memory db");

    int64_t alice_id = 0;
    int64_t bob_id = 0;
    create_test_user(&db, "alice", &alice_id);
    create_test_user(&db, "bob", &bob_id);

    /* 1. Alice mời Bob */
    int64_t req1_id = 0;
    check(friend_repo_create_request(&db, alice_id, bob_id, 1000, &req1_id) == FRIEND_REPO_OK, "create req1");

    /* 2. Bob reject lời mời của Alice */
    check(friend_repo_resolve_request(&db, req1_id, FRIEND_REQUEST_STATUS_REJECTED, 1050) == FRIEND_REPO_OK, "reject req1");

    /* Kiểm tra record đã cập nhật status rejected và resolved_at */
    FriendRequestRecord req_check = {0};
    check(friend_repo_find_request_by_id(&db, req1_id, &req_check) == FRIEND_REPO_OK, "find req1");
    check(req_check.status == FRIEND_REQUEST_STATUS_REJECTED, "status is rejected");
    check(req_check.has_resolved_at && req_check.resolved_at == 1050, "resolved_at is 1050");

    /* Không còn lời mời nào pending giữa Alice và Bob */
    FriendRequestRecord pending_check = {0};
    check(friend_repo_find_pending_pair(&db, alice_id, bob_id, &pending_check) == FRIEND_REPO_ERR_NOT_FOUND, "no pending pair");

    /* 3. Mời lại sau khi reject: Alice có thể mời lại Bob bằng request mới */
    int64_t req2_id = 0;
    check(friend_repo_create_request(&db, alice_id, bob_id, 2000, &req2_id) == FRIEND_REPO_OK, "reinvite Alice -> Bob succeeds");
    check(req2_id != req1_id, "new request id generated");

    db_close(&db);
    printf("PASS\n");
}

/* 5. Vòng đời friendship: tạo, kiểm tra 2 chiều, chặn trùng, và hủy quan hệ */
static void test_friend_repo_friendship_lifecycle(void)
{
    printf("5. test_friend_repo_friendship_lifecycle: vòng đời quan hệ bạn bè... ");
    Database db;
    check(db_open(":memory:", &db) == DB_OK, "open in-memory db");

    int64_t alice_id = 0;
    int64_t bob_id = 0;
    create_test_user(&db, "alice", &alice_id);
    create_test_user(&db, "bob", &bob_id);

    /* Ban đầu chưa là bạn bè */
    bool are_friends = false;
    check(friend_repo_are_friends(&db, alice_id, bob_id, &are_friends) == FRIEND_REPO_OK, "check are_friends");
    check(!are_friends, "initially not friends");

    /* Tạo quan hệ bạn bè */
    check(friend_repo_create_friendship(&db, alice_id, bob_id, 1000) == FRIEND_REPO_OK, "create friendship");

    /* Kiểm tra 2 chiều: cả (alice, bob) và (bob, alice) đều là bạn */
    check(friend_repo_are_friends(&db, alice_id, bob_id, &are_friends) == FRIEND_REPO_OK && are_friends, "alice & bob friends");
    check(friend_repo_are_friends(&db, bob_id, alice_id, &are_friends) == FRIEND_REPO_OK && are_friends, "bob & alice friends");

    /* Cố tình insert friendship lần nữa -> từ chối ALREADY_EXISTS */
    check(friend_repo_create_friendship(&db, bob_id, alice_id, 1001) == FRIEND_REPO_ERR_ALREADY_EXISTS, "duplicate friendship rejected");

    /* Khi đã là bạn, không thể gửi friend request mới */
    int64_t req_id = 0;
    check(friend_repo_create_request(&db, alice_id, bob_id, 1002, &req_id) == FRIEND_REPO_ERR_ALREADY_EXISTS, "friend request between friends rejected");

    /* Hủy bạn bè (remove friendship) */
    check(friend_repo_remove_friendship(&db, bob_id, alice_id) == FRIEND_REPO_OK, "remove friendship");

    /* Kiểm tra không còn là bạn */
    check(friend_repo_are_friends(&db, alice_id, bob_id, &are_friends) == FRIEND_REPO_OK && !are_friends, "no longer friends");

    /* Hủy lần 2 -> báo NOT_FOUND */
    check(friend_repo_remove_friendship(&db, alice_id, bob_id) == FRIEND_REPO_ERR_NOT_FOUND, "remove twice NOT_FOUND");

    db_close(&db);
    printf("PASS\n");
}

/* 6. Phân trang danh sách bạn bè 2 chiều và danh sách lời mời incoming/outgoing */
static void test_friend_repo_pagination_and_bidirectional_list(void)
{
    printf("6. test_friend_repo_pagination_and_bidirectional_list: phân trang bạn bè và request... ");
    Database db;
    check(db_open(":memory:", &db) == DB_OK, "open in-memory db");

    /* Tạo 1 user trung tâm Alice và 5 người dùng khác (u2, u3, u4, u5, u6) */
    int64_t alice = 0;
    create_test_user(&db, "alice", &alice); /* user_id = 1 */

    int64_t others[5] = {0};
    char name[32];
    for (int i = 0; i < 5; ++i) {
        snprintf(name, sizeof(name), "user_%d", i + 2);
        create_test_user(&db, name, &others[i]);
        /* Kết bạn giữa Alice và user_i */
        check(friend_repo_create_friendship(&db, alice, others[i], 1000 + i) == FRIEND_REPO_OK, "create friendship");
    }

    /* 6.1 Phân trang danh sách bạn bè của Alice với limit = 2 */
    FriendRecord friends[3];
    size_t count = 0;
    bool has_more = false;

    /* Trang 1 */
    check(friend_repo_list_friends(&db, alice, 0, 2, friends, &count, &has_more) == FRIEND_REPO_OK, "page 1");
    check(count == 2, "page 1 count is 2");
    check(has_more, "page 1 has_more is true");
    check(friends[0].friend_user_id == others[0] && strcmp(friends[0].username, "user_2") == 0, "friend 1 is user_2");
    check(friends[1].friend_user_id == others[1] && strcmp(friends[1].username, "user_3") == 0, "friend 2 is user_3");

    /* Trang 2 */
    int64_t last_id = friends[1].friend_user_id;
    check(friend_repo_list_friends(&db, alice, last_id, 2, friends, &count, &has_more) == FRIEND_REPO_OK, "page 2");
    check(count == 2, "page 2 count is 2");
    check(has_more, "page 2 has_more is true");
    check(friends[0].friend_user_id == others[2] && strcmp(friends[0].username, "user_4") == 0, "friend 3 is user_4");
    check(friends[1].friend_user_id == others[3] && strcmp(friends[1].username, "user_5") == 0, "friend 4 is user_5");

    /* Trang 3 */
    last_id = friends[1].friend_user_id;
    check(friend_repo_list_friends(&db, alice, last_id, 2, friends, &count, &has_more) == FRIEND_REPO_OK, "page 3");
    check(count == 1, "page 3 count is 1");
    check(!has_more, "page 3 has_more is false (end of list)");
    check(friends[0].friend_user_id == others[4] && strcmp(friends[0].username, "user_6") == 0, "friend 5 is user_6");

    /* 6.2 Phân trang danh sách lời mời: Xóa quan hệ của u2, u3, u4 để tạo lời mời incoming tới Alice */
    check(friend_repo_remove_friendship(&db, alice, others[0]) == FRIEND_REPO_OK, "remove f1");
    check(friend_repo_remove_friendship(&db, alice, others[1]) == FRIEND_REPO_OK, "remove f2");
    check(friend_repo_remove_friendship(&db, alice, others[2]) == FRIEND_REPO_OK, "remove f3");

    int64_t req_id = 0;
    check(friend_repo_create_request(&db, others[0], alice, 2000, &req_id) == FRIEND_REPO_OK, "req u2 -> alice");
    check(friend_repo_create_request(&db, others[1], alice, 2001, &req_id) == FRIEND_REPO_OK, "req u3 -> alice");
    check(friend_repo_create_request(&db, others[2], alice, 2002, &req_id) == FRIEND_REPO_OK, "req u4 -> alice");

    FriendRequestRecord reqs[3];
    /* Lấy incoming của Alice limit = 2 */
    check(friend_repo_list_requests(&db, alice, FRIEND_REQUEST_DIR_INCOMING, 0, 2, reqs, &count, &has_more) == FRIEND_REPO_OK, "reqs page 1");
    check(count == 2, "reqs page 1 count 2");
    check(has_more, "reqs page 1 has_more true");
    check(reqs[0].sender_id == others[0], "sender 1 is user_2");
    check(reqs[1].sender_id == others[1], "sender 2 is user_3");

    last_id = reqs[1].friend_request_id;
    check(friend_repo_list_requests(&db, alice, FRIEND_REQUEST_DIR_INCOMING, last_id, 2, reqs, &count, &has_more) == FRIEND_REPO_OK, "reqs page 2");
    check(count == 1, "reqs page 2 count 1");
    check(!has_more, "reqs page 2 has_more false");
    check(reqs[0].sender_id == others[2], "sender 3 is user_4");

    db_close(&db);
    printf("PASS\n");
}

int main(void)
{
    printf("=== Chạy kiểm thử Module Friend Repository (SQLite Prepared Statements) ===\n");
    test_friend_repo_canonical_pair_and_validation();
    test_friend_repo_create_and_find_request();
    test_friend_repo_partial_unique_pending();
    test_friend_repo_resolve_and_reinvite();
    test_friend_repo_friendship_lifecycle();
    test_friend_repo_pagination_and_bidirectional_list();
    printf("=== Hoàn tất: 6/6 test cases của Module Friend Repository thành công! ===\n");
    return 0;
}
