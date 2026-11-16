CC = cc
CPPFLAGS += -Isrc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g -O0

SERVER_ROUTER_DEPS = src/server/core/session.c src/server/core/session.h src/server/core/router.c src/server/core/router.h src/server/core/delivery.c src/server/core/delivery.h src/server/core/delivery_scheduler.c src/server/core/delivery_scheduler.h src/server/core/request_cache.c src/server/core/request_cache.h src/server/logging/logger.c src/server/logging/logger.h src/server/services/auth.c src/server/services/auth.h src/server/services/friends.c src/server/services/friends.h src/server/services/groups.c src/server/services/groups.h src/server/services/messages.c src/server/services/messages.h src/server/services/password.c src/server/services/password.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/repos/friend_repo.c src/server/repos/friend_repo.h src/server/repos/group_repo.c src/server/repos/group_repo.h src/server/repos/message_repo.c src/server/repos/message_repo.h src/server/repos/activity_repo.c src/server/repos/activity_repo.h src/server/db/db.c src/server/db/db.h src/server/net/connection.c src/server/net/connection.h src/shared/protocol.c src/shared/protocol.h src/shared/buffer.c src/shared/buffer.h src/shared/frame.c src/shared/frame.h src/shared/limits.h

SERVER_ROUTER_OBJS = src/server/net/connection.c src/server/core/session.c src/server/core/router.c src/server/core/delivery.c src/server/core/delivery_scheduler.c src/server/core/request_cache.c src/server/logging/logger.c src/server/services/auth.c src/server/services/friends.c src/server/services/groups.c src/server/services/messages.c src/server/services/password.c src/server/repos/user_repo.c src/server/repos/friend_repo.c src/server/repos/group_repo.c src/server/repos/message_repo.c src/server/repos/activity_repo.c src/server/db/db.c src/shared/protocol.c src/shared/buffer.c src/shared/frame.c

.PHONY: all server client test test-sanitize test-load clean demo web

all: server client

server: bin/chat-server

client: bin/chat-client

web: server
	python3 web/gateway.py

test-load: bin/test-system-load
	./bin/test-system-load

test: bin/test-buffer bin/test-frame bin/test-listener bin/test-connection bin/test-client-transport bin/test-protocol bin/test-db bin/test-password bin/test-user-repo bin/test-activity-repo bin/test-logger bin/test-server-lifecycle bin/test-load-benchmark bin/test-demo-scenarios bin/test-auth bin/test-session bin/test-login bin/test-logout bin/test-friend-repo bin/test-friend-request bin/test-friend-request-list bin/test-friend-request-accept bin/test-friend-request-reject bin/test-friend-remove bin/test-friend-list bin/test-presence bin/test-message-repo bin/test-message-send bin/test-message-ack bin/test-message-history bin/test-delivery-tracker bin/test-delivery-scheduler bin/test-delivery-retry bin/test-delivery-e2e bin/test-group-repo bin/test-group-invites bin/test-group-membership bin/test-group-messages bin/test-group-e2e bin/test-system-load
	./bin/test-buffer
	./bin/test-frame
	./bin/test-listener
	./bin/test-connection
	./bin/test-client-transport
	./bin/test-protocol
	./bin/test-db
	./bin/test-password
	./bin/test-user-repo
	./bin/test-activity-repo
	./bin/test-logger
	./bin/test-server-lifecycle
	./bin/test-load-benchmark
	./bin/test-demo-scenarios
	./bin/test-auth
	./bin/test-session
	./bin/test-login
	./bin/test-logout
	./bin/test-friend-repo
	./bin/test-friend-request
	./bin/test-friend-request-list
	./bin/test-friend-request-accept
	./bin/test-friend-request-reject
	./bin/test-friend-remove
	./bin/test-friend-list
	./bin/test-presence
	./bin/test-message-repo
	./bin/test-message-send
	./bin/test-message-ack
	./bin/test-message-history
	./bin/test-delivery-tracker
	./bin/test-delivery-scheduler
	./bin/test-delivery-retry
	./bin/test-delivery-e2e
	./bin/test-group-repo
	./bin/test-group-invites
	./bin/test-group-membership
	./bin/test-group-messages
	./bin/test-group-e2e
	./bin/test-system-load

test-sanitize: bin/test-buffer-sanitize bin/test-frame-sanitize bin/test-listener-sanitize bin/test-connection-sanitize bin/test-client-transport-sanitize bin/test-protocol-sanitize bin/test-db-sanitize bin/test-password-sanitize bin/test-user-repo-sanitize bin/test-activity-repo-sanitize bin/test-logger-sanitize bin/test-server-lifecycle-sanitize bin/test-load-benchmark-sanitize bin/test-demo-scenarios-sanitize bin/test-auth-sanitize bin/test-session-sanitize bin/test-login-sanitize bin/test-logout-sanitize bin/test-friend-repo-sanitize bin/test-friend-request-sanitize bin/test-friend-request-list-sanitize bin/test-friend-request-accept-sanitize bin/test-friend-request-reject-sanitize bin/test-friend-remove-sanitize bin/test-friend-list-sanitize bin/test-presence-sanitize bin/test-message-repo-sanitize bin/test-message-send-sanitize bin/test-message-ack-sanitize bin/test-message-history-sanitize bin/test-delivery-tracker-sanitize bin/test-delivery-scheduler-sanitize bin/test-delivery-retry-sanitize bin/test-delivery-e2e-sanitize bin/test-group-repo-sanitize bin/test-group-invites-sanitize bin/test-group-membership-sanitize bin/test-group-messages-sanitize bin/test-group-e2e-sanitize bin/test-system-load-sanitize
	./bin/test-buffer-sanitize
	./bin/test-frame-sanitize
	./bin/test-listener-sanitize
	./bin/test-connection-sanitize
	./bin/test-client-transport-sanitize
	./bin/test-protocol-sanitize
	./bin/test-db-sanitize
	./bin/test-password-sanitize
	./bin/test-user-repo-sanitize
	./bin/test-activity-repo-sanitize
	./bin/test-logger-sanitize
	./bin/test-server-lifecycle-sanitize
	./bin/test-load-benchmark-sanitize
	./bin/test-demo-scenarios-sanitize
	./bin/test-auth-sanitize
	./bin/test-session-sanitize
	./bin/test-login-sanitize
	./bin/test-logout-sanitize
	./bin/test-friend-repo-sanitize
	./bin/test-friend-request-sanitize
	./bin/test-friend-request-list-sanitize
	./bin/test-friend-request-accept-sanitize
	./bin/test-friend-request-reject-sanitize
	./bin/test-friend-remove-sanitize
	./bin/test-friend-list-sanitize
	./bin/test-presence-sanitize
	./bin/test-message-repo-sanitize
	./bin/test-message-send-sanitize
	./bin/test-message-ack-sanitize
	./bin/test-message-history-sanitize
	./bin/test-delivery-tracker-sanitize
	./bin/test-delivery-scheduler-sanitize
	./bin/test-delivery-retry-sanitize
	./bin/test-delivery-e2e-sanitize
	./bin/test-group-repo-sanitize
	./bin/test-group-invites-sanitize
	./bin/test-group-membership-sanitize
	./bin/test-group-messages-sanitize
	./bin/test-group-e2e-sanitize
	./bin/test-system-load-sanitize

bin/chat-server: src/server/main.c src/server/net/listener.c src/server/net/listener.h $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) src/server/main.c src/server/net/listener.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/chat-server-sanitize: src/server/main.c src/server/net/listener.c src/server/net/listener.h $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer src/server/main.c src/server/net/listener.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/chat-client: src/client/main.c src/client/transport.c src/client/transport.h src/shared/protocol.c src/shared/protocol.h src/shared/buffer.c src/shared/buffer.h src/shared/frame.c src/shared/frame.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) src/client/main.c src/client/transport.c src/shared/protocol.c src/shared/buffer.c src/shared/frame.c $(LDFLAGS) -lcjson $(LDLIBS) -o $@

bin/test-buffer: tests/unit/test_buffer.c src/shared/buffer.c src/shared/buffer.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_buffer.c src/shared/buffer.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-buffer-sanitize: tests/unit/test_buffer.c src/shared/buffer.c src/shared/buffer.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_buffer.c src/shared/buffer.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-frame: tests/unit/test_frame.c src/shared/frame.c src/shared/frame.h src/shared/buffer.c src/shared/buffer.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_frame.c src/shared/frame.c src/shared/buffer.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-frame-sanitize: tests/unit/test_frame.c src/shared/frame.c src/shared/frame.h src/shared/buffer.c src/shared/buffer.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_frame.c src/shared/frame.c src/shared/buffer.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-listener: tests/unit/test_listener.c src/server/net/listener.c src/server/net/listener.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_listener.c src/server/net/listener.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-listener-sanitize: tests/unit/test_listener.c src/server/net/listener.c src/server/net/listener.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_listener.c src/server/net/listener.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-connection: tests/unit/test_connection.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_connection.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-connection-sanitize: tests/unit/test_connection.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_connection.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-client-transport: tests/unit/test_client_transport.c src/client/transport.c src/client/transport.h src/shared/protocol.c src/shared/protocol.h src/shared/buffer.c src/shared/buffer.h src/shared/frame.c src/shared/frame.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_client_transport.c src/client/transport.c src/shared/protocol.c src/shared/buffer.c src/shared/frame.c $(LDFLAGS) -lcjson $(LDLIBS) -o $@

bin/test-client-transport-sanitize: tests/unit/test_client_transport.c src/client/transport.c src/client/transport.h src/shared/protocol.c src/shared/protocol.h src/shared/buffer.c src/shared/buffer.h src/shared/frame.c src/shared/frame.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_client_transport.c src/client/transport.c src/shared/protocol.c src/shared/buffer.c src/shared/frame.c $(LDFLAGS) -lcjson $(LDLIBS) -o $@

bin/test-protocol: tests/unit/test_protocol.c src/shared/protocol.c src/shared/protocol.h src/shared/frame.c src/shared/frame.h src/shared/buffer.c src/shared/buffer.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_protocol.c src/shared/protocol.c src/shared/frame.c src/shared/buffer.c $(LDFLAGS) -lcjson $(LDLIBS) -o $@

bin/test-protocol-sanitize: tests/unit/test_protocol.c src/shared/protocol.c src/shared/protocol.h src/shared/frame.c src/shared/frame.h src/shared/buffer.c src/shared/buffer.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_protocol.c src/shared/protocol.c src/shared/frame.c src/shared/buffer.c $(LDFLAGS) -lcjson $(LDLIBS) -o $@

bin/test-db: tests/unit/test_db.c src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_db.c src/server/db/db.c $(LDFLAGS) -lsqlite3 $(LDLIBS) -o $@

bin/test-db-sanitize: tests/unit/test_db.c src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_db.c src/server/db/db.c $(LDFLAGS) -lsqlite3 $(LDLIBS) -o $@

bin/test-password: tests/unit/test_password.c src/server/services/password.c src/server/services/password.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_password.c src/server/services/password.c $(LDFLAGS) -lsodium $(LDLIBS) -o $@

bin/test-password-sanitize: tests/unit/test_password.c src/server/services/password.c src/server/services/password.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_password.c src/server/services/password.c $(LDFLAGS) -lsodium $(LDLIBS) -o $@

bin/test-user-repo: tests/unit/test_user_repo.c src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_user_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-user-repo-sanitize: tests/unit/test_user_repo.c src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_user_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-activity-repo: tests/unit/test_activity_repo.c src/server/repos/activity_repo.c src/server/repos/activity_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_activity_repo.c src/server/repos/activity_repo.c src/server/repos/user_repo.c src/server/db/db.c $(LDFLAGS) -lsqlite3 $(LDLIBS) -o $@

bin/test-activity-repo-sanitize: tests/unit/test_activity_repo.c src/server/repos/activity_repo.c src/server/repos/activity_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_activity_repo.c src/server/repos/activity_repo.c src/server/repos/user_repo.c src/server/db/db.c $(LDFLAGS) -lsqlite3 $(LDLIBS) -o $@

bin/test-logger: tests/unit/test_logger.c src/server/logging/logger.c src/server/logging/logger.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_logger.c src/server/logging/logger.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-logger-sanitize: tests/unit/test_logger.c src/server/logging/logger.c src/server/logging/logger.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_logger.c src/server/logging/logger.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-server-lifecycle: tests/unit/test_server_lifecycle.c src/server/net/listener.c src/server/net/listener.h $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_server_lifecycle.c src/server/net/listener.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-server-lifecycle-sanitize: tests/unit/test_server_lifecycle.c src/server/net/listener.c src/server/net/listener.h $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_server_lifecycle.c src/server/net/listener.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-load-benchmark: tests/unit/test_load_benchmark.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_load_benchmark.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-load-benchmark-sanitize: tests/unit/test_load_benchmark.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_load_benchmark.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-demo-scenarios: tests/unit/test_demo_scenarios.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_demo_scenarios.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-demo-scenarios-sanitize: tests/unit/test_demo_scenarios.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_demo_scenarios.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-auth: tests/unit/test_auth.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_auth.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-auth-sanitize: tests/unit/test_auth.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_auth.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-session: tests/unit/test_session.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_session.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-session-sanitize: tests/unit/test_session.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_session.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-login: tests/unit/test_login.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_login.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-login-sanitize: tests/unit/test_login.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_login.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-logout: tests/unit/test_logout.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_logout.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-logout-sanitize: tests/unit/test_logout.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_logout.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-repo: tests/unit/test_friend_repo.c src/server/repos/friend_repo.c src/server/repos/friend_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_repo.c src/server/repos/friend_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-repo-sanitize: tests/unit/test_friend_repo.c src/server/repos/friend_repo.c src/server/repos/friend_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_repo.c src/server/repos/friend_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request: tests/unit/test_friend_request.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_request.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-sanitize: tests/unit/test_friend_request.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_request.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-list: tests/unit/test_friend_request_list.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_request_list.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-list-sanitize: tests/unit/test_friend_request_list.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_request_list.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-accept: tests/unit/test_friend_request_accept.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_request_accept.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-accept-sanitize: tests/unit/test_friend_request_accept.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_request_accept.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-reject: tests/unit/test_friend_request_reject.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_request_reject.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-request-reject-sanitize: tests/unit/test_friend_request_reject.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_request_reject.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-remove: tests/unit/test_friend_remove.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_remove.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-remove-sanitize: tests/unit/test_friend_remove.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_remove.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-list: tests/unit/test_friend_list.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_friend_list.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-friend-list-sanitize: tests/unit/test_friend_list.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_friend_list.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-presence: tests/unit/test_presence.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_presence.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-presence-sanitize: tests/unit/test_presence.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_presence.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-repo: tests/unit/test_message_repo.c src/server/repos/message_repo.c src/server/repos/message_repo.h src/server/repos/friend_repo.c src/server/repos/friend_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_message_repo.c src/server/repos/message_repo.c src/server/repos/friend_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-repo-sanitize: tests/unit/test_message_repo.c src/server/repos/message_repo.c src/server/repos/message_repo.h src/server/repos/friend_repo.c src/server/repos/friend_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_message_repo.c src/server/repos/message_repo.c src/server/repos/friend_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-send: tests/unit/test_message_send.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_message_send.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-send-sanitize: tests/unit/test_message_send.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_message_send.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-ack: tests/unit/test_message_ack.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_message_ack.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-ack-sanitize: tests/unit/test_message_ack.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_message_ack.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-history: tests/unit/test_message_history.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_message_history.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-message-history-sanitize: tests/unit/test_message_history.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_message_history.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-delivery-tracker: tests/unit/test_delivery_tracker.c src/server/core/delivery.c src/server/core/delivery.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_delivery_tracker.c src/server/core/delivery.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-delivery-tracker-sanitize: tests/unit/test_delivery_tracker.c src/server/core/delivery.c src/server/core/delivery.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_delivery_tracker.c src/server/core/delivery.c $(LDFLAGS) $(LDLIBS) -o $@

bin/test-delivery-scheduler: tests/unit/test_delivery_scheduler.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_delivery_scheduler.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-delivery-scheduler-sanitize: tests/unit/test_delivery_scheduler.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_delivery_scheduler.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-delivery-retry: tests/unit/test_delivery_retry.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_delivery_retry.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-delivery-retry-sanitize: tests/unit/test_delivery_retry.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_delivery_retry.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-delivery-e2e: tests/unit/test_delivery_e2e.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_delivery_e2e.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-delivery-e2e-sanitize: tests/unit/test_delivery_e2e.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_delivery_e2e.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-repo: tests/unit/test_group_repo.c src/server/repos/group_repo.c src/server/repos/group_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_group_repo.c src/server/repos/group_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-repo-sanitize: tests/unit/test_group_repo.c src/server/repos/group_repo.c src/server/repos/group_repo.h src/server/repos/user_repo.c src/server/repos/user_repo.h src/server/services/password.c src/server/services/password.h src/server/db/db.c src/server/db/db.h src/shared/limits.h | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_group_repo.c src/server/repos/group_repo.c src/server/repos/user_repo.c src/server/services/password.c src/server/db/db.c $(LDFLAGS) -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-invites: tests/unit/test_group_invites.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_group_invites.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-invites-sanitize: tests/unit/test_group_invites.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_group_invites.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-membership: tests/unit/test_group_membership.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_group_membership.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-membership-sanitize: tests/unit/test_group_membership.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_group_membership.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-messages: tests/unit/test_group_messages.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_group_messages.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-messages-sanitize: tests/unit/test_group_messages.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_group_messages.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-e2e: tests/unit/test_group_e2e.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_group_e2e.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-group-e2e-sanitize: tests/unit/test_group_e2e.c $(SERVER_ROUTER_DEPS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_group_e2e.c $(SERVER_ROUTER_OBJS) $(LDFLAGS) -lcjson -lsqlite3 -lsodium $(LDLIBS) -o $@

bin/test-system-load: tests/unit/test_system_load.c src/server/db/db.c src/server/db/db.h src/shared/protocol.c src/shared/protocol.h src/shared/buffer.c src/shared/buffer.h src/shared/frame.c src/shared/frame.h src/shared/limits.h bin/chat-server | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/unit/test_system_load.c src/server/db/db.c src/shared/protocol.c src/shared/buffer.c src/shared/frame.c $(LDFLAGS) -lcjson -lsqlite3 $(LDLIBS) -o $@

bin/test-system-load-sanitize: tests/unit/test_system_load.c src/server/db/db.c src/server/db/db.h src/shared/protocol.c src/shared/protocol.h src/shared/buffer.c src/shared/buffer.h src/shared/frame.c src/shared/frame.h src/shared/limits.h bin/chat-server-sanitize | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit/test_system_load.c src/server/db/db.c src/shared/protocol.c src/shared/buffer.c src/shared/frame.c $(LDFLAGS) -lcjson -lsqlite3 $(LDLIBS) -o $@

bin:
	mkdir -p "$@"

clean:
	$(RM) bin/chat-server bin/chat-server-sanitize bin/chat-client bin/test-buffer bin/test-buffer-sanitize bin/test-frame bin/test-frame-sanitize bin/test-listener bin/test-listener-sanitize bin/test-connection bin/test-connection-sanitize bin/test-client-transport bin/test-client-transport-sanitize bin/test-protocol bin/test-protocol-sanitize bin/test-db bin/test-db-sanitize bin/test-password bin/test-password-sanitize bin/test-user-repo bin/test-user-repo-sanitize bin/test-activity-repo bin/test-activity-repo-sanitize bin/test-logger bin/test-logger-sanitize bin/test-server-lifecycle bin/test-server-lifecycle-sanitize bin/test-load-benchmark bin/test-load-benchmark-sanitize bin/test-demo-scenarios bin/test-demo-scenarios-sanitize bin/test-auth bin/test-auth-sanitize bin/test-session bin/test-session-sanitize bin/test-login bin/test-login-sanitize bin/test-logout bin/test-logout-sanitize bin/test-friend-repo bin/test-friend-repo-sanitize bin/test-friend-request bin/test-friend-request-sanitize bin/test-friend-request-list bin/test-friend-request-list-sanitize bin/test-friend-request-accept bin/test-friend-request-accept-sanitize bin/test-friend-request-reject bin/test-friend-request-reject-sanitize bin/test-friend-remove bin/test-friend-remove-sanitize bin/test-friend-list bin/test-friend-list-sanitize bin/test-presence bin/test-presence-sanitize bin/test-message-repo bin/test-message-repo-sanitize bin/test-message-send bin/test-message-send-sanitize bin/test-message-ack bin/test-message-ack-sanitize bin/test-message-history bin/test-message-history-sanitize bin/test-delivery-tracker bin/test-delivery-tracker-sanitize bin/test-delivery-scheduler bin/test-delivery-scheduler-sanitize bin/test-delivery-retry bin/test-delivery-retry-sanitize bin/test-delivery-e2e bin/test-delivery-e2e-sanitize bin/test-group-repo bin/test-group-repo-sanitize bin/test-group-invites bin/test-group-invites-sanitize bin/test-group-membership bin/test-group-membership-sanitize bin/test-group-messages bin/test-group-messages-sanitize bin/test-group-e2e bin/test-group-e2e-sanitize bin/test-system-load bin/test-system-load-sanitize
