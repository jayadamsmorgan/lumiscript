#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static lumi_token make_token(lumi_lexer *lexer, lumi_token_type type, const char *start, size_t length) {
    lumi_token token;
    token.type = type;
    token.start = start;
    token.length = length;
    token.line = lexer->line;
    token.column = lexer->column - length;
    token.number = 0.0;
    return token;
}

static int is_identifier_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static int is_identifier_continue(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static lumi_token_type keyword_type(const char *start, size_t length) {
    if (length == 2 && strncmp(start, "if", 2) == 0) {
        return TOKEN_KEYWORD_IF;
    }
    if (length == 3 && strncmp(start, "let", 3) == 0) {
        return TOKEN_KEYWORD_LET;
    }
    if (length == 3 && strncmp(start, "var", 3) == 0) {
        return TOKEN_KEYWORD_VAR;
    }
    if (length == 4 && strncmp(start, "type", 4) == 0) {
        return TOKEN_KEYWORD_TYPE;
    }
    if (length == 4 && strncmp(start, "else", 4) == 0) {
        return TOKEN_KEYWORD_ELSE;
    }
    if (length == 3 && strncmp(start, "key", 3) == 0) {
        return TOKEN_KEYWORD_KEY;
    }
    if (length == 2 && strncmp(start, "in", 2) == 0) {
        return TOKEN_KEYWORD_IN;
    }
    if (length == 3 && strncmp(start, "for", 3) == 0) {
        return TOKEN_KEYWORD_FOR;
    }
    if (length == 4 && strncmp(start, "init", 4) == 0) {
        return TOKEN_KEYWORD_INIT;
    }
    if (length == 6 && strncmp(start, "global", 6) == 0) {
        return TOKEN_KEYWORD_GLOBAL;
    }
    if (length == 6 && strncmp(start, "render", 6) == 0) {
        return TOKEN_KEYWORD_RENDER;
    }
    if (length == 5 && strncmp(start, "color", 5) == 0) {
        return TOKEN_KEYWORD_COLOR;
    }
    if (length == 6 && strncmp(start, "update", 6) == 0) {
        return TOKEN_KEYWORD_UPDATE;
    }
    return TOKEN_IDENTIFIER;
}

void lumi_lexer_init(lumi_lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->cursor = source;
    lexer->line = 1;
    lexer->column = 1;
}

lumi_token lumi_lexer_next(lumi_lexer *lexer) {
    const char *start;
    char c;

    for (;;) {
        c = *lexer->cursor;
        if (c == ' ' || c == '\t' || c == '\r') {
            lexer->cursor++;
            lexer->column++;
            continue;
        }
        break;
    }

    start = lexer->cursor;
    c = *lexer->cursor++;
    lexer->column++;

    if (c == '\0') {
        return make_token(lexer, TOKEN_EOF, start, 0);
    }

    if (c == '\n') {
        lexer->line++;
        lexer->column = 1;
        return make_token(lexer, TOKEN_NEWLINE, start, 1);
    }

    if (is_identifier_start(c)) {
        while (is_identifier_continue(*lexer->cursor)) {
            lexer->cursor++;
            lexer->column++;
        }
        return make_token(lexer, keyword_type(start, (size_t)(lexer->cursor - start)), start,
            (size_t)(lexer->cursor - start));
    }

    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)*lexer->cursor))) {
        char *end_ptr;
        lumi_token token;
        while (isdigit((unsigned char)*lexer->cursor)
            || (*lexer->cursor == '.' && lexer->cursor[1] != '.')) {
            lexer->cursor++;
            lexer->column++;
        }
        token = make_token(lexer, TOKEN_NUMBER, start, (size_t)(lexer->cursor - start));
        token.number = strtod(start, &end_ptr);
        return token;
    }

    switch (c) {
        case '(':
            return make_token(lexer, TOKEN_LPAREN, start, 1);
        case ')':
            return make_token(lexer, TOKEN_RPAREN, start, 1);
        case '{':
            return make_token(lexer, TOKEN_LBRACE, start, 1);
        case '}':
            return make_token(lexer, TOKEN_RBRACE, start, 1);
        case '[':
            return make_token(lexer, TOKEN_LBRACKET, start, 1);
        case ']':
            return make_token(lexer, TOKEN_RBRACKET, start, 1);
        case ',':
            return make_token(lexer, TOKEN_COMMA, start, 1);
        case '.':
            if (*lexer->cursor == '.') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_DOT_DOT, start, 2);
            }
            break;
        case '+':
            return make_token(lexer, TOKEN_PLUS, start, 1);
        case '-':
            return make_token(lexer, TOKEN_MINUS, start, 1);
        case '*':
            return make_token(lexer, TOKEN_STAR, start, 1);
        case '/':
            return make_token(lexer, TOKEN_SLASH, start, 1);
        case '%':
            return make_token(lexer, TOKEN_PERCENT, start, 1);
        case '=':
            if (*lexer->cursor == '=') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_EQ_EQ, start, 2);
            }
            return make_token(lexer, TOKEN_ASSIGN, start, 1);
        case '!':
            if (*lexer->cursor == '=') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_BANG_EQ, start, 2);
            }
            return make_token(lexer, TOKEN_BANG, start, 1);
        case '<':
            if (*lexer->cursor == '=') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_LT_EQ, start, 2);
            }
            return make_token(lexer, TOKEN_LT, start, 1);
        case '>':
            if (*lexer->cursor == '=') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_GT_EQ, start, 2);
            }
            return make_token(lexer, TOKEN_GT, start, 1);
        case '&':
            if (*lexer->cursor == '&') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_AND_AND, start, 2);
            }
            break;
        case '|':
            if (*lexer->cursor == '|') {
                lexer->cursor++;
                lexer->column++;
                return make_token(lexer, TOKEN_OR_OR, start, 2);
            }
            break;
    }

    return make_token(lexer, TOKEN_INVALID, start, 1);
}
