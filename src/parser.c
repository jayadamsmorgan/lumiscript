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

    if (p->current.type == TOKEN_IDENTIFIER && peek_type(p) == TOKEN_ASSIGN) {
        name = p->current;
        advance(p);
        advance(p);
        value = parse_expression(p);
        if (value == NULL) {
            return NULL;
        }
        stmt = new_stmt(STMT_ASSIGN, name.line, name.column);
        if (stmt != NULL) {
            stmt->as.assign.name = copy_string(name.start, name.length);
            stmt->as.assign.value = value;
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
    advance(p);
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

static int declare_symbol(emitter *e, const char *name, symbol_kind kind, size_t index,
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
            if (sym != NULL && sym->kind == SYMBOL_LET && sym->is_const) {
                return sym->value;
            }
            return out;
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
    for (i = 0; i < expr->as.call.arg_count; ++i) {
        if (!emit_const_or_runtime(e, expr->as.call.args[i])) {
            return 0;
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

static int emit_if_expr(emitter *e, const lumi_expr *expr) {
    size_t jump_false;
    size_t jump_end;
    uint16_t stack_before = e->current_stack;

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

static int emit_expr(emitter *e, const lumi_expr *expr) {
    symbol *sym;
    uint16_t index;

    switch (expr->kind) {
        case EXPR_NUMBER:
            if (!add_constant(e, (float)expr->as.number, &index)) {
                return 0;
            }
            return emit_op(e, LUMI_OP_PUSH_CONST_F32, 1, expr->line, expr->column) && emit_u16(e->code, index);
        case EXPR_SYMBOL:
            sym = find_symbol(&e->symbols, expr->as.symbol.name);
            if (sym == NULL) {
                set_emit_error(e, expr->line, expr->column, "unknown symbol '%s'", expr->as.symbol.name);
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
                && emit_u16(e->code, (uint16_t)sym->index);
        case EXPR_UNARY:
            if (!emit_const_or_runtime(e, expr->as.unary.operand)) {
                return 0;
            }
            return emit_op(e, expr->as.unary.op == TOKEN_MINUS ? LUMI_OP_NEG : LUMI_OP_NOT, 0, expr->line, expr->column);
        case EXPR_BINARY:
            if (!emit_const_or_runtime(e, expr->as.binary.left) || !emit_const_or_runtime(e, expr->as.binary.right)) {
                return 0;
            }
            switch (expr->as.binary.op) {
                case TOKEN_PLUS: return emit_op(e, LUMI_OP_ADD, -1, expr->line, expr->column);
                case TOKEN_MINUS: return emit_op(e, LUMI_OP_SUB, -1, expr->line, expr->column);
                case TOKEN_STAR: return emit_op(e, LUMI_OP_MUL, -1, expr->line, expr->column);
                case TOKEN_SLASH: return emit_op(e, LUMI_OP_DIV, -1, expr->line, expr->column);
                case TOKEN_PERCENT: return emit_op(e, LUMI_OP_MOD, -1, expr->line, expr->column);
                case TOKEN_EQ_EQ: return emit_op(e, LUMI_OP_EQ, -1, expr->line, expr->column);
                case TOKEN_BANG_EQ: return emit_op(e, LUMI_OP_NE, -1, expr->line, expr->column);
                case TOKEN_LT: return emit_op(e, LUMI_OP_LT, -1, expr->line, expr->column);
                case TOKEN_LT_EQ: return emit_op(e, LUMI_OP_LE, -1, expr->line, expr->column);
                case TOKEN_GT: return emit_op(e, LUMI_OP_GT, -1, expr->line, expr->column);
                case TOKEN_GT_EQ: return emit_op(e, LUMI_OP_GE, -1, expr->line, expr->column);
                case TOKEN_AND_AND: return emit_op(e, LUMI_OP_AND, -1, expr->line, expr->column);
                case TOKEN_OR_OR: return emit_op(e, LUMI_OP_OR, -1, expr->line, expr->column);
                default:
                    set_emit_error(e, expr->line, expr->column, "unsupported operator");
                    return 0;
            }
        case EXPR_CALL:
            return emit_builtin(e, expr);
        case EXPR_IF:
            return emit_if_expr(e, expr);
    }
    return 0;
}

static int emit_stmt_list(emitter *e, const lumi_stmt_list *list);
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

static int emit_if_stmt(emitter *e, const lumi_stmt *stmt) {
    size_t jump_false;
    size_t jump_end;
    uint16_t stack_before = e->current_stack;

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

static int emit_assignment(emitter *e, const lumi_stmt *stmt) {
    symbol *sym = find_symbol(&e->symbols, stmt->as.assign.name);
    if (sym == NULL) {
        set_emit_error(e, stmt->line, stmt->column, "unknown symbol '%s'", stmt->as.assign.name);
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
        && emit_u16(e->code, (uint16_t)sym->index);
}

static int emit_stmt(emitter *e, const lumi_stmt *stmt) {
    const_value value;

    switch (stmt->kind) {
        case STMT_LET:
            value = eval_expr(e, stmt->as.binding.value);
            return declare_symbol(e, stmt->as.binding.name, SYMBOL_LET, 0, value.kind == CONST_FLOAT, value,
                stmt->as.binding.value, stmt->line, stmt->column);
        case STMT_ASSIGN:
            return emit_assignment(e, stmt);
        case STMT_COLOR:
            if (e->current_section != SECTION_RENDER) {
                set_emit_error(e, stmt->line, stmt->column, "color can only be assigned in render");
                return 0;
            }
            if (!emit_const_or_runtime(e, stmt->as.color.value)) {
                return 0;
            }
            e->render_has_color = 1;
            return emit_op(e, LUMI_OP_SET_COLOR, -1, stmt->line, stmt->column);
        case STMT_IF:
            return emit_if_stmt(e, stmt);
    }
    return 0;
}

static int emit_stmt_list(emitter *e, const lumi_stmt_list *list) {
    size_t i;
    for (i = 0; i < list->count; ++i) {
        if (!emit_stmt(e, list->items[i])) {
            return 0;
        }
    }
    return 1;
}

static int emit_section(emitter *e, section_kind section, const lumi_stmt_list *list) {
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

int lumi_emit_bytecode(const lumi_program *program, lumi_bytecode *out_bytecode, lumi_emit_error *out_error) {
    emitter e;
    size_t i;

    memset(&e, 0, sizeof(e));
    memset(out_bytecode, 0, sizeof(*out_bytecode));
    memset(out_error, 0, sizeof(*out_error));
    e.error = out_error;

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
        const lumi_var_decl *var = &program->vars.items[i];
        const_value value = eval_expr(&e, var->initializer);
        if (value.kind != CONST_FLOAT) {
            set_emit_error(&e, var->initializer->line, var->initializer->column,
                "variable initializer for '%s' must be compile-time constant; use init/update/render for runtime setup", var->name);
            goto fail;
        }
        if (var->storage == LUMI_VAR_GLOBAL) {
            if (!float_buffer_push(&e.initial_globals, value.number)
                || !declare_symbol(&e, var->name, SYMBOL_GLOBAL_VAR, e.global_count++, 0, value, NULL, 1, 1)) {
                goto fail;
            }
        } else {
            if (!float_buffer_push(&e.initial_keys, value.number)
                || !declare_symbol(&e, var->name, SYMBOL_KEY_VAR, e.key_count++, 0, value, NULL, 1, 1)) {
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
