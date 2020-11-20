#include <assert.h>
#include <limits.h>
#include <stdalign.h>
#include <stdlib.h>
#include <string.h>
#include "redismodule.h"
#include "cdefs.h"
#include "cstrings.h"
#include "errors.h"
#include "selva_object.h"
#include "selva_onload.h"
#include "selva_set.h"
#include "svector.h"
#include "tree.h"

#define SELVA_OBJECT_ENCODING_VERSION   0
#define SELVA_OBJECT_KEY_MAX            USHRT_MAX
#define SELVA_OBJECT_SIZE_MAX           SIZE_MAX

#define SELVA_OBJECT_GETKEY_CREATE      0x1 /*!< Create the key and required nested objects. */
#define SELVA_OBJECT_GETKEY_DELETE      0x2 /*!< Delete the key found. */

RB_HEAD(SelvaObjectKeys, SelvaObjectKey);

struct SelvaObjectKey {
    enum SelvaObjectType type; /*!< Type of the value. */
    enum SelvaObjectType subtype; /*!< Subtype of the value. Arrays use this. */
    unsigned short name_len;
    RB_ENTRY(SelvaObjectKey) _entry;
    union {
        void *value; /* The rest of the types use this. */
        double emb_double_value; /* SELVA_OBJECT_DOUBLE */
        long long emb_ll_value; /* SELVA_OBJECT_LONGLONG */
        struct SelvaSet selva_set; /* SELVA_OBJECT_SET */
        SVector array; /* SELVA_OBJECT_ARRAY */
    };
    char name[0];
};

struct SelvaObject {
    size_t obj_size;
    struct SelvaObjectKeys keys_head;
};

struct so_type_name {
    const char * const name;
    size_t len;
};

static RedisModuleType *ObjectType;

static void destroy_selva_object(struct SelvaObject *obj);
static int get_key(struct SelvaObject *obj, const char *key_name_str, size_t key_name_len, unsigned flags, struct SelvaObjectKey **out);
static void replyWithKeyValue(RedisModuleCtx *ctx, struct SelvaObjectKey *key);
static void replyWithObject(RedisModuleCtx *ctx, struct SelvaObject *obj);
RB_PROTOTYPE_STATIC(SelvaObjectKeys, SelvaObjectKey, _entry, SelvaObject_Compare)

static int SelvaObject_Compare(struct SelvaObjectKey *a, struct SelvaObjectKey *b) {
    return strcmp(a->name, b->name);
}

RB_GENERATE_STATIC(SelvaObjectKeys, SelvaObjectKey, _entry, SelvaObject_Compare)

static const struct so_type_name type_names[] = {
    [SELVA_OBJECT_NULL] = { "null", 4 },
    [SELVA_OBJECT_DOUBLE] = { "double", 6 },
    [SELVA_OBJECT_LONGLONG] = { "long long", 9 },
    [SELVA_OBJECT_STRING] = { "string", 6 },
    [SELVA_OBJECT_OBJECT] = { "object", 6 },
    [SELVA_OBJECT_SET] = { "selva_set", 9 },
    [SELVA_OBJECT_ARRAY] = { "array", 7 },
};

struct SelvaObject *new_selva_object(void) {
    struct SelvaObject *obj;

    obj = RedisModule_Alloc(sizeof(*obj));
    obj->obj_size = 0;
    RB_INIT(&obj->keys_head);

    return obj;
}

static int clear_key_value(struct SelvaObjectKey *key) {
    switch (key->type) {
    case SELVA_OBJECT_NULL:
        /* NOP */
        break;
    case SELVA_OBJECT_DOUBLE:
        break;
    case SELVA_OBJECT_LONGLONG:
        break;
    case SELVA_OBJECT_STRING:
        if (key->value) {
            RedisModule_FreeString(NULL, key->value);
        }
        break;
    case SELVA_OBJECT_OBJECT:
        if (key->value) {
            struct SelvaObject *obj = (struct SelvaObject *)key->value;

            destroy_selva_object(obj);
        }
        break;
    case SELVA_OBJECT_SET:
        SelvaSet_Destroy(&key->selva_set);
        break;
    case SELVA_OBJECT_ARRAY:
        /* TODO Clear array key */
        /* There is no foolproof way to know whether an SVector is initialized
         * but presumably we don't have undefined values in a key, similar to
         * how we always expect pointers to be either valid or NULL.
         */
        if (key->subtype == SELVA_OBJECT_STRING) {
            struct SVectorIterator it;
            RedisModuleString *str;

            SVector_ForeachBegin(&it, &key->array);
            while ((str = SVector_Foreach(&it))) {
                RedisModule_FreeString(NULL, str);
            }
        } else {
            fprintf(stderr, "%s: Key clear failed: Unsupported array type (%d)\n",
                    __FILE__, (int)key->subtype);
        }
        SVector_Destroy(&key->array);
        break;
    default:
        fprintf(stderr, "%s: Unknown object value type (%d)\n", __FILE__, (int)key->type);
        return SELVA_EINTYPE;
    }

    key->type = SELVA_OBJECT_NULL;

    return 0;
}

static void destroy_selva_object(struct SelvaObject *obj) {
    struct SelvaObjectKey *key;
    struct SelvaObjectKey *next;

	for (key = RB_MIN(SelvaObjectKeys, &obj->keys_head); key != NULL; key = next) {
		next = RB_NEXT(SelvaObjectKeys, &obj->keys_head, key);
		RB_REMOVE(SelvaObjectKeys, &obj->keys_head, key);
        obj->obj_size--;
        (void)clear_key_value(key);
        RedisModule_Free(key);
    }

    RedisModule_Free(obj);
}

/*
 * Export static functions to create and destroy objects to the unit tests.
 */
#if defined(PU_TEST_BUILD)
struct SelvaObject *(*SelvaObject_New)(void) = new_selva_object;
void (*SelvaObject_Destroy)(struct SelvaObject *obj) = destroy_selva_object;
#endif

size_t SelvaObject_MemUsage(const void *value) {
    struct SelvaObject *obj = (struct SelvaObject *)value;
    struct SelvaObjectKey *key;
    size_t size = sizeof(*obj);

    RB_FOREACH(key, SelvaObjectKeys, &obj->keys_head) {
        size += sizeof(*key) + key->name_len + 1;

        switch (key->type) {
        case SELVA_OBJECT_STRING:
            if (key->value) {
                size_t len;

                (void)RedisModule_StringPtrLen(key->value, &len);
                size += len + 1;
            }
            break;
        case SELVA_OBJECT_OBJECT:
            if (key->value) {
                size += SelvaObject_MemUsage(key->value);
            }
            break;
        default:
            /* 0 */
            break;
        }
    }

    return size;
}

static struct SelvaObject *SelvaObject_Open(RedisModuleCtx *ctx, RedisModuleString *key_name, int mode) {
    struct SelvaObject *obj = NULL;
    RedisModuleKey *key;
    int type;

    key = RedisModule_OpenKey(ctx, key_name, mode);
    type = RedisModule_KeyType(key);

    if (type != REDISMODULE_KEYTYPE_EMPTY &&
        RedisModule_ModuleTypeGetType(key) != ObjectType) {
        RedisModule_CloseKey(key);
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);

        return NULL;
    }

    /* Create an empty value object if the key is currently empty. */
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        if ((mode & REDISMODULE_WRITE) == REDISMODULE_WRITE) {
            obj = new_selva_object();
            if (!obj) {
                replyWithSelvaError(ctx, SELVA_ENOMEM);
            }

            RedisModule_ModuleTypeSetValue(key, ObjectType, obj);
        } else {
            replyWithSelvaError(ctx, SELVA_ENOENT);
        }
    } else {
        obj = RedisModule_ModuleTypeGetValue(key);
        if (!obj) {
            replyWithSelvaError(ctx, SELVA_ENOENT);
        }
    }

    return obj;
}

int SelvaObject_Key2Obj(RedisModuleKey *key, struct SelvaObject **out) {
    struct SelvaObject *obj;

    if (!key) {
        return SELVA_ENOENT;
    }

    /* Create an empty value object if the key is currently empty. */
    if (RedisModule_KeyType(key) == REDISMODULE_KEYTYPE_EMPTY) {
        int err;

        obj = new_selva_object();
        if (!obj) {
            return SELVA_ENOMEM;
        }

        err = RedisModule_ModuleTypeSetValue(key, ObjectType, obj);
        if (err != REDISMODULE_OK) {
            destroy_selva_object(obj);
            return SELVA_ENOENT;
        }
        /* TODO This check is really slow */
#if 0
    } else if (RedisModule_ModuleTypeGetType(key) == ObjectType) {
#endif
    } else {
        obj = RedisModule_ModuleTypeGetValue(key);
        if (!obj) {
            return SELVA_ENOENT;
        }
#if 0
    } else {
        return SELVA_EINVAL;
#endif
    }

    *out = obj;
    return 0;
}

static int get_key_obj(struct SelvaObject *obj, const char *key_name_str, size_t key_name_len, unsigned flags, struct SelvaObjectKey **out) {
    const char *sep = ".";
    const size_t nr_parts = substring_count(key_name_str, ".") + 1;
    char buf[key_name_len]; /* We assume that the length has been sanity checked at this point. */
    char *s = buf;
    struct SelvaObjectKey *key;
    struct SelvaObject *cobj = obj; /* Containing object. */

    strncpy(s, key_name_str, key_name_len);
    s[key_name_len] = '\0';

    size_t nr_parts_found = 0;
    for (s = strtok(s, sep); s; s = strtok(NULL, sep)) {
        const size_t slen = strlen(s);
        int err;

        cobj = obj;
        key = NULL; /* This needs to be cleared on every iteration. */
        nr_parts_found++;
        err = get_key(obj, s, slen, 0, &key);
        if ((err == SELVA_ENOENT || (err == 0 && key->type != SELVA_OBJECT_OBJECT)) &&
            (flags & SELVA_OBJECT_GETKEY_CREATE)) {
            /*
             * Either the nested object doesn't exist yet or the nested key is not an object,
             * but we are allowed to create one here.
             */
            if (!key) {
                /*
                 * Only create the key if it didn't exist. Otherwise we can just
                 * reuse it.
                 */
                if (obj->obj_size == SELVA_OBJECT_SIZE_MAX) {
                    return SELVA_OBJECT_EOBIG;
                }

                const size_t key_size = sizeof(struct SelvaObjectKey) + slen + 1;
                key = RedisModule_Alloc(key_size);
                if (!key) {
                    return SELVA_ENOMEM;
                }

                memset(key, 0, key_size);
                strcpy(key->name, s); /* strok() is safe. */
                key->name_len = slen;
                obj->obj_size++;
                (void)RB_INSERT(SelvaObjectKeys, &obj->keys_head, key);
            } else {
                /*
                 * Clear the old value.
                 */
                clear_key_value(key);
            }
            key->type = SELVA_OBJECT_OBJECT;
            key->value = new_selva_object();

            if (!key->value) {
                /* A partial object might have been created! */
                key->type = SELVA_OBJECT_NULL;
                return SELVA_ENOMEM;
            }

            obj = key->value;
        } else if (err) {
            /*
             * Error, bail out.
             */
            return err;
        } else if (key->type == SELVA_OBJECT_OBJECT) {
            /*
             * Keep nesting or return an object if this was the last token.
             */
            obj = key->value;
        } else {
            /*
             * Found the final key.
             */
            break;
        }
    }

    /*
     * Check that we found what  we were really looking for. Consider the
     * following:
     * We have a key: a.b = "hello"
     * We do a lookup for "a.b.c" but end up to "a.b"
     * Without the following check we'd happily tell the user that the value of
     * "a.b.c" == "hello".
     */
    if (nr_parts_found != nr_parts) {
        return SELVA_ENOENT;
    }

    if (flags & SELVA_OBJECT_GETKEY_DELETE) {
        RB_REMOVE(SelvaObjectKeys, &cobj->keys_head, key);
        obj->obj_size--;
        (void)clear_key_value(key);
        RedisModule_Free(key);
        key = NULL;
    }

    *out = key;
    return 0;
}

static int get_key(struct SelvaObject *obj, const char *key_name_str, size_t key_name_len, unsigned flags, struct SelvaObjectKey **out) {
    struct SelvaObjectKey *filter;
    struct SelvaObjectKey *key;

    if (key_name_len + 1 > SELVA_OBJECT_KEY_MAX) {
        return SELVA_ENAMETOOLONG;
    }

    if (strstr(key_name_str, ".")) {
        return get_key_obj(obj, key_name_str, key_name_len, flags, out);
    }

    const size_t key_size = sizeof(struct SelvaObjectKey) + key_name_len + 1;
    char buf[key_size] __attribute__((aligned(alignof(struct SelvaObjectKey)))); /* RFE This might be dumb */

    filter = (struct SelvaObjectKey *)buf;
    memset(filter, 0, key_size);
    memcpy(filter->name, key_name_str, key_name_len + 1);
    filter->name_len = key_name_len;

    key = RB_FIND(SelvaObjectKeys, &obj->keys_head, filter);
    if (!key && (flags & SELVA_OBJECT_GETKEY_CREATE)) {
        if (obj->obj_size == SELVA_OBJECT_SIZE_MAX) {
            return SELVA_OBJECT_EOBIG;
        }

        key = RedisModule_Alloc(key_size);
        if (!key) {
            return SELVA_ENOMEM;
        }

        memcpy(key, filter, key_size);
        memset(&key->_entry, 0, sizeof(key->_entry)); /* RFE Might not be necessary. */
        obj->obj_size++;
        (void)RB_INSERT(SelvaObjectKeys, &obj->keys_head, key);
    } else if (!key) {
        return SELVA_ENOENT;
    }

    if (flags & SELVA_OBJECT_GETKEY_DELETE) {
        RB_REMOVE(SelvaObjectKeys, &obj->keys_head, key);
        obj->obj_size--;
        (void)clear_key_value(key);
        RedisModule_Free(key);
        key = NULL;
    }

    *out = key;
    return 0;
}

static int get_key_modify(struct SelvaObject *obj, const RedisModuleString *key_name, struct SelvaObjectKey **out) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    /*
     * Do get_key() first without create to avoid clearing the original value that we want to modify.
     * If we get a SELVA_ENOENT error we can safely create the key.
     */
    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err == SELVA_ENOENT) {
        err = get_key(obj, key_name_str, key_name_len, SELVA_OBJECT_GETKEY_CREATE, &key);
    }
    if (err) {
        return err;
    }

    *out = key;
    return 0;
}

int SelvaObject_DelKey(struct SelvaObject *obj, const RedisModuleString *key_name) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, SELVA_OBJECT_GETKEY_DELETE, &key);
    if (err) {
        return err;
    }

    return 0;
}

int SelvaObject_Exists(struct SelvaObject *obj, const RedisModuleString *key_name) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    }

    return 0;
}

int SelvaObject_GetDouble(struct SelvaObject *obj, const RedisModuleString *key_name, double *out) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    } else if (key->type != SELVA_OBJECT_DOUBLE) {
        return SELVA_EINTYPE;
    }

    *out = key->emb_double_value;

    return 0;
}

int SelvaObject_GetLongLong(struct SelvaObject *obj, const RedisModuleString *key_name, long long *out) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    } else if (key->type != SELVA_OBJECT_LONGLONG) {
        return SELVA_EINTYPE;
    }

    *out = key->emb_ll_value;

    return 0;
}

int SelvaObject_GetStr(struct SelvaObject *obj, const RedisModuleString *key_name, RedisModuleString **out) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    } else if (key->type != SELVA_OBJECT_STRING) {
        return SELVA_EINTYPE;
    }
    assert(key->value);

    *out = key->value;

    return 0;
}

int SelvaObject_SetDouble(struct SelvaObject *obj, const RedisModuleString *key_name, double value) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, SELVA_OBJECT_GETKEY_CREATE, &key);
    if (err) {
        return err;
    }

    err = clear_key_value(key);
    if (err) {
        return err;
    }

    key->type = SELVA_OBJECT_DOUBLE;
    key->emb_double_value = value;

    return 0;
}

int SelvaObject_SetLongLong(struct SelvaObject *obj, const RedisModuleString *key_name, double value) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, SELVA_OBJECT_GETKEY_CREATE, &key);
    if (err) {
        return err;
    }

    err = clear_key_value(key);
    if (err) {
        return err;
    }

    key->type = SELVA_OBJECT_LONGLONG;
    key->emb_ll_value = value;

    return 0;
}

int SelvaObject_SetStr(struct SelvaObject *obj, const RedisModuleString *key_name, RedisModuleString *value) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, SELVA_OBJECT_GETKEY_CREATE, &key);
    if (err) {
        return err;
    }

    err = clear_key_value(key);
    if (err) {
        return err;
    }

    RedisModule_RetainString(NULL, value);
    key->type = SELVA_OBJECT_STRING;
    key->value = value;

    return 0;
}

int SelvaObject_AddSet(struct SelvaObject *obj, const RedisModuleString *key_name, RedisModuleString *value) {
    struct SelvaObjectKey *key;
    int err;

    assert(obj);

    err = get_key_modify(obj, key_name, &key);
    if (err) {
        return err;
    }

    if (key->type != SELVA_OBJECT_SET) {
        err = clear_key_value(key);
        if (err) {
            return err;
        }

        SelvaSet_Init(&key->selva_set);
        key->type = SELVA_OBJECT_SET;
    }

    struct SelvaSetElement *el;
    el = RedisModule_Calloc(1, sizeof(struct SelvaSetElement));
    el->value = value;

    if (SelvaSet_Add(&key->selva_set, el) != NULL) {
        RedisModule_Free(el);
        return SELVA_EEXIST;
    }
    RedisModule_RetainString(NULL, value);

    return 0;
}

int SelvaObject_RemSet(struct SelvaObject *obj, const RedisModuleString *key_name, RedisModuleString *value) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    }

    if (key->type != SELVA_OBJECT_SET) {
        return SELVA_EINVAL;
    }

    struct SelvaSetElement *el;
    el = SelvaSet_Find(&key->selva_set, value);
    if (!el) {
        return SELVA_ENOENT;
    }

    /*
     * Remove from the tree and free the string.
     */
    SelvaSet_Remove(&key->selva_set, el);
    SelvaSet_DestroyElement(el);

    return 0;
}

struct SelvaSet *SelvaObject_GetSet(struct SelvaObject *obj, const RedisModuleString *key_name) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err || key->type != SELVA_OBJECT_SET) {
        return NULL;
    }

    return &key->selva_set;
}

int SelvaObject_AddArray(struct SelvaObject *obj, const RedisModuleString *key_name, enum SelvaObjectType subtype, void *p) {
    struct SelvaObjectKey *key;
    int err;

    assert(obj);

    if (subtype != SELVA_OBJECT_STRING) {
        return SELVA_EINTYPE;
    }

    err = get_key_modify(obj, key_name, &key);
    if (err) {
        return err;
    }

    /* TODO Should it fail if the subtype doesn't match? */
    if (key->type != SELVA_OBJECT_ARRAY || key->subtype != subtype) {
        err = clear_key_value(key);
        if (err) {
            return err;
        }

        /*
         * Type must be set before initializing the vector to avoid a situation
         * where we'd have a key with an unknown value type.
         */
        key->type = SELVA_OBJECT_ARRAY;
        key->subtype = subtype;

        if (!SVector_Init(&key->array, 1, NULL)) {
            return SELVA_ENOMEM;
        }
    }

    SVector_Insert(&key->array, p);
    RedisModule_RetainString(NULL, (RedisModuleString *)p);

    return 0;
}

int SelvaObject_GetArray(struct SelvaObject *obj, const RedisModuleString *key_name, enum SelvaObjectType *out_subtype, void **out_p) {
    struct SelvaObjectKey *key;
    TO_STR(key_name);
    int err;

    assert(obj);

    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    }

    if (key->type != SELVA_OBJECT_ARRAY) {
        return SELVA_EINTYPE;
    }

    if (out_subtype) {
        *out_subtype = key->subtype;
    }
    if (out_p) {
        *out_p = &key->array;
    }

    return 0;
}

enum SelvaObjectType SelvaObject_GetType(struct SelvaObject *obj, const char *key_name, size_t key_name_len) {
    struct SelvaObjectKey *key;
    enum SelvaObjectType type = SELVA_OBJECT_NULL;
    int err;

    err = get_key(obj, key_name, key_name_len, 0, &key);
    if (!err) {
        type = key->type;
    }

    return type;
}

int SelvaObject_DelCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    struct SelvaObject *obj;
    int err;

    const size_t ARGV_KEY = 1;
    const size_t ARGV_OKEY = 2;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    obj = SelvaObject_Open(ctx, argv[ARGV_KEY], REDISMODULE_READ);
    if (!obj) {
        return REDISMODULE_OK;
    }

    err = SelvaObject_DelKey(obj, argv[ARGV_OKEY]);
    if (err == SELVA_ENOENT) {
        RedisModule_ReplyWithLongLong(ctx, 0);
    } else if (err) {
        return replyWithSelvaError(ctx, err);
    } else {
        RedisModule_ReplyWithLongLong(ctx, 1);
    }

    return RedisModule_ReplicateVerbatim(ctx);
}

int SelvaObject_ExistsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    struct SelvaObject *obj;
    int err;

    const size_t ARGV_KEY = 1;
    const size_t ARGV_OKEY = 2;

    if (argc < 3) {
        return RedisModule_WrongArity(ctx);
    }

    obj = SelvaObject_Open(ctx, argv[ARGV_KEY], REDISMODULE_READ);
    if (!obj) {
        return REDISMODULE_OK;
    }

    err = SelvaObject_Exists(obj, argv[ARGV_OKEY]);
    if (err == SELVA_ENOENT) {
        return RedisModule_ReplyWithLongLong(ctx, 0);
    } else if (err) {
        return replyWithSelvaError(ctx, err);
    }
    return RedisModule_ReplyWithLongLong(ctx, 1);
}

static void replyWithSelvaSet(RedisModuleCtx *ctx, struct SelvaSet *set) {
    struct SelvaSetElement *el;
    size_t n = 0;

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RB_FOREACH(el, SelvaSetHead, &set->head) {
        RedisModule_ReplyWithString(ctx, el->value);
        n++;
    }

    RedisModule_ReplySetArrayLength(ctx, n);
}

static void replyWithArray(RedisModuleCtx *ctx, enum SelvaObjectType subtype, SVector *array) {
    /* TODO add selva_object_array reply support */
    (void)replyWithSelvaErrorf(ctx, SELVA_EINTYPE, "Array type not supported");
}

static void replyWithKeyValue(RedisModuleCtx *ctx, struct SelvaObjectKey *key) {
    switch (key->type) {
    case SELVA_OBJECT_NULL:
        RedisModule_ReplyWithNull(ctx);
        break;
    case SELVA_OBJECT_DOUBLE:
        RedisModule_ReplyWithDouble(ctx, key->emb_double_value);
        break;
    case SELVA_OBJECT_LONGLONG:
        RedisModule_ReplyWithLongLong(ctx, key->emb_ll_value);
        break;
    case SELVA_OBJECT_STRING:
        if (key->value) {
            RedisModule_ReplyWithString(ctx, key->value);
        } else {
            RedisModule_ReplyWithNull(ctx);
        }
        break;
    case SELVA_OBJECT_OBJECT:
        if (key->value) {
            replyWithObject(ctx, key->value);
        } else {
            RedisModule_ReplyWithNull(ctx);
        }
        break;
    case SELVA_OBJECT_SET:
        replyWithSelvaSet(ctx, &key->selva_set);
        break;
    case SELVA_OBJECT_ARRAY:
        replyWithArray(ctx, key->subtype, &key->array);
        break;
    default:
        (void)replyWithSelvaErrorf(ctx, SELVA_EINTYPE, "invalid key type %d", (int)key->type);
    }
}

static void replyWithObject(RedisModuleCtx *ctx, struct SelvaObject *obj) {
    struct SelvaObjectKey *key;
    size_t n = 0;

    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

    RB_FOREACH(key, SelvaObjectKeys, &obj->keys_head) {
        RedisModule_ReplyWithStringBuffer(ctx, key->name, key->name_len);
        replyWithKeyValue(ctx, key);

        n += 2;
    }

    RedisModule_ReplySetArrayLength(ctx, n);
}

int SelvaObject_ReplyWithObject(RedisModuleCtx *ctx, struct SelvaObject *obj, RedisModuleString *key_name) {
    struct SelvaObjectKey *key;
    int err;

    if (!key_name) {
        replyWithObject(ctx, obj);
        return 0;
    }

    TO_STR(key_name);
    err = get_key(obj, key_name_str, key_name_len, 0, &key);
    if (err) {
        return err;
    }

    replyWithKeyValue(ctx, key);

    return 0;
}

int SelvaObject_GetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    struct SelvaObject *obj;
    struct SelvaObjectKey *key;

    const size_t ARGV_KEY = 1;
    const size_t ARGV_OKEY = 2;

    if (argc < 2) {
        return RedisModule_WrongArity(ctx);
    }

    obj = SelvaObject_Open(ctx, argv[ARGV_KEY], REDISMODULE_READ);
    if (!obj) {
        return REDISMODULE_OK;
    }

    if (argc == 2) {
        replyWithObject(ctx, obj);
        return REDISMODULE_OK;
    }

    for (size_t i = ARGV_OKEY; i < (size_t)argc; i++) {
        RedisModuleString *okey = argv[i];
        TO_STR(okey);
        int err;

        err = get_key(obj, okey_str, okey_len, 0, &key);
        if (err == SELVA_ENOENT) {
            /* Keep looking. */
            continue;
        } else if (err) {
            return replyWithSelvaErrorf(ctx, err, "get_key");
        }

        replyWithKeyValue(ctx, key);
        return REDISMODULE_OK;
    }

    return RedisModule_ReplyWithNull(ctx);
}

int SelvaObject_SetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    struct SelvaObject *obj;
    size_t values_set = 0;
    int err;

    const size_t ARGV_KEY = 1;
    const size_t ARGV_OKEY = 2;
    const size_t ARGV_TYPE = 3;
    const size_t ARGV_OVAL = 4;

    if (argc <= (int)ARGV_TYPE) {
        return RedisModule_WrongArity(ctx);
    }

    size_t type_len;
    const char type = RedisModule_StringPtrLen(argv[ARGV_TYPE], &type_len)[0];

    if (type_len != 1) {
        return replyWithSelvaErrorf(ctx, SELVA_EINVAL, "Invalid or missing type argument");
    }

    if (!(argc == 5 || (type == 'S' && argc >= 5))) {
        return RedisModule_WrongArity(ctx);
    }

    obj = SelvaObject_Open(ctx, argv[ARGV_KEY], REDISMODULE_READ | REDISMODULE_WRITE);
    if (!obj) {
        return replyWithSelvaError(ctx, SELVA_ENOENT);
    }

    switch (type) {
    case 'f': /* SELVA_OBJECT_DOUBLE */
        err = SelvaObject_SetDouble(
            obj,
            argv[ARGV_OKEY],
            strtod(RedisModule_StringPtrLen(argv[ARGV_OVAL], NULL), NULL));
        values_set++;
        break;
    case 'i': /* SELVA_OBJECT_LONGLONG */
        err = SelvaObject_SetLongLong(
            obj,
            argv[ARGV_OKEY],
            strtoll(RedisModule_StringPtrLen(argv[ARGV_OVAL], NULL), NULL, 10));
        values_set++;
        break;
    case 's': /* SELVA_OBJECT_STRING */
        err = SelvaObject_SetStr(obj, argv[ARGV_OKEY], argv[ARGV_OVAL]);
        values_set++;
        break;
    case 'S': /* SELVA_OBJECT_SET */
        for (int i = ARGV_OVAL; i < argc; i++) {
            if (SelvaObject_AddSet(obj, argv[ARGV_OKEY], argv[i]) == 0) {
                values_set++;
            }
        }
        err = 0;
        break;
    default:
        err = SELVA_EINTYPE;
    }
    if (err) {
        return replyWithSelvaError(ctx, err);
    }
    RedisModule_ReplyWithLongLong(ctx, values_set);

    return RedisModule_ReplicateVerbatim(ctx);
}

int SelvaObject_TypeCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    struct SelvaObject *obj;
    struct SelvaObjectKey *key;
    int err;

    const size_t ARGV_KEY = 1;
    const size_t ARGV_OKEY = 2;

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    obj = SelvaObject_Open(ctx, argv[ARGV_KEY], REDISMODULE_READ);
    if (!obj) {
        return replyWithSelvaError(ctx, SELVA_ENOENT);
    }

    enum SelvaObjectType type = SELVA_OBJECT_NULL;
    RedisModuleString *okey = argv[ARGV_OKEY];
    TO_STR(okey);
    err = get_key(obj, okey_str, okey_len, 0, &key);
    if (!err) {
        type = key->type;
    } else if (err != SELVA_ENOENT) {
        return replyWithSelvaErrorf(ctx, err, "get_key");
    }

    if (type >= 0 && type < sizeof(type_names)) {
        const struct so_type_name *tn = &type_names[type];
        RedisModule_ReplyWithStringBuffer(ctx, tn->name, tn->len);
    } else {
        return replyWithSelvaErrorf(ctx, SELVA_EINTYPE, "invalid key type %d", (int)type);
    }

#undef REPLY_WITH_TYPE
    return REDISMODULE_OK;
}

int SelvaObject_LenCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    struct SelvaObject *obj;
    struct SelvaObjectKey *key;
    int err;

    const size_t ARGV_KEY = 1;
    const size_t ARGV_OKEY = 2;

    if (argc != 2 && argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    obj = SelvaObject_Open(ctx, argv[ARGV_KEY], REDISMODULE_READ);
    if (!obj) {
        return REDISMODULE_OK;
    }

    if (argc == 2) {
        RedisModule_ReplyWithLongLong(ctx, obj->obj_size);
        return REDISMODULE_OK;
    }

    RedisModuleString *okey = argv[ARGV_OKEY];
    TO_STR(okey);
    err = get_key(obj, okey_str, okey_len, 0, &key);
    if (err) {
        return replyWithSelvaError(ctx, err);
    }

    switch (key->type) {
    case SELVA_OBJECT_NULL:
        RedisModule_ReplyWithLongLong(ctx, 0);
        break;
    case SELVA_OBJECT_DOUBLE:
    case SELVA_OBJECT_LONGLONG:
        RedisModule_ReplyWithLongLong(ctx, 1);
        break;
    case SELVA_OBJECT_STRING:
        if (key->value) {
            size_t len;

            (void)RedisModule_StringPtrLen(key->value, &len);
            RedisModule_ReplyWithLongLong(ctx, len);
        } else {
            RedisModule_ReplyWithLongLong(ctx, 0);
        }
        break;
    case SELVA_OBJECT_OBJECT:
        if (key->value) {
            struct SelvaObject *obj2 = (struct SelvaObject *)key->value;

            RedisModule_ReplyWithLongLong(ctx, obj2->obj_size);
        } else {
            RedisModule_ReplyWithLongLong(ctx, 0);
        }
        break;
    case SELVA_OBJECT_SET:
        RedisModule_ReplyWithLongLong(ctx, key->selva_set.size);
        break;
    case SELVA_OBJECT_ARRAY:
        RedisModule_ReplyWithLongLong(ctx, SVector_Size(&key->array));
        break;
    default:
        (void)replyWithSelvaErrorf(ctx, SELVA_EINTYPE, "key type not supported %d", (int)key->type);
    }

    return REDISMODULE_OK;
}


void *SelvaObjectTypeRDBLoad(RedisModuleIO *io, int encver) {
    struct SelvaObject *obj;

    if (encver != SELVA_OBJECT_ENCODING_VERSION) {
        /*
         * RFE
         * We should actually log an error here, or try to implement
         * the ability to load older versions of our data structure.
         */
        return NULL;
    }

    obj = new_selva_object();
    if (!obj) {
        RedisModule_LogIOError(io, "warning", "Failed to create a new SelvaObject");
        return NULL;
    }
    const size_t obj_size = RedisModule_LoadUnsigned(io);

    for (size_t i = 0; i < obj_size; i++) {
        RedisModuleString *name;
        enum SelvaObjectType type;

        name = RedisModule_LoadString(io);
        type = RedisModule_LoadUnsigned(io);

        switch (type) {
        case SELVA_OBJECT_NULL:
            RedisModule_LogIOError(io, "warning", "null keys should not exist in RDB");
            break;
        case SELVA_OBJECT_DOUBLE:
            {
                double value;
                int err;

                value = RedisModule_LoadDouble(io);
                err = SelvaObject_SetDouble(obj, name, value);
                if (err) {
                    RedisModule_LogIOError(io, "warning", "Error while loading a double");
                    return NULL;
                }
            }
            break;
        case SELVA_OBJECT_LONGLONG:
            {
                long long value;
                int err;

                value = RedisModule_LoadSigned(io);
                err = SelvaObject_SetLongLong(obj, name, value);
                if (err) {
                    RedisModule_LogIOError(io, "warning", "Error while loading a long long");
                    return NULL;
                }
            }
            break;
        case SELVA_OBJECT_STRING:
            {
                RedisModuleString *value;
                int err;

                value = RedisModule_LoadString(io);
                err = SelvaObject_SetStr(obj, name, value);
                if (err) {
                    RedisModule_LogIOError(io, "warning", "Error while loading a string");
                    return NULL;
                }

                RedisModule_FreeString(NULL, value);
            }
            break;
        case SELVA_OBJECT_OBJECT:
            {
                struct SelvaObjectKey *key;
                TO_STR(name);
                int err;

                err = get_key(obj, name_str, name_len, SELVA_OBJECT_GETKEY_CREATE, &key);
                if (err) {
                    RedisModule_LogIOError(io, "warning", "Error while creating an object key");
                    return NULL;
                }

                key->value = SelvaObjectTypeRDBLoad(io, encver);
                if (!key->value) {
                    RedisModule_LogIOError(io, "warning", "Error while loading an object");
                    return NULL;
                }
                key->type = SELVA_OBJECT_OBJECT;
            }
            break;
        case SELVA_OBJECT_SET:
            {
                const size_t n = RedisModule_LoadUnsigned(io);

                for (size_t j = 0; j < n; j++) {
                    RedisModuleString *value = RedisModule_LoadString(io);

                    SelvaObject_AddSet(obj, name, value);
                }
            }
            break;
        case SELVA_OBJECT_ARRAY:
            /* TODO Support arrays */
            RedisModule_LogIOError(io, "warning", "Array not supported in RDB");
            break;
        default:
            RedisModule_LogIOError(io, "warning", "Unknown type");
        }

        RedisModule_FreeString(NULL, name);
    }

    return obj;
}

void SelvaObjectTypeRDBSave(RedisModuleIO *io, void *value) {
    struct SelvaObject *obj = (struct SelvaObject *)value;
    struct SelvaObjectKey *key;

    RedisModule_SaveUnsigned(io, obj->obj_size);
    RB_FOREACH(key, SelvaObjectKeys, &obj->keys_head) {
        RedisModule_SaveStringBuffer(io, key->name, key->name_len);

        if (key->type != SELVA_OBJECT_NULL) {
            if (!key->value) {
                RedisModule_LogIOError(io, "warning", "Value is NULL");
                continue;
            }

            RedisModule_SaveUnsigned(io, key->type);

            switch (key->type) {
            case SELVA_OBJECT_NULL:
                /* null is implicit value and doesn't need to be persisted. */
                break;
            case SELVA_OBJECT_DOUBLE:
                RedisModule_SaveDouble(io, key->emb_double_value);
                break;
            case SELVA_OBJECT_LONGLONG:
                RedisModule_SaveSigned(io, key->emb_ll_value);
                break;
            case SELVA_OBJECT_STRING:
                if (!key->value) {
                    RedisModule_LogIOError(io, "warning", "STRING value missing");
                }
                RedisModule_SaveString(io, key->value);
                break;
            case SELVA_OBJECT_OBJECT:
                if (!key->value) {
                    RedisModule_LogIOError(io, "warning", "OBJECT value missing");
                }
                SelvaObjectTypeRDBSave(io, key->value);
                break;
            case SELVA_OBJECT_SET:
                {
                    struct SelvaSetElement *el;

                    RedisModule_SaveUnsigned(io, key->selva_set.size);

                    RB_FOREACH(el, SelvaSetHead, &key->selva_set.head) {
                        RedisModule_SaveString(io, el->value);
                    }
                }
                break;
        case SELVA_OBJECT_ARRAY:
            /* TODO Support arrays */
            RedisModule_LogIOError(io, "warning", "Array not supported in RDB");
            break;
            default:
                RedisModule_LogIOError(io, "warning", "Unknown type");
            }
        }
    }
}

void set_aof_rewrite(RedisModuleIO *aof, RedisModuleString *key, struct SelvaObjectKey *okey) {
    RedisModuleString **argv;

    argv = RedisModule_Alloc(okey->selva_set.size);
    if (!argv) {
        RedisModule_LogIOError(aof, "warning", "Alloc failed");
        return;
    }

    size_t argc = 0;
    struct SelvaSetElement *el;
    RB_FOREACH(el, SelvaSetHead, &okey->selva_set.head) {
        argv[argc++] = el->value;
    }

    RedisModule_EmitAOF(aof, "SELVA.OBJECT.SET", "sbcv",
            key,
            okey->name, (size_t)okey->name_len,
            "S",
            argv,
            argc);

    RedisModule_Free(argv);
}

void SelvaObjectTypeAOFRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    struct SelvaObject *obj = (struct SelvaObject *)value;
    struct SelvaObjectKey *okey;

    RB_FOREACH(okey, SelvaObjectKeys, &obj->keys_head) {
        switch (okey->type) {
        case SELVA_OBJECT_NULL:
            /* NOP - NULL is implicit */
            break;
        case SELVA_OBJECT_DOUBLE:
            {
                RedisModuleString *v;
                v = RedisModule_CreateStringPrintf(NULL, "%f", okey->emb_double_value);
                RedisModule_EmitAOF(aof, "SELVA.OBJECT.SET", "sbcs",
                    key,
                    okey->name, (size_t)okey->name_len,
                    "f",
                    v);
                RedisModule_FreeString(NULL, v);
            }
            break;
        case SELVA_OBJECT_LONGLONG:
            RedisModule_EmitAOF(aof, "SELVA.OBJECT.SET", "sbcl",
                key,
                okey->name, (size_t)okey->name_len,
                "i",
                okey->emb_ll_value);
            break;
        case SELVA_OBJECT_STRING:
            RedisModule_EmitAOF(aof, "SELVA.OBJECT.SET", "sbcs",
                key,
                okey->name, (size_t)okey->name_len,
                "s",
                okey->value);
            break;
        case SELVA_OBJECT_OBJECT:
            /* FIXME Nested obj AOF needs a way to pass the full object path */
            RedisModule_LogIOError(aof, "warning", "AOF rewrite not supported for nested objects");
            break;
        case SELVA_OBJECT_SET:
            set_aof_rewrite(aof, key, okey);
            break;
        case SELVA_OBJECT_ARRAY:
            /* TODO Support arrays */
            RedisModule_LogIOError(aof, "warning", "Array not supported in AOF");
            break;
        default:
            RedisModule_LogIOError(aof, "warning", "Unknown type");
        }
    }
}

void SelvaObjectTypeFree(void *value) {
    struct SelvaObject *obj = (struct SelvaObject *)value;

    destroy_selva_object(obj);
}

static int SelvaObject_OnLoad(RedisModuleCtx *ctx) {
    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = SelvaObjectTypeRDBLoad,
        .rdb_save = SelvaObjectTypeRDBSave,
        .aof_rewrite = SelvaObjectTypeAOFRewrite,
        .mem_usage = SelvaObject_MemUsage,
        .free = SelvaObjectTypeFree,
    };

    ObjectType = RedisModule_CreateDataType(ctx, "selva_obj", SELVA_OBJECT_ENCODING_VERSION, &tm);
    if (ObjectType == NULL) {
        return REDISMODULE_ERR;
    }

    /*
     * Register commands.
     */
    if (RedisModule_CreateCommand(ctx, "selva.object.del", SelvaObject_DelCommand, "write", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.exists", SelvaObject_ExistsCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.get", SelvaObject_GetCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
#if 0
        RedisModule_CreateCommand(ctx, "selva.object.getrange", SelvaObject_GetRangeCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.incrby", SelvaObject_IncrbyCommand, "write", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.incrbydouble", SelvaObject_IncrbyDoubleCommand, "write", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.keys", SelvaObject_KeysCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
#endif
        RedisModule_CreateCommand(ctx, "selva.object.len", SelvaObject_LenCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
#if 0
        RedisModule_CreateCommand(ctx, "selva.object.mget", SelvaObject_MgetCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.mset", SelvaObject_MsetCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.scan", SelvaObject_ScanCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
#endif
        RedisModule_CreateCommand(ctx, "selva.object.set", SelvaObject_SetCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR ||
#if 0
        RedisModule_CreateCommand(ctx, "selva.object.setnx", SelvaObject_SetNXCommand, "write deny-oom", 1, 1, 1) == REDISMODULE_ERR ||
        RedisModule_CreateCommand(ctx, "selva.object.strlen", SelvaObject_StrlenCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR ||
#endif
        RedisModule_CreateCommand(ctx, "selva.object.type", SelvaObject_TypeCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR
        /*RedisModule_CreateCommand(ctx, "selva.object.vals", SelvaObject_ValsCommand, "readonly", 1, 1, 1) == REDISMODULE_ERR*/) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}
SELVA_ONLOAD(SelvaObject_OnLoad);
