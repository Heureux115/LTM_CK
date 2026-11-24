#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/frame.h"
#include "shared/protocol.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void expect_rejected(const unsigned char *body, size_t length,
                            ProtocolResult expected, const char *message)
{
    ProtocolRequest request = {0};
    check(protocol_parse_request(body, length, &request) == expected &&
              (expected == PROTOCOL_UNSUPPORTED_VERSION ? request.document != NULL :
                                                         request.document == NULL),
          message);
    protocol_request_destroy(&request);
}

static void test_valid_envelope_and_ping_response(void)
{
    const char body[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                        "\"request_id\":\"42\",\"data\":{}}";
    ProtocolRequest request = {0};
    check(protocol_parse_request((const unsigned char *)body, strlen(body), &request) ==
              PROTOCOL_OK && strcmp(request.type, "system.ping") == 0 &&
              strcmp(request.request_id, "42") == 0 && cJSON_IsObject(request.data),
          "parse valid request envelope and keep its fields alive");

    ByteBuffer output = {0};
    check(buffer_init(&output, 512) == BUFFER_OK, "allocate response frame buffer");
    check(protocol_encode_ping_response(&request, 1792054800000LL, &output) == PROTOCOL_OK,
          "encode ping response as a length-prefixed frame");
    FrameView view = {0};
    check(frame_peek(&output, &view) == FRAME_OK, "response frame is complete");
    char json[512];
    check(view.length < sizeof(json), "response fits test JSON buffer");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *response = cJSON_Parse(json);
    check(response != NULL, "response body is valid JSON");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(response, "kind");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(response, "request_id");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    const cJSON *server_time = cJSON_GetObjectItemCaseSensitive(data, "server_time");
    check(cJSON_IsString(kind) && strcmp(kind->valuestring, "response") == 0 &&
              cJSON_IsString(request_id) && strcmp(request_id->valuestring, "42") == 0 &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) &&
              cJSON_IsNumber(server_time) &&
              server_time->valuedouble == 1792054800000.0,
          "response keeps request ID and returns server time");
    cJSON_Delete(response);
    buffer_destroy(&output);
    protocol_request_destroy(&request);
    check(request.document == NULL && request.type == NULL,
          "destroy clears pointers borrowed from parsed JSON");
}

static void test_invalid_json_and_unsafe_strings(void)
{
    const char malformed[] = "{\"v\":1";
    const char trailing[] = "{} garbage";
    const unsigned char nul_body[] = {'{', '}', 0, '{', '}'};
    const unsigned char bad_utf8[] = {'{', '"', 'x', '"', ':', '"', 0xc0, 0xaf, '"', '}'};
    const char escaped_nul[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"x\","
                               "\"request_id\":\"1\",\"data\":{\"x\":\"\\u0000\"}}";
    const unsigned char bom[] = {0xef, 0xbb, 0xbf, '{', '}'};
    expect_rejected((const unsigned char *)malformed, strlen(malformed),
                    PROTOCOL_INVALID_JSON, "reject truncated JSON");
    expect_rejected((const unsigned char *)trailing, strlen(trailing),
                    PROTOCOL_INVALID_JSON, "reject trailing garbage");
    expect_rejected(nul_body, sizeof(nul_body), PROTOCOL_INVALID_JSON,
                    "reject raw embedded NUL");
    expect_rejected(bad_utf8, sizeof(bad_utf8), PROTOCOL_INVALID_JSON,
                    "reject overlong UTF-8");
    expect_rejected((const unsigned char *)escaped_nul, strlen(escaped_nul),
                    PROTOCOL_INVALID_JSON, "reject escaped NUL before cJSON truncates it");
    expect_rejected(bom, sizeof(bom), PROTOCOL_INVALID_JSON, "reject UTF-8 BOM");
}

static void test_envelope_limits(void)
{
    const char duplicate[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                             "\"request_id\":\"1\",\"data\":{\"x\":1,\"x\":2}}";
    const char unknown[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                           "\"request_id\":\"1\",\"data\":{},\"extra\":1}";
    const char bad_id[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                            "\"request_id\":\"01\",\"data\":{}}";
    const char overflow_id[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                                 "\"request_id\":\"18446744073709551616\",\"data\":{}}";
    const char unsupported[] = "{\"v\":2,\"kind\":\"request\",\"type\":\"system.ping\","
                               "\"request_id\":\"1\",\"data\":{}}";
    const char fractional_version[] = "{\"v\":1.5,\"kind\":\"request\","
                                      "\"type\":\"system.ping\",\"request_id\":\"1\","
                                      "\"data\":{}}";
    const char deep[] = "[[[[[[[[[]]]]]]]]]";
    expect_rejected((const unsigned char *)duplicate, strlen(duplicate),
                    PROTOCOL_INVALID_REQUEST, "reject duplicate nested object keys");
    expect_rejected((const unsigned char *)unknown, strlen(unknown),
                    PROTOCOL_INVALID_REQUEST, "reject unknown envelope field");
    expect_rejected((const unsigned char *)bad_id, strlen(bad_id),
                    PROTOCOL_INVALID_REQUEST, "reject noncanonical request ID");
    expect_rejected((const unsigned char *)overflow_id, strlen(overflow_id),
                    PROTOCOL_INVALID_REQUEST, "reject request ID above uint64 maximum");
    expect_rejected((const unsigned char *)unsupported, strlen(unsupported),
                    PROTOCOL_UNSUPPORTED_VERSION, "report unsupported protocol version");
    expect_rejected((const unsigned char *)fractional_version, strlen(fractional_version),
                    PROTOCOL_INVALID_REQUEST, "reject fractional protocol version");
    expect_rejected((const unsigned char *)deep, strlen(deep),
                    PROTOCOL_INVALID_REQUEST, "reject depth above eight before parsing");

    char many_keys[512] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                          "\"request_id\":\"1\",\"data\":{";
    for (int index = 0; index < 33; ++index) {
        char key[16];
        int written = snprintf(key, sizeof(key), "%s\"k%d\":0", index ? "," : "", index);
        check(written > 0 && (size_t)written < sizeof(key) &&
                  strlen(many_keys) + (size_t)written + 2 < sizeof(many_keys),
              "build test object with thirty-three keys");
        strcat(many_keys, key);
    }
    strcat(many_keys, "}}");
    expect_rejected((const unsigned char *)many_keys, strlen(many_keys),
                    PROTOCOL_INVALID_REQUEST, "reject more than thirty-two keys in one object");
}

static void test_data_validation_is_separate(void)
{
    const char body[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                        "\"request_id\":\"1\",\"data\":{\"unexpected\":true}}";
    ProtocolRequest request = {0};
    check(protocol_parse_request((const unsigned char *)body, strlen(body), &request) ==
              PROTOCOL_OK,
          "generic parser accepts data object for later type-specific validation");
    ByteBuffer output = {0};
    check(buffer_init(&output, 256) == BUFFER_OK, "allocate small ping output");
    check(protocol_encode_ping_response(&request, 1, &output) ==
              PROTOCOL_INVALID_REQUEST && output.length == 0,
          "ping response rejects unexpected ping data without writing output");
    buffer_destroy(&output);
    protocol_request_destroy(&request);

    const char empty_ping[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                              "\"request_id\":\"1\",\"data\":{}}";
    check(protocol_parse_request((const unsigned char *)empty_ping,
                                 strlen(empty_ping), &request) == PROTOCOL_OK,
          "parse ping with empty data for output-cap test");
    check(buffer_init(&output, 8) == BUFFER_OK, "allocate undersized output buffer");
    check(protocol_encode_ping_response(&request, 1, &output) == PROTOCOL_OUTPUT_FULL &&
              output.length == 0,
          "response framing refuses full queue without partial bytes");
    buffer_destroy(&output);
    protocol_request_destroy(&request);
}

static void test_error_envelopes(void)
{
    const char body[] = "{\"v\":2,\"kind\":\"request\",\"type\":\"system.ping\","
                        "\"request_id\":\"7\",\"data\":{}}";
    ProtocolRequest request = {0};
    check(protocol_parse_request((const unsigned char *)body, strlen(body), &request) ==
              PROTOCOL_UNSUPPORTED_VERSION && strcmp(request.request_id, "7") == 0,
          "unsupported version keeps a trustworthy request ID");

    ByteBuffer output = {0};
    check(buffer_init(&output, 512) == BUFFER_OK,
          "allocate output for two protocol error frames");
    check(protocol_encode_error_response(&request, PROTOCOL_ERROR_UNSUPPORTED_VERSION,
                                         &output) == PROTOCOL_OK,
          "encode correlated unsupported-version response");
    FrameView view = {0};
    check(frame_peek(&output, &view) == FRAME_OK, "error response is framed");
    char json[512];
    check(view.length < sizeof(json), "error response fits JSON test buffer");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *envelope = cJSON_Parse(json);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(envelope, "error");
    check(envelope != NULL &&
              cJSON_IsString(cJSON_GetObjectItemCaseSensitive(envelope, "kind")) &&
              strcmp(cJSON_GetObjectItemCaseSensitive(envelope, "kind")->valuestring,
                     "response") == 0 &&
              strcmp(cJSON_GetObjectItemCaseSensitive(envelope, "request_id")->valuestring,
                     "7") == 0 &&
              cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(envelope, "ok")) &&
              strcmp(cJSON_GetObjectItemCaseSensitive(error, "code")->valuestring,
                     "UNSUPPORTED_VERSION") == 0 &&
              cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(error, "retryable")),
          "correlated error follows response envelope");
    cJSON_Delete(envelope);
    check(frame_consume(&output) == FRAME_OK, "consume correlated error");

    check(protocol_encode_protocol_error(PROTOCOL_ERROR_INVALID_JSON, &output) == PROTOCOL_OK &&
              frame_peek(&output, &view) == FRAME_OK && view.length < sizeof(json),
          "encode uncorrelated protocol.error event");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    envelope = cJSON_Parse(json);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(envelope, "data");
    check(envelope != NULL &&
              strcmp(cJSON_GetObjectItemCaseSensitive(envelope, "kind")->valuestring,
                     "event") == 0 &&
              strcmp(cJSON_GetObjectItemCaseSensitive(envelope, "type")->valuestring,
                     "protocol.error") == 0 &&
              cJSON_GetObjectItemCaseSensitive(envelope, "request_id") == NULL &&
              strcmp(cJSON_GetObjectItemCaseSensitive(data, "code")->valuestring,
                     "INVALID_JSON") == 0,
          "protocol.error has no request ID");
    cJSON_Delete(envelope);
    buffer_destroy(&output);
    protocol_request_destroy(&request);
}

static void test_close_response(void)
{
    const char body[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"connection.close\","
                        "\"request_id\":\"8\",\"data\":{}}";
    ProtocolRequest request = {0};
    check(protocol_parse_request((const unsigned char *)body, strlen(body), &request) ==
              PROTOCOL_OK, "parse connection.close request");
    ByteBuffer output = {0};
    check(buffer_init(&output, 256) == BUFFER_OK &&
              protocol_encode_close_response(&request, &output) == PROTOCOL_OK,
          "encode connection.close success frame");
    FrameView view = {0};
    check(frame_peek(&output, &view) == FRAME_OK, "close response frame is complete");
    char json[256];
    check(view.length < sizeof(json), "close response fits test buffer");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *response = cJSON_Parse(json);
    check(response != NULL, "close response contains valid JSON");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(response, "request_id");
    check(cJSON_IsString(type) && strcmp(type->valuestring, "connection.close") == 0 &&
              cJSON_IsString(request_id) && strcmp(request_id->valuestring, "8") == 0 &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "closing")),
          "close response keeps ID and confirms closing true");
    cJSON_Delete(response);
    buffer_destroy(&output);
    protocol_request_destroy(&request);
}

static void test_ready_event(void)
{
    ByteBuffer output = {0};
    check(buffer_init(&output, 512) == BUFFER_OK &&
              protocol_encode_ready_event(1792054800000LL, &output) == PROTOCOL_OK,
          "encode connection.ready after accept");
    FrameView view = {0};
    check(frame_peek(&output, &view) == FRAME_OK &&
              protocol_validate_ready_event(view.body, view.length) == PROTOCOL_OK,
          "client accepts the framed ready event and its version/limit");
    const char bad_version[] = "{\"v\":1,\"kind\":\"event\",\"type\":\"connection.ready\","
                               "\"data\":{\"protocol_version\":2,\"max_frame_bytes\":65536,"
                               "\"server_time\":1}}";
    const char bad_limit[] = "{\"v\":1,\"kind\":\"event\",\"type\":\"connection.ready\","
                             "\"data\":{\"protocol_version\":1,\"max_frame_bytes\":999,"
                             "\"server_time\":1}}";
    const char extra_field[] = "{\"v\":1,\"kind\":\"event\",\"type\":\"connection.ready\","
                               "\"data\":{\"protocol_version\":1,\"max_frame_bytes\":65536,"
                               "\"server_time\":1,\"extra\":true}}";
    check(protocol_validate_ready_event((const unsigned char *)bad_version,
                                        strlen(bad_version)) == PROTOCOL_INVALID_REQUEST &&
              protocol_validate_ready_event((const unsigned char *)bad_limit,
                                            strlen(bad_limit)) == PROTOCOL_INVALID_REQUEST &&
              protocol_validate_ready_event((const unsigned char *)extra_field,
                                            strlen(extra_field)) == PROTOCOL_INVALID_REQUEST,
          "client rejects mismatched version, frame limit and unknown ready field");
    buffer_destroy(&output);
    check(buffer_init(&output, 8) == BUFFER_OK &&
              protocol_encode_ready_event(1, &output) == PROTOCOL_OUTPUT_FULL &&
              output.length == 0,
          "ready event does not leave a partial frame when output is full");
    buffer_destroy(&output);
}

static void test_heartbeat_ping_event(void)
{
    const char valid_nonce[] = "abcdef0123456789abcdef0123456789";
    const char short_nonce[] = "abcdef0123456789";
    const char upper_nonce[] = "ABCDEF0123456789ABCDEF0123456789";
    const char non_hex[] = "abcdef0123456789abcdef012345678z";

    check(protocol_validate_nonce_hex(valid_nonce), "valid 32-char lowercase hex nonce");
    check(!protocol_validate_nonce_hex(short_nonce), "reject short nonce");
    check(!protocol_validate_nonce_hex(upper_nonce), "reject uppercase hex nonce");
    check(!protocol_validate_nonce_hex(non_hex), "reject non-hex character in nonce");
    check(!protocol_validate_nonce_hex(NULL), "reject null pointer nonce");

    ByteBuffer output = {0};
    check(buffer_init(&output, 512) == BUFFER_OK, "allocate heartbeat ping output buffer");
    check(protocol_encode_heartbeat_ping(valid_nonce, &output) == PROTOCOL_OK,
          "encode heartbeat.ping event into frame");
    FrameView view = {0};
    check(frame_peek(&output, &view) == FRAME_OK, "heartbeat.ping frame is complete");

    char json[512];
    check(view.length < sizeof(json), "body fits test buffer");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *doc = cJSON_Parse(json);
    check(doc != NULL, "heartbeat.ping is valid JSON");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, "v");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(doc, "kind");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(doc, "type");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(doc, "data");
    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(data, "nonce");
    check(cJSON_IsNumber(v) && v->valuedouble == 1.0 &&
              cJSON_IsString(kind) && strcmp(kind->valuestring, "event") == 0 &&
              cJSON_IsString(type) && strcmp(type->valuestring, "heartbeat.ping") == 0 &&
              cJSON_IsObject(data) && cJSON_GetArraySize(data) == 1 &&
              cJSON_IsString(nonce) && strcmp(nonce->valuestring, valid_nonce) == 0,
          "heartbeat.ping matches envelope and data shape");
    cJSON_Delete(doc);
    buffer_destroy(&output);

    check(buffer_init(&output, 8) == BUFFER_OK &&
              protocol_encode_heartbeat_ping(valid_nonce, &output) == PROTOCOL_OUTPUT_FULL &&
              output.length == 0,
          "heartbeat.ping respects buffer capacity");
    buffer_destroy(&output);
}

static void test_heartbeat_pong_control(void)
{
    const char valid_pong[] = "{\"v\":1,\"kind\":\"control\",\"type\":\"heartbeat.pong\","
                              "\"data\":{\"nonce\":\"abcdef0123456789abcdef0123456789\"}}";
    char nonce_out[33] = {0};
    check(protocol_parse_heartbeat_pong((const unsigned char *)valid_pong,
                                        strlen(valid_pong), nonce_out) == PROTOCOL_OK &&
              strcmp(nonce_out, "abcdef0123456789abcdef0123456789") == 0,
          "parse valid heartbeat.pong control frame");

    const char bad_ver[] = "{\"v\":2,\"kind\":\"control\",\"type\":\"heartbeat.pong\","
                           "\"data\":{\"nonce\":\"abcdef0123456789abcdef0123456789\"}}";
    const char bad_kind[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"heartbeat.pong\","
                            "\"data\":{\"nonce\":\"abcdef0123456789abcdef0123456789\"}}";
    const char bad_type[] = "{\"v\":1,\"kind\":\"control\",\"type\":\"heartbeat.other\","
                            "\"data\":{\"nonce\":\"abcdef0123456789abcdef0123456789\"}}";
    const char bad_nonce_len[] = "{\"v\":1,\"kind\":\"control\",\"type\":\"heartbeat.pong\","
                                 "\"data\":{\"nonce\":\"abcdef\"}}";
    const char bad_nonce_case[] = "{\"v\":1,\"kind\":\"control\",\"type\":\"heartbeat.pong\","
                                  "\"data\":{\"nonce\":\"ABCDEF0123456789ABCDEF0123456789\"}}";
    const char extra_data[] = "{\"v\":1,\"kind\":\"control\",\"type\":\"heartbeat.pong\","
                              "\"data\":{\"nonce\":\"abcdef0123456789abcdef0123456789\",\"extra\":1}}";
    const char not_json[] = "not-a-json";

    check(protocol_parse_heartbeat_pong((const unsigned char *)bad_ver,
                                        strlen(bad_ver), nonce_out) == PROTOCOL_INVALID_REQUEST &&
              protocol_parse_heartbeat_pong((const unsigned char *)bad_kind,
                                            strlen(bad_kind), nonce_out) == PROTOCOL_INVALID_REQUEST &&
              protocol_parse_heartbeat_pong((const unsigned char *)bad_type,
                                            strlen(bad_type), nonce_out) == PROTOCOL_INVALID_REQUEST &&
              protocol_parse_heartbeat_pong((const unsigned char *)bad_nonce_len,
                                            strlen(bad_nonce_len), nonce_out) == PROTOCOL_INVALID_REQUEST &&
              protocol_parse_heartbeat_pong((const unsigned char *)bad_nonce_case,
                                            strlen(bad_nonce_case), nonce_out) == PROTOCOL_INVALID_REQUEST &&
              protocol_parse_heartbeat_pong((const unsigned char *)extra_data,
                                            strlen(extra_data), nonce_out) == PROTOCOL_INVALID_REQUEST &&
              protocol_parse_heartbeat_pong((const unsigned char *)not_json,
                                            strlen(not_json), nonce_out) == PROTOCOL_INVALID_JSON,
          "reject malformed heartbeat.pong variants");
}

static void test_heartbeat_helpers_and_symmetric_codecs(void)
{
    char gen_nonce[33] = {0};
    check(protocol_generate_nonce_hex(gen_nonce, sizeof(gen_nonce)),
          "generate 32-hex random nonce");
    check(protocol_validate_nonce_hex(gen_nonce),
          "generated nonce is valid 32-hex lowercase");
    check(!protocol_generate_nonce_hex(gen_nonce, 32),
          "reject buffer too small for generated nonce");

    /* Test client parse heartbeat.ping event */
    const char ping_event[] = "{\"v\":1,\"kind\":\"event\",\"type\":\"heartbeat.ping\","
                              "\"data\":{\"nonce\":\"abcdef0123456789abcdef0123456789\"}}";
    char parsed_ping_nonce[33] = {0};
    check(protocol_parse_heartbeat_ping((const unsigned char *)ping_event,
                                        strlen(ping_event), parsed_ping_nonce) == PROTOCOL_OK &&
              strcmp(parsed_ping_nonce, "abcdef0123456789abcdef0123456789") == 0,
          "client parses valid heartbeat.ping event");

    const char bad_ping_event[] = "{\"v\":1,\"kind\":\"event\",\"type\":\"heartbeat.ping\","
                                  "\"data\":{\"nonce\":\"bad\"}}";
    check(protocol_parse_heartbeat_ping((const unsigned char *)bad_ping_event,
                                        strlen(bad_ping_event), parsed_ping_nonce) == PROTOCOL_INVALID_REQUEST,
          "client rejects malformed heartbeat.ping event");

    /* Test client encode heartbeat.pong control frame */
    ByteBuffer output = {0};
    check(buffer_init(&output, 512) == BUFFER_OK, "allocate pong frame buffer");
    check(protocol_encode_heartbeat_pong("abcdef0123456789abcdef0123456789", &output) == PROTOCOL_OK,
          "client encodes heartbeat.pong control frame");
    FrameView view = {0};
    check(frame_peek(&output, &view) == FRAME_OK, "pong frame is complete");

    char parsed_pong_nonce[33] = {0};
    check(protocol_parse_heartbeat_pong(view.body, view.length, parsed_pong_nonce) == PROTOCOL_OK &&
              strcmp(parsed_pong_nonce, "abcdef0123456789abcdef0123456789") == 0,
          "encoded pong frame can be parsed back by server");
    buffer_destroy(&output);
}

static void test_request_id_parsing_and_cache_errors(void)
{
    uint64_t id = 0;
    check(protocol_parse_request_id("1", &id) && id == 1, "parse request_id '1'");
    check(protocol_parse_request_id("42", &id) && id == 42, "parse request_id '42'");
    check(protocol_parse_request_id("18446744073709551615", &id) && id == UINT64_MAX,
          "parse max uint64 request_id");

    check(!protocol_parse_request_id(NULL, &id), "reject NULL request_id");
    check(!protocol_parse_request_id("", &id), "reject empty request_id");
    check(!protocol_parse_request_id("0", &id), "reject request_id '0'");
    check(!protocol_parse_request_id("01", &id), "reject request_id with leading zero");
    check(!protocol_parse_request_id("-1", &id), "reject negative request_id");
    check(!protocol_parse_request_id("42a", &id), "reject non-digit request_id");
    check(!protocol_parse_request_id("18446744073709551616", &id),
          "reject overflow request_id > UINT64_MAX");

    /* Test encode REQUEST_ID_REUSED & REQUEST_ID_EXPIRED error envelopes */
    const char req_body[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                            "\"request_id\":\"5\",\"data\":{}}";
    ProtocolRequest req = {0};
    check(protocol_parse_request((const unsigned char *)req_body, strlen(req_body), &req) == PROTOCOL_OK,
          "parse sample ping request for error encoding");

    ByteBuffer out = {0};
    check(buffer_init(&out, 512) == BUFFER_OK, "allocate error response buffer");

    check(protocol_encode_error_response(&req, PROTOCOL_ERROR_REQUEST_ID_REUSED, &out) == PROTOCOL_OK,
          "encode REQUEST_ID_REUSED error response");
    FrameView view = {0};
    check(frame_peek(&out, &view) == FRAME_OK, "frame is valid");
    char json[512];
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *resp = cJSON_Parse(json);
    check(resp != NULL, "response is valid JSON");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    check(err != NULL, "contains error object");
    check(strcmp(cJSON_GetObjectItemCaseSensitive(err, "code")->valuestring, "REQUEST_ID_REUSED") == 0,
          "error code is REQUEST_ID_REUSED");
    cJSON_Delete(resp);
    buffer_consume(&out, out.length);

    check(protocol_encode_error_response(&req, PROTOCOL_ERROR_REQUEST_ID_EXPIRED, &out) == PROTOCOL_OK,
          "encode REQUEST_ID_EXPIRED error response");
    check(frame_peek(&out, &view) == FRAME_OK, "frame is valid");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    resp = cJSON_Parse(json);
    check(resp != NULL, "response is valid JSON");
    err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    check(err != NULL, "contains error object");
    check(strcmp(cJSON_GetObjectItemCaseSensitive(err, "code")->valuestring, "REQUEST_ID_EXPIRED") == 0,
          "error code is REQUEST_ID_EXPIRED");
    cJSON_Delete(resp);
    buffer_destroy(&out);
    protocol_request_destroy(&req);
}

int main(void)
{
    test_valid_envelope_and_ping_response();
    test_invalid_json_and_unsafe_strings();
    test_envelope_limits();
    test_data_validation_is_separate();
    test_error_envelopes();
    test_close_response();
    test_ready_event();
    test_heartbeat_ping_event();
    test_heartbeat_pong_control();
    test_heartbeat_helpers_and_symmetric_codecs();
    test_request_id_parsing_and_cache_errors();
    puts("11 protocol tests passed.");
    return EXIT_SUCCESS;
}
