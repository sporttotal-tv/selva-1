#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "redismodule.h"

#include "cdefs.h"
#include "hierarchy.h"
#include "modify.h"

static void update_alias(RedisModuleCtx *ctx, RedisModuleKey *alias_key, RedisModuleString *id, RedisModuleString *ref) {
    RedisModuleString *orig;

    /*
     * Remove the alias from the previous "ID.aliases" zset.
     */
    if (!RedisModule_HashGet(alias_key, REDISMODULE_HASH_NONE, ref, &orig, NULL)) {
        TO_STR(orig);
        RedisModuleString *key_name;
        RedisModuleKey *key;

        key_name = RedisModule_CreateStringPrintf(ctx, "%.*s%s", orig_len, orig_str, ".aliases");
        key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_READ | REDISMODULE_WRITE);
        if (key) {
            RedisModule_ZsetRem(key, ref, NULL);
        }

         RedisModule_CloseKey(key);
    }

    RedisModule_HashSet(alias_key, REDISMODULE_HASH_NONE, ref, id, NULL);
}

static int update_hierarchy(
    RedisModuleCtx *ctx,
    SelvaModify_Hierarchy *hierarchy,
    Selva_NodeId node_id,
    const char *field_str,
    struct SelvaModify_OpSet *setOpts
) {
    RedisModuleString *key_name = RedisModule_CreateString(ctx, HIERARCHY_DEFAULT_KEY, sizeof(HIERARCHY_DEFAULT_KEY) - 1);
    hierarchy = SelvaModify_OpenHierarchyKey(ctx, key_name);
    if (!hierarchy) {
        RedisModule_ReplyWithError(ctx, hierarchyStrError[-SELVA_MODIFY_HIERARCHY_ENOENT]);
        return REDISMODULE_ERR;
    }

    /*
     * If the field starts with 'p' we assume "parents"; Otherwise "children".
     * No other field can modify the hierarchy.
     */
    int isFieldParents = field_str[0] == 'p';

    int err = 0;
    if (setOpts->$value_len > 0) {
        size_t nr_nodes = setOpts->$value_len / SELVA_NODE_ID_SIZE;

        if (isFieldParents) { /* parents */
            err = SelvaModify_SetHierarchyParents(ctx, hierarchy, node_id,
                    nr_nodes, (const Selva_NodeId *)setOpts->$value);
        } else { /* children */
            err = SelvaModify_SetHierarchyChildren(ctx, hierarchy, node_id,
                    nr_nodes, (const Selva_NodeId *)setOpts->$value);
        }
    } else {
        if (setOpts->$add_len > 0) {
            size_t nr_nodes = setOpts->$add_len / SELVA_NODE_ID_SIZE;

            if (isFieldParents) { /* parents */
              err = SelvaModify_AddHierarchy(ctx, hierarchy, node_id,
                      nr_nodes, (const Selva_NodeId *)setOpts->$add,
                      0, NULL);
            } else { /* children */
              err = SelvaModify_AddHierarchy(ctx, hierarchy, node_id,
                      0, NULL,
                      nr_nodes, (const Selva_NodeId *)setOpts->$add);
            }
        }
        if (setOpts->$delete_len > 0) {
            size_t nr_nodes = setOpts->$add_len / SELVA_NODE_ID_SIZE;

            if (isFieldParents) { /* parents */
                err = SelvaModify_DelHierarchy(hierarchy, node_id,
                        nr_nodes, (const Selva_NodeId *)setOpts->$delete,
                        0, NULL);
            } else { /* children */
                err = SelvaModify_DelHierarchy(hierarchy, node_id,
                        0, NULL,
                        nr_nodes, (const Selva_NodeId *)setOpts->$delete);
            }
        }
    }

    if (err) {
        RedisModule_ReplyWithError(ctx, hierarchyStrError[-err]);
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

static int update_zset(
    RedisModuleCtx *ctx,
    RedisModuleKey *id_key,
    RedisModuleString *id,
    RedisModuleString *field,
    const char *field_str,
    size_t field_len,
    struct SelvaModify_OpSet *setOpts
) {
    TO_STR(id);
    RedisModuleKey *alias_key = NULL;

    // add in the hash that it's a set/references field
    RedisModuleString *set_field_identifier = RedisModule_CreateString(ctx, "___selva_$set", 13);
    RedisModule_HashSet(id_key, REDISMODULE_HASH_NONE, field, set_field_identifier, NULL);

    RedisModuleString *set_key_name = RedisModule_CreateStringPrintf(ctx, "%.*s%c%.*s", id_len, id_str, '.', field_len, field_str);
    RedisModuleKey *set_key = RedisModule_OpenKey(ctx, set_key_name, REDISMODULE_WRITE);

    if (!set_key) {
        RedisModule_ReplyWithError(ctx, "Unable to open a set key");
        return REDISMODULE_ERR;
    }

    if (!strcmp(field_str, "aliases")) {
        /* TODO NULL check */
        alias_key = open_aliases_key(ctx);
    }

    if (setOpts->$value_len > 0) {
        if (alias_key &&
            RedisModule_ZsetFirstInScoreRange(set_key, REDISMODULE_NEGATIVE_INFINITE, REDISMODULE_POSITIVE_INFINITE, 0, 0) == REDISMODULE_OK) {
            while (!RedisModule_ZsetRangeEndReached(set_key)) {
                RedisModuleString *alias;

                alias = RedisModule_ZsetRangeCurrentElement(set_key, NULL);
                RedisModule_HashSet(alias_key, REDISMODULE_HASH_NONE, alias, REDISMODULE_HASH_DELETE, NULL);
                RedisModule_ZsetRangeNext(set_key);
            }
            RedisModule_ZsetRangeStop(set_key);
        }
        RedisModule_UnlinkKey(set_key);

        char *ptr = setOpts->$value;
        for (size_t i = 0; i < setOpts->$value_len; ) {
            unsigned long part_len = strlen(ptr);
            RedisModuleString *ref = RedisModule_CreateString(ctx, ptr, part_len);

            if (alias_key) {
                update_alias(ctx, alias_key, id, ref);
            }
            RedisModule_ZsetAdd(set_key, 0, ref, NULL);

            // +1 to skip the nullbyte
            ptr += part_len + 1;
            i += part_len + 1;
        }
    } else {
        if (setOpts->$add_len > 0) {
            char *ptr = setOpts->$add;
            for (size_t i = 0; i < setOpts->$add_len; ) {
                unsigned long part_len = strlen(ptr);
                RedisModuleString *ref = RedisModule_CreateString(ctx, ptr, part_len);

                if (alias_key) {
                    update_alias(ctx, alias_key, id, ref);
                }
                RedisModule_ZsetAdd(set_key, 0, ref, NULL);

                // +1 to skip the nullbyte
                ptr += part_len + 1;
                i += part_len + 1;
            }
        }

        if (setOpts->$delete_len > 0) {
            char *ptr = setOpts->$delete;
            for (size_t i = 0; i < setOpts->$delete_len; ) {
                unsigned long part_len = strlen(ptr);

                RedisModuleString *ref = RedisModule_CreateString(ctx, ptr, part_len);
                RedisModule_ZsetRem(set_key, ref, NULL);

                // +1 to skip the nullbyte
                ptr += part_len + 1;
                i += part_len + 1;

                if (alias_key) {
                    RedisModule_HashSet(alias_key, REDISMODULE_HASH_NONE, ref, REDISMODULE_HASH_DELETE, NULL);
                }
            }
        }
    }

    RedisModule_CloseKey(set_key);
    return REDISMODULE_OK;
}

int SelvaModify_ModifySet(
    RedisModuleCtx *ctx,
    SelvaModify_Hierarchy *hierarchy,
    RedisModuleKey *id_key,
    RedisModuleString *id,
    RedisModuleString *field,
    struct SelvaModify_OpSet *setOpts
) {
    TO_STR(id, field);

    if (setOpts->is_reference) {
        Selva_NodeId node_id;

        memset(node_id, '\0', SELVA_NODE_ID_SIZE);
        memcpy(node_id, id_str, min(id_len, SELVA_NODE_ID_SIZE));

        /*
         * Currently only parents and children fields support references (using
         * hierarchy) and we assume the field is either of those.
         */
        return update_hierarchy(ctx, hierarchy, node_id, field_str, setOpts);
    } else {
        return update_zset(ctx, id_key, id, field, field_str, field_len, setOpts);
    }
}

void SelvaModify_ModifyIncrement(
    RedisModuleCtx *ctx,
    RedisModuleKey *id_key,
    RedisModuleString *id,
    RedisModuleString *field,
    const char *field_str,
    size_t field_len,
    RedisModuleString *current_value,
    const char *current_value_str,
    size_t current_value_len,
    struct SelvaModify_OpIncrement *incrementOpts
) {
    size_t id_len;
    const char *id_str = RedisModule_StringPtrLen(id, &id_len);
    int num = current_value == NULL
        ? incrementOpts->$default
        : strtol(current_value_str, NULL, SELVA_NODE_ID_SIZE);
    num += incrementOpts->$increment;

    int num_str_size = (int)ceil(log10(num));
    char increment_str[num_str_size];
    sprintf(increment_str, "%d", num);

    RedisModuleString *increment =
        RedisModule_CreateString(ctx, increment_str, num_str_size);
    RedisModule_HashSet(id_key, REDISMODULE_HASH_NONE, field, increment, NULL);

    if (incrementOpts->index) {
        SelvaModify_Index(id_str, id_len, field_str, field_len, increment_str, num_str_size);
    }
}
