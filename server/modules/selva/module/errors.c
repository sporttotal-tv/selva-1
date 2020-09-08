#include "redismodule.h"
#include "linker_set.h"
#include "errors.h"

const char * const hierarchyStrError[] = {
    [0]                                 = (const char *)"ERR_SELVA No Error",
    [-SELVA_EGENERAL]                   = (const char *)"ERR_SELVA EGENERAL Unknown error",
    [-SELVA_MODIFY_HIERARCHY_EGENERAL]  = (const char *)"ERR_HIERARCHY EGENERAL Unknown error",
    [-SELVA_MODIFY_HIERARCHY_ENOTSUP]   = (const char *)"ERR_HIERARCHY ENOTSUP Operation not supported",
    [-SELVA_MODIFY_HIERARCHY_EINVAL]    = (const char *)"ERR_HIERARCHY EINVAL Invalid argument or input value",
    [-SELVA_MODIFY_HIERARCHY_ENOMEM]    = (const char *)"ERR_HIERARCHY ENOMEM Out of memory",
    [-SELVA_MODIFY_HIERARCHY_ENOENT]    = (const char *)"ERR_HIERARCHY ENOENT Not found",
    [-SELVA_MODIFY_HIERARCHY_EEXIST]    = (const char *)"ERR_HIERARCHY EEXIST Exist",
    [-SELVA_SUBSCRIPTIONS_EGENERAL]     = (const char *)"ERR_SUBSCRIPTIONS EGENERAL Unknown error",
    [-SELVA_SUBSCRIPTIONS_EINVAL]       = (const char *)"ERR_SUBSCRIPTIONS EINVAL Invalid argument or input value",
    [-SELVA_SUBSCRIPTIONS_ENOMEM]       = (const char *)"ERR_SUBSCRIPTIONS ENOMEM Out of memory",
    [-SELVA_SUBSCRIPTIONS_ENOENT]       = (const char *)"ERR_SUBSCRIPTIONS ENOENT Not found",
    [-SELVA_SUBSCRIPTIONS_EEXIST]       = (const char *)"ERR_SUBSCRIPTIONS EEXIST Exist",
    [-SELVA_INVALID_ERROR]              = (const char *)"ERR_SELVA Invalid error code"
};

int replyWithSelvaError(RedisModuleCtx *ctx, int err) {
    if (err >= 0 || -err >= (int)num_elem(hierarchyStrError)) {
        return RedisModule_ReplyWithError(ctx, hierarchyStrError[-SELVA_EGENERAL]);
    }
    return RedisModule_ReplyWithError(ctx, hierarchyStrError[-err]);
}

