#pragma once
#ifndef _MODIFY_RPN_H_
#define _MODIFY_RPN_H_

#define SMALL_OPERAND_SIZE 40
#define SMALL_OPERAND_POOL_SIZE 20
#define RPN_MAX_D 256

enum rpn_error {
    RPN_ERR_OK = 0,     /*!< No error. */
    RPN_ERR_ENOMEM,     /*!< Out of memory. */
    RPN_ERR_NOTSUP,     /*!< Operation not supported. */
    RPN_ERR_ILLOPC,     /*!< Illegal operator. */
    RPN_ERR_ILLOPN,     /*!< Illegal operand. */
    RPN_ERR_BADSTK,     /*!< Stack error. */
    RPN_ERR_TYPE,       /*!< Type error. */
    RPN_ERR_BNDS,       /*!< Register index out of bounds. */
    RPN_ERR_NPE,        /*!< A NULL pointer was encountered. */
    RPN_ERR_NAN,        /*!< Not a number. */
    RPN_ERR_DIV,        /*!< Divide by zero. */
};

struct rpn_operand;
struct RedisModuleCtx;

struct rpn_ctx {
    int depth;
    int nr_reg;
    struct RedisModuleCtx *redis_ctx;
    struct rpn_operand **reg;
    struct rpn_operand *stack[RPN_MAX_D];
};

extern const char *rpn_str_error[11];

struct rpn_ctx *rpn_init(struct RedisModuleCtx *redis_ctx, int nr_reg);
void rpn_destroy(struct rpn_ctx *ctx);
enum rpn_error rpn_set_reg(struct rpn_ctx *ctx, size_t i, const char *s, size_t slen);
enum rpn_error rpn_bool(struct rpn_ctx *ctx, const char *s, size_t slen, int *out);
enum rpn_error rpn_integer(struct rpn_ctx *ctx, const char *s, size_t slen, long long *out);

#endif /* _MODIFY_RPN_H_ */
