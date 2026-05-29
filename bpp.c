/*
    bpp - native B++ compiler

    This is the native compiler path:

        B++ -> C source -> native executable

    Supported in this native compiler:
    - say
    - set and name = value
    - const
    - add/subtract/multiply/divide
    - lists: [], put/remove/empty
    - if/elif/elseif/else/end
    - unless
    - repeat, repeat as
    - for each, for in, range for
    - while, until, forever
    - stop loop, next loop, break, continue
    - true/false/nothing/nil
    - tables: { name = value } and table["name"]

    - def/function and return
    - ask/read/write
    - built-in os module with <bpp unpackage os>

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
    NameList global_consts;
    NameList local_consts;
    NameList *names;
    NameList *consts;
    int indent;
    int temp_counter;
    int had_error;
    int in_function;
    int os_enabled;
    int skip_depth;
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

static int string_in_list(const char *text, const char *const *items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(text, items[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_reserved_identifier(const char *name) {
    static const char *const reserved_words[] = {
        "add", "and", "as", "ask", "break", "case", "char", "const",
        "continue", "default", "def", "divide", "do", "double", "elif",
        "elseif", "else", "empty", "end", "enum", "extern", "false", "float",
        "for", "forever", "goto", "if", "import", "in", "int", "long",
        "loop", "multiply", "next", "nil", "not", "nothing", "os", "read",
        "register", "remove", "repeat", "return", "say", "set", "short",
        "signed", "sizeof", "static", "stop", "struct", "subtract",
        "switch", "times", "true", "typedef", "union", "unless",
        "unsigned", "until", "unpackage", "void", "volatile", "while",
        "write", "function",

        "BPP_BOOL", "BPP_LIST", "BPP_NIL", "BPP_NUMBER", "BPP_STRING", "BPP_TABLE",
        "BppList", "BppTable", "BppTableEntry", "BppType", "BppValue", "FILE", "NULL", "main",
        "size_t", "stdin", "stdout",

        "calloc", "exit", "fabs", "fclose", "fflush", "fgetc", "fgets",
        "fopen", "fprintf", "fputc", "fputs", "fread", "free", "fseek",
        "ftell", "fwrite", "malloc", "memcpy", "memmove", "printf",
        "realloc", "rewind", "snprintf", "strcat", "strcmp", "strcpy",
        "strlen"
    };

    if (string_in_list(name, reserved_words, sizeof(reserved_words) / sizeof(reserved_words[0]))) {
        return 1;
    }

    return starts_with(name, "__bpp_") ||
           starts_with(name, "bpp_") ||
           starts_with(name, "Bpp") ||
           starts_with(name, "BPP_");
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
        if ((ch == '#' || (ch == '-' && line[i + 1] == '-')) && !quote) {
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

static int partition_assignment(const char *text, char **left, char **right) {
    int quote = 0;
    int escaped = 0;
    int depth = 0;
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
        if (depth == 0 && ch == '=') {
            char prev = i > 0 ? text[i - 1] : '\0';
            char next = text[i + 1];
            if (prev == '=' || prev == '!' || prev == '<' || prev == '>' || next == '=') {
                continue;
            }
            *left = slice_trim(text, 0, i);
            *right = slice_trim(text, i + 1, strlen(text));
            return 1;
        }
    }
    return 0;
}

static int brace_delta(const char *text) {
    int quote = 0;
    int escaped = 0;
    int delta = 0;
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
        if (ch == '{') {
            delta++;
        } else if (ch == '}') {
            delta--;
        }
    }
    return delta;
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

static char *to_c_string_text(const char *text) {
    StringBuilder out;
    sb_init(&out);
    sb_append(&out, "\"");
    for (size_t i = 0; text[i]; i++) {
        char ch = text[i];
        if (ch == '"' || ch == '\\') {
            sb_append(&out, "\\");
        }
        if (ch == '\n') {
            sb_append(&out, "\\n");
        } else if (ch == '\r') {
            sb_append(&out, "\\r");
        } else if (ch == '\t') {
            sb_append(&out, "\\t");
        } else {
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

static int trailing_index_start(const char *text) {
    size_t len = strlen(text);
    if (len < 4 || text[len - 1] != ']') {
        return -1;
    }

    int quote = 0;
    int escaped = 0;
    int depth = 0;
    int start = -1;
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
        if (ch == '[') {
            if (depth == 0) {
                start = (int)i;
            }
            depth++;
        } else if (ch == ']') {
            depth--;
            if (depth == 0 && text[i + 1] == '\0' && start > 0) {
                return start;
            }
        }
    }
    return -1;
}

static char *transpile_table_key(const char *key) {
    size_t len = strlen(key);
    if (is_identifier(key)) {
        char *literal = to_c_string_text(key);
        char *out = format_text("bpp_string(%s)", literal);
        free(literal);
        return out;
    }
    if (len >= 2 && ((key[0] == '"' && key[len - 1] == '"') || (key[0] == '\'' && key[len - 1] == '\''))) {
        return transpile_expr(key);
    }
    if (len >= 2 && key[0] == '[' && key[len - 1] == ']') {
        char *inner = slice_trim(key, 1, len - 1);
        char *out = transpile_expr(inner);
        free(inner);
        return out;
    }
    return transpile_expr(key);
}

static char *transpile_table_literal(const char *text) {
    size_t len = strlen(text);
    char *inner = slice_trim(text, 1, len - 1);
    char **entries = NULL;
    size_t entry_count = 0;
    split_top_level_args(inner, &entries, &entry_count);

    StringBuilder call;
    sb_init(&call);
    sb_appendf(&call, "bpp_table_of(%zu", entry_count);
    for (size_t i = 0; i < entry_count; i++) {
        char *key = NULL;
        char *value = NULL;
        if (!partition_assignment(entries[i], &key, &value)) {
            char *bad_key = to_c_string_text("<invalid>");
            sb_appendf(&call, ", bpp_string(%s), bpp_nil()", bad_key);
            free(bad_key);
        } else {
            char *key_c = transpile_table_key(key);
            char *value_c = transpile_expr(value);
            sb_appendf(&call, ", %s, %s", key_c, value_c);
            free(key_c);
            free(value_c);
        }
        free(key);
        free(value);
    }
    sb_append(&call, ")");

    char *out = xstrdup(call.data);
    sb_free(&call);
    free_split_args(entries, entry_count);
    free(inner);
    return out;
}

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
        {"%", "bpp_mod"},
        {"^", "bpp_pow"},
    };

    for (size_t i = 0; i < sizeof(comparisons) / sizeof(comparisons[0]); i++) {
        char *out = transpile_binary(text, comparisons[i][0], comparisons[i][1]);
        if (out) {
            free(copy);
            return out;
        }
    }

    size_t len = strlen(text);
    if (starts_with(text, "not ")) {
        char *body = slice_trim(text, 4, len);
        char *body_c = transpile_expr(body);
        char *out = format_text("bpp_not(%s)", body_c);
        free(body);
        free(body_c);
        free(copy);
        return out;
    }
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
    if (strcmp(text, "{}") == 0) {
        free(copy);
        return xstrdup("bpp_table()");
    }
    if (len >= 2 && text[0] == '{' && text[len - 1] == '}') {
        char *out = transpile_table_literal(text);
        free(copy);
        return out;
    }
    int index_start = trailing_index_start(text);
    if (index_start > 0) {
        char *target = slice_trim(text, 0, (size_t)index_start);
        char *key = slice_trim(text, (size_t)index_start + 1, len - 1);
        char *target_c = transpile_expr(target);
        char *key_c = transpile_expr(key);
        char *out = format_text("bpp_index(%s, %s)", target_c, key_c);
        free(target);
        free(key);
        free(target_c);
        free(key_c);
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
    names_init(&compiler->global_consts);
    names_init(&compiler->local_consts);
    compiler->names = &compiler->globals;
    compiler->consts = &compiler->global_consts;
    compiler->indent = 1;
    compiler->temp_counter = 0;
    compiler->had_error = 0;
    compiler->in_function = 0;
    compiler->os_enabled = 0;
    compiler->skip_depth = 0;
    compiler->block_count = 0;
}

static void compiler_free(Compiler *compiler) {
    sb_free(&compiler->main_body);
    sb_free(&compiler->functions_body);
    names_free(&compiler->globals);
    names_free(&compiler->locals);
    names_free(&compiler->global_consts);
    names_free(&compiler->local_consts);
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

static void compile_reserved_name_error(Compiler *compiler, int line_no, const char *line, const char *name) {
    char *message = format_text("\"%s\" is reserved.", name);
    compile_error(compiler, line_no, line, message, "choose a different name");
    free(message);
}

static int check_name_allowed(Compiler *compiler, int line_no, const char *line, const char *name) {
    if (is_reserved_identifier(name)) {
        compile_reserved_name_error(compiler, line_no, line, name);
        return 0;
    }
    return 1;
}

static int is_const_name(Compiler *compiler, const char *name) {
    return names_contains(compiler->consts, name) ||
           (compiler->consts != &compiler->global_consts && names_contains(&compiler->global_consts, name));
}

static int check_name_writable(Compiler *compiler, int line_no, const char *line, const char *name) {
    if (is_const_name(compiler, name)) {
        compile_error(compiler, line_no, line, "Cannot change a const value.", "choose a different name or remove const");
        return 0;
    }
    return 1;
}

static void skip_bad_block(Compiler *compiler) {
    compiler->skip_depth = 1;
}

static int statement_starts_block(const char *line) {
    if (!ends_with(line, ":")) {
        return 0;
    }
    if (starts_with(line, "elif ") || starts_with(line, "elseif ") || strcmp(line, "else:") == 0) {
        return 0;
    }
    return starts_with(line, "def ") ||
           starts_with(line, "function ") ||
           starts_with(line, "if ") ||
           starts_with(line, "unless ") ||
           starts_with(line, "while ") ||
           starts_with(line, "until ") ||
           starts_with(line, "repeat ") ||
           starts_with(line, "for each ") ||
           starts_with(line, "for ") ||
           strcmp(line, "forever:") == 0;
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

static void declare_const(Compiler *compiler, const char *name, const char *expr_c) {
    names_add(compiler->names, name);
    names_add(compiler->consts, name);
    emit_line(compiler, "BppValue %s = %s;", name, expr_c);
}

static char *without_trailing_colon(const char *line) {
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == ':') {
        return slice_trim(line, 0, len - 1);
    }
    return xstrdup(line);
}

static void assign_result_to_name(Compiler *compiler, int line_no, const char *line, const char *name, const char *expr_c, const char *expected) {
    if (!is_identifier(name)) {
        compile_error(compiler, line_no, line, "Invalid target name.", expected);
    } else if (check_name_allowed(compiler, line_no, line, name) &&
               check_name_writable(compiler, line_no, line, name)) {
        declare_or_set(compiler, name, expr_c);
    }
}

static int compile_os_statement(Compiler *compiler, int line_no, const char *line) {
    if (!compiler->os_enabled) {
        compile_error(compiler, line_no, line, "The os module is not unpackaged.", "start the script with <bpp unpackage os>");
        return 1;
    }

    const char *body = line + 3;

    const struct {
        const char *prefix;
        const char *call;
        const char *expected;
    } no_arg_results[] = {
        {"current folder into ", "bpp_os_current_folder()", "os current folder into name"},
        {"home folder into ", "bpp_os_home_folder()", "os home folder into name"},
        {"temp folder into ", "bpp_os_temp_folder()", "os temp folder into name"},
        {"exit code into ", "bpp_os_exit_code()", "os exit code into name"},
        {"last error into ", "bpp_os_last_error()", "os last error into name"},
        {"process id into ", "bpp_os_process_id()", "os process id into name"}
    };

    for (size_t i = 0; i < sizeof(no_arg_results) / sizeof(no_arg_results[0]); i++) {
        if (starts_with(body, no_arg_results[i].prefix)) {
            char *name = slice_trim(body, strlen(no_arg_results[i].prefix), strlen(body));
            assign_result_to_name(compiler, line_no, line, name, no_arg_results[i].call, no_arg_results[i].expected);
            free(name);
            return 1;
        }
    }

    const struct {
        const char *prefix;
        const char *call;
        const char *expected;
    } expr_results[] = {
        {"file exists ", "bpp_os_file_exists", "os file exists path into name"},
        {"folder exists ", "bpp_os_folder_exists", "os folder exists path into name"},
        {"list folder ", "bpp_os_list_folder", "os list folder path into name"},
        {"env ", "bpp_os_env", "os env name into target"},
        {"run ", "bpp_os_run", "os run command into output"}
    };

    for (size_t i = 0; i < sizeof(expr_results) / sizeof(expr_results[0]); i++) {
        if (starts_with(body, expr_results[i].prefix)) {
            char *rest = slice_trim(body, strlen(expr_results[i].prefix), strlen(body));
            char *expr = NULL;
            char *name = NULL;
            if (!partition_keyword(rest, " into ", &expr, &name)) {
                compile_error(compiler, line_no, line, "Invalid os syntax.", expr_results[i].expected);
            } else {
                char *expr_c = transpile_expr(expr);
                char *call_c = format_text("%s(%s)", expr_results[i].call, expr_c);
                assign_result_to_name(compiler, line_no, line, name, call_c, expr_results[i].expected);
                free(expr_c);
                free(call_c);
            }
            free(rest);
            free(expr);
            free(name);
            return 1;
        }
    }

    if (starts_with(body, "create folder ")) {
        char *path = slice_trim(body, 14, strlen(body));
        if (!path[0]) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os create folder path");
        } else {
            char *path_c = transpile_expr(path);
            emit_line(compiler, "bpp_os_create_folder(%s);", path_c);
            free(path_c);
        }
        free(path);
        return 1;
    }

    if (starts_with(body, "copy file ")) {
        char *rest = slice_trim(body, 10, strlen(body));
        char *from = NULL;
        char *to = NULL;
        if (!partition_keyword(rest, " to ", &from, &to)) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os copy file from to target");
        } else {
            char *from_c = transpile_expr(from);
            char *to_c = transpile_expr(to);
            emit_line(compiler, "bpp_os_copy_file(%s, %s);", from_c, to_c);
            free(from_c);
            free(to_c);
        }
        free(rest);
        free(from);
        free(to);
        return 1;
    }

    if (starts_with(body, "move file ")) {
        char *rest = slice_trim(body, 10, strlen(body));
        char *from = NULL;
        char *to = NULL;
        if (!partition_keyword(rest, " to ", &from, &to)) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os move file from to target");
        } else {
            char *from_c = transpile_expr(from);
            char *to_c = transpile_expr(to);
            emit_line(compiler, "bpp_os_move_file(%s, %s);", from_c, to_c);
            free(from_c);
            free(to_c);
        }
        free(rest);
        free(from);
        free(to);
        return 1;
    }

    if (starts_with(body, "delete file ")) {
        char *path = slice_trim(body, 12, strlen(body));
        if (!path[0]) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os delete file path");
        } else {
            char *path_c = transpile_expr(path);
            emit_line(compiler, "bpp_os_delete_file(%s);", path_c);
            free(path_c);
        }
        free(path);
        return 1;
    }

    if (starts_with(body, "delete folder ")) {
        char *rest = slice_trim(body, 14, strlen(body));
        char *path = NULL;
        char *tail = NULL;
        if (!partition_keyword(rest, " recursively", &path, &tail) || tail[0] != '\0') {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os delete folder path recursively");
        } else {
            char *path_c = transpile_expr(path);
            emit_line(compiler, "bpp_os_delete_folder_recursive(%s);", path_c);
            free(path_c);
        }
        free(rest);
        free(path);
        free(tail);
        return 1;
    }

    if (starts_with(body, "set env ")) {
        char *rest = slice_trim(body, 8, strlen(body));
        char *name = NULL;
        char *value = NULL;
        if (!partition_keyword(rest, " to ", &name, &value)) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os set env name to value");
        } else {
            char *name_c = transpile_expr(name);
            char *value_c = transpile_expr(value);
            emit_line(compiler, "bpp_os_set_env(%s, %s);", name_c, value_c);
            free(name_c);
            free(value_c);
        }
        free(rest);
        free(name);
        free(value);
        return 1;
    }

    if (starts_with(body, "remove env ")) {
        char *name = slice_trim(body, 11, strlen(body));
        if (!name[0]) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os remove env name");
        } else {
            char *name_c = transpile_expr(name);
            emit_line(compiler, "bpp_os_remove_env(%s);", name_c);
            free(name_c);
        }
        free(name);
        return 1;
    }

    if (starts_with(body, "kill process ")) {
        char *pid = slice_trim(body, 13, strlen(body));
        if (!pid[0]) {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os kill process pid");
        } else {
            char *pid_c = transpile_expr(pid);
            emit_line(compiler, "bpp_os_kill_process(%s);", pid_c);
            free(pid_c);
        }
        free(pid);
        return 1;
    }

    if (starts_with(body, "sleep ")) {
        char *rest = slice_trim(body, 6, strlen(body));
        char *amount = NULL;
        char *tail = NULL;
        if (!partition_keyword(rest, " milliseconds", &amount, &tail) || tail[0] != '\0') {
            compile_error(compiler, line_no, line, "Invalid os syntax.", "os sleep amount milliseconds");
        } else {
            char *amount_c = transpile_expr(amount);
            emit_line(compiler, "bpp_os_sleep_ms(%s);", amount_c);
            free(amount_c);
        }
        free(rest);
        free(amount);
        free(tail);
        return 1;
    }

    compile_error(compiler, line_no, line, "Unknown os command.", "use an os command from the B++ syntax reference");
    return 1;
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
            compiler->consts = &compiler->global_consts;
            compiler->in_function = 0;
            compiler->indent = 1;
            names_free(&compiler->locals);
            names_init(&compiler->locals);
            names_free(&compiler->local_consts);
            names_init(&compiler->local_consts);
        }
        return;
    }

    if ((starts_with(line, "elif ") || starts_with(line, "elseif ")) && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        size_t keyword_len = starts_with(line, "elseif ") ? 7 : 5;
        char *cond = slice_trim(inner, keyword_len, strlen(inner));
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

    if (starts_with(line, "os ")) {
        compile_os_statement(compiler, line_no, line);
        return;
    }

    if ((starts_with(line, "def ") || starts_with(line, "function ")) && ends_with(line, ":")) {
        if (compiler->in_function) {
            compile_error(compiler, line_no, line, "Nested functions are not supported in native B++ yet.", NULL);
            skip_bad_block(compiler);
            return;
        }
        if (compiler->indent != 1 || compiler->block_count != 0) {
            compile_error(compiler, line_no, line, "Functions must be defined at the top level in native B++.", NULL);
            skip_bad_block(compiler);
            return;
        }
        char *inner = without_trailing_colon(line);
        size_t keyword_len = starts_with(line, "function ") ? 9 : 4;
        const char *expected_signature = starts_with(line, "function ") ? "function name(arg, arg):" : "def name(arg, arg):";
        char *signature = slice_trim(inner, keyword_len, strlen(inner));
        char *open = strchr(signature, '(');
        char *close = strrchr(signature, ')');
        if (!open || !close || close < open) {
            compile_error(compiler, line_no, line, "Invalid function syntax.", expected_signature);
            skip_bad_block(compiler);
            free(inner);
            free(signature);
            return;
        }
        char *name = slice_trim(signature, 0, (size_t)(open - signature));
        char *params = slice_trim(signature, (size_t)(open - signature) + 1, (size_t)(close - signature));
        if (!is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid function name.", expected_signature);
            skip_bad_block(compiler);
            free(inner);
            free(signature);
            free(name);
            free(params);
            return;
        }

        if (!check_name_allowed(compiler, line_no, line, name)) {
            skip_bad_block(compiler);
            free(inner);
            free(signature);
            free(name);
            free(params);
            return;
        }

        char **args = NULL;
        size_t arg_count = 0;
        split_top_level_args(params, &args, &arg_count);

        NameList seen_params;
        names_init(&seen_params);
        int param_error = 0;
        for (size_t i = 0; i < arg_count; i++) {
            if (!is_identifier(args[i])) {
                compile_error(compiler, line_no, line, "Invalid function parameter.", expected_signature);
                param_error = 1;
            } else if (is_reserved_identifier(args[i])) {
                compile_reserved_name_error(compiler, line_no, line, args[i]);
                param_error = 1;
            } else if (names_contains(&seen_params, args[i])) {
                compile_error(compiler, line_no, line, "Duplicate function parameter.", "use each parameter name once");
                param_error = 1;
            } else {
                names_add(&seen_params, args[i]);
            }
        }
        names_free(&seen_params);
        if (param_error) {
            skip_bad_block(compiler);
            free_split_args(args, arg_count);
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
        names_free(&compiler->local_consts);
        names_init(&compiler->local_consts);
        compiler->consts = &compiler->local_consts;

        sb_appendf(&compiler->functions_body, "static BppValue %s(", name);
        for (size_t i = 0; i < arg_count; i++) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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

    if (starts_with(line, "const ")) {
        char *body = slice_trim(line, 6, strlen(line));
        char *name = NULL;
        char *expr = NULL;
        if (!partition_assignment(body, &name, &expr) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid const syntax.", "const name = value");
        } else if (!check_name_allowed(compiler, line_no, line, name)) {
        } else if (names_contains(compiler->names, name)) {
            compile_error(compiler, line_no, line, "Name is already defined.", "choose a new const name");
        } else {
            char *expr_c = transpile_expr(expr);
            declare_const(compiler, name, expr_c);
            free(expr_c);
        }
        free(body);
        free(name);
        free(expr);
        return;
    }

    if (starts_with(line, "set ")) {
        char *body = slice_trim(line, 4, strlen(line));
        char *name = NULL;
        char *expr = NULL;
        if (!partition_keyword(body, " to ", &name, &expr) || !is_identifier(name)) {
            compile_error(compiler, line_no, line, "Invalid set syntax.", "set name to value");
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
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

    if (starts_with(line, "unless ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *cond = slice_trim(inner, 7, strlen(inner));
        char *expr_c = transpile_expr(cond);
        emit_line(compiler, "if (!bpp_truthy(%s)) {", expr_c);
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

    if (starts_with(line, "until ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *cond = slice_trim(inner, 6, strlen(inner));
        char *expr_c = transpile_expr(cond);
        emit_line(compiler, "while (!bpp_truthy(%s)) {", expr_c);
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
            if (!check_name_allowed(compiler, line_no, line, name) ||
                !check_name_writable(compiler, line_no, line, name)) {
                skip_bad_block(compiler);
            } else {
                char *count_c = transpile_expr(count);
                emit_line(compiler, "for (long __bpp_i_%d = 1, __bpp_limit_%d = (long)bpp_to_number(%s); __bpp_i_%d <= __bpp_limit_%d; __bpp_i_%d++) {", temp, temp, count_c, temp, temp, temp);
                compiler->indent++;
                push_block(compiler, BLOCK_NORMAL);
                emit_line(compiler, "BppValue %s = bpp_number((double)__bpp_i_%d);", name, temp);
                free(count_c);
            }
        } else {
            free(count);
            free(name);
            count = NULL;
            name = NULL;
            if (!partition_keyword(body, " times", &count, &name) || (name && name[0] != '\0')) {
                compile_error(compiler, line_no, line, "Invalid repeat syntax.", "repeat count times:");
                skip_bad_block(compiler);
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
            skip_bad_block(compiler);
        } else if (!check_name_allowed(compiler, line_no, line, name) ||
                   !check_name_writable(compiler, line_no, line, name)) {
            skip_bad_block(compiler);
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

    if (starts_with(line, "for ") && ends_with(line, ":")) {
        char *inner = without_trailing_colon(line);
        char *body = slice_trim(inner, 4, strlen(inner));
        char *name = NULL;
        char *right = NULL;
        int temp = ++compiler->temp_counter;

        if (partition_keyword(body, " in ", &name, &right)) {
            if (!is_identifier(name)) {
                compile_error(compiler, line_no, line, "Invalid for syntax.", "for name in list:");
                skip_bad_block(compiler);
            } else if (!check_name_allowed(compiler, line_no, line, name) ||
                       !check_name_writable(compiler, line_no, line, name)) {
                skip_bad_block(compiler);
            } else {
                char *list_c = transpile_expr(right);
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
            free(right);
            return;
        }

        free(name);
        free(right);
        name = NULL;
        right = NULL;
        if (partition_keyword(body, " from ", &name, &right)) {
            char *start = NULL;
            char *after_direction = NULL;
            char *end = NULL;
            char *step = NULL;
            int down = 0;
            int range_error = 0;

            if (!is_identifier(name)) {
                compile_error(compiler, line_no, line, "Invalid range for syntax.", "for name from start to end:");
                skip_bad_block(compiler);
                range_error = 1;
            } else if (!check_name_allowed(compiler, line_no, line, name) ||
                       !check_name_writable(compiler, line_no, line, name)) {
                skip_bad_block(compiler);
                range_error = 1;
            } else if (partition_keyword(right, " down to ", &start, &after_direction)) {
                down = 1;
            } else if (!partition_keyword(right, " to ", &start, &after_direction)) {
                compile_error(compiler, line_no, line, "Invalid range for syntax.", "for name from start to end:");
                skip_bad_block(compiler);
                range_error = 1;
            }

            if (!range_error && after_direction) {
                if (partition_keyword(after_direction, " step ", &end, &step)) {
                } else {
                    end = xstrdup(after_direction);
                    step = xstrdup("1");
                }

                if (!end[0] || !step[0]) {
                    compile_error(compiler, line_no, line, "Invalid range for syntax.", "for name from start to end step amount:");
                    skip_bad_block(compiler);
                } else {
                    char *start_c = transpile_expr(start);
                    char *end_c = transpile_expr(end);
                    char *step_c = transpile_expr(step);
                    emit_line(compiler, "for (double __bpp_i_%d = bpp_to_number(%s), __bpp_end_%d = bpp_to_number(%s), __bpp_step_%d = fabs(bpp_to_number(%s)); __bpp_i_%d %s __bpp_end_%d; __bpp_i_%d %s (__bpp_step_%d == 0 ? 1 : __bpp_step_%d)) {",
                              temp, start_c, temp, end_c, temp, step_c,
                              temp, down ? ">=" : "<=", temp,
                              temp, down ? "-=" : "+=",
                              temp, temp);
                    compiler->indent++;
                    push_block(compiler, BLOCK_NORMAL);
                    emit_line(compiler, "BppValue %s = bpp_number(__bpp_i_%d);", name, temp);
                    free(start_c);
                    free(end_c);
                    free(step_c);
                }
            }

            free(inner);
            free(body);
            free(name);
            free(right);
            free(start);
            free(after_direction);
            free(end);
            free(step);
            return;
        }

        compile_error(compiler, line_no, line, "Invalid for syntax.", "for name in list: or for name from start to end:");
        skip_bad_block(compiler);
        free(inner);
        free(body);
        free(name);
        free(right);
        return;
    }

    if (strcmp(line, "stop loop") == 0 || strcmp(line, "break") == 0) {
        emit_line(compiler, "break;");
        return;
    }

    if (strcmp(line, "next loop") == 0 || strcmp(line, "continue") == 0) {
        emit_line(compiler, "continue;");
        return;
    }

    {
        char *name = NULL;
        char *expr = NULL;
        if (partition_assignment(line, &name, &expr)) {
            if (!is_identifier(name)) {
                compile_error(compiler, line_no, line, "Invalid assignment syntax.", "name = value");
            } else if (check_name_allowed(compiler, line_no, line, name) &&
                       check_name_writable(compiler, line_no, line, name)) {
                char *expr_c = transpile_expr(expr);
                declare_or_set(compiler, name, expr_c);
                free(expr_c);
            }
            free(name);
            free(expr);
            return;
        }
        free(name);
        free(expr);
    }

    compile_error(compiler, line_no, line, "Unsupported B++ syntax in native mode.", NULL);
}

static const char *runtime_c =
"#include <errno.h>\n"
"#include <math.h>\n"
"#include <stdarg.h>\n"
"#include <stdio.h>\n"
"#include <stdlib.h>\n"
"#include <string.h>\n"
"#include <sys/stat.h>\n"
"#ifdef _WIN32\n"
"#define WIN32_LEAN_AND_MEAN\n"
"#include <direct.h>\n"
"#include <io.h>\n"
"#include <process.h>\n"
"#include <windows.h>\n"
"#ifndef _S_IFDIR\n"
"#define _S_IFDIR S_IFDIR\n"
"#endif\n"
"#else\n"
"#include <dirent.h>\n"
"#include <signal.h>\n"
"#include <sys/types.h>\n"
"#include <sys/wait.h>\n"
"#include <unistd.h>\n"
"#endif\n"
"\n"
"typedef enum { BPP_NIL, BPP_NUMBER, BPP_STRING, BPP_BOOL, BPP_LIST, BPP_TABLE } BppType;\n"
"typedef struct BppValue BppValue;\n"
"typedef struct BppTable BppTable;\n"
"typedef struct { size_t count; size_t cap; BppValue *items; } BppList;\n"
"struct BppValue { BppType type; double number; char *string; int boolean; BppList *list; BppTable *table; };\n"
"typedef struct { char *key; BppValue value; } BppTableEntry;\n"
"struct BppTable { size_t count; size_t cap; BppTableEntry *entries; };\n"
"\n"
"static char *bpp_strdup(const char *text) { size_t n = strlen(text); char *p = (char *)malloc(n + 1); if (!p) exit(2); memcpy(p, text, n + 1); return p; }\n"
"static BppValue bpp_nil(void) { BppValue v; v.type = BPP_NIL; v.number = 0; v.string = NULL; v.boolean = 0; v.list = NULL; v.table = NULL; return v; }\n"
"static BppValue bpp_number(double n) { BppValue v = bpp_nil(); v.type = BPP_NUMBER; v.number = n; return v; }\n"
"static BppValue bpp_string(const char *s) { BppValue v = bpp_nil(); v.type = BPP_STRING; v.string = bpp_strdup(s); return v; }\n"
"static BppValue bpp_bool(int b) { BppValue v = bpp_nil(); v.type = BPP_BOOL; v.boolean = !!b; return v; }\n"
"static BppValue bpp_list(void) { BppValue v = bpp_nil(); v.type = BPP_LIST; v.list = (BppList *)calloc(1, sizeof(BppList)); if (!v.list) exit(2); return v; }\n"
"static BppValue bpp_table(void) { BppValue v = bpp_nil(); v.type = BPP_TABLE; v.table = (BppTable *)calloc(1, sizeof(BppTable)); if (!v.table) exit(2); return v; }\n"
"static void bpp_list_append(BppValue *list, BppValue item);\n"
"static void bpp_table_set(BppValue *table, BppValue key_value, BppValue value);\n"
"static BppValue bpp_list_of(size_t count, ...) { BppValue list = bpp_list(); va_list args; va_start(args, count); for (size_t i = 0; i < count; i++) { bpp_list_append(&list, va_arg(args, BppValue)); } va_end(args); return list; }\n"
"static BppValue bpp_table_of(size_t count, ...) { BppValue table = bpp_table(); va_list args; va_start(args, count); for (size_t i = 0; i < count; i++) { BppValue key = va_arg(args, BppValue); BppValue value = va_arg(args, BppValue); bpp_table_set(&table, key, value); } va_end(args); return table; }\n"
"static double bpp_to_number(BppValue v) { if (v.type == BPP_NUMBER) return v.number; if (v.type == BPP_BOOL) return v.boolean ? 1.0 : 0.0; return 0.0; }\n"
"static int bpp_truthy(BppValue v) { if (v.type == BPP_NIL) return 0; if (v.type == BPP_BOOL) return v.boolean; if (v.type == BPP_NUMBER) return v.number != 0; if (v.type == BPP_STRING) return v.string && v.string[0]; if (v.type == BPP_LIST) return v.list && v.list->count > 0; if (v.type == BPP_TABLE) return v.table && v.table->count > 0; return 0; }\n"
"static char *bpp_to_string(BppValue v) { char buf[128]; if (v.type == BPP_STRING) return bpp_strdup(v.string ? v.string : \"\"); if (v.type == BPP_BOOL) return bpp_strdup(v.boolean ? \"true\" : \"false\"); if (v.type == BPP_NIL) return bpp_strdup(\"nothing\"); if (v.type == BPP_LIST) { snprintf(buf, sizeof(buf), \"[list:%zu]\", v.list ? v.list->count : 0); return bpp_strdup(buf); } if (v.type == BPP_TABLE) { snprintf(buf, sizeof(buf), \"{table:%zu}\", v.table ? v.table->count : 0); return bpp_strdup(buf); } if (fabs(v.number - (long long)v.number) < 0.0000001) snprintf(buf, sizeof(buf), \"%lld\", (long long)v.number); else snprintf(buf, sizeof(buf), \"%g\", v.number); return bpp_strdup(buf); }\n"
"static void bpp_print(BppValue v) { char *s = bpp_to_string(v); fputs(s, stdout); free(s); }\n"
"static void bpp_println(BppValue v) { bpp_print(v); fputc('\\n', stdout); }\n"
"static BppValue bpp_input(BppValue prompt) { bpp_print(prompt); fputc(' ', stdout); fflush(stdout); char buf[4096]; if (!fgets(buf, sizeof(buf), stdin)) return bpp_string(\"\"); size_t n = strlen(buf); while (n > 0 && (buf[n - 1] == '\\n' || buf[n - 1] == '\\r')) buf[--n] = 0; return bpp_string(buf); }\n"
"static BppValue bpp_read_file(BppValue filename) { char *path = bpp_to_string(filename); FILE *f = fopen(path, \"rb\"); free(path); if (!f) return bpp_string(\"\"); fseek(f, 0, SEEK_END); long size = ftell(f); rewind(f); if (size < 0) { fclose(f); return bpp_string(\"\"); } char *buf = (char *)malloc((size_t)size + 1); if (!buf) exit(2); size_t n = fread(buf, 1, (size_t)size, f); buf[n] = 0; fclose(f); BppValue out = bpp_string(buf); free(buf); return out; }\n"
"static void bpp_write_file(BppValue filename, BppValue value) { char *path = bpp_to_string(filename); char *text = bpp_to_string(value); FILE *f = fopen(path, \"wb\"); if (f) { fwrite(text, 1, strlen(text), f); fclose(f); } free(path); free(text); }\n"
"static BppValue bpp_add(BppValue a, BppValue b) { if (a.type == BPP_STRING || b.type == BPP_STRING) { char *sa = bpp_to_string(a); char *sb = bpp_to_string(b); char *out = (char *)malloc(strlen(sa) + strlen(sb) + 1); if (!out) exit(2); strcpy(out, sa); strcat(out, sb); free(sa); free(sb); BppValue v = bpp_string(out); free(out); return v; } return bpp_number(bpp_to_number(a) + bpp_to_number(b)); }\n"
"static BppValue bpp_sub(BppValue a, BppValue b) { return bpp_number(bpp_to_number(a) - bpp_to_number(b)); }\n"
"static BppValue bpp_mul(BppValue a, BppValue b) { return bpp_number(bpp_to_number(a) * bpp_to_number(b)); }\n"
"static BppValue bpp_div(BppValue a, BppValue b) { double d = bpp_to_number(b); return bpp_number(d == 0 ? 0 : bpp_to_number(a) / d); }\n"
"static BppValue bpp_mod(BppValue a, BppValue b) { double d = bpp_to_number(b); return bpp_number(d == 0 ? 0 : fmod(bpp_to_number(a), d)); }\n"
"static BppValue bpp_pow(BppValue a, BppValue b) { return bpp_number(pow(bpp_to_number(a), bpp_to_number(b))); }\n"
"static BppValue bpp_not(BppValue v) { return bpp_bool(!bpp_truthy(v)); }\n"
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
"static void bpp_table_set(BppValue *table, BppValue key_value, BppValue value) { if (table->type != BPP_TABLE) *table = bpp_table(); char *key = bpp_to_string(key_value); for (size_t i = 0; i < table->table->count; i++) { if (strcmp(table->table->entries[i].key, key) == 0) { table->table->entries[i].value = value; free(key); return; } } if (table->table->count == table->table->cap) { table->table->cap = table->table->cap ? table->table->cap * 2 : 8; table->table->entries = (BppTableEntry *)realloc(table->table->entries, table->table->cap * sizeof(BppTableEntry)); if (!table->table->entries) exit(2); } table->table->entries[table->table->count].key = key; table->table->entries[table->table->count].value = value; table->table->count++; }\n"
"static BppValue bpp_table_get(BppValue table, BppValue key_value) { if (table.type != BPP_TABLE || !table.table) return bpp_nil(); char *key = bpp_to_string(key_value); for (size_t i = 0; i < table.table->count; i++) { if (strcmp(table.table->entries[i].key, key) == 0) { BppValue value = table.table->entries[i].value; free(key); return value; } } free(key); return bpp_nil(); }\n"
"static BppValue bpp_index(BppValue value, BppValue key) { if (value.type == BPP_TABLE) return bpp_table_get(value, key); if (value.type == BPP_LIST) { long index = (long)bpp_to_number(key); if (index < 0) return bpp_nil(); return bpp_list_get(value, (size_t)index); } return bpp_nil(); }\n"
"static int bpp_os_last_exit_code_value = 0;\n"
"static char bpp_os_last_error_text[1024] = \"\";\n"
"static void bpp_os_clear_error(void) { bpp_os_last_error_text[0] = '\\0'; }\n"
"static void bpp_os_set_error(const char *text) { snprintf(bpp_os_last_error_text, sizeof(bpp_os_last_error_text), \"%s\", text ? text : \"\"); }\n"
"static void bpp_os_set_errno_error(const char *action) { char buf[1024]; snprintf(buf, sizeof(buf), \"%s: %s\", action, strerror(errno)); bpp_os_set_error(buf); }\n"
"static BppValue bpp_os_last_error(void) { return bpp_string(bpp_os_last_error_text); }\n"
"static BppValue bpp_os_exit_code(void) { return bpp_number((double)bpp_os_last_exit_code_value); }\n"
"#ifdef _WIN32\n"
"static int bpp_os_is_dir_mode(unsigned int mode) { return (mode & _S_IFDIR) != 0; }\n"
"#define bpp_popen _popen\n"
"#define bpp_pclose _pclose\n"
"#else\n"
"static int bpp_os_is_dir_mode(mode_t mode) { return S_ISDIR(mode); }\n"
"#define bpp_popen popen\n"
"#define bpp_pclose pclose\n"
"#endif\n"
"static char *bpp_os_join_path(const char *left, const char *right) {\n"
"#ifdef _WIN32\n"
"    char sep = '\\\\';\n"
"#else\n"
"    char sep = '/';\n"
"#endif\n"
"    size_t left_len = strlen(left);\n"
"    size_t right_len = strlen(right);\n"
"    int needs_sep = left_len > 0 && left[left_len - 1] != '/' && left[left_len - 1] != '\\\\';\n"
"    char *out = (char *)malloc(left_len + right_len + (needs_sep ? 2 : 1));\n"
"    if (!out) exit(2);\n"
"    memcpy(out, left, left_len);\n"
"    size_t pos = left_len;\n"
"    if (needs_sep) out[pos++] = sep;\n"
"    memcpy(out + pos, right, right_len + 1);\n"
"    return out;\n"
"}\n"
"static BppValue bpp_os_current_folder(void) {\n"
"    char buf[4096];\n"
"#ifdef _WIN32\n"
"    char *got = _getcwd(buf, sizeof(buf));\n"
"#else\n"
"    char *got = getcwd(buf, sizeof(buf));\n"
"#endif\n"
"    if (!got) { bpp_os_set_errno_error(\"current folder\"); return bpp_string(\"\"); }\n"
"    bpp_os_clear_error();\n"
"    return bpp_string(buf);\n"
"}\n"
"static BppValue bpp_os_home_folder(void) {\n"
"    const char *home = getenv(\"HOME\");\n"
"#ifdef _WIN32\n"
"    if (!home || !home[0]) home = getenv(\"USERPROFILE\");\n"
"#endif\n"
"    if (!home) home = \"\";\n"
"    bpp_os_clear_error();\n"
"    return bpp_string(home);\n"
"}\n"
"static BppValue bpp_os_temp_folder(void) {\n"
"    const char *tmp = getenv(\"TMPDIR\");\n"
"#ifdef _WIN32\n"
"    if (!tmp || !tmp[0]) tmp = getenv(\"TEMP\");\n"
"    if (!tmp || !tmp[0]) tmp = getenv(\"TMP\");\n"
"#endif\n"
"    if (!tmp || !tmp[0]) tmp = \"/tmp\";\n"
"    bpp_os_clear_error();\n"
"    return bpp_string(tmp);\n"
"}\n"
"static BppValue bpp_os_file_exists(BppValue path_value) { char *path = bpp_to_string(path_value); struct stat st; int ok = stat(path, &st) == 0 && !bpp_os_is_dir_mode(st.st_mode); free(path); bpp_os_clear_error(); return bpp_bool(ok); }\n"
"static BppValue bpp_os_folder_exists(BppValue path_value) { char *path = bpp_to_string(path_value); struct stat st; int ok = stat(path, &st) == 0 && bpp_os_is_dir_mode(st.st_mode); free(path); bpp_os_clear_error(); return bpp_bool(ok); }\n"
"static BppValue bpp_os_list_folder(BppValue path_value) { char *path = bpp_to_string(path_value); BppValue list = bpp_list();\n"
"#ifdef _WIN32\n"
"char *pattern = bpp_os_join_path(path, \"*\"); WIN32_FIND_DATAA data; HANDLE h = FindFirstFileA(pattern, &data); free(pattern); if (h == INVALID_HANDLE_VALUE) { bpp_os_set_error(\"could not list folder\"); free(path); return list; } do { if (strcmp(data.cFileName, \".\") != 0 && strcmp(data.cFileName, \"..\") != 0) bpp_list_append(&list, bpp_string(data.cFileName)); } while (FindNextFileA(h, &data)); FindClose(h);\n"
"#else\n"
"DIR *dir = opendir(path); if (!dir) { bpp_os_set_errno_error(\"list folder\"); free(path); return list; } struct dirent *entry; while ((entry = readdir(dir)) != NULL) { if (strcmp(entry->d_name, \".\") != 0 && strcmp(entry->d_name, \"..\") != 0) bpp_list_append(&list, bpp_string(entry->d_name)); } closedir(dir);\n"
"#endif\n"
"free(path); bpp_os_clear_error(); return list; }\n"
"static void bpp_os_create_folder(BppValue path_value) {\n"
"    char *path = bpp_to_string(path_value);\n"
"    int ok;\n"
"#ifdef _WIN32\n"
"    ok = CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;\n"
"#else\n"
"    ok = mkdir(path, 0777) == 0 || errno == EEXIST;\n"
"#endif\n"
"    if (ok) bpp_os_clear_error(); else bpp_os_set_error(\"could not create folder\");\n"
"    free(path);\n"
"}\n"
"static void bpp_os_copy_file(BppValue from_value, BppValue to_value) { char *from = bpp_to_string(from_value); char *to = bpp_to_string(to_value); FILE *src = fopen(from, \"rb\"); if (!src) { bpp_os_set_errno_error(\"copy file\"); free(from); free(to); return; } FILE *dst = fopen(to, \"wb\"); if (!dst) { bpp_os_set_errno_error(\"copy file\"); fclose(src); free(from); free(to); return; } char buf[8192]; size_t n; int ok = 1; while ((n = fread(buf, 1, sizeof(buf), src)) > 0) { if (fwrite(buf, 1, n, dst) != n) { ok = 0; break; } } if (ferror(src)) ok = 0; fclose(src); fclose(dst); if (ok) bpp_os_clear_error(); else bpp_os_set_error(\"could not copy file\"); free(from); free(to); }\n"
"static void bpp_os_move_file(BppValue from_value, BppValue to_value) { char *from = bpp_to_string(from_value); char *to = bpp_to_string(to_value); if (rename(from, to) == 0) bpp_os_clear_error(); else bpp_os_set_errno_error(\"move file\"); free(from); free(to); }\n"
"static void bpp_os_delete_file(BppValue path_value) { char *path = bpp_to_string(path_value); if (remove(path) == 0) bpp_os_clear_error(); else bpp_os_set_errno_error(\"delete file\"); free(path); }\n"
"static int bpp_os_delete_folder_path(const char *path) {\n"
"#ifdef _WIN32\n"
"char *pattern = bpp_os_join_path(path, \"*\"); WIN32_FIND_DATAA data; HANDLE h = FindFirstFileA(pattern, &data); free(pattern); if (h != INVALID_HANDLE_VALUE) { do { if (strcmp(data.cFileName, \".\") == 0 || strcmp(data.cFileName, \"..\") == 0) continue; char *full = bpp_os_join_path(path, data.cFileName); if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!bpp_os_delete_folder_path(full)) { free(full); FindClose(h); return 0; } } else if (!DeleteFileA(full)) { free(full); FindClose(h); return 0; } free(full); } while (FindNextFileA(h, &data)); FindClose(h); } return RemoveDirectoryA(path) != 0;\n"
"#else\n"
"DIR *dir = opendir(path); if (dir) { struct dirent *entry; while ((entry = readdir(dir)) != NULL) { if (strcmp(entry->d_name, \".\") == 0 || strcmp(entry->d_name, \"..\") == 0) continue; char *full = bpp_os_join_path(path, entry->d_name); struct stat st; if (stat(full, &st) == 0 && bpp_os_is_dir_mode(st.st_mode)) { if (!bpp_os_delete_folder_path(full)) { free(full); closedir(dir); return 0; } } else if (remove(full) != 0) { free(full); closedir(dir); return 0; } free(full); } closedir(dir); } return rmdir(path) == 0;\n"
"#endif\n"
"}\n"
"static void bpp_os_delete_folder_recursive(BppValue path_value) { char *path = bpp_to_string(path_value); if (bpp_os_delete_folder_path(path)) bpp_os_clear_error(); else bpp_os_set_error(\"could not delete folder\"); free(path); }\n"
"static BppValue bpp_os_env(BppValue name_value) {\n"
"    char *name = bpp_to_string(name_value);\n"
"#ifdef _WIN32\n"
"    DWORD needed = GetEnvironmentVariableA(name, NULL, 0);\n"
"    if (needed == 0) { free(name); bpp_os_clear_error(); return bpp_string(\"\"); }\n"
"    char *value = (char *)malloc((size_t)needed);\n"
"    if (!value) exit(2);\n"
"    GetEnvironmentVariableA(name, value, needed);\n"
"    BppValue out = bpp_string(value);\n"
"    free(value);\n"
"#else\n"
"    const char *found = getenv(name);\n"
"    BppValue out = bpp_string(found ? found : \"\");\n"
"#endif\n"
"    free(name);\n"
"    bpp_os_clear_error();\n"
"    return out;\n"
"}\n"
"static void bpp_os_set_env(BppValue name_value, BppValue value_value) {\n"
"    char *name = bpp_to_string(name_value);\n"
"    char *value = bpp_to_string(value_value);\n"
"    int ok;\n"
"#ifdef _WIN32\n"
"    ok = SetEnvironmentVariableA(name, value) != 0;\n"
"#else\n"
"    ok = setenv(name, value, 1) == 0;\n"
"#endif\n"
"    if (ok) bpp_os_clear_error(); else bpp_os_set_errno_error(\"set env\");\n"
"    free(name);\n"
"    free(value);\n"
"}\n"
"static void bpp_os_remove_env(BppValue name_value) {\n"
"    char *name = bpp_to_string(name_value);\n"
"    int ok;\n"
"#ifdef _WIN32\n"
"    ok = SetEnvironmentVariableA(name, NULL) != 0;\n"
"#else\n"
"    ok = unsetenv(name) == 0;\n"
"#endif\n"
"    if (ok) bpp_os_clear_error(); else bpp_os_set_errno_error(\"remove env\");\n"
"    free(name);\n"
"}\n"
"static int bpp_os_decode_exit_code(int status) {\n"
"#ifdef _WIN32\n"
"    return status;\n"
"#else\n"
"    if (WIFEXITED(status)) return WEXITSTATUS(status);\n"
"    return status;\n"
"#endif\n"
"}\n"
"static BppValue bpp_os_run(BppValue command_value) { char *command = bpp_to_string(command_value); FILE *pipe = bpp_popen(command, \"r\"); free(command); if (!pipe) { bpp_os_last_exit_code_value = -1; bpp_os_set_error(\"could not run command\"); return bpp_string(\"\"); } size_t cap = 4096, len = 0; char *out = (char *)malloc(cap); if (!out) exit(2); out[0] = '\\0'; char buf[512]; while (fgets(buf, sizeof(buf), pipe)) { size_t n = strlen(buf); if (len + n + 1 > cap) { while (len + n + 1 > cap) cap *= 2; out = (char *)realloc(out, cap); if (!out) exit(2); } memcpy(out + len, buf, n + 1); len += n; } int status = bpp_pclose(pipe); bpp_os_last_exit_code_value = bpp_os_decode_exit_code(status); bpp_os_clear_error(); BppValue value = bpp_string(out); free(out); return value; }\n"
"static BppValue bpp_os_process_id(void) {\n"
"#ifdef _WIN32\n"
"    return bpp_number((double)_getpid());\n"
"#else\n"
"    return bpp_number((double)getpid());\n"
"#endif\n"
"}\n"
"static void bpp_os_kill_process(BppValue pid_value) {\n"
"    long pid = (long)bpp_to_number(pid_value);\n"
"#ifdef _WIN32\n"
"    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);\n"
"    if (!h) { bpp_os_set_error(\"could not open process\"); return; }\n"
"    if (TerminateProcess(h, 1)) bpp_os_clear_error(); else bpp_os_set_error(\"could not kill process\");\n"
"    CloseHandle(h);\n"
"#else\n"
"    if (kill((pid_t)pid, SIGTERM) == 0) bpp_os_clear_error(); else bpp_os_set_errno_error(\"kill process\");\n"
"#endif\n"
"}\n"
"static void bpp_os_sleep_ms(BppValue amount_value) {\n"
"    double amount = bpp_to_number(amount_value);\n"
"    if (amount < 0) amount = 0;\n"
"#ifdef _WIN32\n"
"    Sleep((DWORD)amount);\n"
"#else\n"
"    usleep((unsigned int)(amount * 1000.0));\n"
"#endif\n"
"    bpp_os_clear_error();\n"
"}\n"
"\n";

typedef enum {
    FAST_T_EOF,
    FAST_T_NUMBER,
    FAST_T_IDENTIFIER,
    FAST_T_PLUS,
    FAST_T_MINUS,
    FAST_T_STAR,
    FAST_T_SLASH,
    FAST_T_LPAREN,
    FAST_T_RPAREN,
    FAST_T_EQ,
    FAST_T_NE,
    FAST_T_GT,
    FAST_T_LT,
    FAST_T_GE,
    FAST_T_LE,
    FAST_T_AND,
    FAST_T_OR
} FastTokenKind;

typedef struct {
    FastTokenKind kind;
    char *text;
} FastToken;

typedef struct {
    FastToken *items;
    size_t count;
    size_t cap;
    size_t pos;
} FastTokenList;

typedef enum {
    FAST_EXPR_NUMBER,
    FAST_EXPR_NAME,
    FAST_EXPR_BINARY
} FastExprKind;

typedef struct FastExpr FastExpr;
struct FastExpr {
    FastExprKind kind;
    char *text;
    char op[4];
    FastExpr *left;
    FastExpr *right;
};

typedef enum {
    FAST_STMT_SET,
    FAST_STMT_ADD,
    FAST_STMT_SUBTRACT,
    FAST_STMT_MULTIPLY,
    FAST_STMT_DIVIDE,
    FAST_STMT_SAY,
    FAST_STMT_IF,
    FAST_STMT_WHILE,
    FAST_STMT_FOREVER,
    FAST_STMT_REPEAT,
    FAST_STMT_REPEAT_AS,
    FAST_STMT_STOP,
    FAST_STMT_NEXT
} FastStmtKind;

typedef struct FastStmt FastStmt;

typedef struct {
    FastStmt **items;
    size_t count;
    size_t cap;
} FastStmtList;

struct FastStmt {
    FastStmtKind kind;
    int line_no;
    char *line;
    char *name;
    int without_newline;
    FastExpr *expr;
    FastStmtList body;
};

typedef struct {
    int line_no;
    char *text;
} FastLine;

typedef struct {
    FastLine *items;
    size_t count;
    size_t cap;
    size_t pos;
    int unsupported;
} FastParser;

static void fast_tokens_init(FastTokenList *tokens) {
    tokens->items = NULL;
    tokens->count = 0;
    tokens->cap = 0;
    tokens->pos = 0;
}

static void fast_tokens_free(FastTokenList *tokens) {
    for (size_t i = 0; i < tokens->count; i++) {
        free(tokens->items[i].text);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
    tokens->cap = 0;
    tokens->pos = 0;
}

static void fast_tokens_add(FastTokenList *tokens, FastTokenKind kind, const char *start, size_t len) {
    if (tokens->count == tokens->cap) {
        tokens->cap = tokens->cap ? tokens->cap * 2 : 32;
        tokens->items = (FastToken *)xrealloc(tokens->items, tokens->cap * sizeof(FastToken));
    }
    tokens->items[tokens->count].kind = kind;
    tokens->items[tokens->count].text = slice_trim(start, 0, len);
    tokens->count++;
}

static int fast_tokenize_expr(const char *text, FastTokenList *tokens) {
    fast_tokens_init(tokens);
    for (size_t i = 0; text[i];) {
        if (isspace((unsigned char)text[i])) {
            i++;
            continue;
        }

        if (isdigit((unsigned char)text[i]) || (text[i] == '.' && isdigit((unsigned char)text[i + 1]))) {
            size_t start = i;
            int seen_dot = 0;
            while (isdigit((unsigned char)text[i]) || text[i] == '.') {
                if (text[i] == '.') {
                    if (seen_dot) {
                        fast_tokens_free(tokens);
                        return 0;
                    }
                    seen_dot = 1;
                }
                i++;
            }
            fast_tokens_add(tokens, FAST_T_NUMBER, text + start, i - start);
            continue;
        }

        if (isalpha((unsigned char)text[i]) || text[i] == '_') {
            size_t start = i;
            while (isalnum((unsigned char)text[i]) || text[i] == '_') {
                i++;
            }
            char *word = slice_trim(text, start, i);
            if (strcmp(word, "and") == 0) {
                fast_tokens_add(tokens, FAST_T_AND, text + start, i - start);
            } else if (strcmp(word, "or") == 0) {
                fast_tokens_add(tokens, FAST_T_OR, text + start, i - start);
            } else {
                fast_tokens_add(tokens, FAST_T_IDENTIFIER, text + start, i - start);
            }
            free(word);
            continue;
        }

        if (text[i] == '=' && text[i + 1] == '=') {
            fast_tokens_add(tokens, FAST_T_EQ, text + i, 2);
            i += 2;
            continue;
        }
        if (text[i] == '!' && text[i + 1] == '=') {
            fast_tokens_add(tokens, FAST_T_NE, text + i, 2);
            i += 2;
            continue;
        }
        if (text[i] == '>' && text[i + 1] == '=') {
            fast_tokens_add(tokens, FAST_T_GE, text + i, 2);
            i += 2;
            continue;
        }
        if (text[i] == '<' && text[i + 1] == '=') {
            fast_tokens_add(tokens, FAST_T_LE, text + i, 2);
            i += 2;
            continue;
        }

        switch (text[i]) {
            case '+': fast_tokens_add(tokens, FAST_T_PLUS, text + i, 1); break;
            case '-': fast_tokens_add(tokens, FAST_T_MINUS, text + i, 1); break;
            case '*': fast_tokens_add(tokens, FAST_T_STAR, text + i, 1); break;
            case '/': fast_tokens_add(tokens, FAST_T_SLASH, text + i, 1); break;
            case '(': fast_tokens_add(tokens, FAST_T_LPAREN, text + i, 1); break;
            case ')': fast_tokens_add(tokens, FAST_T_RPAREN, text + i, 1); break;
            case '>': fast_tokens_add(tokens, FAST_T_GT, text + i, 1); break;
            case '<': fast_tokens_add(tokens, FAST_T_LT, text + i, 1); break;
            default:
                fast_tokens_free(tokens);
                return 0;
        }
        i++;
    }
    fast_tokens_add(tokens, FAST_T_EOF, text + strlen(text), 0);
    return 1;
}

static FastToken *fast_peek(FastTokenList *tokens) {
    return &tokens->items[tokens->pos];
}

static int fast_match(FastTokenList *tokens, FastTokenKind kind) {
    if (fast_peek(tokens)->kind == kind) {
        tokens->pos++;
        return 1;
    }
    return 0;
}

static FastExpr *fast_expr_new(FastExprKind kind) {
    FastExpr *expr = (FastExpr *)xmalloc(sizeof(FastExpr));
    expr->kind = kind;
    expr->text = NULL;
    expr->op[0] = '\0';
    expr->left = NULL;
    expr->right = NULL;
    return expr;
}

static FastExpr *fast_expr_binary(const char *op, FastExpr *left, FastExpr *right) {
    FastExpr *expr = fast_expr_new(FAST_EXPR_BINARY);
    snprintf(expr->op, sizeof(expr->op), "%s", op);
    expr->left = left;
    expr->right = right;
    return expr;
}

static void fast_expr_free(FastExpr *expr) {
    if (!expr) {
        return;
    }
    free(expr->text);
    fast_expr_free(expr->left);
    fast_expr_free(expr->right);
    free(expr);
}

static FastExpr *fast_parse_or(FastTokenList *tokens, int *ok);

static FastExpr *fast_parse_primary(FastTokenList *tokens, int *ok) {
    FastToken *token = fast_peek(tokens);
    if (fast_match(tokens, FAST_T_NUMBER)) {
        FastExpr *expr = fast_expr_new(FAST_EXPR_NUMBER);
        expr->text = xstrdup(token->text);
        return expr;
    }
    if (fast_match(tokens, FAST_T_IDENTIFIER)) {
        FastExpr *expr = fast_expr_new(FAST_EXPR_NAME);
        if (strcmp(token->text, "true") == 0) {
            expr->kind = FAST_EXPR_NUMBER;
            expr->text = xstrdup("1");
        } else if (strcmp(token->text, "false") == 0) {
            expr->kind = FAST_EXPR_NUMBER;
            expr->text = xstrdup("0");
        } else {
            expr->text = xstrdup(token->text);
        }
        return expr;
    }
    if (fast_match(tokens, FAST_T_LPAREN)) {
        FastExpr *expr = fast_parse_or(tokens, ok);
        if (!fast_match(tokens, FAST_T_RPAREN)) {
            *ok = 0;
        }
        return expr;
    }
    *ok = 0;
    return NULL;
}

static FastExpr *fast_parse_unary(FastTokenList *tokens, int *ok) {
    if (fast_match(tokens, FAST_T_MINUS)) {
        FastExpr *zero = fast_expr_new(FAST_EXPR_NUMBER);
        zero->text = xstrdup("0");
        return fast_expr_binary("-", zero, fast_parse_unary(tokens, ok));
    }
    return fast_parse_primary(tokens, ok);
}

static FastExpr *fast_parse_factor(FastTokenList *tokens, int *ok) {
    FastExpr *expr = fast_parse_unary(tokens, ok);
    while (*ok && (fast_peek(tokens)->kind == FAST_T_STAR || fast_peek(tokens)->kind == FAST_T_SLASH)) {
        FastTokenKind kind = fast_peek(tokens)->kind;
        tokens->pos++;
        FastExpr *right = fast_parse_unary(tokens, ok);
        expr = fast_expr_binary(kind == FAST_T_STAR ? "*" : "/", expr, right);
    }
    return expr;
}

static FastExpr *fast_parse_term(FastTokenList *tokens, int *ok) {
    FastExpr *expr = fast_parse_factor(tokens, ok);
    while (*ok && (fast_peek(tokens)->kind == FAST_T_PLUS || fast_peek(tokens)->kind == FAST_T_MINUS)) {
        FastTokenKind kind = fast_peek(tokens)->kind;
        tokens->pos++;
        FastExpr *right = fast_parse_factor(tokens, ok);
        expr = fast_expr_binary(kind == FAST_T_PLUS ? "+" : "-", expr, right);
    }
    return expr;
}

static FastExpr *fast_parse_compare(FastTokenList *tokens, int *ok) {
    FastExpr *expr = fast_parse_term(tokens, ok);
    while (*ok) {
        FastTokenKind kind = fast_peek(tokens)->kind;
        const char *op = NULL;
        if (kind == FAST_T_EQ) op = "==";
        else if (kind == FAST_T_NE) op = "!=";
        else if (kind == FAST_T_GT) op = ">";
        else if (kind == FAST_T_LT) op = "<";
        else if (kind == FAST_T_GE) op = ">=";
        else if (kind == FAST_T_LE) op = "<=";
        else break;
        tokens->pos++;
        FastExpr *right = fast_parse_term(tokens, ok);
        expr = fast_expr_binary(op, expr, right);
    }
    return expr;
}

static FastExpr *fast_parse_and(FastTokenList *tokens, int *ok) {
    FastExpr *expr = fast_parse_compare(tokens, ok);
    while (*ok && fast_match(tokens, FAST_T_AND)) {
        FastExpr *right = fast_parse_compare(tokens, ok);
        expr = fast_expr_binary("and", expr, right);
    }
    return expr;
}

static FastExpr *fast_parse_or(FastTokenList *tokens, int *ok) {
    FastExpr *expr = fast_parse_and(tokens, ok);
    while (*ok && fast_match(tokens, FAST_T_OR)) {
        FastExpr *right = fast_parse_and(tokens, ok);
        expr = fast_expr_binary("or", expr, right);
    }
    return expr;
}

static FastExpr *fast_parse_expr_text(const char *text) {
    FastTokenList tokens;
    if (!fast_tokenize_expr(text, &tokens)) {
        return NULL;
    }
    int ok = 1;
    FastExpr *expr = fast_parse_or(&tokens, &ok);
    if (!ok || fast_peek(&tokens)->kind != FAST_T_EOF) {
        fast_expr_free(expr);
        fast_tokens_free(&tokens);
        return NULL;
    }
    fast_tokens_free(&tokens);
    return expr;
}

static void fast_stmt_list_init(FastStmtList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void fast_stmt_free(FastStmt *stmt);

static void fast_stmt_list_free(FastStmtList *list) {
    for (size_t i = 0; i < list->count; i++) {
        fast_stmt_free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static void fast_stmt_list_add(FastStmtList *list, FastStmt *stmt) {
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 32;
        list->items = (FastStmt **)xrealloc(list->items, list->cap * sizeof(FastStmt *));
    }
    list->items[list->count++] = stmt;
}

static FastStmt *fast_stmt_new(FastStmtKind kind, int line_no, const char *line) {
    FastStmt *stmt = (FastStmt *)xmalloc(sizeof(FastStmt));
    stmt->kind = kind;
    stmt->line_no = line_no;
    stmt->line = xstrdup(line);
    stmt->name = NULL;
    stmt->without_newline = 0;
    stmt->expr = NULL;
    fast_stmt_list_init(&stmt->body);
    return stmt;
}

static void fast_stmt_free(FastStmt *stmt) {
    if (!stmt) {
        return;
    }
    free(stmt->line);
    free(stmt->name);
    fast_expr_free(stmt->expr);
    fast_stmt_list_free(&stmt->body);
    free(stmt);
}

static void fast_parser_add_line(FastParser *parser, int line_no, const char *text) {
    if (parser->count == parser->cap) {
        parser->cap = parser->cap ? parser->cap * 2 : 64;
        parser->items = (FastLine *)xrealloc(parser->items, parser->cap * sizeof(FastLine));
    }
    parser->items[parser->count].line_no = line_no;
    parser->items[parser->count].text = xstrdup(text);
    parser->count++;
}

static void fast_parser_free(FastParser *parser) {
    for (size_t i = 0; i < parser->count; i++) {
        free(parser->items[i].text);
    }
    free(parser->items);
    parser->items = NULL;
    parser->count = 0;
    parser->cap = 0;
    parser->pos = 0;
}

static int fast_parse_block(FastParser *parser, FastStmtList *out, int needs_end);

static int fast_parse_expr_or_unsupported(const char *text, FastExpr **out) {
    FastExpr *expr = fast_parse_expr_text(text);
    if (!expr) {
        return 0;
    }
    *out = expr;
    return 1;
}

static int fast_parse_name_target(const char *name) {
    return is_identifier(name) && !is_reserved_identifier(name);
}

static int fast_parse_statement(FastParser *parser, FastStmtList *out) {
    FastLine *line = &parser->items[parser->pos];
    const char *text = line->text;

    if (strcmp(text, "stop loop") == 0) {
        fast_stmt_list_add(out, fast_stmt_new(FAST_STMT_STOP, line->line_no, text));
        parser->pos++;
        return 1;
    }
    if (strcmp(text, "next loop") == 0) {
        fast_stmt_list_add(out, fast_stmt_new(FAST_STMT_NEXT, line->line_no, text));
        parser->pos++;
        return 1;
    }

    if (starts_with(text, "say ")) {
        char *body = slice_trim(text, 4, strlen(text));
        char *expr_text = NULL;
        char *mode = NULL;
        FastStmt *stmt = fast_stmt_new(FAST_STMT_SAY, line->line_no, text);
        if (partition_keyword(body, " without newline", &expr_text, &mode) && mode[0] == '\0') {
            stmt->without_newline = 1;
        } else {
            free(expr_text);
            expr_text = xstrdup(body);
        }
        int ok = fast_parse_expr_or_unsupported(expr_text, &stmt->expr);
        free(body);
        free(expr_text);
        free(mode);
        if (!ok) {
            fast_stmt_free(stmt);
            return 0;
        }
        fast_stmt_list_add(out, stmt);
        parser->pos++;
        return 1;
    }

    if (starts_with(text, "set ")) {
        char *body = slice_trim(text, 4, strlen(text));
        char *name = NULL;
        char *expr_text = NULL;
        if (!partition_keyword(body, " to ", &name, &expr_text) || !fast_parse_name_target(name)) {
            free(body);
            free(name);
            free(expr_text);
            return 0;
        }
        FastStmt *stmt = fast_stmt_new(FAST_STMT_SET, line->line_no, text);
        stmt->name = xstrdup(name);
        int ok = fast_parse_expr_or_unsupported(expr_text, &stmt->expr);
        free(body);
        free(name);
        free(expr_text);
        if (!ok) {
            fast_stmt_free(stmt);
            return 0;
        }
        fast_stmt_list_add(out, stmt);
        parser->pos++;
        return 1;
    }

    const struct {
        const char *prefix;
        const char *keyword;
        FastStmtKind kind;
    } math_forms[] = {
        {"add ", " to ", FAST_STMT_ADD},
        {"subtract ", " from ", FAST_STMT_SUBTRACT},
        {"multiply ", " by ", FAST_STMT_MULTIPLY},
        {"divide ", " by ", FAST_STMT_DIVIDE}
    };
    for (size_t i = 0; i < sizeof(math_forms) / sizeof(math_forms[0]); i++) {
        if (starts_with(text, math_forms[i].prefix)) {
            size_t prefix_len = strlen(math_forms[i].prefix);
            char *body = slice_trim(text, prefix_len, strlen(text));
            char *left = NULL;
            char *right = NULL;
            int partitioned = partition_keyword(body, math_forms[i].keyword, &left, &right);
            char *name = (math_forms[i].kind == FAST_STMT_MULTIPLY || math_forms[i].kind == FAST_STMT_DIVIDE) ? left : right;
            char *expr_text = (math_forms[i].kind == FAST_STMT_MULTIPLY || math_forms[i].kind == FAST_STMT_DIVIDE) ? right : left;
            if (!partitioned || !fast_parse_name_target(name)) {
                free(body);
                free(left);
                free(right);
                return 0;
            }
            FastStmt *stmt = fast_stmt_new(math_forms[i].kind, line->line_no, text);
            stmt->name = xstrdup(name);
            int ok = fast_parse_expr_or_unsupported(expr_text, &stmt->expr);
            free(body);
            free(left);
            free(right);
            if (!ok) {
                fast_stmt_free(stmt);
                return 0;
            }
            fast_stmt_list_add(out, stmt);
            parser->pos++;
            return 1;
        }
    }

    if (starts_with(text, "if ") && ends_with(text, ":")) {
        char *inner = without_trailing_colon(text);
        char *cond = slice_trim(inner, 3, strlen(inner));
        FastStmt *stmt = fast_stmt_new(FAST_STMT_IF, line->line_no, text);
        int ok = fast_parse_expr_or_unsupported(cond, &stmt->expr);
        free(inner);
        free(cond);
        if (!ok) {
            fast_stmt_free(stmt);
            return 0;
        }
        parser->pos++;
        if (!fast_parse_block(parser, &stmt->body, 1)) {
            fast_stmt_free(stmt);
            return 0;
        }
        fast_stmt_list_add(out, stmt);
        return 1;
    }

    if (starts_with(text, "while ") && ends_with(text, ":")) {
        char *inner = without_trailing_colon(text);
        char *cond = slice_trim(inner, 6, strlen(inner));
        FastStmt *stmt = fast_stmt_new(FAST_STMT_WHILE, line->line_no, text);
        int ok = fast_parse_expr_or_unsupported(cond, &stmt->expr);
        free(inner);
        free(cond);
        if (!ok) {
            fast_stmt_free(stmt);
            return 0;
        }
        parser->pos++;
        if (!fast_parse_block(parser, &stmt->body, 1)) {
            fast_stmt_free(stmt);
            return 0;
        }
        fast_stmt_list_add(out, stmt);
        return 1;
    }

    if (strcmp(text, "forever:") == 0) {
        FastStmt *stmt = fast_stmt_new(FAST_STMT_FOREVER, line->line_no, text);
        parser->pos++;
        if (!fast_parse_block(parser, &stmt->body, 1)) {
            fast_stmt_free(stmt);
            return 0;
        }
        fast_stmt_list_add(out, stmt);
        return 1;
    }

    if (starts_with(text, "repeat ") && ends_with(text, ":")) {
        char *inner = without_trailing_colon(text);
        char *body = slice_trim(inner, 7, strlen(inner));
        char *count = NULL;
        char *name = NULL;
        FastStmt *stmt = NULL;
        if (partition_keyword(body, " times as ", &count, &name) && fast_parse_name_target(name)) {
            stmt = fast_stmt_new(FAST_STMT_REPEAT_AS, line->line_no, text);
            stmt->name = xstrdup(name);
        } else {
            free(count);
            free(name);
            count = NULL;
            name = NULL;
            if (partition_keyword(body, " times", &count, &name) && name && name[0] == '\0') {
                stmt = fast_stmt_new(FAST_STMT_REPEAT, line->line_no, text);
            }
        }
        if (!stmt || !fast_parse_expr_or_unsupported(count, &stmt->expr)) {
            fast_stmt_free(stmt);
            free(inner);
            free(body);
            free(count);
            free(name);
            return 0;
        }
        free(inner);
        free(body);
        free(count);
        free(name);
        parser->pos++;
        if (!fast_parse_block(parser, &stmt->body, 1)) {
            fast_stmt_free(stmt);
            return 0;
        }
        fast_stmt_list_add(out, stmt);
        return 1;
    }

    return 0;
}

static int fast_parse_block(FastParser *parser, FastStmtList *out, int needs_end) {
    while (parser->pos < parser->count) {
        const char *text = parser->items[parser->pos].text;
        if (strcmp(text, "end") == 0) {
            parser->pos++;
            return 1;
        }
        if (starts_with(text, "elif ") || strcmp(text, "else:") == 0 || starts_with(text, "for each ") ||
            starts_with(text, "def ") || starts_with(text, "ask ") || starts_with(text, "read ") ||
            starts_with(text, "write ") || starts_with(text, "put ") || starts_with(text, "remove ") ||
            starts_with(text, "empty ") || starts_with(text, "import ")) {
            return 0;
        }
        if (!fast_parse_statement(parser, out)) {
            return 0;
        }
    }
    return needs_end ? 0 : 1;
}

static int fast_parse_source(const char *src, FastStmtList *program) {
    FastParser parser;
    parser.items = NULL;
    parser.count = 0;
    parser.cap = 0;
    parser.pos = 0;
    parser.unsupported = 0;
    fast_stmt_list_init(program);

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
            fast_parser_add_line(&parser, line_no, trimmed);
        }
        free(without_comment);
        line = strtok(NULL, "\n");
    }
    free(work);

    int ok = fast_parse_block(&parser, program, 0) && parser.pos == parser.count;
    fast_parser_free(&parser);
    if (!ok) {
        fast_stmt_list_free(program);
    }
    return ok;
}

static void fast_collect_expr_names(FastExpr *expr, NameList *names) {
    if (!expr) {
        return;
    }
    if (expr->kind == FAST_EXPR_NAME) {
        names_add(names, expr->text);
    }
    fast_collect_expr_names(expr->left, names);
    fast_collect_expr_names(expr->right, names);
}

static void fast_collect_stmt_names(FastStmtList *list, NameList *names) {
    for (size_t i = 0; i < list->count; i++) {
        FastStmt *stmt = list->items[i];
        if (stmt->name) {
            names_add(names, stmt->name);
        }
        fast_collect_expr_names(stmt->expr, names);
        fast_collect_stmt_names(&stmt->body, names);
    }
}

static char *fast_expr_to_c(FastExpr *expr) {
    if (expr->kind == FAST_EXPR_NUMBER || expr->kind == FAST_EXPR_NAME) {
        return xstrdup(expr->text);
    }

    char *left = fast_expr_to_c(expr->left);
    char *right = fast_expr_to_c(expr->right);
    char *out = NULL;
    if (strcmp(expr->op, "and") == 0) {
        out = format_text("((%s) != 0.0 && (%s) != 0.0)", left, right);
    } else if (strcmp(expr->op, "or") == 0) {
        out = format_text("((%s) != 0.0 || (%s) != 0.0)", left, right);
    } else {
        out = format_text("((%s) %s (%s))", left, expr->op, right);
    }
    free(left);
    free(right);
    return out;
}

static void fast_emit_indent(StringBuilder *out, int indent) {
    for (int i = 0; i < indent; i++) {
        sb_append(out, "    ");
    }
}

static void fast_emit_line(StringBuilder *out, int indent, const char *fmt, ...) {
    fast_emit_indent(out, indent);
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed >= 0) {
        char *buf = (char *)xmalloc((size_t)needed + 1);
        vsnprintf(buf, (size_t)needed + 1, fmt, args);
        sb_append(out, buf);
        free(buf);
    }
    va_end(args);
    sb_append(out, "\n");
}

static void fast_emit_stmt_list(StringBuilder *out, FastStmtList *list, int indent, int *temp_counter) {
    for (size_t i = 0; i < list->count; i++) {
        FastStmt *stmt = list->items[i];
        char *expr = stmt->expr ? fast_expr_to_c(stmt->expr) : NULL;
        switch (stmt->kind) {
            case FAST_STMT_SET:
                fast_emit_line(out, indent, "%s = %s;", stmt->name, expr);
                break;
            case FAST_STMT_ADD:
                fast_emit_line(out, indent, "%s += %s;", stmt->name, expr);
                break;
            case FAST_STMT_SUBTRACT:
                fast_emit_line(out, indent, "%s -= %s;", stmt->name, expr);
                break;
            case FAST_STMT_MULTIPLY:
                fast_emit_line(out, indent, "%s *= %s;", stmt->name, expr);
                break;
            case FAST_STMT_DIVIDE:
                fast_emit_line(out, indent, "%s /= %s;", stmt->name, expr);
                break;
            case FAST_STMT_SAY:
                fast_emit_line(out, indent, stmt->without_newline ? "bpp_fast_print_number(%s, 0);" : "bpp_fast_print_number(%s, 1);", expr);
                break;
            case FAST_STMT_IF:
                fast_emit_line(out, indent, "if ((%s) != 0.0) {", expr);
                fast_emit_stmt_list(out, &stmt->body, indent + 1, temp_counter);
                fast_emit_line(out, indent, "}");
                break;
            case FAST_STMT_WHILE:
                fast_emit_line(out, indent, "while ((%s) != 0.0) {", expr);
                fast_emit_stmt_list(out, &stmt->body, indent + 1, temp_counter);
                fast_emit_line(out, indent, "}");
                break;
            case FAST_STMT_FOREVER:
                fast_emit_line(out, indent, "while (1) {");
                fast_emit_stmt_list(out, &stmt->body, indent + 1, temp_counter);
                fast_emit_line(out, indent, "}");
                break;
            case FAST_STMT_REPEAT: {
                int temp = ++(*temp_counter);
                fast_emit_line(out, indent, "for (long __bpp_fast_i_%d = 0, __bpp_fast_limit_%d = (long)(%s); __bpp_fast_i_%d < __bpp_fast_limit_%d; __bpp_fast_i_%d++) {", temp, temp, expr, temp, temp, temp);
                fast_emit_stmt_list(out, &stmt->body, indent + 1, temp_counter);
                fast_emit_line(out, indent, "}");
                break;
            }
            case FAST_STMT_REPEAT_AS: {
                int temp = ++(*temp_counter);
                fast_emit_line(out, indent, "for (long __bpp_fast_i_%d = 1, __bpp_fast_limit_%d = (long)(%s); __bpp_fast_i_%d <= __bpp_fast_limit_%d; __bpp_fast_i_%d++) {", temp, temp, expr, temp, temp, temp);
                fast_emit_line(out, indent + 1, "%s = (double)__bpp_fast_i_%d;", stmt->name, temp);
                fast_emit_stmt_list(out, &stmt->body, indent + 1, temp_counter);
                fast_emit_line(out, indent, "}");
                break;
            }
            case FAST_STMT_STOP:
                fast_emit_line(out, indent, "break;");
                break;
            case FAST_STMT_NEXT:
                fast_emit_line(out, indent, "continue;");
                break;
        }
        free(expr);
    }
}

static int try_compile_fast_source_to_c(const char *source_path, const char *src, StringBuilder *out) {
    FastStmtList program;
    if (!fast_parse_source(src, &program)) {
        return 0;
    }

    NameList names;
    names_init(&names);
    fast_collect_stmt_names(&program, &names);

    sb_append(out, "/* Generated by native B++ fast numeric backend. */\n");
    if (source_path) {
        sb_appendf(out, "/* Source: %s */\n\n", source_path);
    }
    sb_append(out, "#include <math.h>\n#include <stdio.h>\n\n");
    sb_append(out, "static void bpp_fast_print_number(double value, int newline) {\n");
    sb_append(out, "    if (fabs(value - (long long)value) < 0.0000001) printf(\"%lld\", (long long)value);\n");
    sb_append(out, "    else printf(\"%g\", value);\n");
    sb_append(out, "    if (newline) putchar('\\n');\n");
    sb_append(out, "}\n\n");
    sb_append(out, "int main(void) {\n");
    for (size_t i = 0; i < names.count; i++) {
        fast_emit_line(out, 1, "double %s = 0.0;", names.items[i]);
    }
    if (names.count > 0) {
        sb_append(out, "\n");
    }
    int temp_counter = 0;
    fast_emit_stmt_list(out, &program, 1, &temp_counter);
    sb_append(out, "    return 0;\n");
    sb_append(out, "}\n");

    names_free(&names);
    fast_stmt_list_free(&program);
    return 1;
}

static int compile_source_to_c(const char *source_path, const char *src, StringBuilder *out) {
    if (try_compile_fast_source_to_c(source_path, src, out)) {
        return 1;
    }

    Compiler compiler;
    compiler_init(&compiler, source_path);

    char *work = xstrdup(src);
    int line_no = 0;
    int first_code_seen = 0;
    int table_literal_depth = 0;
    int table_literal_line = 0;
    StringBuilder table_literal;
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
            if (table_literal_depth > 0) {
                sb_append(&table_literal, " ");
                sb_append(&table_literal, trimmed);
                table_literal_depth += brace_delta(trimmed);
                if (table_literal_depth <= 0) {
                    compile_statement(&compiler, table_literal_line, table_literal.data);
                    sb_free(&table_literal);
                    table_literal_depth = 0;
                }
                free(without_comment);
                line = strtok(NULL, "\n");
                continue;
            }
            if (compiler.skip_depth > 0) {
                if (strcmp(trimmed, "end") == 0) {
                    compiler.skip_depth--;
                } else if (statement_starts_block(trimmed)) {
                    compiler.skip_depth++;
                }
            } else {
                if (starts_with(trimmed, "<bpp ")) {
                    if (first_code_seen) {
                        compile_error(&compiler, line_no, trimmed, "B++ directives must be the first code line.", "put <bpp unpackage os> before any code");
                    } else if (strcmp(trimmed, "<bpp unpackage os>") == 0) {
                        compiler.os_enabled = 1;
                        first_code_seen = 1;
                    } else if (starts_with(trimmed, "<bpp unpackage ")) {
                        compile_error(&compiler, line_no, trimmed, "Unknown B++ module.", "available module: os");
                        first_code_seen = 1;
                    } else {
                        compile_error(&compiler, line_no, trimmed, "Invalid B++ directive.", "<bpp unpackage os>");
                        first_code_seen = 1;
                    }
                } else {
                    first_code_seen = 1;
                    int delta = brace_delta(trimmed);
                    if (delta > 0) {
                        sb_init(&table_literal);
                        sb_append(&table_literal, trimmed);
                        table_literal_depth = delta;
                        table_literal_line = line_no;
                    } else {
                        compile_statement(&compiler, line_no, trimmed);
                    }
                }
            }
        }
        free(without_comment);
        line = strtok(NULL, "\n");
    }

    if (table_literal_depth > 0) {
        compile_error(&compiler, table_literal_line, table_literal.data, "Missing closing } for table literal.", NULL);
        sb_free(&table_literal);
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

static char g_update_error[256];

static void set_update_error(const char *message) {
    snprintf(g_update_error, sizeof(g_update_error), "%s", message);
}

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
        set_update_error("could not reach the GitHub Releases API");
        return 0;
    }

    info->tag = json_string_after(json, "\"tag_name\"");
    if (!info->tag) {
        set_update_error("latest GitHub Release was not found; create a published release, not only a tag");
        free(json);
        return 0;
    }

    info->asset_url = json_release_asset_url(json, "bpp.exe");
    free(json);

    if (!info->asset_url) {
        set_update_error("latest GitHub Release is missing the required bpp.exe asset");
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
        fprintf(stderr, "bpp: could not check GitHub Releases for updates: %s.\n", g_update_error);
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
        fprintf(stderr, "bpp: could not check GitHub Releases for updates: %s.\n", g_update_error);
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

static char *quote_shell_arg(const char *text) {
    StringBuilder out;
    sb_init(&out);
    sb_append(&out, "\"");
    for (size_t i = 0; text[i]; i++) {
        if (text[i] == '"') {
            sb_append(&out, "\\\"");
        } else {
            char tmp[2] = {text[i], '\0'};
            sb_append(&out, tmp);
        }
    }
    sb_append(&out, "\"");
    char *result = xstrdup(out.data);
    sb_free(&out);
    return result;
}

static char *temp_path_with_ext(const char *tag, const char *ext) {
    char dir[4096];
#ifdef _WIN32
    DWORD got = GetTempPathA((DWORD)sizeof(dir), dir);
    if (got == 0 || got >= sizeof(dir)) {
        snprintf(dir, sizeof(dir), ".\\");
    }
    int pid = (int)GetCurrentProcessId();
    long tick = (long)GetTickCount();
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) {
        tmp = "/tmp";
    }
    snprintf(dir, sizeof(dir), "%s/", tmp);
    int pid = (int)(time(NULL) % 100000);
    long tick = (long)clock();
#endif
    return format_text("%sbpp_%s_%d_%ld_%lld%s", dir, tag, pid, tick, (long long)time(NULL), ext);
}

#ifdef _WIN32
static const char *find_vsdevcmd(void) {
    static const char *const candidates[] = {
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\BuildTools\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Professional\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\17\\Community\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\17\\BuildTools\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\17\\Professional\\Common7\\Tools\\VsDevCmd.bat",
        "C:\\Program Files\\Microsoft Visual Studio\\17\\Enterprise\\Common7\\Tools\\VsDevCmd.bat"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        DWORD attrs = GetFileAttributesA(candidates[i]);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidates[i];
        }
    }
    return NULL;
}

static int write_windows_build_script(const char *script_path, const char *c_path, const char *exe_path) {
    FILE *script = fopen(script_path, "w");
    if (!script) {
        return 0;
    }
    const char *vsdevcmd = find_vsdevcmd();
    fprintf(script, "@echo off\r\n");
    fprintf(script, "where cl >nul 2>nul\r\n");
    fprintf(script, "if not errorlevel 1 goto build\r\n");
    if (vsdevcmd) {
        fprintf(script, "call \"%s\" -arch=x64 >nul\r\n", vsdevcmd);
        fprintf(script, "if errorlevel 1 exit /b %%errorlevel%%\r\n");
    } else {
        fprintf(script, "echo bpp: C compiler not found. Open a Visual Studio Developer Command Prompt or install GCC/Clang.\r\n");
        fprintf(script, "exit /b 1\r\n");
    }
    fprintf(script, ":build\r\n");
    fprintf(script, "set \"BPP_BUILD_LOG=%%TEMP%%\\bpp_build_%%RANDOM%%.log\"\r\n");
    fprintf(script, "set \"BPP_BUILD_OBJ=%%TEMP%%\\bpp_build_%%RANDOM%%.obj\"\r\n");
    fprintf(script, "set \"BPP_BUILD_PDB=%%TEMP%%\\bpp_build_%%RANDOM%%.pdb\"\r\n");
    fprintf(script, "cl /nologo /Fe:\"%s\" /Fo\"%%BPP_BUILD_OBJ%%\" /Fd\"%%BPP_BUILD_PDB%%\" \"%s\" > \"%%BPP_BUILD_LOG%%\" 2>&1\r\n", exe_path, c_path);
    fprintf(script, "set BPP_BUILD_CODE=%%errorlevel%%\r\n");
    fprintf(script, "if not \"%%BPP_BUILD_CODE%%\"==\"0\" type \"%%BPP_BUILD_LOG%%\"\r\n");
    fprintf(script, "del \"%%BPP_BUILD_OBJ%%\" >nul 2>nul\r\n");
    fprintf(script, "del \"%%BPP_BUILD_PDB%%\" >nul 2>nul\r\n");
    fprintf(script, "del \"%%BPP_BUILD_LOG%%\" >nul 2>nul\r\n");
    fprintf(script, "exit /b %%BPP_BUILD_CODE%%\r\n");
    fclose(script);
    return 1;
}
#endif

static int build_c_executable(const char *c_path, const char *exe_path) {
    int status = 1;
    const char *cc = getenv("BPP_CC");
    if (cc && cc[0]) {
        char *quoted_c = quote_shell_arg(c_path);
        char *quoted_exe = quote_shell_arg(exe_path);
        char *command = format_text("%s %s -o %s -lm", cc, quoted_c, quoted_exe);
        status = system(command);
        free(command);
        free(quoted_c);
        free(quoted_exe);
        return status == 0;
    }

#ifdef _WIN32
    char *script_path = temp_path_with_ext("build", ".cmd");
    if (!write_windows_build_script(script_path, c_path, exe_path)) {
        fprintf(stderr, "bpp: could not create temporary build script.\n");
        free(script_path);
        return 0;
    }
    char *quoted_script = quote_shell_arg(script_path);
    char *command = format_text("cmd /d /s /c %s", quoted_script);
    status = system(command);
    DeleteFileA(script_path);
    free(command);
    free(quoted_script);
    free(script_path);
#else
    char *quoted_c = quote_shell_arg(c_path);
    char *quoted_exe = quote_shell_arg(exe_path);
    char *command = format_text("cc %s -o %s -lm", quoted_c, quoted_exe);
    status = system(command);
    free(command);
    free(quoted_c);
    free(quoted_exe);
#endif
    return status == 0;
}

static int run_executable(const char *exe_path) {
    char *quoted_exe = quote_shell_arg(exe_path);
    int status = system(quoted_exe);
    free(quoted_exe);
    return status;
}

static void print_help(void) {
    printf("B++ compiler %s\n", BPP_VERSION);
    printf("Usage: bpp <source.bpp> [options]\n");
    printf("\n");
    printf("Runs B++ by compiling through C behind the scenes.\n");
    printf("\n");
    printf("Build options:\n");
    printf("  bpp file.bpp             compile and run a B++ program\n");
    printf("  bpp file.bpp -o file.c   write generated C source\n");
    printf("  bpp file.bpp --emit-c    print generated C source\n");
    printf("  bpp file.bpp --exe app   build a native executable\n");
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
    const char *c_output_path = NULL;
    const char *exe_output_path = NULL;
    int emit_c = 0;
    int run_after_build = 1;

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
        if (strcmp(argv[i], "run") == 0 && !source_path) {
            run_after_build = 1;
            continue;
        }
        if (strcmp(argv[i], "build") == 0 && !source_path) {
            run_after_build = 0;
            continue;
        }
        if (strcmp(argv[i], "--emit-c") == 0 || strcmp(argv[i], "emit-c") == 0) {
            emit_c = 1;
            run_after_build = 0;
            continue;
        }
        if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
            exe_output_path = argv[++i];
            run_after_build = 0;
            continue;
        }
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            c_output_path = argv[++i];
            run_after_build = 0;
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

    if (c_output_path) {
        if (!write_file(c_output_path, generated.data)) {
            sb_free(&generated);
            return 1;
        }
        printf("Compiled %s -> %s\n", source_path, c_output_path);
        sb_free(&generated);
        return 0;
    }

    if (emit_c) {
        fputs(generated.data, stdout);
        sb_free(&generated);
        return 0;
    }

    if (!run_after_build && !exe_output_path) {
        fprintf(stderr, "bpp: build needs --exe output.exe\n");
        sb_free(&generated);
        return 2;
    }

    const char *temp_exe_ext =
#ifdef _WIN32
        ".exe";
#else
        "";
#endif
    char *temp_c_path = temp_path_with_ext("run", ".c");
    char *built_exe_path = exe_output_path ? xstrdup(exe_output_path) : temp_path_with_ext("run", temp_exe_ext);
    int using_temp_exe = exe_output_path ? 0 : 1;

    if (!write_file(temp_c_path, generated.data)) {
        sb_free(&generated);
        free(temp_c_path);
        free(built_exe_path);
        return 1;
    }
    sb_free(&generated);

    if (!build_c_executable(temp_c_path, built_exe_path)) {
        fprintf(stderr, "bpp: could not build native executable from generated C.\n");
        remove(temp_c_path);
        if (using_temp_exe) {
            remove(built_exe_path);
        }
        free(temp_c_path);
        free(built_exe_path);
        return 1;
    }

    remove(temp_c_path);

    if (!run_after_build) {
        printf("Built %s -> %s\n", source_path, built_exe_path);
        free(temp_c_path);
        free(built_exe_path);
        return 0;
    }

    int run_status = run_executable(built_exe_path);
    if (using_temp_exe) {
        remove(built_exe_path);
    }
    free(temp_c_path);
    free(built_exe_path);
    return run_status == 0 ? 0 : 1;
}
