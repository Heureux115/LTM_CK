#ifndef TCP_CHAT_LIMITS_H
#define TCP_CHAT_LIMITS_H

/* Cấu hình mạng mặc định dùng chung để server và client không lệch địa chỉ. */
#define CHAT_DEFAULT_HOST "127.0.0.1"
#define CHAT_DEFAULT_PORT 5555
/* Số kết nối chờ trong hàng đợi listen, không phải số client đang chat. */
#define CHAT_LISTEN_BACKLOG 128
#define CHAT_MAX_CONNECTIONS 100u

/* Mỗi frame: 4 byte độ dài body (big-endian), rồi đến 1-65536 byte body. */
#define CHAT_FRAME_HEADER_BYTES 4u
#define CHAT_MAX_FRAME_BODY_BYTES 65536u
/* Sức chứa dự kiến cho tối đa hai frame lớn nhất của một connection. */
#define CHAT_INPUT_BUFFER_CAPACITY (2u * (CHAT_FRAME_HEADER_BYTES + CHAT_MAX_FRAME_BODY_BYTES))
/* Queue response thử nghiệm có cap; output queue nghiệp vụ sẽ thay thế sau. */
#define CHAT_OUTPUT_BUFFER_CAPACITY CHAT_INPUT_BUFFER_CAPACITY
#define CHAT_FRAMES_PER_CONNECTION_TURN 16u
/* Client không đọc: đóng nếu output không gửi tiến thêm byte nào trong 15 giây. */
#define CHAT_OUTPUT_NO_PROGRESS_TIMEOUT_MS 15000
/* Đóng chủ động hoặc lỗi protocol: tối đa 2 giây xả response rồi đóng. */
#define CHAT_CLOSE_FLUSH_TIMEOUT_MS 2000
/* Một lần thử connect → gửi → nhận response có chung một thời hạn. */
#define CHAT_CLIENT_ROUNDTRIP_TIMEOUT_MS 5000

/* Heartbeat: nonce hex 32 ký tự, idle 30s thì ping, chờ pong trong 10s, tối đa 3 lần sai. */
#define CHAT_HEARTBEAT_NONCE_HEX_LEN 32u
#define CHAT_HEARTBEAT_IDLE_TIMEOUT_MS 30000
#define CHAT_HEARTBEAT_PONG_TIMEOUT_MS 10000
#define CHAT_MAX_INVALID_PONGS 3u

/* Request ID cache: tối đa 64 request hoặc 256 KiB/connection để chống replay/trùng ID. */
#define CHAT_REQUEST_CACHE_MAX_ENTRIES 64u
#define CHAT_REQUEST_CACHE_MAX_BYTES (256u * 1024u)

/* Cấu hình cơ sở dữ liệu SQLite */
#define CHAT_DEFAULT_DB_PATH "data/chat.db"
#define CHAT_DB_BUSY_TIMEOUT_MS 100
#define CHAT_SCHEMA_VERSION 5

/* Giới hạn username */
#define CHAT_USERNAME_MIN_LEN 3u
#define CHAT_USERNAME_MAX_LEN 32u

/* Giới hạn mật khẩu và chuỗi băm Argon2id (libsodium) */
#define CHAT_PASSWORD_MIN_BYTES 8u
#define CHAT_PASSWORD_MAX_BYTES 128u
#define CHAT_PASSWORD_HASH_BYTES 128u

/* Giới hạn tin nhắn (Direct Messages & Group Messages) */
#define CHAT_CLIENT_MSG_ID_HEX_LEN 32u
#define CHAT_MESSAGE_BODY_MIN_BYTES 1u
#define CHAT_MESSAGE_BODY_MAX_BYTES 4096u
#define CHAT_DELIVERY_ACK_MAX_ITEMS 32u

/* Cấu hình cơ chế Delivery Replay & Offline Messaging (Milestone 6) */
#define CHAT_DELIVERY_INFLIGHT_MAX_SLOTS 32u
#define CHAT_DELIVERY_ACK_TIMEOUT_MS 15000
#define CHAT_DELIVERY_MAX_ATTEMPTS 3u
#define CHAT_DELIVERY_BATCH_PER_USER 8u
#define CHAT_DELIVERY_BATCH_PER_LOOP 64u
#define CHAT_DELIVERY_OUTPUT_HIGH_WATER_BYTES (256u * 1024u)
#define CHAT_DELIVERY_OUTPUT_HARD_CAP_BYTES (1024u * 1024u)

/* Giới hạn Chat Groups (Milestone 7) */
#define CHAT_GROUP_NAME_MIN_BYTES 1u
#define CHAT_GROUP_NAME_MAX_BYTES 128u
#define CHAT_GROUP_MAX_MEMBERS 50u
#define CHAT_GROUP_LIST_DEFAULT_LIMIT 20u
#define CHAT_GROUP_LIST_MAX_LIMIT 50u

/* Cấu hình File Logger & Log Rotation (Milestone 8 - R17 / T53) */
#define CHAT_DEFAULT_LOG_PATH "data/server.log"
#define CHAT_LOG_MAX_BYTES (5u * 1024u * 1024u) /* 5 MiB rotate threshold */
#define CHAT_LOG_MAX_BACKUP_FILES 3u            /* server.log.1, .2, .3 */

#endif
