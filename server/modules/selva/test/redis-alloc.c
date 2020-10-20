#include <stdlib.h>
#include "sds.h"

struct redisObjectAccessor {
    uint32_t _meta;
    int refcount;
    void *ptr;
};

void *_RedisModule_Alloc(size_t size) {
    return calloc(1, size);
}

void *_RedisModule_Calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

void *_RedisModule_Realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void _RedisModule_Free(void *ptr) {
    free(ptr);
}

struct RedisModuleString *RedisModule_CreateString(struct RedisModuleCtx *ctx __unused, const char *ptr, size_t len) {
	struct redisObjectAccessor *robj;

    robj = calloc(1, sizeof(*robj));
    if (!robj) {
        abort();
    }

    robj->ptr = sdsnewlen(ptr, len);
    if (!robj->ptr) {
        abort();
    }

    return (struct RedisModuleString *)robj;
}

/*
 * Partilally copied from redis-server module.c
 */
const char *RedisModule_StringPtrLen(struct RedisModuleString *str, size_t *len) {
    struct redisObjectAccessor *robj = (struct redisObjectAccessor *)str;

	if (!str) {
		static const char errmsg[] = "(NULL string reply referenced in module)";

		if (len) {
            *len = sizeof(errmsg) - 1;
        }

		return errmsg;
	}

	if (len) {
        *len = sdslen(robj->ptr);
    }

	return robj->ptr;
}

void * (*RedisModule_Alloc)(size_t size) = _RedisModule_Alloc;
void* (*RedisModule_Calloc)(size_t nmemb, size_t size) = _RedisModule_Calloc;
void * (*RedisModule_Realloc)(void *ptr, size_t size) = _RedisModule_Realloc;
void (*RedisModule_Free)(void *ptr) = _RedisModule_Free;