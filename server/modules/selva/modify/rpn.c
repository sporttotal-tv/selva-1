#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdefs.h"
#include "../rmutil/sds.h"
#include "redismodule.h"
#include "hierarchy.h"
#include "rpn.h"

#define RPN_ASSERTS 0
#define RPN_SINGLETON 1

struct redisObjectAccessor {
    uint32_t _meta;
    int refcount;
    void *ptr;
};

#define OPERAND(ctx, x) \
    struct rpn_operand * x __attribute__((cleanup(free_rpn_operand))) = pop(ctx); \
    if (!x) return RPN_ERR_BADSTK

#define OPERAND_GET_S(x) \
     ((const char *)(((x)->sp) ? (x)->sp : (x)->s))

struct rpn_operand {
    struct {
        unsigned in_use :  1; /* In use/in stack, do not free. */
        unsigned pooled :  1; /* Pooled operand, do not free. */
        unsigned regist :  1; /* Register value, do not free. */
        unsigned nan    :  1; /* The register value is not a number. */
        unsigned spare1 : 30;
        unsigned spare2 : 30;
    } flags;
    long long i;
    size_t s_size;
    struct rpn_operand *next_free; /* Next free in pool */
    const char *sp;
    char s[RPN_SMALL_OPERAND_SIZE];
};

static struct rpn_operand *small_operand_pool_next;
static struct rpn_operand small_operand_pool[RPN_SMALL_OPERAND_POOL_SIZE];

const char *rpn_str_error[] = {
    "No error",
    "Out of memory",
    "Operation not supported",
    "Illegal operator",
    "Illegal operand",
    "Stack error",
    "Type error",
    "Register index out of bounds",
    "Null pointer exception",
    "Not a number",
    "Divide by zero",
};

static void free_rpn_operand(void *p);

static void init_pool(void) __attribute__((constructor));
static void init_pool(void) {
    struct rpn_operand *prev = NULL;

    small_operand_pool_next = &small_operand_pool[0];

    for (int i = RPN_SMALL_OPERAND_POOL_SIZE - 1; i >= 0; i--) {
        small_operand_pool[i].next_free = prev;
        prev = &small_operand_pool[i];
    }

}

struct rpn_ctx *rpn_init(RedisModuleCtx *redis_ctx, int nr_reg) {
#if RPN_SINGLETON
    static struct rpn_ctx * ctx;

    if (unlikely(!ctx)) {
        ctx = RedisModule_Alloc(sizeof(struct rpn_ctx));
    }
#else
    struct rpn_ctx * ctx;

    ctx = RedisModule_Alloc(sizeof(struct rpn_ctx));
#endif
    if (!ctx) {
        return NULL;
    }

    ctx->depth = 0;
    ctx->redis_ctx = redis_ctx;
    ctx->nr_reg = nr_reg;

    ctx->reg = RedisModule_Calloc(nr_reg, sizeof(struct rpn_operand *));
    if (!ctx->reg) {
        RedisModule_Free(ctx);
        return NULL;
    }

    return ctx;
}

void rpn_destroy(struct rpn_ctx *ctx) {
    if (ctx) {
        for (int i = 0; i < ctx->nr_reg; i++) {
            struct rpn_operand *v = ctx->reg[i];

            if (!v) {
                continue;
            }

            v->flags.in_use = 0;
            v->flags.regist = 0;

            free_rpn_operand(&v);
        }

        RedisModule_Free(ctx->reg);
#if RPN_SINGLETON
        memset(ctx, 0, sizeof(struct rpn_ctx));
#else
        RedisModule_Free(ctx);
#endif
    }
}

static struct rpn_operand *alloc_rpn_operand(size_t slen) {
    struct rpn_operand *v;

    if (slen <= RPN_SMALL_OPERAND_SIZE && small_operand_pool_next) {
        v = small_operand_pool_next;
        small_operand_pool_next = v->next_free;

        memset(v, 0, sizeof(struct rpn_operand));
        v->flags.pooled = 1;
    } else {
        const size_t size = sizeof(struct rpn_operand) + (slen - SELVA_NODE_ID_SIZE);

#if RPN_ASSERTS
        assert(size > slen);
#endif
        v = RedisModule_Calloc(1, size);
    }

    return v;
}

static void free_rpn_operand(void *p) {
    struct rpn_operand **pp = (struct rpn_operand **)p;
    struct rpn_operand *v;

    if (unlikely(!pp)) {
        return;
    }

    v = *pp;
    if (!v || v->flags.in_use || v->flags.regist) {
        return;
    }

    if (v->flags.pooled) {
        struct rpn_operand *prev = small_operand_pool_next;

        /*
         * Put a pooled operand back to the pool.
         */
        small_operand_pool_next = v;
        small_operand_pool_next->next_free = prev;
    } else {
        RedisModule_Free(v);
    }
}

static enum rpn_error push(struct rpn_ctx *ctx, struct rpn_operand *v) {
	if (ctx->depth >= RPN_MAX_D) {
        fprintf(stderr, "RPN: Stack overflow\n");
        return RPN_ERR_BADSTK;
    }

    v->flags.in_use = 1;
	ctx->stack[ctx->depth++] = v;

    return RPN_ERR_OK;
}

/* TODO Handle errors */
static void push_int_result(struct rpn_ctx *ctx, long long x) {
    struct rpn_operand *v = alloc_rpn_operand(0);

    v->i = x;
    push(ctx, v);
}

/* TODO Handle errors */
static void push_string_result(struct rpn_ctx *ctx, const char *s, size_t slen) {
    const size_t size = slen + 1;
    struct rpn_operand *v = alloc_rpn_operand(size);

    v->s_size = size;
    strncpy(v->s, s, slen); /* TODO or memcpy? */
    v->s[slen] = '\0';
    v->flags.nan = 1;
    push(ctx, v);
}

/* TODO Handle errors */
static void push_empty_value(struct rpn_ctx *ctx) {
    const size_t size = 2;
    struct rpn_operand *v = alloc_rpn_operand(size);

    v->i = 0;
    v->s_size = size;
    v->s[0] = '\0';
    v->s[1] = '\0';
    push(ctx, v);
}


/* TODO Handle errors */
/**
 * Push a RedisModuleString to the stack.
 * Note that the string must not be freed while it's still in use by rpn.
 */
static void push_rm_string_result(struct rpn_ctx *ctx, const RedisModuleString *rms) {
    size_t slen;
    const char *str;
    struct rpn_operand *v;

    str = RedisModule_StringPtrLen(rms, &slen);

    v = alloc_rpn_operand(0);
    v->sp = str;
    v->s_size = slen + 1;

    push(ctx, v);
}

static struct rpn_operand *pop(struct rpn_ctx *ctx) {
	if (!ctx->depth) {
        return NULL;
    }

    struct rpn_operand *v = ctx->stack[--ctx->depth];

#if RPN_ASSERTS
    assert(v);
#endif

    v->flags.in_use = 0;

	return v;
}

static void clear_stack(struct rpn_ctx *ctx) {
    struct rpn_operand *v;

    while ((v = pop(ctx))) {
        v->flags.in_use = 0;
        free_rpn_operand(&v);
    }
}

static int to_bool(struct rpn_operand *v) {
    return (v->s_size > 0 && OPERAND_GET_S(v)[0] != '\0') || !!v->i;
}

enum rpn_error rpn_set_reg(struct rpn_ctx *ctx, size_t i, const char *s, size_t slen) {
    struct rpn_operand *old;

    if (i >= (size_t)ctx->nr_reg) {
        return RPN_ERR_BNDS;
    }

    old = ctx->reg[i];
    if (old) {
        /* Can be freed again unless some other flag inhibits it. */
        old->flags.regist = 0;

        free_rpn_operand(&old);
    }

    if (s) {
        struct rpn_operand *r;

        r = alloc_rpn_operand(0);

        /*
         * Set the string value.
         */
        r->flags.regist = 1; /* Can't be freed when this flag is set. */
        r->s_size = slen;
        r->sp = s;

        /*
         * Set the integer value.
         */
        char *e = (char *)s;
        if (slen > 0) {
            r->i = strtoll(s, &e, 10);
        }
        r->flags.nan = e == s;

        ctx->reg[i] = r;
    } else { /* Otherwise just clear the register. */
        ctx->reg[i] = NULL;
    }

    return RPN_ERR_OK;
}

static enum rpn_error rpn_get_reg(struct rpn_ctx *ctx, const char *str_index, int type) {
    const size_t i = str_index[0] - '0';

    if (i >= (size_t)ctx->nr_reg) {
        fprintf(stderr, "RPN: Register index out of bounds: %zu\n", i);
        return RPN_ERR_BNDS;
    }

    struct rpn_operand *r = ctx->reg[i];

    if (!r) {
        fprintf(stderr, "RPN: Register value is a NULL pointer: %zu\n", i);
        return RPN_ERR_NPE;
    }

    if (type == 0) {
        if (r->flags.nan) {
            fprintf(stderr, "RPN: Register value is not a number: %zu\n", i);
            return RPN_ERR_NAN;
        }

        push(ctx, r);
    } else if (type == 1) {
        push(ctx, r);
    } else {
        fprintf(stderr, "RPN: Unknown read type: %d\n", type);
        return RPN_ERR_TYPE;
    }

    return RPN_ERR_OK;
}

static enum rpn_error rpn_getfld(struct rpn_ctx *ctx, struct rpn_operand *field, int type) {
    static struct redisObjectAccessor *cid;
    RedisModuleKey *id_key;
    RedisModuleString *value = NULL;
    int err;

    if (unlikely(!cid)) {
        static const char empty_id[SELVA_NODE_ID_SIZE];
        cid = (struct redisObjectAccessor *)RedisModule_CreateString(NULL, empty_id, sizeof(empty_id));
    }

#if RPN_ASSERTS
    assert(ctx->reg[0]);
    assert(ctx->redis_ctx);
#endif

    cid->ptr = sdscpylen(cid->ptr, (void *)OPERAND_GET_S(ctx->reg[0]), SELVA_NODE_ID_SIZE);
    id_key = RedisModule_OpenKey(ctx->redis_ctx, (RedisModuleString *)(cid), REDISMODULE_READ);
    if (!id_key) {
        push_empty_value(ctx);
        goto out;
    }

    err = RedisModule_HashGet(id_key, REDISMODULE_HASH_CFIELDS, OPERAND_GET_S(field), &value, NULL);
    if (err == REDISMODULE_ERR || !value) {
        push_empty_value(ctx);
    } else {
        if (type == 0) {
            long long ivalue;
            int err;

            err = RedisModule_StringToLongLong(value, &ivalue);

            if (unlikely(err != REDISMODULE_OK)) {
                fprintf(stderr, "RPN: Field value is not a number: %.*s\n",
                        (int)field->s_size, OPERAND_GET_S(field));

                return RPN_ERR_NAN;
            }

            RedisModule_FreeString(ctx->redis_ctx, value);
            push_int_result(ctx, ivalue);
        } else {
            /*
             * Supposedly there is no need to free `value`
             * because we are using automatic memory management.
             */
            push_rm_string_result(ctx, value);
        }
    }

    goto out;
out:
    RedisModule_CloseKey(id_key);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_add(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i + b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_sub(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i - b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_div(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);
    long long d = b->i;

    if (d == 0) {
        return RPN_ERR_DIV;
    }

    push_int_result(ctx, a->i / d);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_mul(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i * b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_rem(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);
    long long d = b ->i;

    if (d == 0) {
        return RPN_ERR_DIV;
    }

    push_int_result(ctx, a->i % d);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_eq(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i == b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_ne(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i != b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_lt(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i < b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_gt(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i > b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_le(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i <= b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_ge(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, a->i >= b->i);

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_not(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);

    push_int_result(ctx, !to_bool(a));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_and(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, to_bool(a) && to_bool(b));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_or(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, to_bool(a) || to_bool(b));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_xor(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    push_int_result(ctx, to_bool(a) ^ to_bool(b));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_in(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);

    /* TODO */

    return RPN_ERR_NOTSUP;
}

static enum rpn_error rpn_op_typeof(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    char t[SELVA_NODE_TYPE_SIZE];
    const char *s = OPERAND_GET_S(a);

    if (a->s_size < SELVA_NODE_ID_SIZE) {
        return RPN_ERR_TYPE;
    }

#if SELVA_NODE_TYPE_SIZE != 2
#error Expected SELVA_NODE_TYPE_SIZE to be 2
#endif
    t[0] = s[0];
    t[1] = s[1];

    push_string_result(ctx, t, sizeof(t));

    return RPN_ERR_OK;
}

static inline size_t size_min(size_t a, size_t b) {
    return (a < b ? a : b);
}

static enum rpn_error rpn_op_strcmp(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);
    const size_t a_size = a->s_size;
    const size_t b_size = b->s_size;
    const ssize_t sizeDiff = b_size - a_size;

    push_int_result(ctx,
            !sizeDiff &&
            !strncmp(OPERAND_GET_S(a), OPERAND_GET_S(b),
                     size_min(a_size, b_size)));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_idcmp(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);
    OPERAND(ctx, b);
    const int size_ok = a->s_size >= SELVA_NODE_ID_SIZE && b->s_size >= SELVA_NODE_ID_SIZE;

    push_int_result(ctx, size_ok && !memcmp(OPERAND_GET_S(a), OPERAND_GET_S(b), SELVA_NODE_ID_SIZE));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_cidcmp(struct rpn_ctx *ctx) {
    OPERAND(ctx, a);

#if RPN_ASSERTS
    assert(ctx->reg[0]);
    assert(ctx->reg[0]->s_size == SELVA_NODE_ID_SIZE);
#endif

    push_int_result(ctx, !memcmp(OPERAND_GET_S(a), OPERAND_GET_S(ctx->reg[0]), SELVA_NODE_TYPE_SIZE));

    return RPN_ERR_OK;
}

static enum rpn_error rpn_op_getsfld(struct rpn_ctx *ctx) {
    OPERAND(ctx, field);

    return rpn_getfld(ctx, field, 1);
}

static enum rpn_error rpn_op_getifld(struct rpn_ctx *ctx) {
    OPERAND(ctx, field);

    return rpn_getfld(ctx, field, 0);
}

static enum rpn_error rpn_op_abo(struct rpn_ctx *ctx __unused) {
    return RPN_ERR_ILLOPC;
}

typedef enum rpn_error (*rpn_fp)(struct rpn_ctx *ctx);

static rpn_fp funcs[] = {
    rpn_op_add,     /* A */
    rpn_op_sub,     /* B */
    rpn_op_div,     /* C */
    rpn_op_mul,     /* D */
    rpn_op_rem,     /* E */
    rpn_op_eq,      /* F */
    rpn_op_ne,      /* G */
    rpn_op_lt,      /* H */
    rpn_op_gt,      /* I */
    rpn_op_le,      /* J */
    rpn_op_ge,      /* K */
    rpn_op_not,     /* L */
    rpn_op_and,     /* M */
    rpn_op_or,      /* N */
    rpn_op_xor,     /* O */
    rpn_op_abo,     /* P spare */
    rpn_op_abo,     /* Q spare */
    rpn_op_abo,     /* R spare */
    rpn_op_abo,     /* S spare */
    rpn_op_abo,     /* T spare */
    rpn_op_abo,     /* U spare */
    rpn_op_abo,     /* V spare */
    rpn_op_abo,     /* W spare */
    rpn_op_abo,     /* X */
    rpn_op_abo,     /* Y */
    rpn_op_abo,     /* Z */
    rpn_op_abo,     /* N/A */
    rpn_op_abo,     /* N/A */
    rpn_op_abo,     /* N/A */
    rpn_op_abo,     /* N/A */
    rpn_op_abo,     /* N/A */
    rpn_op_abo,     /* N/A */
    rpn_op_in,      /* a */
    rpn_op_typeof,  /* b */
    rpn_op_strcmp,  /* c */
    rpn_op_idcmp,   /* d */
    rpn_op_cidcmp,  /* e */
    rpn_op_getsfld, /* f */
    rpn_op_getifld, /* g */
};

rpn_token *rpn_compile(const char *input, size_t len) {
    const char *w = " \t\n\r\f";
    rpn_token *expr;
    char sa[len + 1];


    memcpy(sa, input, len);
    sa[len] = '\0';

    size_t i = 0;
    size_t size = 2 * sizeof(rpn_token);

    expr = RedisModule_Alloc(size);
    if (!expr) {
        return NULL;
    }

    char *s = sa;
    for (s = strtok(s, w); s; s = strtok(0, w)) {
        rpn_token *new = RedisModule_Realloc(expr, size);
        if (new) {
            expr = new;
        } else {
            RedisModule_Free(expr);
            return NULL;
        }

        strncpy(expr[i++], s, 14);
        size += sizeof(rpn_token);
    }
    memset(expr[i], 0, sizeof(rpn_token));

    return expr;
}

static enum rpn_error rpn(struct rpn_ctx *ctx, const rpn_token *expr) {
    const rpn_token *it = expr;
    const char *s;

    while (*(s = *it++)) {
        size_t op = *s - 'A';

        if (op < sizeof(funcs) / sizeof(void *)) { /* Operator */
            enum rpn_error err;
            err = funcs[op](ctx);
            if (err) {
                clear_stack(ctx);

                return err;
            }
        } else { /* Operand */
            switch (s[0]) {
            case '@':
                {
                    const char *str = s + 1;
                    enum rpn_error err;

                    err = rpn_get_reg(ctx, str, 0);
                    if (err) {
                        clear_stack(ctx);

                        return err;
                    }
                }
                break;
            case '$':
                {
                    const char *str = s + 1;
                    enum rpn_error err;

                    err = rpn_get_reg(ctx, str, 1);
                    if (err) {
                        clear_stack(ctx);

                        return err;
                    }
                }
                break;
            case '#':
                {
                    struct rpn_operand *v;
                    const char *str = s + 1;
                    char *e;
                    enum rpn_error err;

                    v = alloc_rpn_operand(0);
                    v->i = strtoll(str, &e, 10);
                    v->s_size = 0;
                    v->s[0] = '\0';

                    if (unlikely(e == str)) {
                        fprintf(stderr, "RPN: Operand is not a number: %s\n", s);
                        return RPN_ERR_NAN;
                    }

                    /* TODO Handle NULL */
                    err = push(ctx, v);
                    if (err) {
                        clear_stack(ctx);
                        return err;
                    }
                }
                break;
            case '"':
                {
                    struct rpn_operand *v;
                    const char *str = s + 1;
                    size_t size = strlen(str) + 1;
                    enum rpn_error err;

#ifdef RPN_ASSERTS
                    /* We don't expect to see extra long strings here. */
                    assert(size <= 120);
#endif

                    v = alloc_rpn_operand(size);
                    v->s_size = size;
                    strcpy(v->s, str);
                    v->s[size - 1] = '\0';
                    v->flags.nan = 1;

                    err = push(ctx, v);
                    if (err) {
                        clear_stack(ctx);
                        return err;
                    }
                }
                break;
            default:
                clear_stack(ctx);
                return RPN_ERR_ILLOPN;
            }
        }
	}

	if (ctx->depth != 1) {
        clear_stack(ctx);
        return RPN_ERR_BADSTK;
    }

    return RPN_ERR_OK;
}

enum rpn_error rpn_bool(struct rpn_ctx *ctx, const rpn_token *expr, int *out) {
    struct rpn_operand *res;
    enum rpn_error err;

    err = rpn(ctx, expr);
    if (err) {
        return err;
    }

    res = pop(ctx);
    if (!res) {
        return RPN_ERR_BADSTK;
    }

    *out = to_bool(res);
    free_rpn_operand(&res);

    return 0;
}

enum rpn_error rpn_integer(struct rpn_ctx *ctx, const rpn_token *expr, long long *out) {
    struct rpn_operand *res;
    enum rpn_error err;

    err = rpn(ctx, expr);
    if (err) {
        return err;
    }

    res = pop(ctx);
    if (!res) {
        return RPN_ERR_BADSTK;
    }

    *out = res->i;
    free_rpn_operand(&res);

    return 0;
}