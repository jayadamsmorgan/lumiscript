#include "internal.h"
#include "lumi/compiler.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum json_type {
    JSON_UNDEFINED = 0,
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_PRIMITIVE,
} json_type;

typedef struct json_token {
    json_type type;
    int start;
    int end;
    int size;
    int parent;
} json_token;

typedef struct string_buffer {
    char *data;
    size_t count;
    size_t capacity;
} string_buffer;

typedef struct document {
    char *uri;
    char *text;
} document;

typedef struct document_store {
    document *items;
    size_t count;
    size_t capacity;
} document_store;

typedef struct symbol_name {
    char *text;
    int kind;
} symbol_name;

typedef struct symbol_list {
    symbol_name *items;
    size_t count;
    size_t capacity;
} symbol_list;

typedef struct info_entry {
    const char *name;
    const char *kind;
    const char *detail;
    const char *documentation;
} info_entry;

static const info_entry keyword_info[] = {
    {"type", "keyword", "Script type declaration", "Declares whether the script is `static` or `animation`."},
    {"global", "keyword", "Global storage class", "Used with `var` to declare shared persistent state."},
    {"key", "keyword", "Per-key storage class", "Used with `var` to declare persistent state stored separately for each key."},
    {"var", "keyword", "Persistent variable declaration", "Declares a persistent variable. Use `global var` or `key var` at the top level."},
    {"let", "keyword", "Local immutable binding", "Declares an immutable local binding inside a section block."},
    {"init", "section", "Init section", "Runs once when the script state is created or reset."},
    {"update", "section", "Update section", "Runs once per backlight update tick. Only `dt` and `speed` are available."},
    {"render", "section", "Render section", "Runs once per key and must assign `color` on all control-flow paths."},
    {"color", "keyword", "Color statement", "Assigns the final output color for the current render invocation."},
    {"if", "keyword", "Conditional", "Supports statement form and value-form `if ... else ...` expressions."},
    {"else", "keyword", "Else branch", "Required for value-form `if` expressions and optional for statement `if`."},
    {"for", "keyword", "Compile-time loop", "A bounded `for i in start..end { ... }` loop that is unrolled by the compiler."},
    {"in", "keyword", "Range introducer", "Used in `for` loops between the loop variable and the `start..end` range."},
};

static const info_entry input_info[] = {
    {"x", "input", "Key X coordinate", "Per-key X position in the keyboard coordinate space."},
    {"y", "input", "Key Y coordinate", "Per-key Y position in the keyboard coordinate space."},
    {"dt", "input", "Delta time", "Milliseconds since the previous backlight update."},
    {"delta_ms", "input", "Delta time alias", "Alias of `dt`."},
    {"speed", "input", "Speed control", "User-selected backlight speed multiplier."},
    {"pressed", "input", "Pressed flag", "Boolean-like `0` or `1` indicating whether the key is pressed."},
    {"press", "input", "Press percentage", "Key press percentage in the range `0..100`."},
    {"pressed_percentage", "input", "Press percentage alias", "Alias of `press`."},
};

static const info_entry builtin_info[] = {
    {"abs", "function", "abs(x)", "Absolute value."},
    {"sin", "function", "sin(x)", "Sine of `x` in radians."},
    {"cos", "function", "cos(x)", "Cosine of `x` in radians."},
    {"sqrt", "function", "sqrt(x)", "Square root of a non-negative value."},
    {"ceil", "function", "ceil(x)", "Smallest integer value greater than or equal to `x`."},
    {"floor", "function", "floor(x)", "Largest integer value less than or equal to `x`."},
    {"round", "function", "round(x)", "Nearest integer value to `x`."},
    {"clamp", "function", "clamp(x, lo, hi)", "Clamps `x` into the inclusive range `[lo, hi]`."},
    {"dist", "function", "dist(x1, y1, x2, y2)", "Euclidean distance between two points."},
    {"lerp", "function", "lerp(a, b, t)", "Linear interpolation from `a` to `b` by factor `t`."},
    {"min", "function", "min(a, b)", "Returns the smaller of two values."},
    {"max", "function", "max(a, b)", "Returns the larger of two values."},
    {"pow", "function", "pow(x, y)", "Raises `x` to the power `y`."},
    {"rand", "function", "rand()", "Returns a pseudo-random float in the range `[0, 1)`."},
    {"rgb", "function", "rgb(r, g, b)", "Packs RGB channel values into the output color cell."},
    {"hsv", "function", "hsv(h, s, v)", "Converts HSV values into a packed RGB output color."},
};

static int sb_reserve(string_buffer *buf, size_t extra) {
    char *data;
    size_t needed = buf->count + extra + 1;
    size_t capacity = buf->capacity == 0 ? 256 : buf->capacity;

    if (needed <= buf->capacity) {
        return 1;
    }
    while (capacity < needed) {
        capacity *= 2;
    }
    data = realloc(buf->data, capacity);
    if (data == NULL) {
        return 0;
    }
    buf->data = data;
    buf->capacity = capacity;
    return 1;
}

static int sb_append_n(string_buffer *buf, const char *text, size_t len) {
    if (!sb_reserve(buf, len)) {
        return 0;
    }
    memcpy(buf->data + buf->count, text, len);
    buf->count += len;
    buf->data[buf->count] = '\0';
    return 1;
}

static int sb_append(string_buffer *buf, const char *text) {
    return sb_append_n(buf, text, strlen(text));
}

static int sb_appendf(string_buffer *buf, const char *fmt, ...) {
    va_list args;
    va_list copy;
    int needed;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0 || !sb_reserve(buf, (size_t)needed)) {
        va_end(args);
        return 0;
    }
    vsnprintf(buf->data + buf->count, buf->capacity - buf->count, fmt, args);
    va_end(args);
    buf->count += (size_t)needed;
    return 1;
}

static int sb_append_json_escaped(string_buffer *buf, const char *text) {
    size_t i;

    if (!sb_append(buf, "\"")) {
        return 0;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
            case '\\': if (!sb_append(buf, "\\\\")) return 0; break;
            case '"': if (!sb_append(buf, "\\\"")) return 0; break;
            case '\b': if (!sb_append(buf, "\\b")) return 0; break;
            case '\f': if (!sb_append(buf, "\\f")) return 0; break;
            case '\n': if (!sb_append(buf, "\\n")) return 0; break;
            case '\r': if (!sb_append(buf, "\\r")) return 0; break;
            case '\t': if (!sb_append(buf, "\\t")) return 0; break;
            default:
                if (c < 0x20) {
                    if (!sb_appendf(buf, "\\u%04X", c)) {
                        return 0;
                    }
                } else {
                    if (!sb_append_n(buf, (const char *)&text[i], 1)) {
                        return 0;
                    }
                }
                break;
        }
    }
    return sb_append(buf, "\"");
}

static void sb_free(string_buffer *buf) {
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

static int tokenize_json(const char *json, json_token **out_tokens, size_t *out_count) {
    size_t capacity = 256;
    size_t count = 0;
    int parent_stack[256];
    int stack_top = -1;
    int i = 0;
    json_token *tokens = malloc(capacity * sizeof(*tokens));

    if (tokens == NULL) {
        return 0;
    }

    while (json[i] != '\0') {
        char c = json[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':' || c == ',') {
            i++;
            continue;
        }
        if (count == capacity) {
            json_token *grown = realloc(tokens, capacity * 2 * sizeof(*tokens));
            if (grown == NULL) {
                free(tokens);
                return 0;
            }
            tokens = grown;
            capacity *= 2;
        }
        if (c == '{' || c == '[') {
            json_token token;
            token.type = c == '{' ? JSON_OBJECT : JSON_ARRAY;
            token.start = i;
            token.end = -1;
            token.size = 0;
            token.parent = stack_top >= 0 ? parent_stack[stack_top] : -1;
            if (token.parent >= 0) {
                tokens[token.parent].size++;
            }
            tokens[count] = token;
            parent_stack[++stack_top] = (int)count;
            count++;
            i++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (stack_top < 0) {
                free(tokens);
                return 0;
            }
            tokens[parent_stack[stack_top]].end = i + 1;
            stack_top--;
            i++;
            continue;
        }
        if (c == '"') {
            int start = ++i;
            json_token token;
            while (json[i] != '\0') {
                if (json[i] == '\\') {
                    if (json[i + 1] == '\0') {
                        free(tokens);
                        return 0;
                    }
                    i += 2;
                    continue;
                }
                if (json[i] == '"') {
                    break;
                }
                i++;
            }
            if (json[i] != '"') {
                free(tokens);
                return 0;
            }
            token.type = JSON_STRING;
            token.start = start;
            token.end = i;
            token.size = 0;
            token.parent = stack_top >= 0 ? parent_stack[stack_top] : -1;
            if (token.parent >= 0) {
                tokens[token.parent].size++;
            }
            tokens[count++] = token;
            i++;
            continue;
        }
        {
            int start = i;
            json_token token;
            while (json[i] != '\0'
                && json[i] != ' ' && json[i] != '\t' && json[i] != '\r' && json[i] != '\n'
                && json[i] != ',' && json[i] != ']' && json[i] != '}') {
                i++;
            }
            token.type = JSON_PRIMITIVE;
            token.start = start;
            token.end = i;
            token.size = 0;
            token.parent = stack_top >= 0 ? parent_stack[stack_top] : -1;
            if (token.parent >= 0) {
                tokens[token.parent].size++;
            }
            tokens[count++] = token;
        }
    }

    if (stack_top >= 0) {
        free(tokens);
        return 0;
    }

    *out_tokens = tokens;
    *out_count = count;
    return 1;
}

static int token_eq(const char *json, const json_token *token, const char *text) {
    size_t len = strlen(text);
    return token->type == JSON_STRING
        && (size_t)(token->end - token->start) == len
        && strncmp(json + token->start, text, len) == 0;
}

static int json_object_get(const char *json, const json_token *tokens, size_t count, int object_index, const char *key) {
    size_t i;
    for (i = (size_t)object_index + 1; i + 1 < count; ++i) {
        if (tokens[i].parent == object_index && token_eq(json, &tokens[i], key)) {
            return (int)i + 1;
        }
    }
    return -1;
}

static int json_array_first(const json_token *tokens, size_t count, int array_index) {
    size_t i;
    for (i = (size_t)array_index + 1; i < count; ++i) {
        if (tokens[i].parent == array_index) {
            return (int)i;
        }
    }
    return -1;
}

static char *json_token_to_string(const char *json, const json_token *token) {
    string_buffer out = {0};
    int i;

    if (token->type != JSON_STRING) {
        return NULL;
    }
    for (i = token->start; i < token->end; ++i) {
        char c = json[i];
        if (c != '\\') {
            if (!sb_append_n(&out, &json[i], 1)) {
                sb_free(&out);
                return NULL;
            }
            continue;
        }
        i++;
        if (i >= token->end) {
            sb_free(&out);
            return NULL;
        }
        switch (json[i]) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                if (i + 4 >= token->end) {
                    sb_free(&out);
                    return NULL;
                }
                c = '?';
                i += 4;
                break;
            default:
                sb_free(&out);
                return NULL;
        }
        if (!sb_append_n(&out, &c, 1)) {
            sb_free(&out);
            return NULL;
        }
    }
    return out.data;
}

static long json_token_to_long(const char *json, const json_token *token, int *ok) {
    char buffer[64];
    size_t len = (size_t)(token->end - token->start);
    char *end;
    long value;

    *ok = 0;
    if (token->type != JSON_PRIMITIVE || len == 0 || len >= sizeof(buffer)) {
        return 0;
    }
    memcpy(buffer, json + token->start, len);
    buffer[len] = '\0';
    value = strtol(buffer, &end, 10);
    if (*end != '\0') {
        return 0;
    }
    *ok = 1;
    return value;
}

static int send_message(const char *payload) {
    size_t len = strlen(payload);
    if (printf("Content-Length: %zu\r\n\r\n%s", len, payload) < 0) {
        return 0;
    }
    return fflush(stdout) == 0;
}

static int send_response_raw(const char *json, const json_token *id_token, const char *result_json) {
    string_buffer payload = {0};

    if (!sb_append(&payload, "{\"jsonrpc\":\"2.0\",\"id\":")
        || !sb_append_n(&payload, json + id_token->start, (size_t)(id_token->end - id_token->start))
        || !sb_append(&payload, ",\"result\":")
        || !sb_append(&payload, result_json)
        || !sb_append(&payload, "}")) {
        sb_free(&payload);
        return 0;
    }
    if (!send_message(payload.data)) {
        sb_free(&payload);
        return 0;
    }
    sb_free(&payload);
    return 1;
}

static int send_error_response(const char *json, const json_token *id_token, int code, const char *message) {
    string_buffer payload = {0};

    if (!sb_append(&payload, "{\"jsonrpc\":\"2.0\",\"id\":")
        || !sb_append_n(&payload, json + id_token->start, (size_t)(id_token->end - id_token->start))
        || !sb_append(&payload, ",\"error\":{\"code\":")
        || !sb_appendf(&payload, "%d", code)
        || !sb_append(&payload, ",\"message\":")
        || !sb_append_json_escaped(&payload, message)
        || !sb_append(&payload, "}}")) {
        sb_free(&payload);
        return 0;
    }
    if (!send_message(payload.data)) {
        sb_free(&payload);
        return 0;
    }
    sb_free(&payload);
    return 1;
}

static int document_store_set(document_store *store, const char *uri, const char *text) {
    size_t i;
    char *uri_copy = NULL;
    char *text_copy = NULL;

    for (i = 0; i < store->count; ++i) {
        if (strcmp(store->items[i].uri, uri) == 0) {
            text_copy = malloc(strlen(text) + 1);
            if (text_copy == NULL) {
                return 0;
            }
            strcpy(text_copy, text);
            free(store->items[i].text);
            store->items[i].text = text_copy;
            return 1;
        }
    }

    if (store->count == store->capacity) {
        size_t capacity = store->capacity == 0 ? 8 : store->capacity * 2;
        document *items = realloc(store->items, capacity * sizeof(*items));
        if (items == NULL) {
            return 0;
        }
        store->items = items;
        store->capacity = capacity;
    }
    uri_copy = malloc(strlen(uri) + 1);
    text_copy = malloc(strlen(text) + 1);
    if (uri_copy == NULL || text_copy == NULL) {
        free(uri_copy);
        free(text_copy);
        return 0;
    }
    strcpy(uri_copy, uri);
    strcpy(text_copy, text);
    store->items[store->count].uri = uri_copy;
    store->items[store->count].text = text_copy;
    store->count++;
    return 1;
}

static const char *document_store_get(const document_store *store, const char *uri) {
    size_t i;
    for (i = 0; i < store->count; ++i) {
        if (strcmp(store->items[i].uri, uri) == 0) {
            return store->items[i].text;
        }
    }
    return NULL;
}

static void document_store_remove(document_store *store, const char *uri) {
    size_t i;
    for (i = 0; i < store->count; ++i) {
        if (strcmp(store->items[i].uri, uri) == 0) {
            free(store->items[i].uri);
            free(store->items[i].text);
            memmove(&store->items[i], &store->items[i + 1], (store->count - i - 1) * sizeof(*store->items));
            store->count--;
            return;
        }
    }
}

static void document_store_free(document_store *store) {
    size_t i;
    for (i = 0; i < store->count; ++i) {
        free(store->items[i].uri);
        free(store->items[i].text);
    }
    free(store->items);
    memset(store, 0, sizeof(*store));
}

static int position_to_offset(const char *text, int line, int character) {
    int current_line = 0;
    int current_char = 0;
    int offset = 0;

    while (text[offset] != '\0') {
        if (current_line == line && current_char == character) {
            return offset;
        }
        if (text[offset] == '\n') {
            current_line++;
            current_char = 0;
            offset++;
            if (current_line > line) {
                return offset;
            }
            continue;
        }
        current_char++;
        offset++;
    }
    return offset;
}

static int is_word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static char *word_at_position(const char *text, int line, int character, int *out_start, int *out_end) {
    int offset = position_to_offset(text, line, character);
    int start = offset;
    int end = offset;
    char *word;

    if (text[offset] == '\0' && offset > 0) {
        offset--;
    }
    if (!is_word_char(text[offset]) && offset > 0 && is_word_char(text[offset - 1])) {
        offset--;
    }
    if (!is_word_char(text[offset])) {
        return NULL;
    }
    start = offset;
    end = offset;
    while (start > 0 && is_word_char(text[start - 1])) {
        start--;
    }
    while (text[end] != '\0' && is_word_char(text[end])) {
        end++;
    }
    word = malloc((size_t)(end - start) + 1);
    if (word == NULL) {
        return NULL;
    }
    memcpy(word, text + start, (size_t)(end - start));
    word[end - start] = '\0';
    if (out_start != NULL) {
        *out_start = start;
    }
    if (out_end != NULL) {
        *out_end = end;
    }
    return word;
}

static const info_entry *find_info(const info_entry *entries, size_t count, const char *name) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

static int send_diagnostics(const char *uri, const char *text) {
    lumi_compile_result result;
    string_buffer payload = {0};
    int ok;

    memset(&result, 0, sizeof(result));
    ok = lumi_compile_source(text, &result);

    if (!sb_append(&payload, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":")
        || !sb_append_json_escaped(&payload, uri)
        || !sb_append(&payload, ",\"diagnostics\":[")) {
        lumi_compile_result_free(&result);
        sb_free(&payload);
        return 0;
    }

    if (!ok) {
        int start_line = result.error_line > 0 ? (int)result.error_line - 1 : 0;
        int start_char = result.error_column > 0 ? (int)result.error_column - 1 : 0;
        if (!sb_append(&payload, "{\"range\":{\"start\":{\"line\":")
            || !sb_appendf(&payload, "%d", start_line)
            || !sb_append(&payload, ",\"character\":")
            || !sb_appendf(&payload, "%d", start_char)
            || !sb_append(&payload, "},\"end\":{\"line\":")
            || !sb_appendf(&payload, "%d", start_line)
            || !sb_append(&payload, ",\"character\":")
            || !sb_appendf(&payload, "%d", start_char + 1)
            || !sb_append(&payload, "}},\"severity\":1,\"source\":\"lumic\",\"message\":")
            || !sb_append_json_escaped(&payload, result.error_message != NULL ? result.error_message : "compile error")
            || !sb_append(&payload, "}")) {
            lumi_compile_result_free(&result);
            sb_free(&payload);
            return 0;
        }
    }

    if (!sb_append(&payload, "]}}")) {
        lumi_compile_result_free(&result);
        sb_free(&payload);
        return 0;
    }

    lumi_compile_result_free(&result);
    if (!send_message(payload.data)) {
        sb_free(&payload);
        return 0;
    }
    sb_free(&payload);
    return 1;
}

static int symbol_list_contains(const symbol_list *list, const char *text) {
    size_t i;
    for (i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].text, text) == 0) {
            return 1;
        }
    }
    return 0;
}

static int symbol_list_add(symbol_list *list, const char *text, int kind) {
    char *copy;
    symbol_name *items;

    if (symbol_list_contains(list, text)) {
        return 1;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        items = realloc(list->items, capacity * sizeof(*items));
        if (items == NULL) {
            return 0;
        }
        list->items = items;
        list->capacity = capacity;
    }
    copy = malloc(strlen(text) + 1);
    if (copy == NULL) {
        return 0;
    }
    strcpy(copy, text);
    list->items[list->count].text = copy;
    list->items[list->count].kind = kind;
    list->count++;
    return 1;
}

static void symbol_list_free(symbol_list *list) {
    size_t i;
    for (i = 0; i < list->count; ++i) {
        free(list->items[i].text);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int collect_document_symbols(const char *text, symbol_list *symbols) {
    lumi_lexer lexer;
    lumi_token token;
    lumi_token next;

    lumi_lexer_init(&lexer, text);
    for (;;) {
        token = lumi_lexer_next(&lexer);
        if (token.type == TOKEN_EOF || token.type == TOKEN_INVALID) {
            break;
        }
        if (token.type == TOKEN_KEYWORD_GLOBAL || token.type == TOKEN_KEYWORD_KEY) {
            next = lumi_lexer_next(&lexer);
            if (next.type != TOKEN_KEYWORD_VAR) {
                continue;
            }
            next = lumi_lexer_next(&lexer);
            if (next.type == TOKEN_IDENTIFIER) {
                char *name = malloc(next.length + 1);
                if (name == NULL) {
                    return 0;
                }
                memcpy(name, next.start, next.length);
                name[next.length] = '\0';
                if (!symbol_list_add(symbols, name, 6)) {
                    free(name);
                    return 0;
                }
                free(name);
            }
            continue;
        }
        if (token.type == TOKEN_KEYWORD_LET) {
            next = lumi_lexer_next(&lexer);
            if (next.type == TOKEN_IDENTIFIER) {
                char *name = malloc(next.length + 1);
                if (name == NULL) {
                    return 0;
                }
                memcpy(name, next.start, next.length);
                name[next.length] = '\0';
                if (!symbol_list_add(symbols, name, 13)) {
                    free(name);
                    return 0;
                }
                free(name);
            }
            continue;
        }
    }
    return 1;
}

static int append_completion_items(string_buffer *payload, const char *text) {
    static const struct {
        const char *label;
        int kind;
        const char *detail;
    } builtins[] = {
        {"type", 14, "keyword"},
        {"global", 14, "keyword"},
        {"key", 14, "keyword"},
        {"var", 14, "keyword"},
        {"let", 14, "keyword"},
        {"init", 14, "section"},
        {"update", 14, "section"},
        {"render", 14, "section"},
        {"color", 14, "keyword"},
        {"if", 14, "keyword"},
        {"else", 14, "keyword"},
        {"for", 14, "keyword"},
        {"in", 14, "keyword"},
        {"x", 6, "input"},
        {"y", 6, "input"},
        {"dt", 6, "input"},
        {"delta_ms", 6, "input"},
        {"speed", 6, "input"},
        {"pressed", 6, "input"},
        {"press", 6, "input"},
        {"pressed_percentage", 6, "input"},
        {"abs", 3, "builtin"},
        {"sin", 3, "builtin"},
        {"cos", 3, "builtin"},
        {"sqrt", 3, "builtin"},
        {"ceil", 3, "builtin"},
        {"floor", 3, "builtin"},
        {"round", 3, "builtin"},
        {"clamp", 3, "builtin"},
        {"dist", 3, "builtin"},
        {"lerp", 3, "builtin"},
        {"min", 3, "builtin"},
        {"max", 3, "builtin"},
        {"pow", 3, "builtin"},
        {"rand", 3, "builtin"},
        {"rgb", 3, "builtin"},
        {"hsv", 3, "builtin"},
    };
    symbol_list symbols = {0};
    size_t i;
    int first = 1;

    if (!sb_append(payload, "{\"isIncomplete\":false,\"items\":[")) {
        return 0;
    }
    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        if (!first && !sb_append(payload, ",")) {
            symbol_list_free(&symbols);
            return 0;
        }
        first = 0;
        if (!sb_append(payload, "{\"label\":")
            || !sb_append_json_escaped(payload, builtins[i].label)
            || !sb_append(payload, ",\"kind\":")
            || !sb_appendf(payload, "%d", builtins[i].kind)
            || !sb_append(payload, ",\"detail\":")
            || !sb_append_json_escaped(payload, builtins[i].detail)
            || !sb_append(payload, "}")) {
            symbol_list_free(&symbols);
            return 0;
        }
    }

    if (text != NULL && collect_document_symbols(text, &symbols)) {
        for (i = 0; i < symbols.count; ++i) {
            if (!sb_append(payload, ",{\"label\":")
                || !sb_append_json_escaped(payload, symbols.items[i].text)
                || !sb_append(payload, ",\"kind\":")
                || !sb_appendf(payload, "%d", symbols.items[i].kind)
                || !sb_append(payload, ",\"detail\":\"document symbol\"}")) {
                symbol_list_free(&symbols);
                return 0;
            }
        }
    }
    symbol_list_free(&symbols);
    return sb_append(payload, "]}");
}

static int append_hover(string_buffer *payload, const char *word) {
    const info_entry *entry = find_info(keyword_info, sizeof(keyword_info) / sizeof(keyword_info[0]), word);
    if (entry == NULL) {
        entry = find_info(input_info, sizeof(input_info) / sizeof(input_info[0]), word);
    }
    if (entry == NULL) {
        entry = find_info(builtin_info, sizeof(builtin_info) / sizeof(builtin_info[0]), word);
    }
    if (entry == NULL) {
        return sb_append(payload, "null");
    }
    if (!sb_append(payload, "{\"contents\":{\"kind\":\"markdown\",\"value\":")) {
        return 0;
    }
    {
        string_buffer markdown = {0};
        if (!sb_appendf(&markdown, "```lumi\n%s\n```\n\n**%s**\n\n%s", entry->detail, entry->kind, entry->documentation)
            || !sb_append_json_escaped(payload, markdown.data)) {
            sb_free(&markdown);
            return 0;
        }
        sb_free(&markdown);
    }
    return sb_append(payload, "}}");
}

static int append_document_symbols(string_buffer *payload, const char *text) {
    lumi_lexer lexer;
    lumi_token token;
    lumi_token next;
    int first = 1;

    if (!sb_append(payload, "[")) {
        return 0;
    }
    lumi_lexer_init(&lexer, text);
    for (;;) {
        token = lumi_lexer_next(&lexer);
        if (token.type == TOKEN_EOF || token.type == TOKEN_INVALID) {
            break;
        }
        if (token.type == TOKEN_KEYWORD_GLOBAL || token.type == TOKEN_KEYWORD_KEY) {
            next = lumi_lexer_next(&lexer);
            if (next.type != TOKEN_KEYWORD_VAR) {
                continue;
            }
            next = lumi_lexer_next(&lexer);
            if (next.type == TOKEN_IDENTIFIER) {
                char name[128];
                int start_line;
                int start_character;
                int end_line;
                int end_character;
                size_t name_len = next.length < sizeof(name) - 1 ? next.length : sizeof(name) - 1;
                memcpy(name, next.start, name_len);
                name[name_len] = '\0';
                if (!first && !sb_append(payload, ",")) {
                    return 0;
                }
                first = 0;
                start_line = (int)next.line - 1;
                start_character = (int)next.column - 1;
                end_line = start_line;
                end_character = start_character + (int)name_len;
                if (!sb_append(payload, "{\"name\":")
                    || !sb_append_json_escaped(payload, name)
                    || !sb_append(payload, ",\"kind\":13,\"range\":{\"start\":{\"line\":")
                    || !sb_appendf(payload, "%d", start_line)
                    || !sb_append(payload, ",\"character\":")
                    || !sb_appendf(payload, "%d", start_character)
                    || !sb_append(payload, "},\"end\":{\"line\":")
                    || !sb_appendf(payload, "%d", end_line)
                    || !sb_append(payload, ",\"character\":")
                    || !sb_appendf(payload, "%d", end_character)
                    || !sb_append(payload, "}},\"selectionRange\":{\"start\":{\"line\":")
                    || !sb_appendf(payload, "%d", start_line)
                    || !sb_append(payload, ",\"character\":")
                    || !sb_appendf(payload, "%d", start_character)
                    || !sb_append(payload, "},\"end\":{\"line\":")
                    || !sb_appendf(payload, "%d", end_line)
                    || !sb_append(payload, ",\"character\":")
                    || !sb_appendf(payload, "%d", end_character)
                    || !sb_append(payload, "}}}")) {
                    return 0;
                }
            }
            continue;
        }
        if (token.type == TOKEN_KEYWORD_INIT || token.type == TOKEN_KEYWORD_UPDATE || token.type == TOKEN_KEYWORD_RENDER) {
            char name[16];
            int start_line = (int)token.line - 1;
            int start_character = (int)token.column - 1;
            int end_line = start_line;
            int end_character = start_character + (int)token.length;
            size_t name_len = token.length < sizeof(name) - 1 ? token.length : sizeof(name) - 1;
            memcpy(name, token.start, name_len);
            name[name_len] = '\0';
            if (!first && !sb_append(payload, ",")) {
                return 0;
            }
            first = 0;
            if (!sb_append(payload, "{\"name\":")
                || !sb_append_json_escaped(payload, name)
                || !sb_append(payload, ",\"kind\":12,\"range\":{\"start\":{\"line\":")
                || !sb_appendf(payload, "%d", start_line)
                || !sb_append(payload, ",\"character\":")
                || !sb_appendf(payload, "%d", start_character)
                || !sb_append(payload, "},\"end\":{\"line\":")
                || !sb_appendf(payload, "%d", end_line)
                || !sb_append(payload, ",\"character\":")
                || !sb_appendf(payload, "%d", end_character)
                || !sb_append(payload, "}},\"selectionRange\":{\"start\":{\"line\":")
                || !sb_appendf(payload, "%d", start_line)
                || !sb_append(payload, ",\"character\":")
                || !sb_appendf(payload, "%d", start_character)
                || !sb_append(payload, "},\"end\":{\"line\":")
                || !sb_appendf(payload, "%d", end_line)
                || !sb_append(payload, ",\"character\":")
                || !sb_appendf(payload, "%d", end_character)
                || !sb_append(payload, "}}}")) {
                return 0;
            }
        }
    }
    return sb_append(payload, "]");
}

static int handle_completion(const char *json, const json_token *tokens, size_t count, int id_index, const document_store *docs) {
    int params_index = json_object_get(json, tokens, count, 0, "params");
    int text_document_index;
    int uri_index;
    char *uri;
    const char *text;
    string_buffer result = {0};

    if (params_index < 0) {
        return send_error_response(json, &tokens[id_index], -32602, "missing params");
    }
    text_document_index = json_object_get(json, tokens, count, params_index, "textDocument");
    uri_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "uri") : -1;
    uri = uri_index >= 0 ? json_token_to_string(json, &tokens[uri_index]) : NULL;
    text = uri != NULL ? document_store_get(docs, uri) : NULL;

    if (!append_completion_items(&result, text)) {
        free(uri);
        sb_free(&result);
        return 0;
    }
    free(uri);
    if (!send_response_raw(json, &tokens[id_index], result.data)) {
        sb_free(&result);
        return 0;
    }
    sb_free(&result);
    return 1;
}

static int handle_hover(const char *json, const json_token *tokens, size_t count, int id_index, const document_store *docs) {
    int params_index = json_object_get(json, tokens, count, 0, "params");
    int text_document_index;
    int position_index;
    int uri_index;
    int line_index;
    int char_index;
    char *uri;
    const char *text;
    string_buffer result = {0};
    char *word;
    int ok;
    int line;
    int character;

    if (params_index < 0) {
        return send_error_response(json, &tokens[id_index], -32602, "missing params");
    }
    text_document_index = json_object_get(json, tokens, count, params_index, "textDocument");
    position_index = json_object_get(json, tokens, count, params_index, "position");
    uri_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "uri") : -1;
    line_index = position_index >= 0 ? json_object_get(json, tokens, count, position_index, "line") : -1;
    char_index = position_index >= 0 ? json_object_get(json, tokens, count, position_index, "character") : -1;
    if (uri_index < 0 || line_index < 0 || char_index < 0) {
        return send_error_response(json, &tokens[id_index], -32602, "missing textDocument or position");
    }
    uri = json_token_to_string(json, &tokens[uri_index]);
    if (uri == NULL) {
        return send_error_response(json, &tokens[id_index], -32602, "invalid uri");
    }
    text = document_store_get(docs, uri);
    free(uri);
    if (text == NULL) {
        return send_response_raw(json, &tokens[id_index], "null");
    }
    line = (int)json_token_to_long(json, &tokens[line_index], &ok);
    if (!ok) {
        return send_error_response(json, &tokens[id_index], -32602, "invalid line");
    }
    character = (int)json_token_to_long(json, &tokens[char_index], &ok);
    if (!ok) {
        return send_error_response(json, &tokens[id_index], -32602, "invalid character");
    }
    word = word_at_position(text, line, character, NULL, NULL);
    if (word == NULL) {
        return send_response_raw(json, &tokens[id_index], "null");
    }
    if (!append_hover(&result, word)) {
        free(word);
        sb_free(&result);
        return 0;
    }
    free(word);
    if (!send_response_raw(json, &tokens[id_index], result.data)) {
        sb_free(&result);
        return 0;
    }
    sb_free(&result);
    return 1;
}

static int handle_document_symbols(const char *json, const json_token *tokens, size_t count, int id_index, const document_store *docs) {
    int params_index = json_object_get(json, tokens, count, 0, "params");
    int text_document_index;
    int uri_index;
    char *uri;
    const char *text;
    string_buffer result = {0};

    if (params_index < 0) {
        return send_error_response(json, &tokens[id_index], -32602, "missing params");
    }
    text_document_index = json_object_get(json, tokens, count, params_index, "textDocument");
    uri_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "uri") : -1;
    if (uri_index < 0) {
        return send_error_response(json, &tokens[id_index], -32602, "missing uri");
    }
    uri = json_token_to_string(json, &tokens[uri_index]);
    if (uri == NULL) {
        return send_error_response(json, &tokens[id_index], -32602, "invalid uri");
    }
    text = document_store_get(docs, uri);
    free(uri);
    if (text == NULL) {
        return send_response_raw(json, &tokens[id_index], "[]");
    }
    if (!append_document_symbols(&result, text)) {
        sb_free(&result);
        return 0;
    }
    if (!send_response_raw(json, &tokens[id_index], result.data)) {
        sb_free(&result);
        return 0;
    }
    sb_free(&result);
    return 1;
}

static int handle_did_open(const char *json, const json_token *tokens, size_t count, const document_store *docs_unused, document_store *docs) {
    int params_index = json_object_get(json, tokens, count, 0, "params");
    int text_document_index;
    int uri_index;
    int text_index;
    char *uri;
    char *text;
    (void)docs_unused;

    if (params_index < 0) {
        return 1;
    }
    text_document_index = json_object_get(json, tokens, count, params_index, "textDocument");
    uri_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "uri") : -1;
    text_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "text") : -1;
    if (uri_index < 0 || text_index < 0) {
        return 1;
    }
    uri = json_token_to_string(json, &tokens[uri_index]);
    text = json_token_to_string(json, &tokens[text_index]);
    if (uri == NULL || text == NULL) {
        free(uri);
        free(text);
        return 0;
    }
    if (!document_store_set(docs, uri, text) || !send_diagnostics(uri, text)) {
        free(uri);
        free(text);
        return 0;
    }
    free(uri);
    free(text);
    return 1;
}

static int handle_did_change(const char *json, const json_token *tokens, size_t count, document_store *docs) {
    int params_index = json_object_get(json, tokens, count, 0, "params");
    int text_document_index;
    int uri_index;
    int changes_index;
    int first_change;
    int text_index;
    char *uri;
    char *text;

    if (params_index < 0) {
        return 1;
    }
    text_document_index = json_object_get(json, tokens, count, params_index, "textDocument");
    changes_index = json_object_get(json, tokens, count, params_index, "contentChanges");
    uri_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "uri") : -1;
    first_change = changes_index >= 0 ? json_array_first(tokens, count, changes_index) : -1;
    text_index = first_change >= 0 ? json_object_get(json, tokens, count, first_change, "text") : -1;
    if (uri_index < 0 || text_index < 0) {
        return 1;
    }
    uri = json_token_to_string(json, &tokens[uri_index]);
    text = json_token_to_string(json, &tokens[text_index]);
    if (uri == NULL || text == NULL) {
        free(uri);
        free(text);
        return 0;
    }
    if (!document_store_set(docs, uri, text) || !send_diagnostics(uri, text)) {
        free(uri);
        free(text);
        return 0;
    }
    free(uri);
    free(text);
    return 1;
}

static int handle_did_close(const char *json, const json_token *tokens, size_t count, document_store *docs) {
    int params_index = json_object_get(json, tokens, count, 0, "params");
    int text_document_index;
    int uri_index;
    char *uri;
    string_buffer payload = {0};

    if (params_index < 0) {
        return 1;
    }
    text_document_index = json_object_get(json, tokens, count, params_index, "textDocument");
    uri_index = text_document_index >= 0 ? json_object_get(json, tokens, count, text_document_index, "uri") : -1;
    if (uri_index < 0) {
        return 1;
    }
    uri = json_token_to_string(json, &tokens[uri_index]);
    if (uri == NULL) {
        return 0;
    }
    document_store_remove(docs, uri);
    if (!sb_append(&payload, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":")
        || !sb_append_json_escaped(&payload, uri)
        || !sb_append(&payload, ",\"diagnostics\":[]}}")) {
        free(uri);
        sb_free(&payload);
        return 0;
    }
    free(uri);
    if (!send_message(payload.data)) {
        sb_free(&payload);
        return 0;
    }
    sb_free(&payload);
    return 1;
}

static char *read_message(void) {
    char line[256];
    size_t content_length = 0;
    char *payload;

    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }
        if (strncmp(line, "Content-Length:", 15) == 0) {
            content_length = (size_t)strtoul(line + 15, NULL, 10);
        }
    }
    if (content_length == 0) {
        return NULL;
    }
    payload = malloc(content_length + 1);
    if (payload == NULL) {
        return NULL;
    }
    if (fread(payload, 1, content_length, stdin) != content_length) {
        free(payload);
        return NULL;
    }
    payload[content_length] = '\0';
    return payload;
}

int main(void) {
    document_store docs = {0};
    int shutdown_requested = 0;
    int exit_requested = 0;
    int exit_code = 1;

    while (!exit_requested) {
        char *message = read_message();
        json_token *tokens = NULL;
        size_t token_count = 0;
        int method_index;
        int id_index;
        char *method;

        if (message == NULL) {
            break;
        }
        if (!tokenize_json(message, &tokens, &token_count) || token_count == 0 || tokens[0].type != JSON_OBJECT) {
            free(tokens);
            free(message);
            continue;
        }

        method_index = json_object_get(message, tokens, token_count, 0, "method");
        id_index = json_object_get(message, tokens, token_count, 0, "id");
        method = method_index >= 0 ? json_token_to_string(message, &tokens[method_index]) : NULL;

        if (method != NULL) {
            if (strcmp(method, "initialize") == 0) {
                const char *result =
                    "{\"capabilities\":{\"textDocumentSync\":1,"
                    "\"hoverProvider\":true,"
                    "\"completionProvider\":{\"resolveProvider\":false},"
                    "\"documentSymbolProvider\":true},"
                    "\"serverInfo\":{\"name\":\"lumils\",\"version\":\"0.1.0\"}}";
                if (id_index >= 0 && !send_response_raw(message, &tokens[id_index], result)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "shutdown") == 0) {
                shutdown_requested = 1;
                if (id_index >= 0 && !send_response_raw(message, &tokens[id_index], "null")) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "exit") == 0) {
                exit_requested = 1;
                exit_code = shutdown_requested ? 0 : 1;
            } else if (strcmp(method, "textDocument/didOpen") == 0) {
                if (!handle_did_open(message, tokens, token_count, &docs, &docs)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "textDocument/didChange") == 0) {
                if (!handle_did_change(message, tokens, token_count, &docs)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "textDocument/didClose") == 0) {
                if (!handle_did_close(message, tokens, token_count, &docs)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "textDocument/completion") == 0) {
                if (id_index >= 0 && !handle_completion(message, tokens, token_count, id_index, &docs)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "textDocument/hover") == 0) {
                if (id_index >= 0 && !handle_hover(message, tokens, token_count, id_index, &docs)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
                if (id_index >= 0 && !handle_document_symbols(message, tokens, token_count, id_index, &docs)) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            } else if (id_index >= 0) {
                if (!send_error_response(message, &tokens[id_index], -32601, "method not found")) {
                    free(method);
                    free(tokens);
                    free(message);
                    break;
                }
            }
        }

        free(method);
        free(tokens);
        free(message);
    }

    document_store_free(&docs);
    return exit_code;
}
