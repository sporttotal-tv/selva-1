#ifdef __STDC_ALLOC_LIB__
#define __STDC_WANT_LIB_EXT2__ 1
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define _SELVA_MODIFY_HIERARCHY_INTERNAL_
#include "redismodule.h"
#include "cdefs.h"
#include "async_task.h"
#include "errors.h"
#include "hierarchy.h"
#include "rpn.h"
#include "selva_onload.h"
#include "subscriptions.h"
#include "svector.h"

struct subscriptionMarker {
    unsigned marker_flags;
    Selva_NodeId node_id;
    enum SelvaModify_HierarchyTraversal dir;
    struct rpn_ctx *filter_ctx;
    rpn_token *filter_expression;
    char *fields; /* \n separated and \0 terminated. */
    struct Selva_Subscription *sub; /* Pointer back to the subscription. */
};

struct Selva_Subscription {
    Selva_SubscriptionId sub_id;
    unsigned marker_flags_filter; /* All marker flags OR'd for faster lookup. */
    RB_ENTRY(Selva_Subscription) _sub_index_entry;
    SVector markers; /* struct subscriptionMarker */
};

static void clear_sub(struct SelvaModify_Hierarchy *hierarchy, struct subscriptionMarker *marker, Selva_NodeId node_id);

static int marker_svector_compare(const void ** restrict a_raw, const void ** restrict b_raw) {
    const struct subscriptionMarker *a = *(const struct subscriptionMarker **)a_raw;
    const struct subscriptionMarker *b = *(const struct subscriptionMarker **)b_raw;

    return (uintptr_t)a - (uintptr_t)b;
}

static int subscription_svector_compare(const void ** restrict a_raw, const void ** restrict b_raw) {
    const struct Selva_Subscription *a = *(const struct Selva_Subscription **)a_raw;
    const struct Selva_Subscription *b = *(const struct Selva_Subscription **)b_raw;

    return memcmp(a->sub_id, b->sub_id, sizeof(Selva_SubscriptionId));
}

static int subscription_rb_compare(const struct Selva_Subscription *a, const struct Selva_Subscription *b) {
    return memcmp(a->sub_id, b->sub_id, sizeof(Selva_SubscriptionId));
}

RB_PROTOTYPE_STATIC(hierarchy_subscriptions_tree, Selva_Subscription, _sub_index_entry, subscription_rb_compare)
RB_GENERATE_STATIC(hierarchy_subscriptions_tree, Selva_Subscription, _sub_index_entry, subscription_rb_compare)

static char *subId2str(Selva_SubscriptionId sub_id) {
    static char str[2 * sizeof(Selva_SubscriptionId) + 1];

    for (size_t i = 0; i < sizeof(Selva_SubscriptionId); i++) {
        sprintf(str + 2 * i, "%02x", sub_id[i]);
    }
    str[sizeof(str) - 1] = '\0';

    return str;
}

static void destroy_marker(struct subscriptionMarker *marker) {
    rpn_destroy(marker->filter_ctx);
    RedisModule_Free(marker->filter_expression);
    RedisModule_Free(marker->fields);
    RedisModule_Free(marker);
}

static void destroy_sub(SelvaModify_Hierarchy *hierarchy, struct Selva_Subscription *sub) {
    struct subscriptionMarker **it;

    if (SVector_Size(&sub->markers) > 0) {
        svector_autofree SVector markers = {0};

        if (!SVector_Clone(&markers, &sub->markers, NULL)) {
            fprintf(stderr, "Hierarchy: Subs ENOMEM, can't destroy a subscription\n");
            return;
        }

        /* TODO implement safe foreach instead of using cloning */
        SVECTOR_FOREACH(it, &markers) {
            struct subscriptionMarker *marker = *it;

            clear_sub(hierarchy, marker, marker->node_id);
            destroy_marker(marker);
        }
        /* This isn't technically necessary but we dot it for clarity. */
        SVector_Clear(&sub->markers);
    }

    RB_REMOVE(hierarchy_subscriptions_tree, &hierarchy->subs_head, sub);
    SVector_Destroy(&sub->markers);
    RedisModule_Free(sub);
}

void Selva_DestroySubscriptions(SelvaModify_Hierarchy *hierarchy) {
    struct hierarchy_subscriptions_tree *subs_head = &hierarchy->subs_head;
    struct Selva_Subscription *sub;
    struct Selva_Subscription *next;

	for (sub = RB_MIN(hierarchy_subscriptions_tree, subs_head); sub != NULL; sub = next) {
		next = RB_NEXT(hierarchy_subscriptions_tree, subs_head, sub);
        destroy_sub(hierarchy, sub);
    }
}

static void init_node_metadata_subs(
        Selva_NodeId id __unused,
        struct SelvaModify_HierarchyMetaData *metadata) {
    /* TODO Lazy alloc */
    SVector_Init(&metadata->sub_markers, 1, marker_svector_compare);
}
SELVA_MODIFY_HIERARCHY_METADATA_CONSTRUCTOR(init_node_metadata_subs);

static void deinit_node_metadata_subs(
        Selva_NodeId id __unused,
        struct SelvaModify_HierarchyMetaData *metadata) {
    SVector_Destroy(&metadata->sub_markers);
}
SELVA_MODIFY_HIERARCHY_METADATA_DESTRUCTOR(deinit_node_metadata_subs);

static struct Selva_Subscription *find_sub(SelvaModify_Hierarchy *hierarchy, Selva_SubscriptionId sub_id) {
    struct Selva_Subscription filter;

    memcpy(&filter.sub_id, sub_id, sizeof(Selva_SubscriptionId));
    return RB_FIND(hierarchy_subscriptions_tree, &hierarchy->subs_head, &filter);
}

static int set_marker(Selva_NodeId id, void *arg, struct SelvaModify_HierarchyMetaData *metadata) {
    struct subscriptionMarker *marker;

    marker = (struct subscriptionMarker *)arg;
    fprintf(stderr, "Set sub %s marker to %.*s\n",
            subId2str(marker->sub->sub_id), (int)SELVA_NODE_ID_SIZE, id);
    SVector_InsertFast(&metadata->sub_markers, marker);

    return 0;
}

static int clear_marker(Selva_NodeId id, void *arg, struct SelvaModify_HierarchyMetaData *metadata) {
    struct subscriptionMarker *marker;

    marker = (struct subscriptionMarker*)arg;
    fprintf(stderr, "Clear sub %s from %.*s (nr_subs: %zd)\n",
            subId2str(marker->sub->sub_id), (int)SELVA_NODE_ID_SIZE, id,
            SVector_Size(&metadata->sub_markers));
    SVector_Remove(&metadata->sub_markers, marker);

    return 0;
}

/**
 * Create a subscription.
 */
static struct Selva_Subscription *create_subscription(
        struct SelvaModify_Hierarchy *hierarchy,
        Selva_SubscriptionId sub_id) {
    struct Selva_Subscription *sub;

    sub = RedisModule_Alloc(sizeof(struct Selva_Subscription));
    if (!sub) {
        return NULL;
    }

    memset(sub, 0, sizeof(struct Selva_Subscription));
    memcpy(sub->sub_id, sub_id, sizeof(sub->sub_id));

    if (!SVector_Init(&sub->markers, 1, NULL)) {
        RedisModule_Free(sub);
        return NULL;
    }

    /*
     * Add to the list of subscriptions.
     */
    if (unlikely(RB_INSERT(hierarchy_subscriptions_tree, &hierarchy->subs_head, sub) != NULL)) {
        SVector_Destroy(&sub->markers);
        RedisModule_Free(sub);
        return NULL;
    }

    return sub;
}

/**
 * Add a new marker to a subscription.
 * Specifiers:
 *  - n = node_id, copied.
 *  - d = traversal direction, value.
 *  - r = RPN context and expression, pointers used directly and must be valid
 *        until the subscription is deleted.
 *  - f = field names, separated by `\n` and terminated by `\0`. The string is
 *        duplicated.
 */
static int Selva_AddSubscriptionMarker(
        struct SelvaModify_Hierarchy *hierarchy,
        Selva_SubscriptionId sub_id,
        unsigned flags,
        const char *fmt,
        ...) {
    va_list args;
    struct Selva_Subscription *sub;
    struct subscriptionMarker *marker;

    sub = find_sub(hierarchy, sub_id);
    if (!sub) {
        sub = create_subscription(hierarchy, sub_id);
        if (!sub) {
            return SELVA_SUBSCRIPTIONS_ENOMEM;
        }
    }

    marker = RedisModule_Alloc(sizeof(struct subscriptionMarker));
    if (!marker) {
        /* The subscription won't be freed. */
        return SELVA_SUBSCRIPTIONS_ENOMEM;
    }
    memset(marker, 0, sizeof(*marker));

    marker->marker_flags = flags;
    marker->sub = sub;
    sub->marker_flags_filter |= flags;

    va_start(args, fmt);
    while (*fmt != '\0') {
        char c = *fmt;
        switch (c) {
        case 'n': /* node_id */
            memcpy(marker->node_id, va_arg(args, char *), SELVA_NODE_ID_SIZE);
            break;
        case 'd': /* Hierarchy traversal direction */
            marker->dir = va_arg(args, enum SelvaModify_HierarchyTraversal);
            break;
        case 'r': /* RPN filter */
            marker->filter_ctx = va_arg(args, struct rpn_ctx *);
            marker->filter_expression = va_arg(args, rpn_token *);
            break;
        case 'f': /* Fields */
            marker->fields = RedisModule_Strdup(va_arg(args, char *));
            break;
        default:
            fprintf(stderr, "Subscriptions: Invalid marker specifier '%c' for subscription %s\n",
                    c, subId2str(sub_id));
            return SELVA_SUBSCRIPTIONS_EINVAL;
        }
        fmt++;
    }
    va_end(args);

    SVector_Insert(&sub->markers, marker);

    return 0;
}

static int refreshSubscription(struct SelvaModify_Hierarchy *hierarchy, struct Selva_Subscription *sub) {
    struct subscriptionMarker **it;
    int res = 0;

    SVECTOR_FOREACH(it, &sub->markers) {
        struct subscriptionMarker *marker = *it;
        int err;

        if (marker->dir == SELVA_HIERARCHY_TRAVERSAL_NONE) {
            /* This is a non-traversing marker. */
            continue;
        }

        /*
         * Set subscription markers.
         */
        struct SelvaModify_HierarchyCallback cb = {
            .node_cb = set_marker,
            .node_arg = marker,
        };

        err = SelvaModify_TraverseHierarchy(hierarchy, marker->node_id, marker->dir, &cb);
        if (err) {
            fprintf(stderr, "Subscriptions: Could not fully apply a subscription: %s\n", subId2str(sub->sub_id));
            res = err; /* Report the last error */
        }
    }

    return res;
}

/**
 * Clear subscription starting from node_id.
 * Clear subscription starting from node_id and remove the subscription if
 * node_id is the starting point.
 */
static void clear_sub(struct SelvaModify_Hierarchy *hierarchy, struct subscriptionMarker *marker, Selva_NodeId node_id) {
    struct SelvaModify_HierarchyCallback cb = {
        .node_cb = clear_marker,
        .node_arg = marker,
    };

    /*
     * Remove subscription markers.
     */
    (void)SelvaModify_TraverseHierarchy(hierarchy, node_id, marker->dir, &cb);

    /*
     * TODO To delete the subscription when it has been deleted from all nodes
     * we need to check if it still exists in the subscription root node.
     */
}

void Selva_DeleteSubscription(struct SelvaModify_Hierarchy *hierarchy, Selva_SubscriptionId sub_id) {
    struct Selva_Subscription *sub;

    sub = find_sub(hierarchy, sub_id);
    if (sub) {
        destroy_sub(hierarchy, sub);
    }
}

void Selva_ClearAllSubscriptionMarkers(
        struct SelvaModify_Hierarchy *hierarchy,
        Selva_NodeId node_id,
        struct SelvaModify_HierarchyMetaData *metadata) {
    const size_t nr_markers = SVector_Size(&metadata->sub_markers);
    svector_autofree SVector markers = {0};
    struct subscriptionMarker **it;

    if (nr_markers == 0) {
        return;
    }

    fprintf(stderr, "Removing %zu subscription markers from %.*s\n",
            nr_markers, (int)SELVA_NODE_ID_SIZE, node_id);

    if (!SVector_Clone(&markers, &metadata->sub_markers, NULL)) {
        fprintf(stderr, "Hierarchy: Markers ENOMEM\n");
        return;
    }

    /*
     * Remove each subscription marker from this node and its ancestors/descendants.
     */
    SVECTOR_FOREACH(it, &markers) {
        struct subscriptionMarker *marker = *it;

        clear_sub(hierarchy, marker, node_id);
    }
    SVector_Clear(&metadata->sub_markers);
}

static int parse_subscription_id(Selva_SubscriptionId id, RedisModuleString *arg) {
    char byte[3] = { '\0', '\0', '\0' };
    TO_STR(arg);

    if (arg_len != 2 * sizeof(Selva_SubscriptionId)) {
        return SELVA_SUBSCRIPTIONS_EINVAL;
    }

    for (size_t i = 0; i < sizeof(Selva_SubscriptionId); i++) {
        unsigned long v;

        byte[0] = arg_str[2 * i];
        byte[1] = arg_str[2 * i + 1];
        v = strtoul(byte, NULL, 16);

        if (unlikely(v > 0xff)) {
            return SELVA_SUBSCRIPTIONS_EINVAL;
        }

        id[i] = v;
    }

    return 0;
}

static int parse_subscription_type(enum SelvaModify_HierarchyTraversal *dir, RedisModuleString *arg) {
    TO_STR(arg);

    if (!strncmp("node", arg_str, arg_len)) {
        *dir = SELVA_HIERARCHY_TRAVERSAL_NODE;
    } else if (!strncmp("ancestors", arg_str, arg_len)) {
        *dir = SELVA_HIERARCHY_TRAVERSAL_DFS_ANCESTORS;
    } else if (!strncmp("descendants", arg_str, arg_len)) {
        *dir = SELVA_HIERARCHY_TRAVERSAL_DFS_DESCENDANTS;
    } else {
        return SELVA_SUBSCRIPTIONS_EINVAL;
    }

    return 0;
}

/*
 * KEY SUB_ID node|ancestors|descendants NODE_ID [fields <fieldnames \n separated>] [filter expression] [filter args...]
 */
int Selva_SubscribeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    int err;

    const size_t ARGV_REDIS_KEY     = 1;
    const size_t ARGV_SUB_ID        = 2;
    const size_t ARGV_MARKER_TYPE   = 3;
    const size_t ARGV_NODE_ID       = 4;
    const size_t ARGV_FIELDS        = 5;
    const size_t ARGV_FIELD_NAMES   = 6;
    size_t ARGV_FILTER_EXPR         = 5;
    size_t ARGV_FILTER_ARGS         = 6;
#define SHIFT_ARGS(i) \
    ARGV_FILTER_EXPR += i; \
    ARGV_FILTER_ARGS += i

    if (argc < 5) {
        return RedisModule_WrongArity(ctx);
    }

    /*
     * Open the Redis key.
     */
    SelvaModify_Hierarchy *hierarchy = SelvaModify_OpenHierarchy(ctx, argv[ARGV_REDIS_KEY], REDISMODULE_READ | REDISMODULE_WRITE);
    if (!hierarchy) {
        return REDISMODULE_OK;
    }

    /*
     * Get the subscription id.
     */
    Selva_SubscriptionId sub_id;
    err = parse_subscription_id(sub_id, argv[ARGV_SUB_ID]);
    if (err) {
        return replyWithSelvaError(ctx, err);
    }

    /*
     * Parse the traversal argument.
     */
    enum SelvaModify_HierarchyTraversal sub_dir;
    err = parse_subscription_type(&sub_dir, argv[ARGV_MARKER_TYPE]);
    if (err) {
        return replyWithSelvaError(ctx, err);
    }

    /*
     * Get the nodeId.
     */
    Selva_NodeId node_id;
    size_t len;
    const char *str = RedisModule_StringPtrLen(argv[ARGV_NODE_ID], &len);
    memset(node_id, 0, SELVA_NODE_ID_SIZE);
    memcpy(node_id, str, (SELVA_NODE_ID_SIZE > len) ? len : SELVA_NODE_ID_SIZE);

    /*
     * Get field names for change events.
     * Optional.
     */
    const char *fields = NULL;
    if (argc > (int)ARGV_FIELD_NAMES) {
        const char *fields_arg;

        fields_arg = RedisModule_StringPtrLen(argv[ARGV_FIELDS], NULL);
        if(strcmp(fields_arg, "fields")) {
            return replyWithSelvaError(ctx, SELVA_SUBSCRIPTIONS_EINVAL);
        }

        fields = RedisModule_StringPtrLen(argv[ARGV_FIELD_NAMES], NULL);

        SHIFT_ARGS(2);
    }

    /*
     * Parse & compile the filter expression.
     * Optional.
     */
    struct rpn_ctx *filter_ctx = NULL;
    rpn_token *filter_expression = NULL;
    if (argc > (int)ARGV_FILTER_EXPR) {
        if (argc >= (int)ARGV_FILTER_EXPR + 1) {
            const int nr_reg = argc - ARGV_FILTER_ARGS + 2;
            const char *input;
            size_t input_len;

            filter_ctx = rpn_init(ctx, nr_reg);
            if (!filter_ctx) {
                err = SELVA_SUBSCRIPTIONS_ENOMEM;
                goto out;
            }

            /*
             * Compile the filter expression.
             */
            input = RedisModule_StringPtrLen(argv[ARGV_FILTER_EXPR], &input_len);
            filter_expression = rpn_compile(input, input_len);
            if (!filter_expression) {
                fprintf(stderr, "Hierarchy: Failed to compile a filter expression: %.*s\n",
                        (int)input_len, input);
                err = SELVA_RPN_ECOMP;
                goto out;
            }

            /*
             * Get the filter expression arguments and set them to the registers.
             */
            for (size_t i = ARGV_FILTER_ARGS; i < (size_t)argc; i++) {
                /* reg[0] is reserved for the current nodeId */
                const size_t reg_i = i - ARGV_FILTER_ARGS + 1;
                size_t str_len;
                const char *str;
                const char *arg;

                /*
                 * Args needs to be duplicated so the strings don't get freed
                 * when the command returns.
                 */
                str = RedisModule_StringPtrLen(argv[i], &str_len);
                str_len++;
                arg = RedisModule_Alloc(str_len);

                rpn_set_reg(filter_ctx, reg_i, arg, str_len, RPN_SET_REG_FLAG_RMFREE);
            }
        }
    }

    unsigned marker_flags = SELVA_SUBSCRIPTION_FLAG_CH_HIERARCHY;
    err = Selva_AddSubscriptionMarker(hierarchy, sub_id, marker_flags, "nd",
                                      node_id, sub_dir);
    if (err) {
        goto out;
    }

    RedisModule_ReplyWithLongLong(ctx, 1);
#if 0
    RedisModule_ReplicateVerbatim(ctx);
#endif
out:
    if (err) {
        if (filter_ctx) {
            rpn_destroy(filter_ctx);
            RedisModule_Free(filter_expression);
        }

        replyWithSelvaError(ctx, err);
    }

    return REDISMODULE_OK;
#undef SHIFT_ARGS
}

/*
 * KEY SUB_ID
 */
int Selva_RefreshSubscriptionCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    int err;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    const size_t ARGV_REDIS_KEY = 1;
    const size_t ARGV_SUB_ID    = 2;

    /*
     * Open the Redis key.
     */
    SelvaModify_Hierarchy *hierarchy = SelvaModify_OpenHierarchy(ctx, argv[ARGV_REDIS_KEY], REDISMODULE_READ | REDISMODULE_WRITE);
    if (!hierarchy) {
        return REDISMODULE_OK;
    }

    Selva_SubscriptionId sub_id;
    err = parse_subscription_id(sub_id, argv[ARGV_SUB_ID]);
    if (err) {
        fprintf(stderr, "Invalid sub_id\n");
        return replyWithSelvaError(ctx, err);
    }

    struct Selva_Subscription *sub;
    sub = find_sub(hierarchy, sub_id);
    if (!sub) {
        replyWithSelvaError(ctx, SELVA_SUBSCRIPTIONS_ENOENT);
        return REDISMODULE_OK;
    }

    err = refreshSubscription(hierarchy, sub);
    if (err) {
        replyWithSelvaError(ctx, err);
    } else {
        RedisModule_ReplyWithLongLong(ctx, 1);
    }

    return REDISMODULE_OK;
}

/*
 * KEY
 */
int Selva_SubscriptionsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    const size_t ARGV_REDIS_KEY = 1;

    /*
     * Open the Redis key.
     */
    SelvaModify_Hierarchy *hierarchy = SelvaModify_OpenHierarchy(ctx, argv[ARGV_REDIS_KEY], REDISMODULE_READ);
    if (!hierarchy) {
        return REDISMODULE_OK;
    }

    struct Selva_Subscription *sub;
    size_t array_len = 0;

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RB_FOREACH(sub, hierarchy_subscriptions_tree, &hierarchy->subs_head) {
        RedisModule_ReplyWithStringBuffer(ctx, subId2str(sub->sub_id), 2 * sizeof(Selva_SubscriptionId));
        array_len++;
    }

    RedisModule_ReplySetArrayLength(ctx, array_len);

    return REDISMODULE_OK;
}

/*
 * KEY SUB_ID
 */
int Selva_SubscriptionCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    int err;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    const size_t ARGV_REDIS_KEY = 1;
    const size_t ARGV_SUB_ID    = 2;

    /*
     * Open the Redis key.
     */
    SelvaModify_Hierarchy *hierarchy = SelvaModify_OpenHierarchy(ctx, argv[ARGV_REDIS_KEY], REDISMODULE_READ);
    if (!hierarchy) {
        return REDISMODULE_OK;
    }

    Selva_SubscriptionId sub_id;
    err = parse_subscription_id(sub_id, argv[ARGV_SUB_ID]);
    if (err) {
        fprintf(stderr, "Invalid sub_id\n");
        return replyWithSelvaError(ctx, err);
    }

    struct Selva_Subscription *sub;
    sub = find_sub(hierarchy, sub_id);
    if (!sub) {
        replyWithSelvaError(ctx, SELVA_SUBSCRIPTIONS_ENOENT);
        return REDISMODULE_OK;
    }

    size_t array_len = 0;
    struct subscriptionMarker **it;
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    SVECTOR_FOREACH(it, &sub->markers) {
        RedisModuleString *str;
        struct subscriptionMarker *marker = *it;

        str = RedisModule_CreateStringPrintf(ctx,
                "flags: %u\n"
                "node_id: \"%.*s\"\n"
                "dir: %s\n"
                "expression: %s\n"
                "fields: \"%s\"",
                marker->marker_flags,
                (int)SELVA_NODE_ID_SIZE, marker->node_id,
                SelvaModify_HierarchyDir2str(marker->dir),
                (marker->filter_ctx) ? "set" : "unset",
                marker->fields);
        if (!str) {
            return REDISMODULE_ERR;
        }
        RedisModule_ReplyWithString(ctx, str);
        array_len++;
    }
    RedisModule_ReplySetArrayLength(ctx, array_len);

    return REDISMODULE_OK;
}

/*
 * KEY SUB_ID
 */
int Selva_UnsubscribeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    int err;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    const size_t ARGV_REDIS_KEY = 1;
    const size_t ARGV_SUB_ID    = 2;

    /*
     * Open the Redis key.
     */
    SelvaModify_Hierarchy *hierarchy = SelvaModify_OpenHierarchy(ctx, argv[ARGV_REDIS_KEY], REDISMODULE_READ | REDISMODULE_WRITE);
    if (!hierarchy) {
        return REDISMODULE_OK;
    }

    Selva_SubscriptionId sub_id;
    err = parse_subscription_id(sub_id, argv[ARGV_SUB_ID]);
    if (err) {
        fprintf(stderr, "Invalid sub_id\n");
        return replyWithSelvaError(ctx, err);
    }

    struct Selva_Subscription *sub;
    sub = find_sub(hierarchy, sub_id);
    if (!sub) {
        RedisModule_ReplyWithLongLong(ctx, 0);
        return REDISMODULE_OK;
    }

    destroy_sub(hierarchy, sub);

    RedisModule_ReplyWithLongLong(ctx, 1);
    return REDISMODULE_OK;
}

static int Hierarchy_Subscriptions_OnLoad(RedisModuleCtx *ctx) {
    /*
     * Register commands.
     */
    if (RedisModule_CreateCommand(ctx, "selva.subscriptions.add", Selva_SubscribeCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.subscriptions.refresh", Selva_RefreshSubscriptionCommand, "write deny-oom", 1, 1 ,1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.subscriptions.list", Selva_SubscriptionsCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.subscriptions.debug", Selva_SubscriptionCommand, "readonly deny-script", 1, 1, 1) ||
        RedisModule_CreateCommand(ctx, "selva.subscriptions.del", Selva_UnsubscribeCommand, "write", 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
SELVA_ONLOAD(Hierarchy_Subscriptions_OnLoad);