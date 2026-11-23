#define _POSIX_C_SOURCE 200809L

#include "server/core/request_cache.h"

#include <stdlib.h>
#include <string.h>

void request_cache_init(RequestCache *cache)
{
    if (cache == NULL) {
        return;
    }
    memset(cache, 0, sizeof(*cache));
}

void request_cache_destroy(RequestCache *cache)
{
    if (cache == NULL) {
        return;
    }
    for (size_t i = 0; i < cache->count; ++i) {
        size_t idx = (cache->head + i) % CHAT_REQUEST_CACHE_MAX_ENTRIES;
        free(cache->entries[idx].type);
        free(cache->entries[idx].data_json);
        free(cache->entries[idx].response_frame);
    }
    memset(cache, 0, sizeof(*cache));
}

CacheLookupResult request_cache_check(const RequestCache *cache, uint64_t high_water,
                                      uint64_t request_id, const char *type,
                                      const char *data_json,
                                      const RequestCacheEntry **entry_out)
{
    if (cache == NULL || type == NULL || data_json == NULL) {
        return CACHE_LOOKUP_NEW;
    }
    if (request_id > high_water) {
        return CACHE_LOOKUP_NEW;
    }
    /* Tìm trong cache từ mới nhất về cũ nhất */
    for (size_t i = cache->count; i > 0; --i) {
        size_t idx = (cache->head + i - 1) % CHAT_REQUEST_CACHE_MAX_ENTRIES;
        if (cache->entries[idx].request_id == request_id) {
            if (strcmp(cache->entries[idx].type, type) == 0 &&
                strcmp(cache->entries[idx].data_json, data_json) == 0) {
                if (entry_out != NULL) {
                    *entry_out = &cache->entries[idx];
                }
                return CACHE_LOOKUP_MATCH;
            }
            return CACHE_LOOKUP_REUSED;
        }
    }
    return CACHE_LOOKUP_EXPIRED;
}

static void evict_oldest(RequestCache *cache)
{
    if (cache->count == 0) {
        return;
    }
    RequestCacheEntry *oldest = &cache->entries[cache->head];
    size_t entry_bytes = sizeof(RequestCacheEntry) +
                         (oldest->type ? strlen(oldest->type) + 1 : 0) +
                         (oldest->data_json ? strlen(oldest->data_json) + 1 : 0) +
                         oldest->response_frame_length;
    free(oldest->type);
    free(oldest->data_json);
    free(oldest->response_frame);
    memset(oldest, 0, sizeof(*oldest));
    if (cache->total_bytes >= entry_bytes) {
        cache->total_bytes -= entry_bytes;
    } else {
        cache->total_bytes = 0;
    }
    cache->head = (cache->head + 1) % CHAT_REQUEST_CACHE_MAX_ENTRIES;
    --cache->count;
}

bool request_cache_put(RequestCache *cache, uint64_t request_id,
                       const char *type, const char *data_json,
                       const unsigned char *response_frame, size_t frame_length)
{
    if (cache == NULL || type == NULL || data_json == NULL ||
        response_frame == NULL || frame_length == 0) {
        return false;
    }
    size_t new_bytes = sizeof(RequestCacheEntry) + strlen(type) + 1 +
                       strlen(data_json) + 1 + frame_length;
    /* Nếu một frame vượt quá trần toàn cache thì không lưu */
    if (new_bytes > CHAT_REQUEST_CACHE_MAX_BYTES) {
        return false;
    }
    /* Thu hồi bớt các entry cũ nhất nếu vượt giới hạn số lượng hoặc dung lượng */
    while (cache->count >= CHAT_REQUEST_CACHE_MAX_ENTRIES ||
           cache->total_bytes + new_bytes > CHAT_REQUEST_CACHE_MAX_BYTES) {
        evict_oldest(cache);
    }

    char *type_dup = strdup(type);
    char *data_dup = strdup(data_json);
    unsigned char *frame_dup = malloc(frame_length);
    if (type_dup == NULL || data_dup == NULL || frame_dup == NULL) {
        free(type_dup);
        free(data_dup);
        free(frame_dup);
        return false;
    }
    memcpy(frame_dup, response_frame, frame_length);

    size_t insert_idx = (cache->head + cache->count) % CHAT_REQUEST_CACHE_MAX_ENTRIES;
    cache->entries[insert_idx] = (RequestCacheEntry){
        .request_id = request_id,
        .type = type_dup,
        .data_json = data_dup,
        .response_frame = frame_dup,
        .response_frame_length = frame_length
    };
    ++cache->count;
    cache->total_bytes += new_bytes;
    return true;
}
