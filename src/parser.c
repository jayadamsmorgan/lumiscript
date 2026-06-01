#include "internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct parser {
    lumi_lexer lexer;
    lumi_token current;
    lumi_token previous;
    lumi_parse_error *error;
    int had_error;
} parser;

typedef enum const_kind {
    CONST_UNKNOWN = 0,
    CONST_FLOAT,
} const_kind;

typedef struct const_value {
    const_kind kind;
    float number;
} const_value;

typedef enum symbol_kind {
    SYMBOL_INPUT = 0,
    SYMBOL_GLOBAL_VAR,
    SYMBOL_KEY_VAR,
    SYMBOL_LET,
} symbol_kind;

typedef enum section_kind {
    SECTION_INIT = 0,
    SECTION_UPDATE,
    SECTION_RENDER,
} section_kind;

typedef struct symbol {
    char *name;
    symbol_kind kind;
    size_t index;
    size_t array_size;
    int is_const;
    const_value value;
    const lumi_expr *expr;
    size_t scope_depth;
} symbol;

typedef struct symbol_table {
    symbol *items;
    size_t count;
    size_t capacity;
} symbol_table;

typedef struct byte_buffer {
    uint8_t *data;
    size_t count;
    size_t capacity;
} byte_buffer;

typedef struct float_buffer {
    float *data;
    size_t count;
    size_t capacity;
} float_buffer;

typedef struct emitter {
    lumi_emit_error *error;
    symbol_table symbols;
    float_buffer constants;
    float_buffer initial_globals;
    float_buffer initial_keys;
    byte_buffer init_code;
    byte_buffer update_code;
    byte_buffer render_code;
    byte_buffer *code;
    section_kind current_section;
    size_t scope_depth;
    uint16_t current_stack;
    uint16_t max_stack;
    size_t global_count;
    size_t key_count;
    int render_has_color;
    int optimization_level;
} emitter;

static void *xrealloc(void *ptr, size_t size) {
    void *out = realloc(ptr, size);
    if (out == NULL) {
        free(ptr);
    }
    return out;
}

static char *copy_string(const char *start, size_t length) {
    char *out = malloc(length + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, start, length);
    out[length] = '\0';
    return out;
}

static void set_parse_error(parser *p, size_t line, size_t column, const char *fmt, ...) {
    va_list args;
    int needed;

    if (p->had_error) {
        return;
    }

    p->error->line = line;
    p->error->column = column;
    va_start(args, fmt);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    p->error->message = malloc((size_t)needed + 1);
    if (p->error->message != NULL) {
        va_start(args, fmt);
        vsnprintf(p->error->message, (size_t)needed + 1, fmt, args);
        va_end(args);
    }
    p->had_error = 1;
}

static void set_emit_error(emitter *e, size_t line, size_t column, const char *fmt, ...) {
    va_list args;
    int needed;

    if (e->error->message != NULL) {
        return;
    }

    e->error->line = line;
    e->error->column = column;
    va_start(args, fmt);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    e->error->message = malloc((size_t)needed + 1);
    if (e->error->message != NULL) {
        va_start(args, fmt);
        vsnprintf(e->error->message, (size_t)needed + 1, fmt, args);
        va_end(args);
    }
}

static int stmt_list_push(lumi_stmt_list *list, lumi_stmt *stmt) {
    lumi_stmt **items;
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        items = xrealloc(list->items, new_capacity * sizeof(*items));
        if (items == NULL) {
            return 0;
        }
        list->items = items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = stmt;
    return 1;
}

static int var_list_push(lumi_var_decl_list *list, lumi_var_decl decl) {
    lumi_var_decl *items;
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        items = xrealloc(list->items, new_capacity * sizeof(*items));
        if (items == NULL) {
            return 0;
        }
        list->items = items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = decl;
    return 1;
}

static lumi_expr *new_expr(lumi_expr_kind kind, size_t line, size_t column) {
    lumi_expr *expr = calloc(1, sizeof(*expr));
    if (expr != NULL) {
        expr->kind = kind;
        expr->line = line;
        expr->column = column;
    }
    return expr;
}

static lumi_stmt *new_stmt(lumi_stmt_kind kind, size_t line, size_t column) {
    lumi_stmt *stmt = calloc(1, sizeof(*stmt));
    if (stmt != NULL) {
        stmt->kind = kind;
        stmt->line = line;
        stmt->column = column;
    }
    return stmt;
}

static void advance(parser *p) {
    p->previous = p->current;
    p->current = lumi_lexer_next(&p->lexer);
}

static lumi_token_type peek_type(parser *p) {
    lumi_lexer snapshot = p->lexer;
    lumi_token peeked = lumi_lexer_next(&snapshot);
    return peeked.type;
}

static int match(parser *p, lumi_token_type type) {
    if (p->current.type != type) {
        return 0;
    }
    advance(p);
    return 1;
}

static int expect(parser *p, lumi_token_type type, const char *message) {
    if (p->current.type == type) {
        advance(p);
        return 1;
    }
    set_parse_error(p, p->current.line, p->current.column, "%s", message);
    return 0;
}

static void skip_newlines(parser *p) {
    while (match(p, TOKEN_NEWLINE)) {
    }
}

static lumi_expr *parse_expression(parser *p);
static void free_expr(lumi_expr *expr);
static void free_stmt(lumi_stmt *stmt);

static lumi_expr *parse_if_expression(parser *p) {
    lumi_expr *expr;
    lumi_expr *condition;
    lumi_expr *then_value;
    lumi_expr *else_value;
    lumi_token token = p->previous;

    condition = parse_expression(p);
    if (condition == NULL) {
        return NULL;
    }
    if (!expect(p, TOKEN_LBRACE, "expected '{' after if condition")) {
        return NULL;
    }
    skip_newlines(p);
    then_value = parse_expression(p);
    if (then_value == NULL) {
        return NULL;
    }
    skip_newlines(p);
    if (!expect(p, TOKEN_RBRACE, "expected '}' after if expression branch")) {
        return NULL;
    }
    skip_newlines(p);
    if (!expect(p, TOKEN_KEYWORD_ELSE, "if expression requires else branch")) {
        return NULL;
    }
    if (!expect(p, TOKEN_LBRACE, "expected '{' after else")) {
        return NULL;
    }
    skip_newlines(p);
    else_value = parse_expression(p);
    if (else_value == NULL) {
        return NULL;
    }
    skip_newlines(p);
    if (!expect(p, TOKEN_RBRACE, "expected '}' after else branch")) {
        return NULL;
    }

    expr = new_expr(EXPR_IF, token.line, token.column);
    if (expr == NULL) {
        return NULL;
    }
    expr->as.if_expr.condition = condition;
    expr->as.if_expr.then_value = then_value;
    expr->as.if_expr.else_value = else_value;
    return expr;
}

static lumi_expr *parse_primary(parser *p) {
    lumi_expr *expr;
    lumi_token token = p->current;

    if (match(p, TOKEN_NUMBER)) {
        expr = new_expr(EXPR_NUMBER, token.line, token.column);
        if (expr != NULL) {
            expr->as.number = token.number;
        }
        return expr;
    }

    if (match(p, TOKEN_KEYWORD_IF)) {
        return parse_if_expression(p);
    }

    if (match(p, TOKEN_IDENTIFIER)) {
        char *name = copy_string(token.start, token.length);
        if (name == NULL) {
            return NULL;
        }
        if (match(p, TOKEN_LPAREN)) {
            lumi_expr **args = NULL;
            size_t arg_count = 0;
            size_t arg_capacity = 0;
            expr = new_expr(EXPR_CALL, token.line, token.column);
            if (expr == NULL) {
                free(name);
                return NULL;
            }
            skip_newlines(p);
            if (!match(p, TOKEN_RPAREN)) {
                do {
                    lumi_expr *arg = parse_expression(p);
                    lumi_expr **new_args;
                    if (arg == NULL) {
                        free(name);
                        free(expr);
                        return NULL;
                    }
                    if (arg_count == arg_capacity) {
                        arg_capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
                        new_args = xrealloc(args, arg_capacity * sizeof(*new_args));
                        if (new_args == NULL) {
                            free(name);
                            free(expr);
                            return NULL;
                        }
                        args = new_args;
                    }
                    args[arg_count++] = arg;
                    skip_newlines(p);
                } while (match(p, TOKEN_COMMA) && (skip_newlines(p), 1));
                skip_newlines(p);
                if (!expect(p, TOKEN_RPAREN, "expected ')' after function arguments")) {
                    free(name);
                    free(expr);
                    free(args);
                    return NULL;
                }
            }
            expr->as.call.name = name;
            expr->as.call.args = args;
            expr->as.call.arg_count = arg_count;
            return expr;
        }
        if (match(p, TOKEN_LBRACKET)) {
            lumi_expr *index_expr;
            index_expr = parse_expression(p);
            if (index_expr == NULL) {
                free(name);
                return NULL;
            }
            if (!expect(p, TOKEN_RBRACKET, "expected ']' after index expression")) {
                free(name);
                free_expr(index_expr);
                return NULL;
            }
            expr = new_expr(EXPR_INDEX, token.line, token.column);
            if (expr == NULL) {
                free(name);
                free_expr(index_expr);
                return NULL;
            }
            expr->as.index.name = name;
            expr->as.index.index = index_expr;
            return expr;
        }
        expr = new_expr(EXPR_SYMBOL, token.line, token.column);
        if (expr != NULL) {
            expr->as.symbol.name = name;
        } else {
            free(name);
        }
        return expr;
    }

    if (match(p, TOKEN_LPAREN)) {
        skip_newlines(p);
        expr = parse_expression(p);
        if (expr == NULL) {
            return NULL;
        }
        skip_newlines(p);
        if (!expect(p, TOKEN_RPAREN, "expected ')' after expression")) {
            return NULL;
        }
        return expr;
    }

    if (p->current.type == TOKEN_INVALID) {
        set_parse_error(p, p->current.line, p->current.column, "invalid character '%.*s'", (int)p->current.length, p->current.start);
        return NULL;
    }

    set_parse_error(p, p->current.line, p->current.column, "expected expression");
    return NULL;
}

static lumi_expr *parse_unary(parser *p) {
    lumi_token token = p->current;
    lumi_expr *operand;
    lumi_expr *expr;

    if (!match(p, TOKEN_MINUS) && !match(p, TOKEN_BANG)) {
        return parse_primary(p);
    }

    operand = parse_unary(p);
    if (operand == NULL) {
        return NULL;
    }
    expr = new_expr(EXPR_UNARY, token.line, token.column);
    if (expr == NULL) {
        return NULL;
    }
    expr->as.unary.op = token.type;
    expr->as.unary.operand = operand;
    return expr;
}

static lumi_expr *parse_factor(parser *p) {
    lumi_expr *expr = parse_unary(p);
    while (expr != NULL && (p->current.type == TOKEN_STAR || p->current.type == TOKEN_SLASH || p->current.type == TOKEN_PERCENT)) {
        lumi_token token = p->current;
        lumi_expr *right;
        lumi_expr *node;
        advance(p);
        right = parse_unary(p);
        if (right == NULL) {
            return NULL;
        }
        node = new_expr(EXPR_BINARY, token.line, token.column);
        if (node == NULL) {
            return NULL;
        }
        node->as.binary.op = token.type;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static lumi_expr *parse_term(parser *p) {
    lumi_expr *expr = parse_factor(p);
    while (expr != NULL && (p->current.type == TOKEN_PLUS || p->current.type == TOKEN_MINUS)) {
        lumi_token token = p->current;
        lumi_expr *right;
        lumi_expr *node;
        advance(p);
        right = parse_factor(p);
        if (right == NULL) {
            return NULL;
        }
        node = new_expr(EXPR_BINARY, token.line, token.column);
        if (node == NULL) {
            return NULL;
        }
        node->as.binary.op = token.type;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static lumi_expr *parse_comparison(parser *p) {
    lumi_expr *expr = parse_term(p);
    while (expr != NULL && (p->current.type == TOKEN_LT || p->current.type == TOKEN_LT_EQ
        || p->current.type == TOKEN_GT || p->current.type == TOKEN_GT_EQ)) {
        lumi_token token = p->current;
        lumi_expr *right;
        lumi_expr *node;
        advance(p);
        right = parse_term(p);
        if (right == NULL) {
            return NULL;
        }
        node = new_expr(EXPR_BINARY, token.line, token.column);
        if (node == NULL) {
            return NULL;
        }
        node->as.binary.op = token.type;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static lumi_expr *parse_equality(parser *p) {
    lumi_expr *expr = parse_comparison(p);
    while (expr != NULL && (p->current.type == TOKEN_EQ_EQ || p->current.type == TOKEN_BANG_EQ)) {
        lumi_token token = p->current;
        lumi_expr *right;
        lumi_expr *node;
        advance(p);
        right = parse_comparison(p);
        if (right == NULL) {
            return NULL;
        }
        node = new_expr(EXPR_BINARY, token.line, token.column);
        if (node == NULL) {
            return NULL;
        }
        node->as.binary.op = token.type;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static lumi_expr *parse_and(parser *p) {
    lumi_expr *expr = parse_equality(p);
    while (expr != NULL && p->current.type == TOKEN_AND_AND) {
        lumi_token token = p->current;
        lumi_expr *right;
        lumi_expr *node;
        advance(p);
        right = parse_equality(p);
        if (right == NULL) {
            return NULL;
        }
        node = new_expr(EXPR_BINARY, token.line, token.column);
        if (node == NULL) {
            return NULL;
        }
        node->as.binary.op = token.type;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static lumi_expr *parse_expression(parser *p) {
    lumi_expr *expr = parse_and(p);
    while (expr != NULL && p->current.type == TOKEN_OR_OR) {
        lumi_token token = p->current;
        lumi_expr *right;
        lumi_expr *node;
        advance(p);
        right = parse_and(p);
        if (right == NULL) {
            return NULL;
        }
        node = new_expr(EXPR_BINARY, token.line, token.column);
        if (node == NULL) {
            return NULL;
        }
        node->as.binary.op = token.type;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static lumi_stmt *parse_statement(parser *p);

static int parse_block(parser *p, lumi_stmt_list *out_list) {
    skip_newlines(p);
    while (p->current.type != TOKEN_RBRACE && p->current.type != TOKEN_EOF && !p->had_error) {
        lumi_stmt *stmt = parse_statement(p);
        if (stmt == NULL) {
            return 0;
        }
        if (!stmt_list_push(out_list, stmt)) {
            set_parse_error(p, stmt->line, stmt->column, "out of memory");
            return 0;
        }
        skip_newlines(p);
    }
    return expect(p, TOKEN_RBRACE, "expected '}' after block");
}

static lumi_stmt *parse_statement(parser *p) {
    lumi_stmt *stmt;
    lumi_token name;
    lumi_expr *value;

    if (match(p, TOKEN_KEYWORD_LET)) {
        if (p->current.type != TOKEN_IDENTIFIER) {
            set_parse_error(p, p->current.line, p->current.column, "expected identifier after let");
            return NULL;
        }
        name = p->current;
        advance(p);
        if (!expect(p, TOKEN_ASSIGN, "expected '=' after identifier")) {
            return NULL;
        }
        value = parse_expression(p);
        if (value == NULL) {
            return NULL;
        }
        stmt = new_stmt(STMT_LET, name.line, name.column);
        if (stmt != NULL) {
            stmt->as.binding.name = copy_string(name.start, name.length);
            stmt->as.binding.value = value;
        }
        return stmt;
    }

    if (p->current.type == TOKEN_IDENTIFIER
        && (peek_type(p) == TOKEN_ASSIGN || peek_type(p) == TOKEN_LBRACKET)) {
        lumi_expr *index_expr = NULL;
        name = p->current;
        advance(p);
        if (match(p, TOKEN_LBRACKET)) {
            index_expr = parse_expression(p);
            if (index_expr == NULL) {
                return NULL;
            }
            if (!expect(p, TOKEN_RBRACKET, "expected ']' after index expression")) {
                free_expr(index_expr);
                return NULL;
            }
        }
        if (!expect(p, TOKEN_ASSIGN, "expected '=' after assignment target")) {
            free_expr(index_expr);
            return NULL;
        }
        value = parse_expression(p);
        if (value == NULL) {
            free_expr(index_expr);
            return NULL;
        }
        stmt = new_stmt(STMT_ASSIGN, name.line, name.column);
        if (stmt != NULL) {
            stmt->as.assign.name = copy_string(name.start, name.length);
            stmt->as.assign.value = value;
            stmt->as.assign.index = index_expr;
        } else {
            free_expr(index_expr);
        }
        return stmt;
    }

    if (match(p, TOKEN_KEYWORD_COLOR)) {
        value = parse_expression(p);
        if (value == NULL) {
            return NULL;
        }
        stmt = new_stmt(STMT_COLOR, p->previous.line, p->previous.column);
        if (stmt != NULL) {
            stmt->as.color.value = value;
        }
        return stmt;
    }

    if (match(p, TOKEN_KEYWORD_IF)) {
        lumi_expr *condition = parse_expression(p);
        lumi_stmt_list then_branch = {0};
        lumi_stmt_list else_branch = {0};
        lumi_token token = p->previous;

        if (condition == NULL) {
            return NULL;
        }
        if (!expect(p, TOKEN_LBRACE, "expected '{' after if condition")) {
            return NULL;
        }
        if (!parse_block(p, &then_branch)) {
            return NULL;
        }
        if (match(p, TOKEN_KEYWORD_ELSE)) {
            if (!expect(p, TOKEN_LBRACE, "expected '{' after else")) {
                return NULL;
            }
            if (!parse_block(p, &else_branch)) {
                return NULL;
            }
        }
        stmt = new_stmt(STMT_IF, token.line, token.column);
        if (stmt != NULL) {
            stmt->as.if_stmt.condition = condition;
            stmt->as.if_stmt.then_branch = then_branch;
            stmt->as.if_stmt.else_branch = else_branch;
        }
        return stmt;
    }

    if (match(p, TOKEN_KEYWORD_FOR)) {
        lumi_expr *start_expr;
        lumi_expr *end_expr;
        lumi_stmt_list body = {0};
        lumi_token token = p->previous;
        if (p->current.type != TOKEN_IDENTIFIER) {
            set_parse_error(p, p->current.line, p->current.column, "expected loop variable name");
            return NULL;
        }
        name = p->current;
        advance(p);
        if (!expect(p, TOKEN_KEYWORD_IN, "expected 'in' after loop variable")) {
            return NULL;
        }
        start_expr = parse_expression(p);
        if (start_expr == NULL) {
            return NULL;
        }
        if (!expect(p, TOKEN_DOT_DOT, "expected '..' in for range")) {
            free_expr(start_expr);
            return NULL;
        }
        end_expr = parse_expression(p);
        if (end_expr == NULL) {
            free_expr(start_expr);
            return NULL;
        }
        if (!expect(p, TOKEN_LBRACE, "expected '{' after for range")) {
            free_expr(start_expr);
            free_expr(end_expr);
            return NULL;
        }
        if (!parse_block(p, &body)) {
            free_expr(start_expr);
            free_expr(end_expr);
            return NULL;
        }
        stmt = new_stmt(STMT_FOR, token.line, token.column);
        if (stmt != NULL) {
            stmt->as.for_stmt.name = copy_string(name.start, name.length);
            stmt->as.for_stmt.start = start_expr;
            stmt->as.for_stmt.end = end_expr;
            stmt->as.for_stmt.body = body;
        }
        return stmt;
    }

    if (p->current.type == TOKEN_INVALID) {
        set_parse_error(p, p->current.line, p->current.column, "invalid character '%.*s'", (int)p->current.length, p->current.start);
        return NULL;
    }

    set_parse_error(p, p->current.line, p->current.column, "unexpected token");
    return NULL;
}

static int parse_var_decl(parser *p, lumi_program *program, lumi_var_storage_kind storage) {
    lumi_var_decl decl;

    if (!expect(p, TOKEN_KEYWORD_VAR, "expected 'var' after storage class")) {
        return 0;
    }
    if (p->current.type != TOKEN_IDENTIFIER) {
        set_parse_error(p, p->current.line, p->current.column, "expected variable name");
        return 0;
    }
    memset(&decl, 0, sizeof(decl));
    decl.name = copy_string(p->current.start, p->current.length);
    decl.storage = storage;
    decl.array_size = 1;
    advance(p);
    if (match(p, TOKEN_LBRACKET)) {
        if (p->current.type != TOKEN_NUMBER) {
            set_parse_error(p, p->current.line, p->current.column, "expected array size");
            free(decl.name);
            return 0;
        }
        if (p->current.number < 1 || p->current.number != (double)(size_t)p->current.number) {
            set_parse_error(p, p->current.line, p->current.column, "array size must be a positive integer");
            free(decl.name);
            return 0;
        }
        decl.array_size = (size_t)p->current.number;
        advance(p);
        if (!expect(p, TOKEN_RBRACKET, "expected ']' after array size")) {
            free(decl.name);
            return 0;
        }
    }
    if (!expect(p, TOKEN_ASSIGN, "expected '=' after variable name")) {
        free(decl.name);
        return 0;
    }
    decl.initializer = parse_expression(p);
    if (decl.initializer == NULL) {
        free(decl.name);
        return 0;
    }
    if (!var_list_push(&program->vars, decl)) {
        free(decl.name);
        return 0;
    }
    return 1;
}

static int parse_section(parser *p, lumi_section *section, const char *label) {
    if (section->present) {
        set_parse_error(p, p->previous.line, p->previous.column, "section '%s' already defined", label);
        return 0;
    }
    section->present = 1;
    if (!expect(p, TOKEN_LBRACE, "expected '{' after section name")) {
        return 0;
    }
    return parse_block(p, &section->statements);
}

static void free_expr(lumi_expr *expr) {
    size_t i;
    if (expr == NULL) {
        return;
    }
    switch (expr->kind) {
        case EXPR_SYMBOL:
            free(expr->as.symbol.name);
            break;
        case EXPR_INDEX:
            free(expr->as.index.name);
            free_expr(expr->as.index.index);
            break;
        case EXPR_UNARY:
            free_expr(expr->as.unary.operand);
            break;
        case EXPR_BINARY:
            free_expr(expr->as.binary.left);
            free_expr(expr->as.binary.right);
            break;
        case EXPR_CALL:
            free(expr->as.call.name);
            for (i = 0; i < expr->as.call.arg_count; ++i) {
                free_expr(expr->as.call.args[i]);
            }
            free(expr->as.call.args);
            break;
        case EXPR_IF:
            free_expr(expr->as.if_expr.condition);
            free_expr(expr->as.if_expr.then_value);
            free_expr(expr->as.if_expr.else_value);
            break;
        case EXPR_NUMBER:
            break;
    }
    free(expr);
}

static void free_stmt(lumi_stmt *stmt) {
    size_t i;
    if (stmt == NULL) {
        return;
    }
    switch (stmt->kind) {
        case STMT_LET:
            free(stmt->as.binding.name);
            free_expr(stmt->as.binding.value);
            break;
        case STMT_ASSIGN:
            free(stmt->as.assign.name);
            free_expr(stmt->as.assign.index);
            free_expr(stmt->as.assign.value);
            break;
        case STMT_COLOR:
            free_expr(stmt->as.color.value);
            break;
        case STMT_IF:
            free_expr(stmt->as.if_stmt.condition);
            for (i = 0; i < stmt->as.if_stmt.then_branch.count; ++i) {
                free_stmt(stmt->as.if_stmt.then_branch.items[i]);
            }
            for (i = 0; i < stmt->as.if_stmt.else_branch.count; ++i) {
                free_stmt(stmt->as.if_stmt.else_branch.items[i]);
            }
            free(stmt->as.if_stmt.then_branch.items);
            free(stmt->as.if_stmt.else_branch.items);
            break;
        case STMT_FOR:
            free(stmt->as.for_stmt.name);
            free_expr(stmt->as.for_stmt.start);
            free_expr(stmt->as.for_stmt.end);
            for (i = 0; i < stmt->as.for_stmt.body.count; ++i) {
                free_stmt(stmt->as.for_stmt.body.items[i]);
            }
            free(stmt->as.for_stmt.body.items);
            break;
    }
    free(stmt);
}

int lumi_parse(const char *source, lumi_program *out_program, lumi_parse_error *out_error) {
    parser p;

    memset(&p, 0, sizeof(p));
    memset(out_program, 0, sizeof(*out_program));
    memset(out_error, 0, sizeof(*out_error));
    lumi_lexer_init(&p.lexer, source);
    p.error = out_error;
    advance(&p);
    skip_newlines(&p);

    while (p.current.type != TOKEN_EOF && !p.had_error) {
        if (match(&p, TOKEN_KEYWORD_TYPE)) {
            if (out_program->type_seen) {
                set_parse_error(&p, p.previous.line, p.previous.column, "program type already declared");
                break;
            }
            if (p.current.type != TOKEN_IDENTIFIER) {
                set_parse_error(&p, p.current.line, p.current.column, "expected program type name");
                break;
            }
            if (strncmp(p.current.start, "static", p.current.length) == 0 && p.current.length == 6) {
                out_program->program_type = LUMI_PROGRAM_STATIC;
            } else if (strncmp(p.current.start, "animation", p.current.length) == 0 && p.current.length == 9) {
                out_program->program_type = LUMI_PROGRAM_ANIMATION;
            } else {
                set_parse_error(&p, p.current.line, p.current.column, "unknown program type");
                break;
            }
            out_program->type_seen = 1;
            advance(&p);
        } else if (match(&p, TOKEN_KEYWORD_GLOBAL)) {
            if (!parse_var_decl(&p, out_program, LUMI_VAR_GLOBAL)) {
                break;
            }
        } else if (match(&p, TOKEN_KEYWORD_KEY)) {
            if (!parse_var_decl(&p, out_program, LUMI_VAR_KEY)) {
                break;
            }
        } else if (match(&p, TOKEN_KEYWORD_INIT)) {
            if (!parse_section(&p, &out_program->init_section, "init")) {
                break;
            }
        } else if (match(&p, TOKEN_KEYWORD_UPDATE)) {
            if (!parse_section(&p, &out_program->update_section, "update")) {
                break;
            }
        } else if (match(&p, TOKEN_KEYWORD_RENDER)) {
            if (!parse_section(&p, &out_program->render_section, "render")) {
                break;
            }
        } else {
            set_parse_error(&p, p.current.line, p.current.column, "unexpected top-level token");
            break;
        }
        skip_newlines(&p);
    }

    if (p.had_error) {
        lumi_program_free(out_program);
        return 0;
    }
    return 1;
}

void lumi_program_free(lumi_program *program) {
    size_t i;
    if (program == NULL) {
        return;
    }
    for (i = 0; i < program->vars.count; ++i) {
        free(program->vars.items[i].name);
        free_expr(program->vars.items[i].initializer);
    }
    free(program->vars.items);
    for (i = 0; i < program->init_section.statements.count; ++i) {
        free_stmt(program->init_section.statements.items[i]);
    }
    for (i = 0; i < program->update_section.statements.count; ++i) {
        free_stmt(program->update_section.statements.items[i]);
    }
    for (i = 0; i < program->render_section.statements.count; ++i) {
        free_stmt(program->render_section.statements.items[i]);
    }
    free(program->init_section.statements.items);
    free(program->update_section.statements.items);
    free(program->render_section.statements.items);
    memset(program, 0, sizeof(*program));
}

static int symbol_table_push(symbol_table *table, symbol value) {
    symbol *items;
    if (table->count == table->capacity) {
        size_t new_capacity = table->capacity == 0 ? 8 : table->capacity * 2;
        items = xrealloc(table->items, new_capacity * sizeof(*items));
        if (items == NULL) {
            return 0;
        }
        table->items = items;
        table->capacity = new_capacity;
    }
    table->items[table->count++] = value;
    return 1;
}

static symbol *find_symbol(symbol_table *table, const char *name) {
    size_t i = table->count;
    while (i > 0) {
        i--;
        if (strcmp(table->items[i].name, name) == 0) {
            return &table->items[i];
        }
    }
    return NULL;
}

static void pop_scope_symbols(symbol_table *table, size_t scope_depth) {
    while (table->count > 0 && table->items[table->count - 1].scope_depth >= scope_depth) {
        free(table->items[table->count - 1].name);
        table->count--;
    }
}

static int float_buffer_push(float_buffer *buf, float value) {
    float *items;
    if (buf->count == buf->capacity) {
        size_t new_capacity = buf->capacity == 0 ? 8 : buf->capacity * 2;
        items = xrealloc(buf->data, new_capacity * sizeof(*items));
        if (items == NULL) {
            return 0;
        }
        buf->data = items;
        buf->capacity = new_capacity;
    }
    buf->data[buf->count++] = value;
    return 1;
}

static int byte_buffer_push(byte_buffer *buf, uint8_t value) {
    uint8_t *items;
    if (buf->count == buf->capacity) {
        size_t new_capacity = buf->capacity == 0 ? 64 : buf->capacity * 2;
        items = xrealloc(buf->data, new_capacity);
        if (items == NULL) {
            return 0;
        }
        buf->data = items;
        buf->capacity = new_capacity;
    }
    buf->data[buf->count++] = value;
    return 1;
}

static int emit_u16(byte_buffer *buf, uint16_t value) {
    return byte_buffer_push(buf, (uint8_t)(value & 0xFF)) && byte_buffer_push(buf, (uint8_t)(value >> 8));
}

static uint16_t read_u16_bytes(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void write_u16_bytes(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)(value >> 8);
}

static int adjust_stack(emitter *e, int delta, size_t line, size_t column) {
    int next = (int)e->current_stack + delta;
    if (next < 0) {
        set_emit_error(e, line, column, "internal stack underflow while emitting bytecode");
        return 0;
    }
    e->current_stack = (uint16_t)next;
    if (e->current_stack > e->max_stack) {
        e->max_stack = e->current_stack;
    }
    return 1;
}

static int emit_op(emitter *e, uint8_t opcode, int delta, size_t line, size_t column) {
    if (!byte_buffer_push(e->code, opcode)) {
        return 0;
    }
    return adjust_stack(e, delta, line, column);
}

static int declare_symbol(emitter *e, const char *name, symbol_kind kind, size_t index, size_t array_size,
    int is_const, const_value value, const lumi_expr *expr, size_t line, size_t column) {
    symbol sym;
    if (find_symbol(&e->symbols, name) != NULL && kind != SYMBOL_LET) {
        set_emit_error(e, line, column, "symbol '%s' already defined", name);
        return 0;
    }
    if (kind == SYMBOL_LET && find_symbol(&e->symbols, name) != NULL) {
        set_emit_error(e, line, column, "symbol '%s' already defined in this scope", name);
        return 0;
    }
    sym.name = copy_string(name, strlen(name));
    if (sym.name == NULL) {
        return 0;
    }
    sym.kind = kind;
    sym.index = index;
    sym.array_size = array_size;
    sym.is_const = is_const;
    sym.value = value;
    sym.expr = expr;
    sym.scope_depth = e->scope_depth;
    return symbol_table_push(&e->symbols, sym);
}

static int add_input(emitter *e, const char *name, size_t slot) {
    symbol sym;
    sym.name = copy_string(name, strlen(name));
    if (sym.name == NULL) {
        return 0;
    }
    sym.kind = SYMBOL_INPUT;
    sym.index = slot;
    sym.array_size = 1;
    sym.is_const = 0;
    sym.value.kind = CONST_UNKNOWN;
    sym.expr = NULL;
    sym.scope_depth = 0;
    return symbol_table_push(&e->symbols, sym);
}

static int section_allows_input(section_kind section, size_t slot) {
    if (section == SECTION_RENDER) {
        (void)slot;
        return 1;
    }
    if (section == SECTION_UPDATE) {
        return slot == LUMI_INPUT_DT || slot == LUMI_INPUT_SPEED;
    }
    return 0;
}

static const_value eval_expr(emitter *e, const lumi_expr *expr);

static int float_to_index(float value, size_t *out_index) {
    size_t index = (size_t)value;
    if (value < 0.0f || value != (float)index) {
        return 0;
    }
    *out_index = index;
    return 1;
}

static int resolve_symbol_slot(emitter *e, const char *name, const lumi_expr *index_expr,
    symbol **out_symbol, size_t *out_slot, size_t line, size_t column) {
    symbol *sym = find_symbol(&e->symbols, name);
    const_value index_value;
    size_t index;

    if (sym == NULL) {
        set_emit_error(e, line, column, "unknown symbol '%s'", name);
        return 0;
    }
    if (index_expr == NULL) {
        if (sym->array_size != 1) {
            set_emit_error(e, line, column, "array '%s' requires an index", name);
            return 0;
        }
        *out_symbol = sym;
        *out_slot = sym->index;
        return 1;
    }
    if (sym->array_size == 1) {
        set_emit_error(e, line, column, "symbol '%s' is not an array", name);
        return 0;
    }
    index_value = eval_expr(e, index_expr);
    if (index_value.kind != CONST_FLOAT || !float_to_index(index_value.number, &index)) {
        set_emit_error(e, index_expr->line, index_expr->column,
            "array index for '%s' must be compile-time integer", name);
        return 0;
    }
    if (index >= sym->array_size) {
        set_emit_error(e, index_expr->line, index_expr->column,
            "array index %zu out of bounds for '%s[%zu]'", index, name, sym->array_size);
        return 0;
    }
    *out_symbol = sym;
    *out_slot = sym->index + index;
    return 1;
}

static const_value eval_binary(lumi_token_type op, const_value left, const_value right) {
    const_value out = {CONST_UNKNOWN, 0.0f};
    if (left.kind != CONST_FLOAT || right.kind != CONST_FLOAT) {
        return out;
    }
    out.kind = CONST_FLOAT;
    switch (op) {
        case TOKEN_PLUS: out.number = left.number + right.number; break;
        case TOKEN_MINUS: out.number = left.number - right.number; break;
        case TOKEN_STAR: out.number = left.number * right.number; break;
        case TOKEN_SLASH: out.number = right.number == 0.0f ? 0.0f : left.number / right.number; break;
        case TOKEN_PERCENT: out.number = right.number == 0.0f ? 0.0f : fmodf(left.number, right.number); break;
        case TOKEN_EQ_EQ: out.number = left.number == right.number; break;
        case TOKEN_BANG_EQ: out.number = left.number != right.number; break;
        case TOKEN_LT: out.number = left.number < right.number; break;
        case TOKEN_LT_EQ: out.number = left.number <= right.number; break;
        case TOKEN_GT: out.number = left.number > right.number; break;
        case TOKEN_GT_EQ: out.number = left.number >= right.number; break;
        case TOKEN_AND_AND: out.number = (left.number != 0.0f) && (right.number != 0.0f); break;
        case TOKEN_OR_OR: out.number = (left.number != 0.0f) || (right.number != 0.0f); break;
        default: out.kind = CONST_UNKNOWN; break;
    }
    return out;
}

static const_value eval_call(emitter *e, const lumi_expr *expr) {
    const_value args[4];
    size_t i;
    const_value out = {CONST_UNKNOWN, 0.0f};
    (void)e;

    if (expr->as.call.arg_count > 4) {
        return out;
    }
    for (i = 0; i < expr->as.call.arg_count; ++i) {
        args[i] = eval_expr(e, expr->as.call.args[i]);
        if (args[i].kind != CONST_FLOAT) {
            return out;
        }
    }
    if (strcmp(expr->as.call.name, "abs") == 0 && expr->as.call.arg_count == 1) {
        out.kind = CONST_FLOAT;
        out.number = fabsf(args[0].number);
    } else if (strcmp(expr->as.call.name, "sin") == 0 && expr->as.call.arg_count == 1) {
        out.kind = CONST_FLOAT;
        out.number = sinf(args[0].number);
    } else if (strcmp(expr->as.call.name, "cos") == 0 && expr->as.call.arg_count == 1) {
        out.kind = CONST_FLOAT;
        out.number = cosf(args[0].number);
    } else if (strcmp(expr->as.call.name, "sqrt") == 0 && expr->as.call.arg_count == 1) {
        if (args[0].number < 0.0f) {
            return out;
        }
        out.kind = CONST_FLOAT;
        out.number = sqrtf(args[0].number);
    } else if (strcmp(expr->as.call.name, "ceil") == 0 && expr->as.call.arg_count == 1) {
        out.kind = CONST_FLOAT;
        out.number = ceilf(args[0].number);
    } else if (strcmp(expr->as.call.name, "floor") == 0 && expr->as.call.arg_count == 1) {
        out.kind = CONST_FLOAT;
        out.number = floorf(args[0].number);
    } else if (strcmp(expr->as.call.name, "round") == 0 && expr->as.call.arg_count == 1) {
        out.kind = CONST_FLOAT;
        out.number = roundf(args[0].number);
    } else if (strcmp(expr->as.call.name, "clamp") == 0 && expr->as.call.arg_count == 3) {
        float v = args[0].number;
        if (v < args[1].number) {
            v = args[1].number;
        }
        if (v > args[2].number) {
            v = args[2].number;
        }
        out.kind = CONST_FLOAT;
        out.number = v;
    } else if (strcmp(expr->as.call.name, "dist") == 0 && expr->as.call.arg_count == 4) {
        float dx = args[0].number - args[2].number;
        float dy = args[1].number - args[3].number;
        out.kind = CONST_FLOAT;
        out.number = sqrtf(dx * dx + dy * dy);
    } else if (strcmp(expr->as.call.name, "lerp") == 0 && expr->as.call.arg_count == 3) {
        out.kind = CONST_FLOAT;
        out.number = args[0].number + (args[1].number - args[0].number) * args[2].number;
    } else if (strcmp(expr->as.call.name, "min") == 0 && expr->as.call.arg_count == 2) {
        out.kind = CONST_FLOAT;
        out.number = args[0].number < args[1].number ? args[0].number : args[1].number;
    } else if (strcmp(expr->as.call.name, "max") == 0 && expr->as.call.arg_count == 2) {
        out.kind = CONST_FLOAT;
        out.number = args[0].number > args[1].number ? args[0].number : args[1].number;
    } else if (strcmp(expr->as.call.name, "pow") == 0 && expr->as.call.arg_count == 2) {
        out.kind = CONST_FLOAT;
        out.number = powf(args[0].number, args[1].number);
    }
    return out;
}

static const_value eval_expr(emitter *e, const lumi_expr *expr) {
    const_value out = {CONST_UNKNOWN, 0.0f};
    symbol *sym;

    switch (expr->kind) {
        case EXPR_NUMBER:
            out.kind = CONST_FLOAT;
            out.number = (float)expr->as.number;
            return out;
        case EXPR_SYMBOL:
            sym = find_symbol(&e->symbols, expr->as.symbol.name);
            if (sym != NULL && sym->array_size == 1 && sym->kind == SYMBOL_LET && sym->is_const) {
                return sym->value;
            }
            return out;
        case EXPR_INDEX: {
            size_t slot;
            if (!resolve_symbol_slot(e, expr->as.index.name, expr->as.index.index, &sym, &slot, expr->line, expr->column)) {
                return out;
            }
            if (sym->kind == SYMBOL_LET && sym->is_const) {
                return sym->value;
            }
            return out;
        }
        case EXPR_UNARY: {
            const_value inner = eval_expr(e, expr->as.unary.operand);
            if (inner.kind != CONST_FLOAT) {
                return out;
            }
            out.kind = CONST_FLOAT;
            out.number = expr->as.unary.op == TOKEN_MINUS ? -inner.number : (inner.number == 0.0f);
            return out;
        }
        case EXPR_BINARY:
            return eval_binary(expr->as.binary.op, eval_expr(e, expr->as.binary.left), eval_expr(e, expr->as.binary.right));
        case EXPR_CALL:
            return eval_call(e, expr);
        case EXPR_IF: {
            const_value cond = eval_expr(e, expr->as.if_expr.condition);
            if (cond.kind != CONST_FLOAT) {
                return out;
            }
            return cond.number != 0.0f ? eval_expr(e, expr->as.if_expr.then_value) : eval_expr(e, expr->as.if_expr.else_value);
        }
    }
    return out;
}

static int add_constant(emitter *e, float value, uint16_t *out_index) {
    size_t i;
    for (i = 0; i < e->constants.count; ++i) {
        if (e->constants.data[i] == value) {
            *out_index = (uint16_t)i;
            return 1;
        }
    }
    if (!float_buffer_push(&e->constants, value)) {
        return 0;
    }
    *out_index = (uint16_t)(e->constants.count - 1);
    return 1;
}

static int emit_expr(emitter *e, const lumi_expr *expr);
static int expr_is_discardable(emitter *e, const lumi_expr *expr);

static int emit_const_or_runtime(emitter *e, const lumi_expr *expr) {
    const_value value = eval_expr(e, expr);
    uint16_t index;
    if (value.kind == CONST_FLOAT) {
        if (!add_constant(e, value.number, &index)) {
            return 0;
        }
        return emit_op(e, LUMI_OP_PUSH_CONST_F32, 1, expr->line, expr->column) && emit_u16(e->code, index);
    }
    return emit_expr(e, expr);
}

static int expr_equal(const lumi_expr *a, const lumi_expr *b) {
    size_t i;
    if (a == b) {
        return 1;
    }
    if (a == NULL || b == NULL || a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
        case EXPR_NUMBER:
            return a->as.number == b->as.number;
        case EXPR_SYMBOL:
            return strcmp(a->as.symbol.name, b->as.symbol.name) == 0;
        case EXPR_INDEX:
            return strcmp(a->as.index.name, b->as.index.name) == 0
                && expr_equal(a->as.index.index, b->as.index.index);
        case EXPR_UNARY:
            return a->as.unary.op == b->as.unary.op
                && expr_equal(a->as.unary.operand, b->as.unary.operand);
        case EXPR_BINARY:
            return a->as.binary.op == b->as.binary.op
                && expr_equal(a->as.binary.left, b->as.binary.left)
                && expr_equal(a->as.binary.right, b->as.binary.right);
        case EXPR_CALL:
            if (strcmp(a->as.call.name, b->as.call.name) != 0 || a->as.call.arg_count != b->as.call.arg_count) {
                return 0;
            }
            for (i = 0; i < a->as.call.arg_count; ++i) {
                if (!expr_equal(a->as.call.args[i], b->as.call.args[i])) {
                    return 0;
                }
            }
            return 1;
        case EXPR_IF:
            return expr_equal(a->as.if_expr.condition, b->as.if_expr.condition)
                && expr_equal(a->as.if_expr.then_value, b->as.if_expr.then_value)
                && expr_equal(a->as.if_expr.else_value, b->as.if_expr.else_value);
    }
    return 0;
}

static int call_args_are_same_expr(const lumi_expr *expr) {
    size_t i;
    if (expr->as.call.arg_count < 2) {
        return 0;
    }
    for (i = 1; i < expr->as.call.arg_count; ++i) {
        if (!expr_equal(expr->as.call.args[0], expr->as.call.args[i])) {
            return 0;
        }
    }
    return 1;
}

static int emit_builtin(emitter *e, const lumi_expr *expr) {
    lumi_builtin_id builtin = 0;
    size_t expected = 0;
    size_t i;

    if (strcmp(expr->as.call.name, "abs") == 0) {
        builtin = LUMI_BUILTIN_ABS;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "sin") == 0) {
        builtin = LUMI_BUILTIN_SIN;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "cos") == 0) {
        builtin = LUMI_BUILTIN_COS;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "sqrt") == 0) {
        builtin = LUMI_BUILTIN_SQRT;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "ceil") == 0) {
        builtin = LUMI_BUILTIN_CEIL;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "floor") == 0) {
        builtin = LUMI_BUILTIN_FLOOR;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "round") == 0) {
        builtin = LUMI_BUILTIN_ROUND;
        expected = 1;
    } else if (strcmp(expr->as.call.name, "clamp") == 0) {
        builtin = LUMI_BUILTIN_CLAMP;
        expected = 3;
    } else if (strcmp(expr->as.call.name, "dist") == 0) {
        builtin = LUMI_BUILTIN_DIST;
        expected = 4;
    } else if (strcmp(expr->as.call.name, "lerp") == 0) {
        builtin = LUMI_BUILTIN_LERP;
        expected = 3;
    } else if (strcmp(expr->as.call.name, "min") == 0) {
        builtin = LUMI_BUILTIN_MIN;
        expected = 2;
    } else if (strcmp(expr->as.call.name, "max") == 0) {
        builtin = LUMI_BUILTIN_MAX;
        expected = 2;
    } else if (strcmp(expr->as.call.name, "pow") == 0) {
        builtin = LUMI_BUILTIN_POW;
        expected = 2;
    } else if (strcmp(expr->as.call.name, "rand") == 0) {
        builtin = LUMI_BUILTIN_RAND;
        expected = 0;
    } else if (strcmp(expr->as.call.name, "rgb") == 0) {
        builtin = LUMI_BUILTIN_RGB;
        expected = 3;
    } else if (strcmp(expr->as.call.name, "hsv") == 0) {
        builtin = LUMI_BUILTIN_HSV;
        expected = 3;
    } else {
        set_emit_error(e, expr->line, expr->column, "unknown function '%s'", expr->as.call.name);
        return 0;
    }
    if (expr->as.call.arg_count != expected) {
        set_emit_error(e, expr->line, expr->column, "function '%s' expects %zu arguments", expr->as.call.name, expected);
        return 0;
    }
    if (e->optimization_level >= 3 && call_args_are_same_expr(expr) && expr_is_discardable(e, expr->as.call.args[0])) {
        if (!emit_const_or_runtime(e, expr->as.call.args[0])) {
            return 0;
        }
        for (i = 1; i < expr->as.call.arg_count; ++i) {
            if (!emit_op(e, LUMI_OP_DUP, 1, expr->as.call.args[i]->line, expr->as.call.args[i]->column)) {
                return 0;
            }
        }
    } else {
        if (e->error->message != NULL) {
            return 0;
        }
        for (i = 0; i < expr->as.call.arg_count; ++i) {
            if (!emit_const_or_runtime(e, expr->as.call.args[i])) {
                return 0;
            }
        }
    }
    if (e->optimization_level >= 3) {
        if (builtin == LUMI_BUILTIN_CLAMP) {
            return emit_op(e, LUMI_OP_CLAMP, -2, expr->line, expr->column);
        }
        if (builtin == LUMI_BUILTIN_DIST) {
            return emit_op(e, LUMI_OP_DIST, -3, expr->line, expr->column);
        }
        if (builtin == LUMI_BUILTIN_RGB) {
            return emit_op(e, LUMI_OP_RGB, -2, expr->line, expr->column);
        }
        if (builtin == LUMI_BUILTIN_HSV) {
            return emit_op(e, LUMI_OP_HSV, -2, expr->line, expr->column);
        }
    }
    return emit_op(e, LUMI_OP_CALL_BUILTIN, (int)(1 - (int)expr->as.call.arg_count), expr->line, expr->column)
        && byte_buffer_push(e->code, (uint8_t)builtin)
        && byte_buffer_push(e->code, (uint8_t)expr->as.call.arg_count);
}

static int patch_jump(byte_buffer *code, size_t at, uint16_t target) {
    if (at + 1 >= code->count) {
        return 0;
    }
    code->data[at] = (uint8_t)(target & 0xFF);
    code->data[at + 1] = (uint8_t)(target >> 8);
    return 1;
}

static int expr_const_float(emitter *e, const lumi_expr *expr, float *out_value) {
    const_value value = eval_expr(e, expr);
    if (value.kind != CONST_FLOAT) {
        return 0;
    }
    *out_value = value.number;
    return 1;
}

static int call_is_discardable_builtin(const lumi_expr *expr) {
    const char *name = expr->as.call.name;
    size_t argc = expr->as.call.arg_count;
    if (strcmp(name, "rand") == 0) {
        return 0;
    }
    return (strcmp(name, "abs") == 0 && argc == 1)
        || (strcmp(name, "sin") == 0 && argc == 1)
        || (strcmp(name, "cos") == 0 && argc == 1)
        || (strcmp(name, "sqrt") == 0 && argc == 1)
        || (strcmp(name, "ceil") == 0 && argc == 1)
        || (strcmp(name, "floor") == 0 && argc == 1)
        || (strcmp(name, "round") == 0 && argc == 1)
        || (strcmp(name, "clamp") == 0 && argc == 3)
        || (strcmp(name, "dist") == 0 && argc == 4)
        || (strcmp(name, "lerp") == 0 && argc == 3)
        || (strcmp(name, "min") == 0 && argc == 2)
        || (strcmp(name, "max") == 0 && argc == 2)
        || (strcmp(name, "pow") == 0 && argc == 2)
        || (strcmp(name, "rgb") == 0 && argc == 3)
        || (strcmp(name, "hsv") == 0 && argc == 3);
}

static int expr_is_discardable(emitter *e, const lumi_expr *expr) {
    symbol *sym;
    size_t slot;
    size_t i;

    switch (expr->kind) {
        case EXPR_NUMBER:
            return 1;
        case EXPR_SYMBOL:
            sym = find_symbol(&e->symbols, expr->as.symbol.name);
            if (sym == NULL) {
                return 0;
            }
            return sym->kind != SYMBOL_LET || sym->expr == NULL || expr_is_discardable(e, sym->expr);
        case EXPR_INDEX:
            if (!expr_is_discardable(e, expr->as.index.index)) {
                return 0;
            }
            if (!resolve_symbol_slot(e, expr->as.index.name, expr->as.index.index, &sym, &slot, expr->line, expr->column)) {
                return 0;
            }
            (void)slot;
            return sym->kind != SYMBOL_LET || sym->expr == NULL || expr_is_discardable(e, sym->expr);
        case EXPR_UNARY:
            return expr_is_discardable(e, expr->as.unary.operand);
        case EXPR_BINARY:
            return expr_is_discardable(e, expr->as.binary.left) && expr_is_discardable(e, expr->as.binary.right);
        case EXPR_CALL:
            if (!call_is_discardable_builtin(expr)) {
                return 0;
            }
            if (strcmp(expr->as.call.name, "sqrt") == 0 && expr->as.call.arg_count == 1) {
                float value;
                if (!expr_const_float(e, expr->as.call.args[0], &value) || value < 0.0f) {
                    return 0;
                }
            }
            for (i = 0; i < expr->as.call.arg_count; ++i) {
                if (!expr_is_discardable(e, expr->as.call.args[i])) {
                    return 0;
                }
            }
            return 1;
        case EXPR_IF:
            return expr_is_discardable(e, expr->as.if_expr.condition)
                && expr_is_discardable(e, expr->as.if_expr.then_value)
                && expr_is_discardable(e, expr->as.if_expr.else_value);
    }
    return 0;
}

static int emit_const_float(emitter *e, float value, size_t line, size_t column) {
    uint16_t index;
    if (!add_constant(e, value, &index)) {
        return 0;
    }
    return emit_op(e, LUMI_OP_PUSH_CONST_F32, 1, line, column) && emit_u16(e->code, index);
}

static void clear_expr_payload(lumi_expr *expr) {
    size_t i;
    switch (expr->kind) {
        case EXPR_SYMBOL:
            free(expr->as.symbol.name);
            break;
        case EXPR_INDEX:
            free(expr->as.index.name);
            free_expr(expr->as.index.index);
            break;
        case EXPR_UNARY:
            free_expr(expr->as.unary.operand);
            break;
        case EXPR_BINARY:
            free_expr(expr->as.binary.left);
            free_expr(expr->as.binary.right);
            break;
        case EXPR_CALL:
            free(expr->as.call.name);
            for (i = 0; i < expr->as.call.arg_count; ++i) {
                free_expr(expr->as.call.args[i]);
            }
            free(expr->as.call.args);
            break;
        case EXPR_IF:
            free_expr(expr->as.if_expr.condition);
            free_expr(expr->as.if_expr.then_value);
            free_expr(expr->as.if_expr.else_value);
            break;
        case EXPR_NUMBER:
            break;
    }
}

static void rewrite_expr_as_number(lumi_expr *expr, float value) {
    clear_expr_payload(expr);
    expr->kind = EXPR_NUMBER;
    expr->as.number = value;
}

static lumi_expr *take_expr_child(lumi_expr *expr, lumi_expr *child) {
    free(expr);
    return child;
}

static lumi_expr *simplify_expr(emitter *e, lumi_expr *expr) {
    const_value value;
    float left;
    float right;
    size_t i;

    if (expr == NULL || e->error->message != NULL || e->optimization_level < 1) {
        return expr;
    }

    switch (expr->kind) {
        case EXPR_UNARY:
            expr->as.unary.operand = simplify_expr(e, expr->as.unary.operand);
            break;
        case EXPR_BINARY:
            expr->as.binary.left = simplify_expr(e, expr->as.binary.left);
            expr->as.binary.right = simplify_expr(e, expr->as.binary.right);
            break;
        case EXPR_CALL:
            for (i = 0; i < expr->as.call.arg_count; ++i) {
                expr->as.call.args[i] = simplify_expr(e, expr->as.call.args[i]);
            }
            break;
        case EXPR_IF:
            expr->as.if_expr.condition = simplify_expr(e, expr->as.if_expr.condition);
            if (expr_const_float(e, expr->as.if_expr.condition, &left)) {
                lumi_expr *chosen = left != 0.0f ? expr->as.if_expr.then_value : expr->as.if_expr.else_value;
                lumi_expr *discarded = left != 0.0f ? expr->as.if_expr.else_value : expr->as.if_expr.then_value;
                free_expr(expr->as.if_expr.condition);
                free_expr(discarded);
                return simplify_expr(e, take_expr_child(expr, chosen));
            }
            if (e->error->message != NULL) {
                return expr;
            }
            expr->as.if_expr.then_value = simplify_expr(e, expr->as.if_expr.then_value);
            expr->as.if_expr.else_value = simplify_expr(e, expr->as.if_expr.else_value);
            break;
        case EXPR_INDEX:
            expr->as.index.index = simplify_expr(e, expr->as.index.index);
            break;
        case EXPR_NUMBER:
        case EXPR_SYMBOL:
            break;
    }

    if (e->error->message != NULL) {
        return expr;
    }

    value = eval_expr(e, expr);
    if (value.kind == CONST_FLOAT && expr_is_discardable(e, expr)) {
        rewrite_expr_as_number(expr, value.number);
        return expr;
    }
    if (e->error->message != NULL || expr->kind != EXPR_BINARY || e->optimization_level < 2) {
        return expr;
    }

    switch (expr->as.binary.op) {
        case TOKEN_PLUS:
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f) {
                lumi_expr *left_expr = expr->as.binary.left;
                free_expr(expr->as.binary.right);
                return take_expr_child(expr, left_expr);
            }
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f) {
                lumi_expr *right_expr = expr->as.binary.right;
                free_expr(expr->as.binary.left);
                return take_expr_child(expr, right_expr);
            }
            break;
        case TOKEN_MINUS:
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f) {
                lumi_expr *left_expr = expr->as.binary.left;
                free_expr(expr->as.binary.right);
                return take_expr_child(expr, left_expr);
            }
            break;
        case TOKEN_STAR:
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f
                && expr_is_discardable(e, expr->as.binary.left)) {
                rewrite_expr_as_number(expr, 0.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                rewrite_expr_as_number(expr, 0.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 1.0f) {
                lumi_expr *left_expr = expr->as.binary.left;
                free_expr(expr->as.binary.right);
                return take_expr_child(expr, left_expr);
            }
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 1.0f) {
                lumi_expr *right_expr = expr->as.binary.right;
                free_expr(expr->as.binary.left);
                return take_expr_child(expr, right_expr);
            }
            break;
        case TOKEN_SLASH:
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                rewrite_expr_as_number(expr, 0.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 1.0f) {
                lumi_expr *left_expr = expr->as.binary.left;
                free_expr(expr->as.binary.right);
                return take_expr_child(expr, left_expr);
            }
            break;
        case TOKEN_PERCENT:
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                rewrite_expr_as_number(expr, 0.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            break;
        case TOKEN_AND_AND:
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                rewrite_expr_as_number(expr, 0.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f
                && expr_is_discardable(e, expr->as.binary.left)) {
                rewrite_expr_as_number(expr, 0.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            break;
        case TOKEN_OR_OR:
            if (expr_const_float(e, expr->as.binary.left, &left) && left != 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                rewrite_expr_as_number(expr, 1.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right != 0.0f
                && expr_is_discardable(e, expr->as.binary.left)) {
                rewrite_expr_as_number(expr, 1.0f);
                return expr;
            }
            if (e->error->message != NULL) {
                return expr;
            }
            break;
        default:
            break;
    }
    return expr;
}

static int emit_if_expr(emitter *e, const lumi_expr *expr) {
    size_t jump_false;
    size_t jump_end;
    uint16_t stack_before = e->current_stack;
    float condition;

    if (e->optimization_level >= 1 && expr_const_float(e, expr->as.if_expr.condition, &condition)) {
        return emit_const_or_runtime(e, condition != 0.0f ? expr->as.if_expr.then_value : expr->as.if_expr.else_value);
    }

    if (!emit_const_or_runtime(e, expr->as.if_expr.condition)) {
        return 0;
    }
    if (!emit_op(e, LUMI_OP_JUMP_IF_FALSE, -1, expr->line, expr->column)) {
        return 0;
    }
    jump_false = e->code->count;
    if (!emit_u16(e->code, 0)) {
        return 0;
    }
    if (!emit_const_or_runtime(e, expr->as.if_expr.then_value)) {
        return 0;
    }
    if (e->current_stack != (uint16_t)(stack_before + 1)) {
        set_emit_error(e, expr->line, expr->column, "if expression branch must leave one value");
        return 0;
    }
    if (!emit_op(e, LUMI_OP_JUMP, 0, expr->line, expr->column)) {
        return 0;
    }
    jump_end = e->code->count;
    if (!emit_u16(e->code, 0)) {
        return 0;
    }
    if (!patch_jump(e->code, jump_false, (uint16_t)e->code->count)) {
        return 0;
    }
    e->current_stack = stack_before;
    if (!emit_const_or_runtime(e, expr->as.if_expr.else_value)) {
        return 0;
    }
    if (e->current_stack != (uint16_t)(stack_before + 1)) {
        set_emit_error(e, expr->line, expr->column, "if expression branch must leave one value");
        return 0;
    }
    return patch_jump(e->code, jump_end, (uint16_t)e->code->count);
}

static int emit_optimized_binary(emitter *e, const lumi_expr *expr) {
    float left;
    float right;

    if (e->optimization_level < 2) {
        return 0;
    }

    switch (expr->as.binary.op) {
        case TOKEN_PLUS:
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f) {
                return emit_const_or_runtime(e, expr->as.binary.left);
            }
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f) {
                return emit_const_or_runtime(e, expr->as.binary.right);
            }
            break;
        case TOKEN_MINUS:
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f) {
                return emit_const_or_runtime(e, expr->as.binary.left);
            }
            break;
        case TOKEN_STAR:
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f
                && expr_is_discardable(e, expr->as.binary.left)) {
                return emit_const_float(e, 0.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                return emit_const_float(e, 0.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 1.0f) {
                return emit_const_or_runtime(e, expr->as.binary.left);
            }
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 1.0f) {
                return emit_const_or_runtime(e, expr->as.binary.right);
            }
            break;
        case TOKEN_SLASH:
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                return emit_const_float(e, 0.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 1.0f) {
                return emit_const_or_runtime(e, expr->as.binary.left);
            }
            break;
        case TOKEN_PERCENT:
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                return emit_const_float(e, 0.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            break;
        case TOKEN_AND_AND:
            if (expr_const_float(e, expr->as.binary.left, &left) && left == 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                return emit_const_float(e, 0.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right == 0.0f
                && expr_is_discardable(e, expr->as.binary.left)) {
                return emit_const_float(e, 0.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            break;
        case TOKEN_OR_OR:
            if (expr_const_float(e, expr->as.binary.left, &left) && left != 0.0f
                && expr_is_discardable(e, expr->as.binary.right)) {
                return emit_const_float(e, 1.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (expr_const_float(e, expr->as.binary.right, &right) && right != 0.0f
                && expr_is_discardable(e, expr->as.binary.left)) {
                return emit_const_float(e, 1.0f, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            break;
        default:
            break;
    }
    return 0;
}

static int emit_binary_opcode(emitter *e, lumi_token_type op, size_t line, size_t column) {
    switch (op) {
        case TOKEN_PLUS: return emit_op(e, LUMI_OP_ADD, -1, line, column);
        case TOKEN_MINUS: return emit_op(e, LUMI_OP_SUB, -1, line, column);
        case TOKEN_STAR: return emit_op(e, LUMI_OP_MUL, -1, line, column);
        case TOKEN_SLASH: return emit_op(e, LUMI_OP_DIV, -1, line, column);
        case TOKEN_PERCENT: return emit_op(e, LUMI_OP_MOD, -1, line, column);
        case TOKEN_EQ_EQ: return emit_op(e, LUMI_OP_EQ, -1, line, column);
        case TOKEN_BANG_EQ: return emit_op(e, LUMI_OP_NE, -1, line, column);
        case TOKEN_LT: return emit_op(e, LUMI_OP_LT, -1, line, column);
        case TOKEN_LT_EQ: return emit_op(e, LUMI_OP_LE, -1, line, column);
        case TOKEN_GT: return emit_op(e, LUMI_OP_GT, -1, line, column);
        case TOKEN_GT_EQ: return emit_op(e, LUMI_OP_GE, -1, line, column);
        case TOKEN_AND_AND: return emit_op(e, LUMI_OP_AND, -1, line, column);
        case TOKEN_OR_OR: return emit_op(e, LUMI_OP_OR, -1, line, column);
        default:
            set_emit_error(e, line, column, "unsupported operator");
            return 0;
    }
}

static int emit_expr(emitter *e, const lumi_expr *expr) {
    symbol *sym;
    uint16_t index;
    size_t slot;

    switch (expr->kind) {
        case EXPR_NUMBER:
            if (!add_constant(e, (float)expr->as.number, &index)) {
                return 0;
            }
            return emit_op(e, LUMI_OP_PUSH_CONST_F32, 1, expr->line, expr->column) && emit_u16(e->code, index);
        case EXPR_SYMBOL:
            if (!resolve_symbol_slot(e, expr->as.symbol.name, NULL, &sym, &slot, expr->line, expr->column)) {
                return 0;
            }
            if (sym->kind == SYMBOL_INPUT) {
                if (!section_allows_input(e->current_section, sym->index)) {
                    set_emit_error(e, expr->line, expr->column, "input '%s' is not available in this section", expr->as.symbol.name);
                    return 0;
                }
                return emit_op(e, LUMI_OP_LOAD_INPUT, 1, expr->line, expr->column)
                    && byte_buffer_push(e->code, (uint8_t)sym->index);
            }
            if (sym->kind == SYMBOL_KEY_VAR && e->current_section != SECTION_RENDER) {
                set_emit_error(e, expr->line, expr->column, "key variable '%s' is only available in render", expr->as.symbol.name);
                return 0;
            }
            if (sym->kind == SYMBOL_LET && sym->is_const) {
                if (!add_constant(e, sym->value.number, &index)) {
                    return 0;
                }
                return emit_op(e, LUMI_OP_PUSH_CONST_F32, 1, expr->line, expr->column) && emit_u16(e->code, index);
            }
            if (sym->kind == SYMBOL_LET && sym->expr != NULL) {
                return emit_const_or_runtime(e, sym->expr);
            }
            return emit_op(e, sym->kind == SYMBOL_GLOBAL_VAR ? LUMI_OP_LOAD_GLOBAL : LUMI_OP_LOAD_KEY, 1, expr->line, expr->column)
                && emit_u16(e->code, (uint16_t)slot);
        case EXPR_INDEX:
            if (!resolve_symbol_slot(e, expr->as.index.name, expr->as.index.index, &sym, &slot, expr->line, expr->column)) {
                return 0;
            }
            if (sym->kind == SYMBOL_INPUT) {
                set_emit_error(e, expr->line, expr->column, "inputs cannot be indexed");
                return 0;
            }
            if (sym->kind == SYMBOL_KEY_VAR && e->current_section != SECTION_RENDER) {
                set_emit_error(e, expr->line, expr->column, "key variable '%s' is only available in render", expr->as.index.name);
                return 0;
            }
            if (sym->kind == SYMBOL_LET && sym->is_const) {
                if (!add_constant(e, sym->value.number, &index)) {
                    return 0;
                }
                return emit_op(e, LUMI_OP_PUSH_CONST_F32, 1, expr->line, expr->column) && emit_u16(e->code, index);
            }
            if (sym->kind == SYMBOL_LET && sym->expr != NULL) {
                return emit_const_or_runtime(e, sym->expr);
            }
            return emit_op(e, sym->kind == SYMBOL_GLOBAL_VAR ? LUMI_OP_LOAD_GLOBAL : LUMI_OP_LOAD_KEY, 1, expr->line, expr->column)
                && emit_u16(e->code, (uint16_t)slot);
        case EXPR_UNARY:
            if (!emit_const_or_runtime(e, expr->as.unary.operand)) {
                return 0;
            }
            return emit_op(e, expr->as.unary.op == TOKEN_MINUS ? LUMI_OP_NEG : LUMI_OP_NOT, 0, expr->line, expr->column);
        case EXPR_BINARY:
            if (emit_optimized_binary(e, expr)) {
                return 1;
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (e->optimization_level >= 3
                && expr_equal(expr->as.binary.left, expr->as.binary.right)
                && expr_is_discardable(e, expr->as.binary.left)) {
                if (!emit_const_or_runtime(e, expr->as.binary.left)
                    || !emit_op(e, LUMI_OP_DUP, 1, expr->as.binary.right->line, expr->as.binary.right->column)) {
                    return 0;
                }
                return emit_binary_opcode(e, expr->as.binary.op, expr->line, expr->column);
            }
            if (e->error->message != NULL) {
                return 0;
            }
            if (!emit_const_or_runtime(e, expr->as.binary.left) || !emit_const_or_runtime(e, expr->as.binary.right)) {
                return 0;
            }
            return emit_binary_opcode(e, expr->as.binary.op, expr->line, expr->column);
        case EXPR_CALL:
            return emit_builtin(e, expr);
        case EXPR_IF:
            return emit_if_expr(e, expr);
    }
    return 0;
}

static int emit_stmt_list(emitter *e, lumi_stmt_list *list);
static int stmt_list_guarantees_color(const lumi_stmt_list *list);

static int stmt_guarantees_color(const lumi_stmt *stmt) {
    switch (stmt->kind) {
        case STMT_COLOR:
            return 1;
        case STMT_IF:
            if (stmt->as.if_stmt.else_branch.count == 0) {
                return 0;
            }
            return stmt_list_guarantees_color(&stmt->as.if_stmt.then_branch)
                && stmt_list_guarantees_color(&stmt->as.if_stmt.else_branch);
        case STMT_LET:
        case STMT_ASSIGN:
        case STMT_FOR:
            return 0;
    }
    return 0;
}

static int stmt_list_guarantees_color(const lumi_stmt_list *list) {
    size_t i;
    for (i = 0; i < list->count; ++i) {
        if (stmt_guarantees_color(list->items[i])) {
            return 1;
        }
    }
    return 0;
}

static int emit_if_stmt(emitter *e, lumi_stmt *stmt) {
    size_t jump_false;
    size_t jump_end;
    uint16_t stack_before = e->current_stack;
    float condition;

    stmt->as.if_stmt.condition = simplify_expr(e, stmt->as.if_stmt.condition);
    if (e->error->message != NULL) {
        return 0;
    }
    if (stmt->as.if_stmt.then_branch.count == 0 && stmt->as.if_stmt.else_branch.count == 0
        && expr_is_discardable(e, stmt->as.if_stmt.condition)) {
        return 1;
    }
    if (e->error->message != NULL) {
        return 0;
    }
    if (e->optimization_level >= 1 && expr_const_float(e, stmt->as.if_stmt.condition, &condition)) {
        lumi_stmt_list *branch = condition != 0.0f ? &stmt->as.if_stmt.then_branch : &stmt->as.if_stmt.else_branch;
        e->scope_depth++;
        if (!emit_stmt_list(e, branch)) {
            return 0;
        }
        pop_scope_symbols(&e->symbols, e->scope_depth);
        e->scope_depth--;
        if (e->current_stack != stack_before) {
            set_emit_error(e, stmt->line, stmt->column, "if statement branch must preserve stack depth");
            return 0;
        }
        return 1;
    }
    if (e->error->message != NULL) {
        return 0;
    }

    if (!emit_const_or_runtime(e, stmt->as.if_stmt.condition)) {
        return 0;
    }
    if (!emit_op(e, LUMI_OP_JUMP_IF_FALSE, -1, stmt->line, stmt->column)) {
        return 0;
    }
    jump_false = e->code->count;
    if (!emit_u16(e->code, 0)) {
        return 0;
    }

    e->scope_depth++;
    if (!emit_stmt_list(e, &stmt->as.if_stmt.then_branch)) {
        return 0;
    }
    pop_scope_symbols(&e->symbols, e->scope_depth);
    e->scope_depth--;
    if (e->current_stack != stack_before) {
        set_emit_error(e, stmt->line, stmt->column, "if statement branch must preserve stack depth");
        return 0;
    }
    if (stmt->as.if_stmt.else_branch.count > 0) {
        if (!emit_op(e, LUMI_OP_JUMP, 0, stmt->line, stmt->column)) {
            return 0;
        }
        jump_end = e->code->count;
        if (!emit_u16(e->code, 0)) {
            return 0;
        }
        if (!patch_jump(e->code, jump_false, (uint16_t)e->code->count)) {
            return 0;
        }
        e->current_stack = stack_before;
        e->scope_depth++;
        if (!emit_stmt_list(e, &stmt->as.if_stmt.else_branch)) {
            return 0;
        }
        pop_scope_symbols(&e->symbols, e->scope_depth);
        e->scope_depth--;
        if (e->current_stack != stack_before) {
            set_emit_error(e, stmt->line, stmt->column, "if statement branch must preserve stack depth");
            return 0;
        }
        return patch_jump(e->code, jump_end, (uint16_t)e->code->count);
    }
    return patch_jump(e->code, jump_false, (uint16_t)e->code->count);
}

static int emit_assignment(emitter *e, lumi_stmt *stmt) {
    symbol *sym;
    size_t slot;
    stmt->as.assign.index = simplify_expr(e, stmt->as.assign.index);
    stmt->as.assign.value = simplify_expr(e, stmt->as.assign.value);
    if (e->error->message != NULL) {
        return 0;
    }
    if (!resolve_symbol_slot(e, stmt->as.assign.name, stmt->as.assign.index, &sym, &slot, stmt->line, stmt->column)) {
        return 0;
    }
    if (sym->kind != SYMBOL_GLOBAL_VAR && sym->kind != SYMBOL_KEY_VAR) {
        set_emit_error(e, stmt->line, stmt->column, "only variables can be reassigned");
        return 0;
    }
    if (sym->kind == SYMBOL_KEY_VAR && e->current_section != SECTION_RENDER) {
        set_emit_error(e, stmt->line, stmt->column, "key variable '%s' can only be assigned in render", stmt->as.assign.name);
        return 0;
    }
    if (!emit_const_or_runtime(e, stmt->as.assign.value)) {
        return 0;
    }
    return emit_op(e, sym->kind == SYMBOL_GLOBAL_VAR ? LUMI_OP_STORE_GLOBAL : LUMI_OP_STORE_KEY, -1, stmt->line, stmt->column)
        && emit_u16(e->code, (uint16_t)slot);
}

static int emit_for_stmt(emitter *e, lumi_stmt *stmt) {
    const_value start_value;
    const_value end_value;
    size_t start_index;
    size_t end_index;
    size_t i;

    stmt->as.for_stmt.start = simplify_expr(e, stmt->as.for_stmt.start);
    stmt->as.for_stmt.end = simplify_expr(e, stmt->as.for_stmt.end);
    if (e->error->message != NULL) {
        return 0;
    }
    start_value = eval_expr(e, stmt->as.for_stmt.start);
    end_value = eval_expr(e, stmt->as.for_stmt.end);

    if (start_value.kind != CONST_FLOAT || end_value.kind != CONST_FLOAT
        || !float_to_index(start_value.number, &start_index)
        || !float_to_index(end_value.number, &end_index)) {
        set_emit_error(e, stmt->line, stmt->column, "for loop range must be compile-time integer bounds");
        return 0;
    }
    if (end_index < start_index) {
        set_emit_error(e, stmt->line, stmt->column, "for loop end must be greater than or equal to start");
        return 0;
    }
    if (stmt->as.for_stmt.body.count == 0) {
        return 1;
    }
    for (i = start_index; i < end_index; ++i) {
        const_value loop_value;
        loop_value.kind = CONST_FLOAT;
        loop_value.number = (float)i;
        e->scope_depth++;
        if (!declare_symbol(e, stmt->as.for_stmt.name, SYMBOL_LET, 0, 1, 1, loop_value, NULL, stmt->line, stmt->column)
            || !emit_stmt_list(e, &stmt->as.for_stmt.body)) {
            return 0;
        }
        pop_scope_symbols(&e->symbols, e->scope_depth);
        e->scope_depth--;
    }
    return 1;
}

static int emit_stmt(emitter *e, lumi_stmt *stmt) {
    const_value value;

    switch (stmt->kind) {
        case STMT_LET:
            stmt->as.binding.value = simplify_expr(e, stmt->as.binding.value);
            if (e->error->message != NULL) {
                return 0;
            }
            value = eval_expr(e, stmt->as.binding.value);
            return declare_symbol(e, stmt->as.binding.name, SYMBOL_LET, 0, 1, value.kind == CONST_FLOAT, value,
                stmt->as.binding.value, stmt->line, stmt->column);
        case STMT_ASSIGN:
            return emit_assignment(e, stmt);
        case STMT_COLOR:
            if (e->current_section != SECTION_RENDER) {
                set_emit_error(e, stmt->line, stmt->column, "color can only be assigned in render");
                return 0;
            }
            stmt->as.color.value = simplify_expr(e, stmt->as.color.value);
            if (e->error->message != NULL) {
                return 0;
            }
            if (!emit_const_or_runtime(e, stmt->as.color.value)) {
                return 0;
            }
            e->render_has_color = 1;
            return emit_op(e, LUMI_OP_SET_COLOR, -1, stmt->line, stmt->column);
        case STMT_IF:
            return emit_if_stmt(e, stmt);
        case STMT_FOR:
            return emit_for_stmt(e, stmt);
    }
    return 0;
}

static int expr_indices_match(emitter *e, const lumi_expr *a, const lumi_expr *b) {
    const_value av;
    const_value bv;
    size_t ai;
    size_t bi;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    av = eval_expr(e, a);
    bv = eval_expr(e, b);
    return av.kind == CONST_FLOAT && bv.kind == CONST_FLOAT
        && float_to_index(av.number, &ai) && float_to_index(bv.number, &bi)
        && ai == bi;
}

static int expr_reads_target(emitter *e, const lumi_expr *expr, const char *name, const lumi_expr *index) {
    size_t i;
    if (expr == NULL) {
        return 0;
    }
    switch (expr->kind) {
        case EXPR_SYMBOL:
            return index == NULL && strcmp(expr->as.symbol.name, name) == 0;
        case EXPR_INDEX:
            return strcmp(expr->as.index.name, name) == 0 && expr_indices_match(e, expr->as.index.index, index);
        case EXPR_UNARY:
            return expr_reads_target(e, expr->as.unary.operand, name, index);
        case EXPR_BINARY:
            return expr_reads_target(e, expr->as.binary.left, name, index)
                || expr_reads_target(e, expr->as.binary.right, name, index);
        case EXPR_CALL:
            for (i = 0; i < expr->as.call.arg_count; ++i) {
                if (expr_reads_target(e, expr->as.call.args[i], name, index)) {
                    return 1;
                }
            }
            return 0;
        case EXPR_IF:
            return expr_reads_target(e, expr->as.if_expr.condition, name, index)
                || expr_reads_target(e, expr->as.if_expr.then_value, name, index)
                || expr_reads_target(e, expr->as.if_expr.else_value, name, index);
        case EXPR_NUMBER:
            return 0;
    }
    return 0;
}

static int stmt_reads_target(emitter *e, const lumi_stmt *stmt, const char *name, const lumi_expr *index) {
    size_t i;
    switch (stmt->kind) {
        case STMT_LET:
            return expr_reads_target(e, stmt->as.binding.value, name, index);
        case STMT_ASSIGN:
            return expr_reads_target(e, stmt->as.assign.index, name, index)
                || expr_reads_target(e, stmt->as.assign.value, name, index);
        case STMT_COLOR:
            return expr_reads_target(e, stmt->as.color.value, name, index);
        case STMT_IF:
            if (expr_reads_target(e, stmt->as.if_stmt.condition, name, index)) {
                return 1;
            }
            for (i = 0; i < stmt->as.if_stmt.then_branch.count; ++i) {
                if (stmt_reads_target(e, stmt->as.if_stmt.then_branch.items[i], name, index)) {
                    return 1;
                }
            }
            for (i = 0; i < stmt->as.if_stmt.else_branch.count; ++i) {
                if (stmt_reads_target(e, stmt->as.if_stmt.else_branch.items[i], name, index)) {
                    return 1;
                }
            }
            return 0;
        case STMT_FOR:
            if (expr_reads_target(e, stmt->as.for_stmt.start, name, index)
                || expr_reads_target(e, stmt->as.for_stmt.end, name, index)) {
                return 1;
            }
            for (i = 0; i < stmt->as.for_stmt.body.count; ++i) {
                if (stmt_reads_target(e, stmt->as.for_stmt.body.items[i], name, index)) {
                    return 1;
                }
            }
            return 0;
    }
    return 0;
}

static int same_assignment_target(emitter *e, const lumi_stmt *a, const lumi_stmt *b) {
    return a->kind == STMT_ASSIGN && b->kind == STMT_ASSIGN
        && strcmp(a->as.assign.name, b->as.assign.name) == 0
        && expr_indices_match(e, a->as.assign.index, b->as.assign.index);
}

static int assignment_is_dead_store(emitter *e, lumi_stmt_list *list, size_t at) {
    lumi_stmt *stmt = list->items[at];
    size_t i;
    if (e->optimization_level < 2 || stmt->kind != STMT_ASSIGN || !expr_is_discardable(e, stmt->as.assign.value)) {
        return 0;
    }
    for (i = at + 1; i < list->count; ++i) {
        lumi_stmt *next = list->items[i];
        if (stmt_reads_target(e, next, stmt->as.assign.name, stmt->as.assign.index)) {
            return 0;
        }
        if (same_assignment_target(e, stmt, next)) {
            return 1;
        }
        if (next->kind == STMT_IF || next->kind == STMT_FOR || next->kind == STMT_COLOR) {
            return 0;
        }
    }
    return 0;
}

static int emit_stmt_list(emitter *e, lumi_stmt_list *list) {
    size_t i;
    for (i = 0; i < list->count; ++i) {
        if (assignment_is_dead_store(e, list, i)) {
            continue;
        }
        if (!emit_stmt(e, list->items[i])) {
            return 0;
        }
    }
    return 1;
}

static int emit_section(emitter *e, section_kind section, lumi_stmt_list *list) {
    uint16_t saved_stack = e->current_stack;
    byte_buffer *saved_code = e->code;
    size_t saved_scope = e->scope_depth;

    e->current_section = section;
    e->current_stack = 0;
    e->scope_depth = 1;
    if (section == SECTION_INIT) {
        e->code = &e->init_code;
    } else if (section == SECTION_UPDATE) {
        e->code = &e->update_code;
    } else {
        e->code = &e->render_code;
    }
    if (!emit_stmt_list(e, list)) {
        return 0;
    }
    pop_scope_symbols(&e->symbols, e->scope_depth);
    e->scope_depth = saved_scope;
    e->code = saved_code;
    e->current_stack = saved_stack;
    return 1;
}

static int bytecode_instruction_size(const byte_buffer *code, size_t pc, size_t *out_size) {
    uint8_t op;
    if (pc >= code->count) {
        return 0;
    }
    op = code->data[pc];
    switch (op) {
        case LUMI_OP_PUSH_CONST_F32:
        case LUMI_OP_LOAD_GLOBAL:
        case LUMI_OP_STORE_GLOBAL:
        case LUMI_OP_LOAD_KEY:
        case LUMI_OP_STORE_KEY:
        case LUMI_OP_CALL_BUILTIN:
        case LUMI_OP_JUMP:
        case LUMI_OP_JUMP_IF_FALSE:
            if (pc + 3 > code->count) {
                return 0;
            }
            *out_size = 3;
            return 1;
        case LUMI_OP_LOAD_INPUT:
            if (pc + 2 > code->count) {
                return 0;
            }
            *out_size = 2;
            return 1;
        case LUMI_OP_ADD:
        case LUMI_OP_SUB:
        case LUMI_OP_MUL:
        case LUMI_OP_DIV:
        case LUMI_OP_MOD:
        case LUMI_OP_NEG:
        case LUMI_OP_NOT:
        case LUMI_OP_EQ:
        case LUMI_OP_NE:
        case LUMI_OP_LT:
        case LUMI_OP_LE:
        case LUMI_OP_GT:
        case LUMI_OP_GE:
        case LUMI_OP_AND:
        case LUMI_OP_OR:
        case LUMI_OP_SET_COLOR:
        case LUMI_OP_HALT:
        case LUMI_OP_CLAMP:
        case LUMI_OP_DIST:
        case LUMI_OP_RGB:
        case LUMI_OP_HSV:
        case LUMI_OP_DUP:
            *out_size = 1;
            return 1;
        default:
            return 0;
    }
}

static int optimize_noop_jumps(emitter *e, byte_buffer *code) {
    unsigned char *remove_instr;
    size_t *pc_map;
    uint8_t *new_data;
    size_t pc = 0;
    size_t new_count = 0;
    size_t out = 0;

    if (code->count == 0) {
        return 1;
    }
    remove_instr = calloc(code->count, sizeof(*remove_instr));
    pc_map = malloc((code->count + 1) * sizeof(*pc_map));
    if (remove_instr == NULL || pc_map == NULL) {
        free(remove_instr);
        free(pc_map);
        set_emit_error(e, 1, 1, "out of memory while optimizing bytecode");
        return 0;
    }

    while (pc < code->count) {
        size_t size;
        size_t i;
        int remove = 0;
        if (!bytecode_instruction_size(code, pc, &size)) {
            free(remove_instr);
            free(pc_map);
            set_emit_error(e, 1, 1, "invalid bytecode while optimizing");
            return 0;
        }
        if (code->data[pc] == LUMI_OP_JUMP) {
            uint16_t target = read_u16_bytes(code->data + pc + 1);
            remove = target == pc + size;
        }
        remove_instr[pc] = (unsigned char)remove;
        for (i = 0; i < size; ++i) {
            pc_map[pc + i] = new_count;
        }
        if (!remove) {
            new_count += size;
        }
        pc += size;
    }
    pc_map[code->count] = new_count;

    if (new_count == code->count) {
        free(remove_instr);
        free(pc_map);
        return 1;
    }

    new_data = malloc(new_count == 0 ? 1 : new_count);
    if (new_data == NULL) {
        free(remove_instr);
        free(pc_map);
        set_emit_error(e, 1, 1, "out of memory while optimizing bytecode");
        return 0;
    }

    pc = 0;
    while (pc < code->count) {
        size_t size;
        if (!bytecode_instruction_size(code, pc, &size)) {
            free(new_data);
            free(remove_instr);
            free(pc_map);
            set_emit_error(e, 1, 1, "invalid bytecode while optimizing");
            return 0;
        }
        if (!remove_instr[pc]) {
            memcpy(new_data + out, code->data + pc, size);
            if (code->data[pc] == LUMI_OP_JUMP || code->data[pc] == LUMI_OP_JUMP_IF_FALSE) {
                uint16_t target = read_u16_bytes(code->data + pc + 1);
                size_t mapped_target;
                if (target > code->count) {
                    free(new_data);
                    free(remove_instr);
                    free(pc_map);
                    set_emit_error(e, 1, 1, "jump target out of range while optimizing");
                    return 0;
                }
                mapped_target = pc_map[target];
                if (mapped_target > UINT16_MAX) {
                    free(new_data);
                    free(remove_instr);
                    free(pc_map);
                    set_emit_error(e, 1, 1, "optimized bytecode is too large");
                    return 0;
                }
                write_u16_bytes(new_data + out + 1, (uint16_t)mapped_target);
            }
            out += size;
        }
        pc += size;
    }

    free(code->data);
    code->data = new_data;
    code->count = new_count;
    code->capacity = new_count;
    free(remove_instr);
    free(pc_map);
    return 1;
}

static int thread_jumps(emitter *e, byte_buffer *code) {
    size_t pc = 0;
    while (pc < code->count) {
        size_t size;
        if (!bytecode_instruction_size(code, pc, &size)) {
            set_emit_error(e, 1, 1, "invalid bytecode while threading jumps");
            return 0;
        }
        if (code->data[pc] == LUMI_OP_JUMP || code->data[pc] == LUMI_OP_JUMP_IF_FALSE) {
            uint16_t original = read_u16_bytes(code->data + pc + 1);
            uint16_t target = original;
            size_t guard = 0;
            while (target < code->count && code->data[target] == LUMI_OP_JUMP && guard++ < code->count) {
                target = read_u16_bytes(code->data + target + 1);
            }
            if (guard >= code->count) {
                set_emit_error(e, 1, 1, "jump cycle while optimizing");
                return 0;
            }
            if (target != original) {
                write_u16_bytes(code->data + pc + 1, target);
            }
        }
        pc += size;
    }
    return 1;
}

static int remove_unreachable_code(emitter *e, byte_buffer *code) {
    unsigned char *is_start;
    unsigned char *reachable;
    size_t *worklist;
    size_t work_count = 0;
    size_t pc = 0;
    int changed = 0;

    if (code->count == 0) {
        return 1;
    }
    is_start = calloc(code->count, sizeof(*is_start));
    reachable = calloc(code->count, sizeof(*reachable));
    worklist = malloc(code->count * sizeof(*worklist));
    if (is_start == NULL || reachable == NULL || worklist == NULL) {
        free(is_start);
        free(reachable);
        free(worklist);
        set_emit_error(e, 1, 1, "out of memory while removing dead code");
        return 0;
    }

    while (pc < code->count) {
        size_t size;
        is_start[pc] = 1;
        if (!bytecode_instruction_size(code, pc, &size)) {
            free(is_start);
            free(reachable);
            free(worklist);
            set_emit_error(e, 1, 1, "invalid bytecode while removing dead code");
            return 0;
        }
        pc += size;
    }

#define PUSH_REACHABLE(target_pc) \
    do { \
        size_t _target = (target_pc); \
        if (_target >= code->count || !is_start[_target]) { \
            free(is_start); \
            free(reachable); \
            free(worklist); \
            set_emit_error(e, 1, 1, "invalid jump target while removing dead code"); \
            return 0; \
        } \
        if (!reachable[_target]) { \
            reachable[_target] = 1; \
            worklist[work_count++] = _target; \
        } \
    } while (0)

    PUSH_REACHABLE(0);
    while (work_count > 0) {
        size_t current = worklist[--work_count];
        size_t size;
        uint8_t op;
        if (!bytecode_instruction_size(code, current, &size)) {
            free(is_start);
            free(reachable);
            free(worklist);
            set_emit_error(e, 1, 1, "invalid bytecode while removing dead code");
            return 0;
        }
        op = code->data[current];
        if (op == LUMI_OP_JUMP) {
            PUSH_REACHABLE(read_u16_bytes(code->data + current + 1));
        } else if (op == LUMI_OP_JUMP_IF_FALSE) {
            PUSH_REACHABLE(read_u16_bytes(code->data + current + 1));
            if (current + size < code->count) {
                PUSH_REACHABLE(current + size);
            }
        } else if (op != LUMI_OP_HALT && current + size < code->count) {
            PUSH_REACHABLE(current + size);
        }
    }
#undef PUSH_REACHABLE

    for (pc = 0; pc < code->count; ++pc) {
        if (is_start[pc] && !reachable[pc]) {
            changed = 1;
            break;
        }
    }

    if (changed) {
        unsigned char *remove_instr = calloc(code->count, sizeof(*remove_instr));
        size_t *pc_map = malloc((code->count + 1) * sizeof(*pc_map));
        uint8_t *new_data;
        size_t new_count = 0;
        size_t out = 0;

        if (remove_instr == NULL || pc_map == NULL) {
            free(remove_instr);
            free(pc_map);
            free(is_start);
            free(reachable);
            free(worklist);
            set_emit_error(e, 1, 1, "out of memory while removing dead code");
            return 0;
        }

        pc = 0;
        while (pc < code->count) {
            size_t size;
            size_t i;
            if (!bytecode_instruction_size(code, pc, &size)) {
                free(remove_instr);
                free(pc_map);
                free(is_start);
                free(reachable);
                free(worklist);
                set_emit_error(e, 1, 1, "invalid bytecode while removing dead code");
                return 0;
            }
            remove_instr[pc] = !reachable[pc];
            for (i = 0; i < size; ++i) {
                pc_map[pc + i] = new_count;
            }
            if (!remove_instr[pc]) {
                new_count += size;
            }
            pc += size;
        }
        pc_map[code->count] = new_count;

        new_data = malloc(new_count == 0 ? 1 : new_count);
        if (new_data == NULL) {
            free(remove_instr);
            free(pc_map);
            free(is_start);
            free(reachable);
            free(worklist);
            set_emit_error(e, 1, 1, "out of memory while removing dead code");
            return 0;
        }

        pc = 0;
        while (pc < code->count) {
            size_t size;
            if (!bytecode_instruction_size(code, pc, &size)) {
                free(new_data);
                free(remove_instr);
                free(pc_map);
                free(is_start);
                free(reachable);
                free(worklist);
                set_emit_error(e, 1, 1, "invalid bytecode while removing dead code");
                return 0;
            }
            if (!remove_instr[pc]) {
                memcpy(new_data + out, code->data + pc, size);
                if (code->data[pc] == LUMI_OP_JUMP || code->data[pc] == LUMI_OP_JUMP_IF_FALSE) {
                    uint16_t target = read_u16_bytes(code->data + pc + 1);
                    size_t mapped_target = pc_map[target];
                    if (mapped_target > UINT16_MAX) {
                        free(new_data);
                        free(remove_instr);
                        free(pc_map);
                        free(is_start);
                        free(reachable);
                        free(worklist);
                        set_emit_error(e, 1, 1, "optimized bytecode is too large");
                        return 0;
                    }
                    write_u16_bytes(new_data + out + 1, (uint16_t)mapped_target);
                }
                out += size;
            }
            pc += size;
        }

        free(code->data);
        code->data = new_data;
        code->count = new_count;
        code->capacity = new_count;
        free(remove_instr);
        free(pc_map);
    }

    free(is_start);
    free(reachable);
    free(worklist);
    return 1;
}

static int mark_section_constants(emitter *e, const byte_buffer *code, unsigned char *used) {
    size_t pc = 0;
    while (pc < code->count) {
        size_t size;
        if (!bytecode_instruction_size(code, pc, &size)) {
            set_emit_error(e, 1, 1, "invalid bytecode while optimizing constants");
            return 0;
        }
        if (code->data[pc] == LUMI_OP_PUSH_CONST_F32) {
            uint16_t index = read_u16_bytes(code->data + pc + 1);
            if (index >= e->constants.count) {
                set_emit_error(e, 1, 1, "constant index out of range while optimizing");
                return 0;
            }
            used[index] = 1;
        }
        pc += size;
    }
    return 1;
}

static int rewrite_section_constants(emitter *e, byte_buffer *code, const size_t *mapping) {
    size_t pc = 0;
    while (pc < code->count) {
        size_t size;
        if (!bytecode_instruction_size(code, pc, &size)) {
            set_emit_error(e, 1, 1, "invalid bytecode while rewriting constants");
            return 0;
        }
        if (code->data[pc] == LUMI_OP_PUSH_CONST_F32) {
            uint16_t index = read_u16_bytes(code->data + pc + 1);
            if (mapping[index] > UINT16_MAX) {
                set_emit_error(e, 1, 1, "constant table is too large");
                return 0;
            }
            write_u16_bytes(code->data + pc + 1, (uint16_t)mapping[index]);
        }
        pc += size;
    }
    return 1;
}

static int compact_constants(emitter *e) {
    unsigned char *used;
    size_t *mapping;
    float *new_constants;
    size_t i;
    size_t new_count = 0;

    if (e->constants.count == 0) {
        return 1;
    }
    used = calloc(e->constants.count, sizeof(*used));
    mapping = malloc(e->constants.count * sizeof(*mapping));
    if (used == NULL || mapping == NULL) {
        free(used);
        free(mapping);
        set_emit_error(e, 1, 1, "out of memory while optimizing constants");
        return 0;
    }
    if (!mark_section_constants(e, &e->init_code, used)
        || !mark_section_constants(e, &e->update_code, used)
        || !mark_section_constants(e, &e->render_code, used)) {
        free(used);
        free(mapping);
        return 0;
    }

    for (i = 0; i < e->constants.count; ++i) {
        if (used[i]) {
            mapping[i] = new_count++;
        }
    }
    if (new_count == e->constants.count) {
        free(used);
        free(mapping);
        return 1;
    }

    new_constants = malloc((new_count == 0 ? 1 : new_count) * sizeof(*new_constants));
    if (new_constants == NULL) {
        free(used);
        free(mapping);
        set_emit_error(e, 1, 1, "out of memory while optimizing constants");
        return 0;
    }
    for (i = 0; i < e->constants.count; ++i) {
        if (used[i]) {
            new_constants[mapping[i]] = e->constants.data[i];
        }
    }
    if (!rewrite_section_constants(e, &e->init_code, mapping)
        || !rewrite_section_constants(e, &e->update_code, mapping)
        || !rewrite_section_constants(e, &e->render_code, mapping)) {
        free(new_constants);
        free(used);
        free(mapping);
        return 0;
    }

    free(e->constants.data);
    e->constants.data = new_constants;
    e->constants.count = new_count;
    e->constants.capacity = new_count;
    free(used);
    free(mapping);
    return 1;
}

static int instruction_stack_delta(const byte_buffer *code, size_t pc, int *out_delta) {
    uint8_t op = code->data[pc];
    switch (op) {
        case LUMI_OP_PUSH_CONST_F32:
        case LUMI_OP_LOAD_INPUT:
        case LUMI_OP_LOAD_GLOBAL:
        case LUMI_OP_LOAD_KEY:
        case LUMI_OP_DUP:
            *out_delta = 1;
            return 1;
        case LUMI_OP_STORE_GLOBAL:
        case LUMI_OP_STORE_KEY:
        case LUMI_OP_SET_COLOR:
        case LUMI_OP_JUMP_IF_FALSE:
            *out_delta = -1;
            return 1;
        case LUMI_OP_ADD:
        case LUMI_OP_SUB:
        case LUMI_OP_MUL:
        case LUMI_OP_DIV:
        case LUMI_OP_MOD:
        case LUMI_OP_EQ:
        case LUMI_OP_NE:
        case LUMI_OP_LT:
        case LUMI_OP_LE:
        case LUMI_OP_GT:
        case LUMI_OP_GE:
        case LUMI_OP_AND:
        case LUMI_OP_OR:
            *out_delta = -1;
            return 1;
        case LUMI_OP_NEG:
        case LUMI_OP_NOT:
        case LUMI_OP_JUMP:
        case LUMI_OP_HALT:
            *out_delta = 0;
            return 1;
        case LUMI_OP_CLAMP:
        case LUMI_OP_RGB:
        case LUMI_OP_HSV:
            *out_delta = -2;
            return 1;
        case LUMI_OP_DIST:
            *out_delta = -3;
            return 1;
        case LUMI_OP_CALL_BUILTIN:
            if (pc + 2 >= code->count) {
                return 0;
            }
            *out_delta = 1 - (int)code->data[pc + 2];
            return 1;
        default:
            return 0;
    }
}

static int compute_section_stack_depth(emitter *e, const byte_buffer *code, uint16_t *inout_max) {
    int *stack_at;
    size_t *worklist;
    size_t work_count = 0;
    size_t i;

    if (code->count == 0) {
        return 1;
    }
    stack_at = malloc(code->count * sizeof(*stack_at));
    worklist = malloc(code->count * sizeof(*worklist));
    if (stack_at == NULL || worklist == NULL) {
        free(stack_at);
        free(worklist);
        set_emit_error(e, 1, 1, "out of memory while recomputing stack depth");
        return 0;
    }
    for (i = 0; i < code->count; ++i) {
        stack_at[i] = -1;
    }
    stack_at[0] = 0;
    worklist[work_count++] = 0;

#define PUSH_STACK_TARGET(target_pc, stack_value) \
    do { \
        size_t _target = (target_pc); \
        int _stack = (stack_value); \
        if (_target >= code->count) { \
            free(stack_at); \
            free(worklist); \
            set_emit_error(e, 1, 1, "jump target out of range while recomputing stack depth"); \
            return 0; \
        } \
        if (stack_at[_target] == -1) { \
            stack_at[_target] = _stack; \
            worklist[work_count++] = _target; \
        } else if (stack_at[_target] != _stack) { \
            free(stack_at); \
            free(worklist); \
            set_emit_error(e, 1, 1, "inconsistent stack depth while recomputing bytecode"); \
            return 0; \
        } \
    } while (0)

    while (work_count > 0) {
        size_t pc = worklist[--work_count];
        size_t size;
        int delta;
        int next_stack;
        uint8_t op;
        if (!bytecode_instruction_size(code, pc, &size) || !instruction_stack_delta(code, pc, &delta)) {
            free(stack_at);
            free(worklist);
            set_emit_error(e, 1, 1, "invalid bytecode while recomputing stack depth");
            return 0;
        }
        next_stack = stack_at[pc] + delta;
        if (next_stack < 0 || next_stack > UINT16_MAX) {
            free(stack_at);
            free(worklist);
            set_emit_error(e, 1, 1, "invalid stack depth while recomputing bytecode");
            return 0;
        }
        if ((uint16_t)next_stack > *inout_max) {
            *inout_max = (uint16_t)next_stack;
        }
        op = code->data[pc];
        if (op == LUMI_OP_JUMP) {
            PUSH_STACK_TARGET(read_u16_bytes(code->data + pc + 1), next_stack);
        } else if (op == LUMI_OP_JUMP_IF_FALSE) {
            PUSH_STACK_TARGET(read_u16_bytes(code->data + pc + 1), next_stack);
            if (pc + size < code->count) {
                PUSH_STACK_TARGET(pc + size, next_stack);
            }
        } else if (op != LUMI_OP_HALT && pc + size < code->count) {
            PUSH_STACK_TARGET(pc + size, next_stack);
        }
    }
#undef PUSH_STACK_TARGET

    free(stack_at);
    free(worklist);
    return 1;
}

static int recompute_max_stack(emitter *e) {
    uint16_t max_stack = 0;
    if (!compute_section_stack_depth(e, &e->init_code, &max_stack)
        || !compute_section_stack_depth(e, &e->update_code, &max_stack)
        || !compute_section_stack_depth(e, &e->render_code, &max_stack)) {
        return 0;
    }
    e->max_stack = max_stack;
    return 1;
}

static int optimize_emitted_bytecode(emitter *e) {
    if (!thread_jumps(e, &e->init_code)
        || !thread_jumps(e, &e->update_code)
        || !thread_jumps(e, &e->render_code)
        || !remove_unreachable_code(e, &e->init_code)
        || !remove_unreachable_code(e, &e->update_code)
        || !remove_unreachable_code(e, &e->render_code)
        || !optimize_noop_jumps(e, &e->init_code)
        || !optimize_noop_jumps(e, &e->update_code)
        || !optimize_noop_jumps(e, &e->render_code)) {
        return 0;
    }
    return thread_jumps(e, &e->init_code)
        && thread_jumps(e, &e->update_code)
        && thread_jumps(e, &e->render_code)
        && compact_constants(e)
        && recompute_max_stack(e);
}

int lumi_emit_bytecode(lumi_program *program, int optimization_level, lumi_bytecode *out_bytecode, lumi_emit_error *out_error) {
    emitter e;
    size_t i;

    memset(&e, 0, sizeof(e));
    memset(out_bytecode, 0, sizeof(*out_bytecode));
    memset(out_error, 0, sizeof(*out_error));
    e.error = out_error;
    e.optimization_level = optimization_level;
    if (e.optimization_level < 0) {
        e.optimization_level = 0;
    }
    if (e.optimization_level > 3) {
        e.optimization_level = 3;
    }

    if (!program->type_seen) {
        set_emit_error(&e, 1, 1, "program type must be declared");
        goto fail;
    }
    if (!program->render_section.present) {
        set_emit_error(&e, 1, 1, "render section must be defined");
        goto fail;
    }

    add_input(&e, "x", LUMI_INPUT_X);
    add_input(&e, "y", LUMI_INPUT_Y);
    add_input(&e, "dt", LUMI_INPUT_DT);
    add_input(&e, "delta_ms", LUMI_INPUT_DT);
    add_input(&e, "speed", LUMI_INPUT_SPEED);
    add_input(&e, "pressed", LUMI_INPUT_PRESSED);
    add_input(&e, "press", LUMI_INPUT_PRESS);
    add_input(&e, "pressed_percentage", LUMI_INPUT_PRESS);

    for (i = 0; i < program->vars.count; ++i) {
        lumi_var_decl *var = &program->vars.items[i];
        const_value value;
        size_t j;
        var->initializer = simplify_expr(&e, var->initializer);
        if (e.error->message != NULL) {
            goto fail;
        }
        value = eval_expr(&e, var->initializer);
        if (value.kind != CONST_FLOAT) {
            set_emit_error(&e, var->initializer->line, var->initializer->column,
                "variable initializer for '%s' must be compile-time constant; use init/update/render for runtime setup", var->name);
            goto fail;
        }
        if (var->storage == LUMI_VAR_GLOBAL) {
            size_t base = e.global_count;
            for (j = 0; j < var->array_size; ++j) {
                if (!float_buffer_push(&e.initial_globals, value.number)) {
                    goto fail;
                }
                e.global_count++;
            }
            if (!declare_symbol(&e, var->name, SYMBOL_GLOBAL_VAR, base, var->array_size, 0, value, NULL, 1, 1)) {
                goto fail;
            }
        } else {
            size_t base = e.key_count;
            for (j = 0; j < var->array_size; ++j) {
                if (!float_buffer_push(&e.initial_keys, value.number)) {
                    goto fail;
                }
                e.key_count++;
            }
            if (!declare_symbol(&e, var->name, SYMBOL_KEY_VAR, base, var->array_size, 0, value, NULL, 1, 1)) {
                goto fail;
            }
        }
    }

    if (program->init_section.present && !emit_section(&e, SECTION_INIT, &program->init_section.statements)) {
        goto fail;
    }
    if (program->update_section.present && !emit_section(&e, SECTION_UPDATE, &program->update_section.statements)) {
        goto fail;
    }
    if (!emit_section(&e, SECTION_RENDER, &program->render_section.statements)) {
        goto fail;
    }
    if (!stmt_list_guarantees_color(&program->render_section.statements)) {
        set_emit_error(&e, 1, 1, "render section must assign color on all control-flow paths");
        goto fail;
    }
    e.code = &e.init_code;
    if (!emit_op(&e, LUMI_OP_HALT, 0, 1, 1)) {
        goto fail;
    }
    e.code = &e.update_code;
    if (!emit_op(&e, LUMI_OP_HALT, 0, 1, 1)) {
        goto fail;
    }
    e.code = &e.render_code;
    if (!emit_op(&e, LUMI_OP_HALT, 0, 1, 1)) {
        goto fail;
    }
    if (e.optimization_level >= 2 && !optimize_emitted_bytecode(&e)) {
        goto fail;
    }

    out_bytecode->program_type = program->program_type;
    out_bytecode->constants = e.constants.data;
    out_bytecode->constant_count = e.constants.count;
    out_bytecode->initial_globals = e.initial_globals.data;
    out_bytecode->global_count = e.initial_globals.count;
    out_bytecode->initial_keys = e.initial_keys.data;
    out_bytecode->key_count = e.initial_keys.count;
    out_bytecode->max_stack_depth = e.max_stack;
    out_bytecode->init_code = e.init_code.data;
    out_bytecode->init_size = e.init_code.count;
    out_bytecode->update_code = e.update_code.data;
    out_bytecode->update_size = e.update_code.count;
    out_bytecode->render_code = e.render_code.data;
    out_bytecode->render_size = e.render_code.count;

    for (i = 0; i < e.symbols.count; ++i) {
        free(e.symbols.items[i].name);
    }
    free(e.symbols.items);
    return 1;

fail:
    free(e.constants.data);
    free(e.initial_globals.data);
    free(e.initial_keys.data);
    free(e.init_code.data);
    free(e.update_code.data);
    free(e.render_code.data);
    for (i = 0; i < e.symbols.count; ++i) {
        free(e.symbols.items[i].name);
    }
    free(e.symbols.items);
    return 0;
}
