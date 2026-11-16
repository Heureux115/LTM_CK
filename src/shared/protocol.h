#ifndef TCP_CHAT_PROTOCOL_H
#define TCP_CHAT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cjson/cJSON.h>

#include "shared/buffer.h"
#include "shared/limits.h"

typedef enum {
    PROTOCOL_OK = 0,
    PROTOCOL_INVALID_JSON,
    PROTOCOL_INVALID_REQUEST,
    PROTOCOL_UNSUPPORTED_VERSION,
    PROTOCOL_ALLOCATION_ERROR,
    PROTOCOL_OUTPUT_FULL
} ProtocolResult;

typedef enum {
    PROTOCOL_ERROR_INVALID_JSON = 0,
    PROTOCOL_ERROR_INVALID_REQUEST,
    PROTOCOL_ERROR_UNSUPPORTED_VERSION,
    PROTOCOL_ERROR_UNKNOWN_TYPE,
    PROTOCOL_ERROR_VALIDATION,
    PROTOCOL_ERROR_REQUEST_ID_REUSED,
    PROTOCOL_ERROR_REQUEST_ID_EXPIRED,
    PROTOCOL_ERROR_USERNAME_TAKEN,
    PROTOCOL_ERROR_STORAGE,
    PROTOCOL_ERROR_INTERNAL,
    PROTOCOL_ERROR_AUTH_REQUIRED,
    PROTOCOL_ERROR_ALREADY_AUTHENTICATED,
    PROTOCOL_ERROR_ALREADY_ONLINE,
    PROTOCOL_ERROR_AUTH_FAILED,
    PROTOCOL_ERROR_NOT_FOUND,
    PROTOCOL_ERROR_CONFLICT,
    PROTOCOL_ERROR_FORBIDDEN,
    PROTOCOL_ERROR_IDEMPOTENCY_CONFLICT,
    PROTOCOL_ERROR_GROUP_CLOSED,
    PROTOCOL_ERROR_LIMIT_EXCEEDED
} ProtocolErrorCode;

/* type/request_id/data trỏ vào document; chỉ dùng trước protocol_request_destroy(). */
typedef struct {
    cJSON *document;
    const char *type;
    const char *request_id;
    const cJSON *data;
} ProtocolRequest;

/* Kiểm tra chuỗi số nguyên dương tăng dần canonical, không leading zero, max UINT64_MAX. */
bool protocol_parse_request_id(const char *request_id, uint64_t *id_out);

/* Chỉ kiểm tra JSON/envelope; version khác nhưng envelope hợp lệ vẫn giữ request để trả lỗi có ID. */
ProtocolResult protocol_parse_request(const unsigned char *body, size_t length,
                                       ProtocolRequest *request);
void protocol_request_destroy(ProtocolRequest *request);
/* Tạo frame response cho system.ping. */
ProtocolResult protocol_encode_ping_response(const ProtocolRequest *request,
                                              int64_t server_time_ms, ByteBuffer *output);
/* Xác nhận connection.close; caller chỉ đóng socket sau khi frame này đã gửi. */
ProtocolResult protocol_encode_close_response(const ProtocolRequest *request,
                                               ByteBuffer *output);
/* Tạo frame response cho auth.register. */
ProtocolResult protocol_encode_register_response(const ProtocolRequest *request,
                                                 int64_t user_id,
                                                 const char *username,
                                                 ByteBuffer *output);
/* Tạo frame response cho auth.login. */
ProtocolResult protocol_encode_login_response(const ProtocolRequest *request,
                                              int64_t user_id,
                                              const char *username,
                                              int64_t heartbeat_idle_ms,
                                              int64_t heartbeat_timeout_ms,
                                              ByteBuffer *output);
/* Tạo frame response cho auth.logout; trả data: {"logged_out": true}. */
ProtocolResult protocol_encode_logout_response(const ProtocolRequest *request,
                                               ByteBuffer *output);
/* Cấu trúc phần tử FriendRequest trên Wire Protocol */
typedef struct {
    int64_t friend_request_id;
    int64_t sender_id;
    const char *sender_username;
    int64_t recipient_id;
    const char *recipient_username;
    const char *status;
    int64_t created_at;
} ProtocolFriendRequest;

typedef struct {
    int64_t user_id;
    const char *username;
    bool online;
} ProtocolFriend;

/* Tạo frame response cho friend.request.send. */
ProtocolResult protocol_encode_friend_request_send_response(const ProtocolRequest *request,
                                                             int64_t friend_request_id,
                                                             int64_t sender_id,
                                                             const char *sender_username,
                                                             int64_t recipient_id,
                                                             const char *recipient_username,
                                                             const char *status,
                                                             int64_t created_at,
                                                             ByteBuffer *output);
/* Tạo frame response cho friend.request.list. */
ProtocolResult protocol_encode_friend_request_list_response(const ProtocolRequest *request,
                                                             const ProtocolFriendRequest *requests,
                                                             size_t request_count,
                                                             bool has_more,
                                                             int64_t next_after_id,
                                                             ByteBuffer *output);
/* Tạo frame event friend.request.received (server push đến người nhận online). */
ProtocolResult protocol_encode_friend_request_received_event(int64_t friend_request_id,
                                                             int64_t sender_id,
                                                             const char *sender_username,
                                                             int64_t recipient_id,
                                                             const char *recipient_username,
                                                             const char *status,
                                                             int64_t created_at,
                                                             ByteBuffer *output);
/* Tạo frame response cho friend.request.accept. */
ProtocolResult protocol_encode_friend_request_accept_response(const ProtocolRequest *request,
                                                              int64_t friend_user_id,
                                                              const char *friend_username,
                                                              bool friend_online,
                                                              ByteBuffer *output);
/* Tạo frame response cho friend.request.reject. */
ProtocolResult protocol_encode_friend_request_reject_response(const ProtocolRequest *request,
                                                              int64_t friend_request_id,
                                                              ByteBuffer *output);
/* Tạo frame response cho friend.remove. */
ProtocolResult protocol_encode_friend_remove_response(const ProtocolRequest *request,
                                                      int64_t friend_user_id,
                                                      ByteBuffer *output);
/* Tạo frame response cho friend.list. */
ProtocolResult protocol_encode_friend_list_response(const ProtocolRequest *request,
                                                    const ProtocolFriend *friends,
                                                    size_t friend_count,
                                                    bool has_more,
                                                    int64_t next_after_id,
                                                    ByteBuffer *output);
/* Tạo frame event friend.request.resolved (server push thông báo kết quả accept/reject). */
ProtocolResult protocol_encode_friend_request_resolved_event(int64_t friend_request_id,
                                                             const char *status,
                                                             int64_t by_user_id,
                                                             ByteBuffer *output);
/* Tạo frame event friend.changed (server push khi kết bạn/hủy bạn). */
ProtocolResult protocol_encode_friend_changed_event(const char *action,
                                                    int64_t friend_user_id,
                                                    const char *friend_username,
                                                    bool friend_online,
                                                    ByteBuffer *output);
/* Tạo frame event presence.changed (server push khi bạn bè online/offline). */
ProtocolResult protocol_encode_presence_changed_event(int64_t user_id,
                                                      bool online,
                                                      const char *reason,
                                                      int64_t at_ms,
                                                      ByteBuffer *output);
/* Tạo frame response cho direct.send. */
ProtocolResult protocol_encode_direct_send_response(const ProtocolRequest *request,
                                                    const char *scope,
                                                    int64_t message_id,
                                                    const char *client_msg_id,
                                                    int64_t created_at,
                                                    const char *status,
                                                    ByteBuffer *output);
/* Tạo frame event direct.message (server push đến người nhận online). */
ProtocolResult protocol_encode_direct_message_event(int64_t message_id,
                                                    const char *client_msg_id,
                                                    int64_t sender_id,
                                                    const char *sender_username,
                                                    int64_t recipient_id,
                                                    const char *text,
                                                    int64_t created_at,
                                                    ByteBuffer *output);
/* Cấu trúc một tin nhắn trong batch xác nhận đã nhận (delivery ACK). */
typedef struct {
    char scope[16];
    int64_t message_id;
} ProtocolAckItem;

/* Tạo frame response cho delivery.ack thành công: {"acked": [{"scope": "...", "message_id": "..."}, ...]}. */
ProtocolResult protocol_encode_delivery_ack_response(const ProtocolRequest *request,
                                                     const ProtocolAckItem *items,
                                                     size_t count,
                                                     ByteBuffer *output);

/* Kiểu dữ liệu item tin nhắn trực tiếp trong danh sách lịch sử */
typedef struct {
    int64_t message_id;
    int64_t sender_id;
    char sender_username[CHAT_USERNAME_MAX_LEN + 1];
    int64_t recipient_id;
    char recipient_username[CHAT_USERNAME_MAX_LEN + 1];
    char client_msg_id[CHAT_CLIENT_MSG_ID_HEX_LEN + 1];
    char text[CHAT_MESSAGE_BODY_MAX_BYTES + 1];
    int64_t created_at;
    int64_t delivered_at;
} ProtocolDirectMessageItem;

/* Tạo frame response cho direct.history: {"items": [...], "has_more": bool, "next_after_id": ID hoặc null}. */
ProtocolResult protocol_encode_direct_history_response(const ProtocolRequest *request,
                                                       const ProtocolDirectMessageItem *items,
                                                       size_t item_count,
                                                       bool has_more,
                                                       int64_t next_after_id,
                                                       ByteBuffer *output);

/* Cấu trúc phần tử Group trên Wire Protocol */
typedef struct {
    int64_t group_id;
    char name[CHAT_GROUP_NAME_MAX_BYTES + 1];
    char visibility[16]; /* "public" hoặc "private" */
    int64_t owner_user_id;
    int32_t member_count;
    int64_t created_at;
} ProtocolGroup;

/* Cấu trúc phần tử Member của Group */
typedef struct {
    int64_t user_id;
    char username[CHAT_USERNAME_MAX_LEN + 1];
    char role[16]; /* "owner" hoặc "member" */
    int64_t joined_at;
} ProtocolGroupMember;

/* Cấu trúc thông tin nhóm đã đóng */
typedef struct {
    int64_t group_id;
    char group_name[CHAT_GROUP_NAME_MAX_BYTES + 1];
    int64_t by_user_id;
    int64_t closed_at;
    char reason[32]; /* "owner_left" */
} ProtocolClosedGroup;

/* Cấu trúc lời mời tham gia nhóm */
typedef struct {
    int64_t invitation_id;
    int64_t group_id;
    char group_name[CHAT_GROUP_NAME_MAX_BYTES + 1];
    int64_t inviter_user_id;
    char inviter_username[CHAT_USERNAME_MAX_LEN + 1];
    int64_t invitee_user_id;
    char invitee_username[CHAT_USERNAME_MAX_LEN + 1];
    char status[16]; /* "pending" */
    int64_t created_at;
} ProtocolGroupInvitation;

/* Tạo frame response cho group.create, group.invite.accept, group.owner.transfer */
ProtocolResult protocol_encode_group_response(const ProtocolRequest *request,
                                              const ProtocolGroup *group,
                                              ByteBuffer *output);

/* Tạo frame response cho group.list */
ProtocolResult protocol_encode_group_list_response(const ProtocolRequest *request,
                                                   const ProtocolGroup *groups,
                                                   size_t count,
                                                   bool has_more,
                                                   int64_t next_after_id,
                                                   ByteBuffer *output);

/* Tạo frame response cho group.closed.list */
ProtocolResult protocol_encode_group_closed_list_response(const ProtocolRequest *request,
                                                          const ProtocolClosedGroup *groups,
                                                          size_t count,
                                                          bool has_more,
                                                          int64_t next_after_id,
                                                          ByteBuffer *output);

/* Tạo frame response cho group.members */
ProtocolResult protocol_encode_group_members_response(const ProtocolRequest *request,
                                                      const ProtocolGroupMember *members,
                                                      size_t count,
                                                      bool has_more,
                                                      int64_t next_after_id,
                                                      ByteBuffer *output);

/* Tạo frame response cho group.invite.send */
ProtocolResult protocol_encode_group_invite_send_response(const ProtocolRequest *request,
                                                          const ProtocolGroupInvitation *invitation,
                                                          ByteBuffer *output);

/* Tạo frame response cho group.invite.list */
ProtocolResult protocol_encode_group_invite_list_response(const ProtocolRequest *request,
                                                          const ProtocolGroupInvitation *invitations,
                                                          size_t count,
                                                          bool has_more,
                                                          int64_t next_after_id,
                                                          ByteBuffer *output);

/* Tạo frame response cho group.invite.reject */
ProtocolResult protocol_encode_group_invite_reject_response(const ProtocolRequest *request,
                                                            int64_t invitation_id,
                                                            ByteBuffer *output);

/* Tạo frame response cho group.member.remove */
ProtocolResult protocol_encode_group_member_remove_response(const ProtocolRequest *request,
                                                            int64_t group_id,
                                                            int64_t member_user_id,
                                                            ByteBuffer *output);

/* Tạo frame response cho group.leave */
ProtocolResult protocol_encode_group_leave_response(const ProtocolRequest *request,
                                                    int64_t group_id,
                                                    bool closed,
                                                    ByteBuffer *output);

/* Tạo server push event group.invite.received */
ProtocolResult protocol_encode_group_invite_received_event(const ProtocolGroupInvitation *invitation,
                                                           ByteBuffer *output);

/* Tạo server push event group.invite.resolved */
ProtocolResult protocol_encode_group_invite_resolved_event(int64_t invitation_id,
                                                           int64_t group_id,
                                                           const char *status,
                                                           int64_t by_user_id,
                                                           ByteBuffer *output);

/* Tạo server push event group.member.changed */
ProtocolResult protocol_encode_group_member_changed_event(int64_t group_id,
                                                          int64_t user_id,
                                                          const char *username,
                                                          const char *action,
                                                          int64_t by_user_id,
                                                          int32_t member_count,
                                                          ByteBuffer *output);

/* Tạo server push event group.owner.changed */
ProtocolResult protocol_encode_group_owner_changed_event(int64_t group_id,
                                                         int64_t previous_owner_user_id,
                                                         int64_t new_owner_user_id,
                                                         int64_t at_ms,
                                                         ByteBuffer *output);

/* Tạo server push event group.closed */
ProtocolResult protocol_encode_group_closed_event(int64_t group_id,
                                                  const char *group_name,
                                                  int64_t by_user_id,
                                                  int64_t at_ms,
                                                  ByteBuffer *output);

/* Tạo frame response cho group.send */
ProtocolResult protocol_encode_group_send_response(const ProtocolRequest *request,
                                                   const char *scope,
                                                   int64_t message_id,
                                                   const char *client_msg_id,
                                                   int64_t created_at,
                                                   const char *status,
                                                   ByteBuffer *output);

/* Tạo server push event group.message (giao tin snapshot chưa ACK đến member) */
ProtocolResult protocol_encode_group_message_event(int64_t message_id,
                                                   const char *client_msg_id,
                                                   int64_t group_id,
                                                   const char *group_name,
                                                   int64_t sender_id,
                                                   const char *sender_username,
                                                   const char *text,
                                                   int64_t created_at,
                                                   ByteBuffer *output);

/* Lỗi có request ID trả response; lỗi không tin được envelope trả event protocol.error. */
ProtocolResult protocol_encode_error_response(const ProtocolRequest *request,
                                               ProtocolErrorCode code, ByteBuffer *output);
ProtocolResult protocol_encode_protocol_error(ProtocolErrorCode code, ByteBuffer *output);
/* Handshake đầu kết nối: server encode event, client kiểm tra version/giới hạn trước request. */
ProtocolResult protocol_encode_ready_event(int64_t server_time_ms, ByteBuffer *output);
ProtocolResult protocol_validate_ready_event(const unsigned char *body, size_t length);
/* Heartbeat: sinh/kiểm tra nonce hex32, encode/parse heartbeat.ping và heartbeat.pong. */
bool protocol_validate_nonce_hex(const char *nonce);
bool protocol_generate_nonce_hex(char *nonce_out, size_t size);
ProtocolResult protocol_encode_heartbeat_ping(const char *nonce, ByteBuffer *output);
ProtocolResult protocol_parse_heartbeat_ping(const unsigned char *body, size_t length,
                                             char *nonce_out);
ProtocolResult protocol_encode_heartbeat_pong(const char *nonce, ByteBuffer *output);
ProtocolResult protocol_parse_heartbeat_pong(const unsigned char *body, size_t length,
                                             char *nonce_out);

#endif
