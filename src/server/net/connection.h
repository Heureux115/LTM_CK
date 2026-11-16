#ifndef TCP_CHAT_CONNECTION_H
#define TCP_CHAT_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server/core/delivery.h"
#include "server/core/request_cache.h"
#include "server/core/session.h"
#include "server/db/db.h"
#include "shared/buffer.h"

/* Mỗi client giữ fd và byte chưa tạo thành frame hoàn chỉnh của riêng mình. */
typedef struct {
    int fd;
    uint64_t generation; /* Số thế hệ tăng dần toàn cục chống fd reuse. */
    ByteBuffer input;
    ByteBuffer output;    /* Frame response chờ gửi, giữ nguyên thứ tự. */
    size_t output_offset; /* Số byte đầu queue đã gửi thành công. */
    int64_t output_last_progress_ms; /* CLOCK_MONOTONIC khi queue bắt đầu/chuyển byte. */
    int64_t close_deadline_ms; /* Hạn xả response cuối trước khi buộc đóng. */
    int64_t last_activity_ms;  /* Mốc CLOCK_MONOTONIC khi nhận byte/frame hợp lệ cuối. */
    bool pending_frame;
    bool peer_eof;       /* Peer đã ngừng gửi; vẫn có thể nhận response đang chờ. */
    bool close_after_flush; /* Không nhận request mới sau lỗi nghiêm trọng/close hợp lệ. */
    bool ready_queued; /* Chống xếp connection.ready lần thứ hai cùng socket. */
    bool heartbeat_outstanding; /* Đang chờ client gửi heartbeat.pong. */
    char heartbeat_nonce[33];   /* Nonce hex32 của ping đang chờ pong. */
    int64_t heartbeat_deadline_ms; /* Hạn 10s nhận pong theo CLOCK_MONOTONIC. */
    unsigned int invalid_pong_count; /* Đếm số lần pong sai nonce; >= 3 đóng socket. */
    uint64_t high_water_request_id;  /* Request ID cao nhất từng quan sát trong connection. */
    RequestCache request_cache;      /* Cache FIFO tối đa 64 entries hoặc 256 KiB response. */
    DeliveryTracker delivery_tracker; /* Quản lý cửa sổ in-flight tối đa 32 tin và timer ACK. */
    int64_t delivery_cursor_message_id; /* Con trỏ tạm quét tin direct pending khi in-flight > 0; reset khi session mất. */
    int64_t delivery_cursor_group_message_id; /* Con trỏ tạm quét tin group pending khi in-flight > 0; reset khi session mất. */
    bool delivery_scope_turn;        /* Cờ luân phiên giữa direct (false) và group (true). */
    bool delivery_fatal_timeout;     /* Đánh dấu kết nối bị quá hạn ACK sau 3 lần gửi (kịch bản T37). */
    Database *db;                    /* Database SQLite chia sẻ từ server. */
    SessionManager *session_mgr;     /* Bảng quản lý phiên session toàn server. */
} Connection;

typedef enum {
    CONNECTION_RECEIVE_OK = 0,
    CONNECTION_RECEIVE_EOF,
    CONNECTION_RECEIVE_INVALID_FRAME,
    CONNECTION_RECEIVE_IO_ERROR,
    CONNECTION_RECEIVE_INPUT_FULL,
    CONNECTION_RECEIVE_OUTPUT_FULL,
    CONNECTION_RECEIVE_OUTPUT_TIMEOUT,
    CONNECTION_RECEIVE_CLOSE_AFTER_FLUSH,
    CONNECTION_RECEIVE_HEARTBEAT_TIMEOUT,
    CONNECTION_RECEIVE_DELIVERY_ACK_TIMEOUT
} ConnectionReceiveResult;

/* Bắt đầu với Connection = {0}; thành công mới chuyển quyền sở hữu fd. */
BufferResult connection_init(Connection *connection, int fd);
/* Gắn handle database SQLite cho connection. */
void connection_set_db(Connection *connection, Database *db);
/* Gắn SessionManager toàn server cho connection. */
void connection_set_session_manager(Connection *connection, SessionManager *session_mgr);
/* Lấy bộ theo dõi delivery tracker của connection. */
DeliveryTracker *connection_get_delivery_tracker(Connection *connection);
/* Lấy số thế hệ generation hiện tại của connection. */
uint64_t connection_get_generation(const Connection *connection);
/* Sau accept, xếp đúng một connection.ready trước mọi response/request; POLLOUT gửi sau. */
bool connection_queue_ready(Connection *connection);
/* Xếp frame heartbeat.ping vào output, lưu nonce và deadline nhận pong. */
bool connection_queue_heartbeat_ping(Connection *connection, const char *nonce,
                                     int64_t deadline_ms);
/* Đọc tối đa một chunk TCP; dispatch tối đa 16 frame JSON qua router. */
ConnectionReceiveResult connection_receive(Connection *connection, size_t *frames_processed);
/* Gửi phần output còn lại; EAGAIN giữ offset cho lượt POLLOUT tiếp theo. */
bool connection_flush(Connection *connection);
/* Queue có byte nhưng 15 giây không gửi tiến: peer không đọc hoặc đường gửi kẹt. */
bool connection_output_timed_out(const Connection *connection);
/* Đóng sau tối đa 2 giây dù client không đọc response cuối. */
bool connection_close_timed_out(const Connection *connection);
/* Hết hạn chờ heartbeat.pong khớp nonce. */
bool connection_heartbeat_timed_out(const Connection *connection);
/* Quá 30 giây im lặng không gửi frame/byte: cần server phát heartbeat.ping. */
bool connection_needs_heartbeat_ping(const Connection *connection);
/* Quá hạn nhận delivery.ack sau 3 lần gửi (kịch bản T37). */
bool connection_delivery_fatal_timed_out(const Connection *connection);
void connection_set_delivery_fatal_timeout(Connection *connection, bool timed_out);
/* Đóng fd, giải phóng buffer và đặt lại trạng thái. */
void connection_destroy(Connection *connection);

#endif
