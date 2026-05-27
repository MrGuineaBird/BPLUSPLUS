/*
    bpp - native B++ compiler

    This is the native compiler path:

        B++ -> C source -> native executable

    Supported in this native compiler:
    - say
    - set
    - add/subtract/multiply/divide
    - lists: [], put/remove/empty
    - if/elif/else/end
    - repeat, repeat as
    - for each
    - while, forever
    - stop loop, next loop
    - true/false/nothing/nil

    - functions and return
    - ask/read/write

    Deliberately not supported in native mode:
    - foreign-language imports
    - foreign-language passthrough
*/

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#endif

#include "bpp_version.h"

#define BPP_UPDATE_REPO "MrGuineaBird/BPLUSPLUS"
#define BPP_UPDATE_API "https://api.github.com/repos/" BPP_UPDATE_REPO "/releases/latest"
#define BPP_UPDATE_CHECK_SECONDS 86400

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StringBuilder;

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} NameList;

typedef enum {
    BLOCK_NORMAL,
    BLOCK_FUNCTION
} BlockKind;

typedef struct {
    const char *source_path;
    StringBuilder main_body;
    StringBuilder functions_body;
    StringBuilder *body;
    NameList globals;
    NameList locals;
    NameList *names;
    int indent;
    int temp_counter;
    int had_error;
    int in_function;
    BlockKind blocks[256];
    int block_count;
} Compiler;

static void die_out_of_memory(void) {
    fprintf(stderr, "bpp: out of memory\n");
    exit(2);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) {
        die_out_of_memory();
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size ? size : 1);
    if (!next) {
        die_out_of_memory();
    }
    return next;
}

static char *xstrdup(const char *text) {
    size_t len = strlen(text);
    char *copy = (char *)xmalloc(len + 1);
    memcpy(copy, text, len + 1);
    return copy;
}

static char *format_text(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return xstrdup("");
    }
    char *out = (char *)xmalloc((size_t)needed + 1);
    vsnprintf(out, (size_t)needed + 1, fmt, args);
    va_end(args);
    return out;
}

static void sb_init(StringBuilder *sb) {
    sb->cap = 4096;
    sb->len = 0;
    sb->data = (char *)xmalloc(sb->cap);
    sb->data[0] = '\0';
}

static void sb_free(StringBuilder *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void sb_reserve(StringBuilder *sb, size_t extra) {
    size_t needed = sb->len + extra + 1;
    if (needed <= sb->cap) {
        return;
    }
    while (sb->cap < needed) {
        sb->cap *= 2;
    }
    sb->data = (char *)xrealloc(sb->data, sb->cap);
}

static void sb_append(StringBuilder *sb, const char *text) {
    size_t len = strlen(text);
    sb_reserve(sb, len);
    memcpy(sb->data + sb->len, text, len + 1);
    sb->len += len;
}

static void sb_append_len(StringBuilder *sb, const char *text, size_t len) {
    sb_reserve(sb, len);
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(StringBuilder *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return;
    }
    sb_reserve(sb, (size_t)needed);
    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args);
    sb->len += (size_t)needed;
    va_end(args);
}

static void names_init(NameList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void names_free(NameList *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static int names_contains(NameList *list, const char *name) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int names_add(NameList *list, const char *name) {
    if (names_contains(list, name)) {
        return 0;
    }
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = (char **)xrealloc(list->items, list->cap * sizeof(char *));
    }
    list->items[list->count++] = xstrdup(name);
    return 1;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "bpp: could not open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    char *data = (char *)xmalloc((size_t)size + 1);
    size_t read = fread(data, 1, (size_t)size, file);
    data[read] = '\0';
    fclose(file);
    return data;
}

static int write_file(const char *path, const char *data) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "bpp: could not write %s: %s\n", path, strerror(errno));
        return 0;
    }
    fwrite(data, 1, strlen(data), file);
    fclose(file);
    return 1;
}

static char *trim_in_place(char *text) {
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
    return text;
}

static int starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int ends_with(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int is_identifier(const char *text) {
    if (!text[0] || !(isalpha((unsigned char)text[0]) || text[0] == '_')) {
        return 0;
    }
    for (size_t i = 1; text[i]; i++) {
        if (!(isalnum((unsigned char)text[i]) || text[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

static char *slice_trim(const char *text, size_t start, size_t end) {
    while (start < end && isspace((unsigned char)text[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)text[end - 1])) {
        end--;
    }
    char *out = (char *)xmalloc(end - start + 1);
    memcpy(out, text + start, end - start);
    out[end - start] = '\0';
    return out;
}

static char *strip_comment_copy(const char *line) {
    StringBuilder out;
    sb_init(&out);
    int quote = 0;
    int escaped = 0;
    for (size_t i = 0; line[i]; i++) {
        char ch = line[i];
        if (escaped) {
            char tmp[2] = {ch, '\0'};
            sb_append(&out, tmp);
            escaped = 0;
            continue;
        }
        if (quote && ch == '\\') {
            char tmp[2] = {ch, '\0'};
            sb_append(&out, tmp);
            escaped = 1;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            if (!quote) {
                quote = ch;
            } else if (quote == ch) {
                quote = 0;
            }
            char tmp[2] = {ch, '\0'};
            sb_append(&out, tmp);
            continue;
        }
        if (ch == '#' && !quote) {
            break;
        }
        char tmp[2] = {ch, '\0'};
        sb_append(&out, tmp);
    }
    char *copy = xstrdup(out.data);
    sb_free(&out);
    return copy;
}

static int find_top_level_token(const char *text, const char *token) {
    int quote = 0;
    int escaped = 0;
    int depth = 0;
    size_t token_len = strlen(token);
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (quote && ch == '\\') {
            escaped = 1;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            if (!quote) {
                quote = ch;
            } else if (quote == ch) {
                quote = 0;
            }
            continue;
        }
        if (quote) {
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') {
            depth++;
            continue;
        }
        if (ch == ')' || ch == ']' || ch == '}') {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if (depth == 0 && strncmp(text + i, token, token_len) == 0) {
            if ((strcmp(token, "-") == 0 || strcmp(token, "+") == 0) && i == 0) {
                continue;
            }
            return (int)i;
        }
    }
    return -1;
}

static int partition_keyword(const char *text, const char *keyword, char **left, char **right) {
    int pos = find_top_level_token(text, keyword);
    if (pos < 0) {
        return 0;
    }
    *left = slice_trim(text, 0, (size_t)pos);
    *right = slice_trim(text, (size_t)pos + strlen(keyword), strlen(text));
    return 1;
}

static int split_top_level_args(const char *text, char ***out_args, size_t *out_count) {
    NameList args;
    names_init(&args);
    int quote = 0;
    int escaped = 0;
    int depth = 0;
    size_t start = 0;
    size_t len = strlen(text);

    for (size_t i = 0; i <= len; i++) {
        char ch = text[i];
        if (escaped) {
            escaped = 0;
            continue;
        }
        if (quote && ch == '\\') {
            escaped = 1;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            if (!quote) {
                quote = ch;
            } else if (quote == ch) {
                quote = 0;
            }
            continue;
        }
        if (quote) {
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') {
            depth++;
            continue;
        }
        if (ch == ')' || ch == ']' || ch == '}') {
            if (depth > 0) {
                depth--;
            }
            continue;
        }
        if ((ch == ',' && depth == 0) || ch == '\0') {
            char *part = slice_trim(text, start, i);
            if (part[0]) {
                names_add(&args, part);
            }
            free(part);
            start = i + 1;
        }
    }

    *out_args = args.items;
    *out_count = args.count;
    return 1;
}

static void free_split_args(char **args, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(args[i]);
    }
    free(args);
}

static char *to_c_string_literal(const char *literal) {
    size_t len = strlen(literal);
    if (len >= 2 && literal[0] == '"' && literal[len - 1] == '"') {
        return xstrdup(literal);
    }

    StringBuilder out;
    sb_init(&out);
    sb_append(&out, "\"");
    if (len >= 2 && literal[0] == '\'' && literal[len - 1] == '\'') {
        for (size_t i = 1; i + 1 < len; i++) {
            char ch = literal[i];
            if (ch == '"' || ch == '\\') {
                sb_append(&out, "\\");
            }
            char tmp[2] = {ch, '\0'};
            sb_append(&out, tmp);
        }
    }
    sb_append(&out, "\"");
    char *result = xstrdup(out.data);
    sb_free(&out);
    return result;
}

static int is_number_literal(const char *text) {
    size_t i = 0;
    if (text[i] == '-' || text[i] == '+') {
        i++;
    }
    int seen_digit = 0;
    int seen_dot = 0;
    for (; text[i]; i++) {
        if (isdigit((unsigned char)text[i])) {
            seen_digit = 1;
            continue;
        }
        if (text[i] == '.' && !seen_dot) {
            seen_dot = 1;
            continue;
        }
        return 0;
    }
    return seen_digit;
}

static char *transpile_expr(const char *expr);

static char *transpile_binary(const char *expr, const char *token, const char *function_name) {
    char *left = NULL;
    char *right = NULL;
    if (!partition_keyword(expr, token, &left, &right)) {
        return NULL;
    }
    char *left_c = transpile_expr(left);
    char *right_c = transpile_expr(right);
    char *out = format_text("%s(%s, %s)", function_name, left_c, right_c);
    free(left);
    free(right);
    free(left_c);
    free(right_c);
    return out;
}

static char *transpile_expr(const char *expr) {
    char *copy = xstrdup(expr);
    char *text = trim_in_place(copy);

    const char *comparisons[][2] = {
        {" or ", "bpp_or"},
        {" and ", "bpp_and"},
        {"==", "bpp_eq"},
        {"!=", "bpp_ne"},
        {">=", "bpp_ge"},
        {"<=", "bpp_le"},
        {">", "bpp_gt"},
        {"<", "bpp_lt"},
        {"+", "bpp_add"},
        {"-", "bpp_sub"},
        {"*", "bpp_mul"},
        {"/", "bpp_div"},
    };

    for (size_t i = 0; i < sizeof(comparisons) / sizeof(comparisons[0]); i++) {
        char *out = transpile_binary(text, comparisons[i][0], comparisons[i][1]);
        if (out) {
            free(copy);
            return out;
        }
    }

    size_t len = strlen(text);
    if (strcmp(text, "true") == 0 || strcmp(text, "True") == 0) {
        free(copy);
        return xstrdup("bpp_bool(1)");
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "False") == 0) {
        free(copy);
        return xstrdup("bpp_bool(0)");
    }
    if (strcmp(text, "nothing") == 0 || strcmp(text, "nil") == 0 || strcmp(text, "None") == 0) {
        free(copy);
        return xstrdup("bpp_nil()");
    }
    if (strcmp(text, "[]") == 0) {
        free(copy);
        return xstrdup("bpp_list()");
    }
    if (len >= 2 && text[0] == '[' && text[len - 1] == ']') {
        char *inner = slice_trim(text, 1, len - 1);
        char **args = NULL;
        size_t arg_count = 0;
        split_top_level_args(inner, &args, &arg_count);
        StringBuilder call;
        sb_init(&call);
        sb_appendf(&call, "bpp_list_of(%zu", arg_count);
        for (size_t i = 0; i < arg_count; i++) {
            char *arg_c = transpile_expr(args[i]);
            sb_appendf(&call, ", %s", arg_c);
            free(arg_c);
        }
        sb_append(&call, ")");
        char *out = xstrdup(call.data);
        sb_free(&call);
        free_split_args(args, arg_count);
        free(inner);
        free(copy);
        return out;
    }
    if (len >= 2 && ((text[0] == '"' && text[len - 1] == '"') || (text[0] == '\'' && text[len - 1] == '\''))) {
        char *literal = to_c_string_literal(text);
        char *out = format_text("bpp_string(%s)", literal);
        free(literal);
        free(copy);
        return out;
    }
    if (len >= 3 && text[len - 1] == ')') {
        char *open = strchr(text, '(');
        if (open) {
            char *name = slice_trim(text, 0, (size_t)(open - text));
            if (is_identifier(name)) {
                char *inner = slice_trim(text, (size_t)(open - text) + 1, len - 1);
                char **args = NULL;
                size_t arg_count = 0;
                split_top_level_args(inner, &args, &arg_count);
                StringBuilder call;
                sb_init(&call);
                sb_appendf(&call, "%s(", name);
                for (size_t i = 0; i < arg_count; i++) {
                    char *arg_c = transpile_expr(args[i]);
                    if (i > 0) {
                        sb_append(&call, ", ");
                    }
                    sb_append(&call, arg_c);
                    free(arg_c);
                }
                sb_append(&call, ")");
                char *out = xstrdup(call.data);
                sb_free(&call);
                free_split_args(args, arg_count);
                free(inner);
                free(name);
                free(copy);
                return out;
            }
            free(name);
        }
    }
    if (is_number_literal(text)) {
        char *out = format_text("bpp_number(%s)", text);
        free(copy);
        return out;
    }
    if (is_identifier(text)) {
        char *out = xstrdup(text);
        free(copy);
        return out;
    }

    char *out = format_text("bpp_string(\"<unsupported expression: %s>\")", text);
    free(copy);
    return out;
}

static void compiler_init(Compiler *compiler, const char *source_path) {
    compiler->source_path = source_path;
    sb_init(&compiler->main_body);
    sb_init(&compiler->functions_body);
    compiler->body = &compiler->main_body;
    names_init(&compiler->globals);
    names_init(&compiler->locals);
    compiler->names = &compiler->globals;
    compiler->indent = 1;
    compiler->temp_counter = 0;
    compiler->had_error = 0;
    compiler->in_function = 0;
    compiler->block_count = 0;
}

static void compiler_free(Compiler *compiler) {
    sb_free(&compiler->main_body);
    sb_free(&compiler->functions_body);
    names_free(&compiler->globals);
    names_free(&compiler->locals);
}

static void emit_indent(Compiler *compiler) {
    for (int i = 0; i < compiler->indent; i++) {
        sb_append(compiler->body, "    ");
    }
}

static void emit_line(Compiler *compiler, const char *fmt, ...) {
    emit_indent(compiler);
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed >= 0) {
        char *buf = (char *)xmalloc((size_t)needed + 1);
        vsnprintf(buf, (size_t)needed + 1, fmt, args);
        sb_append(compiler->body, buf);
        free(buf);
    }
    va_end(args);
    sb_append(compiler->body, "\n");
}

static void compile_error(Compiler *compiler, int line_no, const char *line, const char *message, const char *expected) {
    fprintf(stderr, "[Line %d] %s\n", line_no, message);
    fprintf(stderr, "    --> %s\n", line);
    if (expected) {
        fprintf(stderr, "    expected: %s\n", expected);
    }
    compiler->had_error = 1;
}

static void push_block(Compiler *compiler, BlockKind kind) {
    if (compiler->block_count < (int)(sizeof(compiler->blocks) / sizeof(compiler->blocks[0]))) {
        compiler->blocks[compiler->block_count++] = kind;
    }
}

static BlockKind pop_block(Compiler *compiler) {
    if (compiler->block_count <= 0) {
        return BLOCK_NORMAL;
    }
    return compiler->blocks[--compiler->block_count];
}

static void declare_or_set(Compiler *compiler, const char *name, const char *expr_c) {
    if (names_add(compiler->names, name)) {
        emit_line(compiler, "BppValue %s = %s;", name, expr_c);
    } else {
        emit_line(compiler, "%s = %s;", name, expr_c);
    }
}

static char *without_trailing_colon(const char *line) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == ':') {
        return slice_trim(line, 0, len - 1);
    }
    return xstrdup(line);
}

static void compile_statement(Compiler *compiler, int line_no, const char *line) {
    if (strcmp(line, "end") == 0) {
        if (compiler->block_count <= 0) {
            compile_error(compiler, line_no, line, "Unexpected end.", NULL);
            return;
        }
        BlockKind kind = pop_block(compiler);
        if (kind == BLOCK_FUNCTION) {
            emit_line(compiler, "return bpp_nil();");
        }
        compiler->indent--;
        emit_line(compiler, "}");
        if (kind == BLOCK_FUNCTION) {
            compiler->body = &compiler->main_body;
            compiler->names = &compiler->globals;
            compiler->in_function = 0;
            compiler->indent = 1;
            names_free(&compiler->locals);
            names_init(&compiler->locals);
        }
        return;
    }

    if (starts_with(line, "elif ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *cond = slice_trim(inner, 5, strlen(inner));
        char *expr_c = transpile_expr(cond);
        compiler->indent--;
        emit_line(compiler, "} else if (bpp_truthy(%s)) {", expr_c);
        compiler->indent++;
        free(inner);
        free(cond);
        free(expr_c);
        return;
    }

    if (strcmp(line, "else:") == 0) {
        compiler->indent--;
        emit_line(compiler, "} else {");
        compiler->indent++;
        return;
    }

    if (starts_with(line, "def ") && ends_with(line, ":")) {
        if (compiler->in_function) {
            compile_error(compiler, line_no, line, "Nested functions are not supported in native B++ yet.", NULL);
            return;
        }
        if (compiler->indent != 1 || compiler->block_count != 0) {
            compile_error(compiler, line_no, line, "Functions must be defined at the top level in native B++.", NULL);
            return;
        }
        char *inner = without_trailing_colon(line);
        char *signature = slice_trim(inner, 4, strlen(inner));
        char *open = strchr(signature, '(');
        char *close = strrchr(signature, ')');
        if (!open || !close || close < open) {
            compile_error(compiler, line_no, line, "Invalid function syntax.", "def name(arg, arg):");
            free(inner);
            free(signature);
            return;
        }
        char *name = slice_trim(signature, 0, (size_t)(open - signature));
        char *params = slice_trim(signature, (size_t)(open - signature) + 1, (size_t)(close - signature));
        if (!is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid function name.", "def name(arg, arg):");
            free(inner);
            free(signature);
            free(name);
            free(params);
            return;
        }

        compiler->body = &compiler->functions_body;
        compiler->names = &compiler->locals;
        compiler->in_function = 1;
        compiler->indent = 0;
        names_free(&compiler->locals);
        names_init(&compiler->locals);

        char **args = NULL;
        size_t arg_count = 0;
        split_top_level_args(params, &args, &arg_count);
        sb_appendf(&compiler->functions_body, "static BppValue %s(", name);
        for (size_t i = 0; i < arg_count; i++) {
            if (!is_identifier(args[i])) {
                compile_error(compiler, line_no, line, "Invalid function parameter.", "def name(arg, arg):");
            }
            if (i > 0) {
                sb_append(&compiler->functions_body, ", ");
            }
            sb_appendf(&compiler->functions_body, "BppValue %s", args[i]);
            names_add(&compiler->locals, args[i]);
        }
        sb_append(&compiler->functions_body, ") {\n");
        compiler->indent = 1;
        push_block(compiler, BLOCK_FUNCTION);
        free_split_args(args, arg_count);
        free(inner);
        free(signature);
        free(name);
        free(params);
        return;
    }

    if (starts_with(line, "return")) {
        if (!compiler->in_function) {
            compile_error(compiler, line_no, line, "return can only be used inside a function.", NULL);
            return;
        }
        char *body = slice_trim(line, 6, strlen(line));
        if (body[0]) {
            char *expr_c = transpile_expr(body);
            emit_line(compiler, "return %s;", expr_c);
            free(expr_c);
        } else {
            emit_line(compiler, "return bpp_nil();");
        }
        free(body);
        return;
    }

    if (starts_with(line, "say ")) {
        char *body = slice_trim(line, 4, strlen(line));
        char *expr = NULL;
        char *mode = NULL;
        if (partition_keyword(body, " without newline", &expr, &mode) && mode[0] == '\0') {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "bpp_print(%s);", expr_c);
            free(expr);
            free(mode);
            free(expr_c);
        } else {
            char *expr_c = transpile_expr(body);
            emit_line(compiler, "bpp_println(%s);", expr_c);
            free(expr_c);
            free(expr);
            free(mode);
        }
        free(body);
        return;
    }

    if (starts_with(line, "ask ")) {
        char *body = slice_trim(line, 4, strlen(line));
        char *prompt = NULL;
        char *name = NULL;
        if (!partition_keyword(body, " into ", &prompt, &name) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid ask syntax.", "ask prompt into name");
        } else {
            char *prompt_c = transpile_expr(prompt);
            char *expr_c = format_text("bpp_input(%s)", prompt_c);
            declare_or_set(compiler, name, expr_c);
            free(prompt_c);
            free(expr_c);
        }
        free(body);
        free(prompt);
        free(name);
        return;
    }

    if (starts_with(line, "write ")) {
        char *body = slice_trim(line, 6, strlen(line));
        char *value = NULL;
        char *filename = NULL;
        if (!partition_keyword(body, " to ", &value, &filename)) {
            compile_error(compiler, line_no, line, "Invalid write syntax.", "write value to filename");
        } else {
            char *value_c = transpile_expr(value);
            char *filename_c = transpile_expr(filename);
            emit_line(compiler, "bpp_write_file(%s, %s);", filename_c, value_c);
            free(value_c);
            free(filename_c);
        }
        free(body);
        free(value);
        free(filename);
        return;
    }

    if (starts_with(line, "read ")) {
        char *body = slice_trim(line, 5, strlen(line));
        char *filename = NULL;
        char *name = NULL;
        if (!partition_keyword(body, " into ", &filename, &name) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid read syntax.", "read filename into name");
        } else {
            char *filename_c = transpile_expr(filename);
            char *expr_c = format_text("bpp_read_file(%s)", filename_c);
            declare_or_set(compiler, name, expr_c);
            free(filename_c);
            free(expr_c);
        }
        free(body);
        free(filename);
        free(name);
        return;
    }

    if (starts_with(line, "import ")) {
        compile_error(compiler, line_no, line, "Native B++ does not support foreign-language imports.", "use B++ built-ins or native modules");
        return;
    }

    if (starts_with(line, "set ")) {
        char *body = slice_trim(line, 4, strlen(line));
        char *name = NULL;
        char *expr = NULL;
        if (!partition_keyword(body, " to ", &name, &expr) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid set syntax.", "set name to value");
        } else {
            char *expr_c = transpile_expr(expr);
            declare_or_set(compiler, name, expr_c);
            free(expr_c);
        }
        free(body);
        free(name);
        free(expr);
        return;
    }

    if (starts_with(line, "add ")) {
        char *body = slice_trim(line, 4, strlen(line));
        char *expr = NULL;
        char *name = NULL;
        if (!partition_keyword(body, " to ", &expr, &name) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid add syntax.", "add value to name");
        } else {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "%s = bpp_add(%s, %s);", name, name, expr_c);
            free(expr_c);
        }
        free(body);
        free(expr);
        free(name);
        return;
    }

    if (starts_with(line, "subtract ")) {
        char *body = slice_trim(line, 9, strlen(line));
        char *expr = NULL;
        char *name = NULL;
        if (!partition_keyword(body, " from ", &expr, &name) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid subtract syntax.", "subtract value from name");
        } else {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "%s = bpp_sub(%s, %s);", name, name, expr_c);
            free(expr_c);
        }
        free(body);
        free(expr);
        free(name);
        return;
    }

    if (starts_with(line, "multiply ")) {
        char *body = slice_trim(line, 9, strlen(line));
        char *name = NULL;
        char *expr = NULL;
        if (!partition_keyword(body, " by ", &name, &expr) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid multiply syntax.", "multiply name by value");
        } else {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "%s = bpp_mul(%s, %s);", name, name, expr_c);
            free(expr_c);
        }
        free(body);
        free(name);
        free(expr);
        return;
    }

    if (starts_with(line, "divide ")) {
        char *body = slice_trim(line, 7, strlen(line));
        char *name = NULL;
        char *expr = NULL;
        if (!partition_keyword(body, " by ", &name, &expr) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid divide syntax.", "divide name by value");
        } else {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "%s = bpp_div(%s, %s);", name, name, expr_c);
            free(expr_c);
        }
        free(body);
        free(name);
        free(expr);
        return;
    }

    if (starts_with(line, "put ")) {
        char *body = slice_trim(line, 4, strlen(line));
        char *expr = NULL;
        char *name = NULL;
        if (!partition_keyword(body, " in ", &expr, &name) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid put syntax.", "put value in list");
        } else {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "bpp_list_append(&%s, %s);", name, expr_c);
            free(expr_c);
        }
        free(body);
        free(expr);
        free(name);
        return;
    }

    if (starts_with(line, "remove ")) {
        char *body = slice_trim(line, 7, strlen(line));
        char *expr = NULL;
        char *name = NULL;
        if (!partition_keyword(body, " from ", &expr, &name) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid remove syntax.", "remove value from list");
        } else {
            char *expr_c = transpile_expr(expr);
            emit_line(compiler, "bpp_list_remove(&%s, %s);", name, expr_c);
            free(expr_c);
        }
        free(body);
        free(expr);
        free(name);
        return;
    }

    if (starts_with(line, "empty ")) {
        char *name = slice_trim(line, 6, strlen(line));
        if (!is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid empty syntax.", "empty list");
        } else {
            emit_line(compiler, "bpp_list_empty(&%s);", name);
        }
        free(name);
        return;
    }

    if (starts_with(line, "if ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *cond = slice_trim(inner, 3, strlen(inner));
        char *expr_c = transpile_expr(cond);
        emit_line(compiler, "if (bpp_truthy(%s)) {", expr_c);
        compiler->indent++;
        push_block(compiler, BLOCK_NORMAL);
        free(inner);
        free(cond);
        free(expr_c);
        return;
    }

    if (starts_with(line, "while ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *cond = slice_trim(inner, 6, strlen(inner));
        char *expr_c = transpile_expr(cond);
        emit_line(compiler, "while (bpp_truthy(%s)) {", expr_c);
        compiler->indent++;
        push_block(compiler, BLOCK_NORMAL);
        free(inner);
        free(cond);
        free(expr_c);
        return;
    }

    if (strcmp(line, "forever:") == 0) {
        emit_line(compiler, "while (1) {");
        compiler->indent++;
        push_block(compiler, BLOCK_NORMAL);
        return;
    }

    if (starts_with(line, "repeat ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *body = slice_trim(inner, 7, strlen(inner));
        char *count = NULL;
        char *name = NULL;
        int temp = ++compiler->temp_counter;
        if (partition_keyword(body, " times as ", &count, &name) && is_identifier(name)) {
            char *count_c = transpile_expr(count);
            emit_line(compiler, "for (long __bpp_i_%d = 1, __bpp_limit_%d = (long)bpp_to_number(%s); __bpp_i_%d <= __bpp_limit_%d; __bpp_i_%d++) {", temp, temp, count_c, temp, temp, temp);
            compiler->indent++;
            push_block(compiler, BLOCK_NORMAL);
            emit_line(compiler, "BppValue %s = bpp_number((double)__bpp_i_%d);", name, temp);
            free(count_c);
        } else {
            free(count);
            free(name);
            count = NULL;
            name = NULL;
            if (!partition_keyword(body, " times", &count, &name) || (name && name[0] != '\0')) {
                compile_error(compiler, line_no, line, "Invalid repeat syntax.", "repeat count times:");
            } else {
                char *count_c = transpile_expr(count);
                emit_line(compiler, "for (long __bpp_i_%d = 0, __bpp_limit_%d = (long)bpp_to_number(%s); __bpp_i_%d < __bpp_limit_%d; __bpp_i_%d++) {", temp, temp, count_c, temp, temp, temp);
                compiler->indent++;
                push_block(compiler, BLOCK_NORMAL);
                free(count_c);
            }
        }
        free(inner);
        free(body);
        free(count);
        free(name);
        return;
    }

    if (starts_with(line, "for each ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *body = slice_trim(inner, 9, strlen(inner));
        char *name = NULL;
        char *list_expr = NULL;
        int temp = ++compiler->temp_counter;
        if (!partition_keyword(body, " in ", &name, &list_expr) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid for each syntax.", "for each name in list:");
        } else {
            char *list_c = transpile_expr(list_expr);
            emit_line(compiler, "BppValue __bpp_list_%d = %s;", temp, list_c);
            emit_line(compiler, "for (size_t __bpp_i_%d = 0; __bpp_i_%d < bpp_list_len(__bpp_list_%d); __bpp_i_%d++) {", temp, temp, temp, temp);
            compiler->indent++;
            push_block(compiler, BLOCK_NORMAL);
            emit_line(compiler, "BppValue %s = bpp_list_get(__bpp_list_%d, __bpp_i_%d);", name, temp, temp);
            free(list_c);
        }
        free(inner);
        free(body);
        free(name);
        free(list_expr);
        return;
    }

    if (strcmp(line, "stop loop") == 0) {
        emit_line(compiler, "break;");
        return;
    }

    if (strcmp(line, "next loop") == 0) {
        emit_line(compiler, "continue;");
        return;
    }

    compile_error(compiler, line_no, line, "Unsupported B++ syntax in native mode.", NULL);
}

static const char *runtime_c =
"#include <math.h>\n"
"#include <stdarg.h>\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"\n"
"typedef enum { BPP_NIL, BPP_NUMBER, BPP_STRING, BPP_BOOL, BPP_LIST } BppType;\n"
"typedef struct BppValue BppValue;\n"
"typedef struct { size_t count; size_t cap; BppValue *items; } BppList;\n"
"struct BppValue { BppType type; double number; char *string; int boolean; BppList *list; };\n"
"\n"
"static char *bpp_strdup(const char *text) { size_t n = strlen(text); char *p = (char *)malloc(n + 1); if (!p) exit(2); memcpy(p, text, n + 1); return p; }\n"
"static BppValue bpp_nil(void) { BppValue v; v.type = BPP_NIL; v.number = 0; v.string = NULL; v.boolean = 0; v.list = NULL; return v; }\n"
"static BppValue bpp_number(double n) { BppValue v = bpp_nil(); v.type = BPP_NUMBER; v.number = n; return v; }\n"
"static BppValue bpp_string(const char *s) { BppValue v = bpp_nil(); v.type = BPP_STRING; v.string = bpp_strdup(s); return v; }\n"
"static BppValue bpp_bool(int b) { BppValue v = bpp_nil(); v.type = BPP_BOOL; v.boolean = !!b; return v; }\n"
"static BppValue bpp_list(void) { BppValue v = bpp_nil(); v.type = BPP_LIST; v.list = (BppList *)calloc(1, sizeof(BppList)); if (!v.list) exit(2); return v; }\n"
"static void bpp_list_append(BppValue *list, BppValue item);\n"
"static BppValue bpp_list_of(size_t count, ...) { BppValue list = bpp_list(); va_list args; va_start(args, count); for (size_t i = 0; i < count; i++) { bpp_list_append(&list, va_arg(args, BppValue)); } va_end(args); return list; }\n"
"static double bpp_to_number(BppValue v) { if (v.type == BPP_NUMBER) return v.number; if (v.type == BPP_BOOL) return v.boolean ? 1.0 : 0.0; return 0.0; }\n"
"static int bpp_truthy(BppValue v) { if (v.type == BPP_NIL) return 0; if (v.type == BPP_BOOL) return v.boolean; if (v.type == BPP_NUMBER) return v.number != 0; if (v.type == BPP_STRING) return v.string && v.string[0]; if (v.type == BPP_LIST) return v.list && v.list->count > 0; return 0; }\n"
"static char *bpp_to_string(BppValue v) { char buf[128]; if (v.type == BPP_STRING) return bpp_strdup(v.string ? v.string : \"\"); if (v.type == BPP_BOOL) return bpp_strdup(v.boolean ? \"true\" : \"false\"); if (v.type == BPP_NIL) return bpp_strdup(\"nothing\"); if (v.type == BPP_LIST) { snprintf(buf, sizeof(buf), \"[list:%zu]\", v.list ? v.list->count : 0); return bpp_strdup(buf); } if (fabs(v.number - (long long)v.number) < 0.0000001) snprintf(buf, sizeof(buf), \"%lld\", (long long)v.number); else snprintf(buf, sizeof(buf), \"%g\", v.number); return bpp_strdup(buf); }\n"
"static void bpp_print(BppValue v) { char *s = bpp_to_string(v); fputs(s, stdout); free(s); }\n"
"static void bpp_println(BppValue v) { bpp_print(v); fputc('\\n', stdout); }\n"
"static BppValue bpp_input(BppValue prompt) { bpp_print(prompt); fputc(' ', stdout); fflush(stdout); char buf[4096]; if (!fgets(buf, sizeof(buf), stdin)) return bpp_string(\"\"); size_t n = strlen(buf); while (n > 0 && (buf[n - 1] == '\\n' || buf[n - 1] == '\\r')) buf[--n] = 0; return bpp_string(buf); }\n"
"static BppValue bpp_read_file(BppValue filename) { char *path = bpp_to_string(filename); FILE *f = fopen(path, \"rb\"); free(path); if (!f) return bpp_string(\"\"); fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f); if (size < 0) { fclose(f); return bpp_string(\"\"); } char *buf = (char *)malloc((size_t)size + 1); if (!buf) exit(2); size_t n = fread(buf, 1, (size_t)size, f); buf[n] = 0; fclose(f); BppValue out = bpp_string(buf); free(buf); return out; }\n"
"static void bpp_write_file(BppValue filename, BppValue value) { char *path = bpp_to_string(filename); char *text = bpp_to_string(value); FILE *f = fopen(path, \"wb\"); if (f) { fwrite(text, 1, strlen(text), f); fclose(f); } free(path); free(text); }\n"
"static BppValue bpp_add(BppValue a, BppValue b) { if (a.type == BPP_STRING || b.type == BPP_STRING) { char *sa = bpp_to_string(a); char *sb = bpp_to_string(b); char *out = (char *)malloc(strlen(sa) + strlen(sb) + 1); if (!out) exit(2); strcpy(out, sa); strcat(out, sb); free(sa); free(sb); BppValue v = bpp_string(out); free(out); return v; } return bpp_number(bpp_to_number(a) + bpp_to_number(b)); }\n"
"static BppValue bpp_sub(BppValue a, BppValue b) { return bpp_number(bpp_to_number(a) - bpp_to_number(b)); }\n"
"static BppValue bpp_mul(BppValue a, BppValue b) { return bpp_number(bpp_to_number(a) * bpp_to_number(b)); }\n"
"static BppValue bpp_div(BppValue a, BppValue b) { double d = bpp_to_number(b); return bpp_number(d == 0 ? 0 : bpp_to_number(a) / d); }\n"
"static BppValue bpp_eq(BppValue a, BppValue b) { if (a.type == BPP_STRING || b.type == BPP_STRING) { char *sa = bpp_to_string(a); char *sb = bpp_to_string(b); int ok = strcmp(sa, sb) == 0; free(sa); free(sb); return bpp_bool(ok); } return bpp_bool(bpp_to_number(a) == bpp_to_number(b)); }\n"
"static BppValue bpp_ne(BppValue a, BppValue b) { return bpp_bool(!bpp_truthy(bpp_eq(a, b))); }\n"
"static BppValue bpp_gt(BppValue a, BppValue b) { return bpp_bool(bpp_to_number(a) > bpp_to_number(b)); }\n"
"static BppValue bpp_lt(BppValue a, BppValue b) { return bpp_bool(bpp_to_number(a) < bpp_to_number(b)); }\n"
"static BppValue bpp_ge(BppValue a, BppValue b) { return bpp_bool(bpp_to_number(a) >= bpp_to_number(b)); }\n"
"static BppValue bpp_le(BppValue a, BppValue b) { return bpp_bool(bpp_to_number(a) <= bpp_to_number(b)); }\n"
"static BppValue bpp_and(BppValue a, BppValue b) { return bpp_bool(bpp_truthy(a) && bpp_truthy(b)); }\n"
"static BppValue bpp_or(BppValue a, BppValue b) { return bpp_bool(bpp_truthy(a) || bpp_truthy(b)); }\n"
"static void bpp_list_append(BppValue *list, BppValue item) { if (list->type != BPP_LIST) *list = bpp_list(); if (list->list->count == list->list->cap) { list->list->cap = list->list->cap ? list->list->cap * 2 : 8; list->list->items = (BppValue *)realloc(list->list->items, list->list->cap * sizeof(BppValue)); if (!list->list->items) exit(2); } list->list->items[list->list->count++] = item; }\n"
"static size_t bpp_list_len(BppValue list) { return list.type == BPP_LIST && list.list ? list.list->count : 0; }\n"
"static BppValue bpp_list_get(BppValue list, size_t index) { if (list.type != BPP_LIST || !list.list || index >= list.list->count) return bpp_nil(); return list.list->items[index]; }\n"
"static void bpp_list_empty(BppValue *list) { if (list->type == BPP_LIST && list->list) list->list->count = 0; }\n"
"static void bpp_list_remove(BppValue *list, BppValue item) { if (list->type != BPP_LIST || !list->list) return; for (size_t i = 0; i < list->list->count; i++) { if (bpp_truthy(bpp_eq(list->list->items[i], item))) { memmove(&list->list->items[i], &list->list->items[i + 1], (list->list->count - i - 1) * sizeof(BppValue)); list->list->count--; return; } } }\n"
"\n";

static int compile_source_to_c(const char *source_path, const char *src, StringBuilder *out) {
    Compiler compiler;
    compiler_init(&compiler, source_path);

    char *work = xstrdup(src);
    int line_no = 0;
    char *line = strtok(work, "\n");
    while (line) {
        line_no++;
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }
        char *without_comment = strip_comment_copy(line);
        char *trimmed = trim_in_place(without_comment);
        if (trimmed[0]) {
            compile_statement(&compiler, line_no, trimmed);
        }
        free(without_comment);
        line = strtok(NULL, "\n");
    }

    if (!compiler.had_error && compiler.indent != 1) {
        fprintf(stderr, "Missing end.\n");
        compiler.had_error = 1;
    }

    if (!compiler.had_error) {
        sb_append(out, "/* Generated by native B++. */\n");
        if (source_path) {
            sb_appendf(out, "/* Source: %s */\n\n", source_path);
        }
        sb_append(out, runtime_c);
        sb_append(out, compiler.functions_body.data);
        if (compiler.functions_body.len) {
            sb_append(out, "\n");
        }
        sb_append(out, "int main(void) {\n");
        sb_append(out, compiler.main_body.data);
        sb_append(out, "    return 0;\n");
        sb_append(out, "}\n");
    }

    int ok = !compiler.had_error;
    compiler_free(&compiler);
    free(work);
    return ok;
}

#ifdef _WIN32

typedef struct {
    char *tag;
    char *asset_url;
} UpdateInfo;

static void update_info_free(UpdateInfo *info) {
    free(info->tag);
    free(info->asset_url);
    info->tag = NULL;
    info->asset_url = NULL;
}

static int http_get_text(const char *url, char **out_text) {
    const char *headers = "User-Agent: B++ updater\r\nAccept: application/vnd.github+json\r\n";
    HINTERNET internet = InternetOpenA("B++ updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!internet) {
        return 0;
    }

    HINTERNET request = InternetOpenUrlA(
        internet,
        url,
        headers,
        (DWORD)-1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE,
        0
    );
    if (!request) {
        InternetCloseHandle(internet);
        return 0;
    }

    StringBuilder response;
    sb_init(&response);

    char buffer[8192];
    DWORD read = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read > 0) {
        sb_append_len(&response, buffer, read);
    }

    InternetCloseHandle(request);
    InternetCloseHandle(internet);
    *out_text = response.data;
    return 1;
}

static int http_download_file(const char *url, const char *path) {
    const char *headers = "User-Agent: B++ updater\r\n";
    HINTERNET internet = InternetOpenA("B++ updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!internet) {
        return 0;
    }

    HINTERNET request = InternetOpenUrlA(
        internet,
        url,
        headers,
        (DWORD)-1,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE,
        0
    );
    if (!request) {
        InternetCloseHandle(internet);
        return 0;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        InternetCloseHandle(request);
        InternetCloseHandle(internet);
        return 0;
    }

    int ok = 1;
    char buffer[8192];
    DWORD read = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read > 0) {
        if (fwrite(buffer, 1, read, file) != read) {
            ok = 0;
            break;
        }
    }

    fclose(file);
    InternetCloseHandle(request);
    InternetCloseHandle(internet);
    return ok;
}

static char *json_string_after(const char *start, const char *key) {
    const char *found = strstr(start, key);
    if (!found) {
        return NULL;
    }

    const char *colon = strchr(found, ':');
    if (!colon) {
        return NULL;
    }

    const char *quote = strchr(colon, '"');
    if (!quote) {
        return NULL;
    }
    quote++;

    StringBuilder out;
    sb_init(&out);
    for (const char *p = quote; *p; p++) {
        if (*p == '\\' && p[1]) {
            sb_append_len(&out, p + 1, 1);
            p++;
            continue;
        }
        if (*p == '"') {
            return out.data;
        }
        sb_append_len(&out, p, 1);
    }

    sb_free(&out);
    return NULL;
}

static char *json_release_asset_url(const char *json, const char *asset_name) {
    const char *p = json;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        char *name = json_string_after(p, "\"name\"");
        if (!name) {
            p += 6;
            continue;
        }

        int matches = strcmp(name, asset_name) == 0;
        free(name);
        if (matches) {
            const char *url_key = strstr(p, "\"browser_download_url\"");
            const char *next_name = strstr(p + 6, "\"name\"");
            if (url_key && (!next_name || url_key < next_name)) {
                return json_string_after(url_key, "\"browser_download_url\"");
            }
        }
        p += 6;
    }

    return NULL;
}

static int next_version_part(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    while (*p && !isdigit(*p)) {
        p++;
    }
    if (!*p) {
        *text = (const char *)p;
        return -1;
    }

    int value = 0;
    while (*p && isdigit(*p)) {
        value = value * 10 + (*p - '0');
        p++;
    }
    *text = (const char *)p;
    return value;
}

static int compare_versions(const char *left, const char *right) {
    const char *a = left;
    const char *b = right;
    for (int i = 0; i < 8; i++) {
        int av = next_version_part(&a);
        int bv = next_version_part(&b);
        if (av < 0 && bv < 0) {
            return 0;
        }
        if (av < 0) {
            av = 0;
        }
        if (bv < 0) {
            bv = 0;
        }
        if (av != bv) {
            return av > bv ? 1 : -1;
        }
    }
    return 0;
}

static int fetch_update_info(UpdateInfo *info) {
    info->tag = NULL;
    info->asset_url = NULL;

    char *json = NULL;
    if (!http_get_text(BPP_UPDATE_API, &json)) {
        return 0;
    }

    info->tag = json_string_after(json, "\"tag_name\"");
    info->asset_url = json_release_asset_url(json, "bpp.exe");
    free(json);

    if (!info->tag || !info->asset_url) {
        update_info_free(info);
        return 0;
    }

    return 1;
}

static void bpp_appdata_dir(char *out, size_t out_size) {
    DWORD got = GetEnvironmentVariableA("LOCALAPPDATA", out, (DWORD)out_size);
    if (got == 0 || got >= out_size) {
        GetEnvironmentVariableA("USERPROFILE", out, (DWORD)out_size);
        strncat(out, "\\AppData\\Local", out_size - strlen(out) - 1);
    }
    strncat(out, "\\Bpp", out_size - strlen(out) - 1);
}

static void update_config_path(char *out, size_t out_size) {
    bpp_appdata_dir(out, out_size);
    CreateDirectoryA(out, NULL);
    strncat(out, "\\updates.cfg", out_size - strlen(out) - 1);
}

static void read_update_config(int *auto_enabled, long long *last_check) {
    char path[MAX_PATH];
    update_config_path(path, sizeof(path));
    *auto_enabled = 1;
    *last_check = 0;

    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return;
    }
    rewind(file);

    char *text = (char *)xmalloc((size_t)size + 1);
    size_t read = fread(text, 1, (size_t)size, file);
    text[read] = '\0';
    fclose(file);

    char *auto_line = strstr(text, "auto=");
    if (auto_line) {
        *auto_enabled = atoi(auto_line + 5) != 0;
    }

    char *last_line = strstr(text, "last_check=");
    if (last_line) {
        *last_check = _atoi64(last_line + 11);
    }

    free(text);
}

static void write_update_config(int auto_enabled, long long last_check) {
    char path[MAX_PATH];
    update_config_path(path, sizeof(path));

    FILE *file = fopen(path, "w");
    if (!file) {
        return;
    }
    fprintf(file, "auto=%d\nlast_check=%lld\n", auto_enabled ? 1 : 0, last_check);
    fclose(file);
}

static int command_updates(void) {
    int auto_enabled = 1;
    long long last_check = 0;
    read_update_config(&auto_enabled, &last_check);

    printf("B++ updates\n");
    printf("source: https://github.com/%s\n", BPP_UPDATE_REPO);
    printf("asset: bpp.exe\n");
    printf("auto checks: %s\n", auto_enabled ? "on" : "off");
    if (last_check > 0) {
        printf("last check: %lld\n", last_check);
    } else {
        printf("last check: never\n");
    }
    return 0;
}

static int command_set_auto_updates(int enabled) {
    int current_auto = 1;
    long long last_check = 0;
    read_update_config(&current_auto, &last_check);
    write_update_config(enabled, last_check);
    printf("B++ automatic update checks are %s.\n", enabled ? "on" : "off");
    return 0;
}

static int command_check_update(void) {
    UpdateInfo info;
    if (!fetch_update_info(&info)) {
        fprintf(stderr, "bpp: could not check GitHub Releases for updates.\n");
        return 1;
    }

    int newer = compare_versions(info.tag, BPP_VERSION) > 0;
    if (newer) {
        printf("B++ update available: %s (installed %s)\n", info.tag, BPP_VERSION);
        printf("Run: bpp update\n");
    } else {
        printf("B++ is up to date: %s\n", BPP_VERSION);
    }

    update_info_free(&info);
    return 0;
}

static int launch_update_script(const char *downloaded_exe, const char *target_exe, const char *tag) {
    char temp_dir[MAX_PATH];
    char script_path[MAX_PATH];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    snprintf(script_path, sizeof(script_path), "%sbpp_apply_update.cmd", temp_dir);

    FILE *script = fopen(script_path, "w");
    if (!script) {
        return 0;
    }

    fprintf(script, "@echo off\r\n");
    fprintf(script, "timeout /t 2 /nobreak >nul\r\n");
    fprintf(script, "copy /Y \"%s\" \"%s\" >nul\r\n", downloaded_exe, target_exe);
    fprintf(script, "if errorlevel 1 (\r\n");
    fprintf(script, "  echo B++ update failed.\r\n");
    fprintf(script, "  pause\r\n");
    fprintf(script, "  exit /b 1\r\n");
    fprintf(script, ")\r\n");
    fprintf(script, "del \"%s\" >nul 2>nul\r\n", downloaded_exe);
    fprintf(script, "echo B++ updated to %s.\r\n", tag);
    fprintf(script, "del \"%%~f0\" >nul 2>nul\r\n");
    fclose(script);

    char command_line[MAX_PATH * 2];
    snprintf(command_line, sizeof(command_line), "cmd.exe /C \"\"%s\"\"", script_path);

    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    if (!CreateProcessA(NULL, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        return 0;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

static int command_update(void) {
    UpdateInfo info;
    if (!fetch_update_info(&info)) {
        fprintf(stderr, "bpp: could not check GitHub Releases for updates.\n");
        return 1;
    }

    if (compare_versions(info.tag, BPP_VERSION) <= 0) {
        printf("B++ is already up to date: %s\n", BPP_VERSION);
        update_info_free(&info);
        return 0;
    }

    char temp_dir[MAX_PATH];
    char downloaded_exe[MAX_PATH];
    char current_exe[MAX_PATH];
    GetTempPathA(sizeof(temp_dir), temp_dir);
    snprintf(downloaded_exe, sizeof(downloaded_exe), "%sbpp_update.exe", temp_dir);
    GetModuleFileNameA(NULL, current_exe, sizeof(current_exe));

    printf("Downloading B++ %s...\n", info.tag);
    if (!http_download_file(info.asset_url, downloaded_exe)) {
        fprintf(stderr, "bpp: could not download bpp.exe from the latest release.\n");
        update_info_free(&info);
        return 1;
    }

    if (!launch_update_script(downloaded_exe, current_exe, info.tag)) {
        fprintf(stderr, "bpp: could not launch the updater helper.\n");
        DeleteFileA(downloaded_exe);
        update_info_free(&info);
        return 1;
    }

    printf("B++ will finish updating after this command exits.\n");
    update_info_free(&info);
    return 0;
}

static void maybe_auto_check_updates(void) {
    int auto_enabled = 1;
    long long last_check = 0;
    long long now = (long long)time(NULL);
    read_update_config(&auto_enabled, &last_check);

    if (!auto_enabled || now <= 0 || now - last_check < BPP_UPDATE_CHECK_SECONDS) {
        return;
    }

    write_update_config(auto_enabled, now);

    UpdateInfo info;
    if (!fetch_update_info(&info)) {
        return;
    }

    if (compare_versions(info.tag, BPP_VERSION) > 0) {
        fprintf(stderr, "B++ update available: %s. Run `bpp update`.\n", info.tag);
    }

    update_info_free(&info);
}

#else

static int command_updates(void) {
    printf("B++ updates\n");
    printf("source: https://github.com/%s\n", BPP_UPDATE_REPO);
    printf("native updater: not available in this build yet\n");
    return 0;
}

static int command_set_auto_updates(int enabled) {
    (void)enabled;
    fprintf(stderr, "bpp: automatic updates are not available in this build yet.\n");
    return 1;
}

static int command_check_update(void) {
    fprintf(stderr, "bpp: GitHub updates are not available in this build yet.\n");
    return 1;
}

static int command_update(void) {
    fprintf(stderr, "bpp: GitHub updates are not available in this build yet.\n");
    return 1;
}

static void maybe_auto_check_updates(void) {
}

#endif

static void print_help(void) {
    printf("B++ compiler %s\n", BPP_VERSION);
    printf("Usage: bpp <source.bpp> [-o output.c]\n");
    printf("\n");
    printf("Compiles B++ source to C source.\n");
    printf("\n");
    printf("Commands:\n");
    printf("  bpp check-update    check GitHub Releases for a newer B++\n");
    printf("  bpp update          install the newest bpp.exe release asset\n");
    printf("  bpp updates         show update settings\n");
    printf("  bpp --auto          enable automatic update checks\n");
    printf("  bpp --no-auto       disable automatic update checks\n");
}

int main(int argc, char **argv) {
    const char *source_path = NULL;
    const char *output_path = NULL;

    if (argc == 2) {
        if (strcmp(argv[1], "updates") == 0) {
            return command_updates();
        }
        if (strcmp(argv[1], "check-update") == 0) {
            return command_check_update();
        }
        if (strcmp(argv[1], "update") == 0) {
            return command_update();
        }
        if (strcmp(argv[1], "--auto") == 0) {
            return command_set_auto_updates(1);
        }
        if (strcmp(argv[1], "--no-auto") == 0) {
            return command_set_auto_updates(0);
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("B++ compiler %s\n", BPP_VERSION);
            return 0;
        }
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output_path = argv[++i];
            continue;
        }
        if (!source_path) {
            source_path = argv[i];
            continue;
        }
        fprintf(stderr, "bpp: unexpected argument: %s\n", argv[i]);
        return 2;
    }

    if (!source_path) {
        print_help();
        return 2;
    }

    maybe_auto_check_updates();

    char *src = read_file(source_path);
    if (!src) {
        return 1;
    }

    StringBuilder generated;
    sb_init(&generated);
    int ok = compile_source_to_c(source_path, src, &generated);
    free(src);
    if (!ok) {
        sb_free(&generated);
        return 1;
    }

    if (output_path) {
        if (!write_file(output_path, generated.data)) {
            sb_free(&generated);
            return 1;
        }
        printf("Compiled %s -> %s\n", source_path, output_path);
    } else {
        fputs(generated.data, stdout);
    }

    sb_free(&generated);
    return 0;
}
