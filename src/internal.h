#ifndef LUMI_INTERNAL_H
#define LUMI_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "lumi/bytecode.h"

typedef enum lumi_token_type {
    TOKEN_EOF = 0,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_NEWLINE,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_COMMA,
    TOKEN_DOT_DOT,
    TOKEN_ASSIGN,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_BANG,
    TOKEN_EQ_EQ,
    TOKEN_BANG_EQ,
    TOKEN_LT,
    TOKEN_LT_EQ,
    TOKEN_GT,
    TOKEN_GT_EQ,
    TOKEN_AND_AND,
    TOKEN_OR_OR,
    TOKEN_KEYWORD_TYPE,
    TOKEN_KEYWORD_LET,
    TOKEN_KEYWORD_VAR,
    TOKEN_KEYWORD_COLOR,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_GLOBAL,
    TOKEN_KEYWORD_KEY,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_IN,
    TOKEN_KEYWORD_INIT,
    TOKEN_KEYWORD_UPDATE,
    TOKEN_KEYWORD_RENDER,
    TOKEN_INVALID,
} lumi_token_type;

typedef struct lumi_token {
    lumi_token_type type;
    const char *start;
    size_t length;
    size_t line;
    size_t column;
    double number;
} lumi_token;

typedef struct lumi_lexer {
    const char *source;
    const char *cursor;
    size_t line;
    size_t column;
} lumi_lexer;

void lumi_lexer_init(lumi_lexer *lexer, const char *source);
lumi_token lumi_lexer_next(lumi_lexer *lexer);

typedef enum lumi_expr_kind {
    EXPR_NUMBER = 0,
    EXPR_SYMBOL,
    EXPR_INDEX,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,
    EXPR_IF,
} lumi_expr_kind;

typedef struct lumi_expr lumi_expr;
typedef struct lumi_stmt lumi_stmt;

struct lumi_expr {
    lumi_expr_kind kind;
    size_t line;
    size_t column;
    union {
        double number;
        struct {
            char *name;
        } symbol;
        struct {
            char *name;
            lumi_expr *index;
        } index;
        struct {
            lumi_token_type op;
            lumi_expr *operand;
        } unary;
        struct {
            lumi_token_type op;
            lumi_expr *left;
            lumi_expr *right;
        } binary;
        struct {
            char *name;
            lumi_expr **args;
            size_t arg_count;
        } call;
        struct {
            lumi_expr *condition;
            lumi_expr *then_value;
            lumi_expr *else_value;
        } if_expr;
    } as;
};

typedef struct lumi_stmt_list {
    lumi_stmt **items;
    size_t count;
    size_t capacity;
} lumi_stmt_list;

typedef enum lumi_stmt_kind {
    STMT_LET = 0,
    STMT_ASSIGN,
    STMT_COLOR,
    STMT_IF,
    STMT_FOR,
} lumi_stmt_kind;

struct lumi_stmt {
    lumi_stmt_kind kind;
    size_t line;
    size_t column;
    union {
        struct {
            char *name;
            lumi_expr *value;
        } binding;
        struct {
            char *name;
            lumi_expr *value;
            lumi_expr *index;
        } assign;
        struct {
            lumi_expr *value;
        } color;
        struct {
            lumi_expr *condition;
            lumi_stmt_list then_branch;
            lumi_stmt_list else_branch;
        } if_stmt;
        struct {
            char *name;
            lumi_expr *start;
            lumi_expr *end;
            lumi_stmt_list body;
        } for_stmt;
    } as;
};

typedef enum lumi_var_storage_kind {
    LUMI_VAR_GLOBAL = 0,
    LUMI_VAR_KEY,
} lumi_var_storage_kind;

typedef struct lumi_var_decl {
    char *name;
    lumi_var_storage_kind storage;
    size_t array_size;
    lumi_expr *initializer;
} lumi_var_decl;

typedef struct lumi_var_decl_list {
    lumi_var_decl *items;
    size_t count;
    size_t capacity;
} lumi_var_decl_list;

typedef struct lumi_section {
    int present;
    lumi_stmt_list statements;
} lumi_section;

typedef struct lumi_program {
    lumi_program_type program_type;
    int type_seen;
    lumi_var_decl_list vars;
    lumi_section init_section;
    lumi_section update_section;
    lumi_section render_section;
} lumi_program;

typedef struct lumi_parse_error {
    char *message;
    size_t line;
    size_t column;
} lumi_parse_error;

int lumi_parse(const char *source, lumi_program *out_program, lumi_parse_error *out_error);
void lumi_program_free(lumi_program *program);

typedef struct lumi_emit_error {
    char *message;
    size_t line;
    size_t column;
} lumi_emit_error;

int lumi_emit_bytecode(lumi_program *program, int optimization_level, lumi_bytecode *out_bytecode, lumi_emit_error *out_error);

#endif
