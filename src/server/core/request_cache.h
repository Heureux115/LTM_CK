#ifndef TCP_CHAT_REQUEST_CACHE_H
#define TCP_CHAT_REQUEST_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "shared/limits.h"

typedef enum {
    CACHE_LOOKUP_NEW = 0,
    CACHE_LOOKUP_MATCH,
    CACHE_LOOKUP_REUSED,
    CACHE_LOOKUP_EXPIRED
} CacheLookupResult;

typedef struct {
    uint64_t request_id;
    char *type;
    char *data_json;
    unsigned char *response_frame;
    size_t response_frame_length;
} RequestCacheEntry;

typedef struct {
    RequestCacheEntry entries[CHAT_REQUEST_CACHE_MAX_ENTRIES];
    size_t count;
    size_t head;
    size_t total_bytes;
} RequestCache;

void request_cache_init(RequestCache *cache);
void request_cache_destroy(RequestCache *cache);

/* Kiểm tra request_id đối với high_water và cache */
CacheLookupResult request_cache_check(const RequestCache *cache, uint64_t high_water,
                                      uint64_t request_id, const char *type,
                                      const char *data_json,
                                      const RequestCacheEntry **entry_out);

/* Lưu trữ response frame vừa sinh vào cache */
bool request_cache_put(RequestCache *cache, uint64_t request_id,
                       const char *type, const char *data_json,
                       const unsigned char *response_frame, size_t frame_length);

#endif
