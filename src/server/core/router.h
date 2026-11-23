#ifndef TCP_CHAT_ROUTER_H
#define TCP_CHAT_ROUTER_H

#include <stddef.h>
#include <stdint.h>

#include "server/core/request_cache.h"
#include "server/core/session.h"
#include "server/db/db.h"
#include "shared/buffer.h"

typedef enum {
    ROUTER_OK = 0,
    ROUTER_CLOSE_AFTER_FLUSH,
    ROUTER_OUTPUT_FULL,
    ROUTER_INTERNAL_ERROR
} RouterResult;

/* Con trỏ hàm điều phối server push event tới connection của người nhận online */
typedef bool (*RouterNotificationFn)(void *user_data, int64_t recipient_user_id, const ByteBuffer *frame_buf);

/* Con trỏ hàm điều phối push event tin nhắn có theo dõi in-flight delivery tracker */
typedef bool (*RouterDeliveryPushFn)(void *user_data,
                                     int64_t recipient_user_id,
                                     const char *scope,
                                     int64_t message_id,
                                     const ByteBuffer *frame_buf);

/* Con trỏ hàm thông báo giải phóng in-flight tracker khi client gửi delivery.ack thành công */
typedef void (*RouterDeliveryAckFn)(void *user_data,
                                    const char *connection_id,
                                    uint64_t connection_generation,
                                    const char *scope,
                                    int64_t message_id);

/* Đăng ký notification dispatcher toàn server */
void router_set_notification_dispatcher(RouterNotificationFn fn, void *user_data);

/* Đăng ký callback đồng bộ push tin nhắn và delivery.ack với in-flight tracker của server */
void router_set_delivery_hooks(RouterDeliveryPushFn push_fn,
                               RouterDeliveryAckFn ack_fn,
                               void *user_data);

/* Phát sự kiện presence.changed (server push) tới tất cả bạn bè đang online */
void router_broadcast_presence(Database *db, const SessionManager *session_mgr,
                               int64_t user_id, bool online, const char *reason, int64_t now_ms);

/* Nhận body frame, kiểm tra replay cache / high_water, router nghiệp vụ (ping, close, auth.register...),
   kiểm tra Authentication Gate (A, U, B), trả response/error; lỗi envelope nghiêm trọng cần flush rồi đóng. */
RouterResult router_handle_frame(const unsigned char *body, size_t length,
                                  ByteBuffer *output,
                                  RequestCache *cache,
                                  uint64_t *high_water_request_id,
                                  Database *db,
                                  SessionManager *session_mgr,
                                  const char *connection_id,
                                  uint64_t connection_generation);

#endif
