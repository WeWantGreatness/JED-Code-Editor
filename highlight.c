#include "highlight.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Keyword lists are provided via `keyword_highlight/generated_index.c` and
 * accessed through `kh_find()` / `set_language()`. We initialize pointers
 * lazily at startup by selecting the default language ("c"). */
/* Pointers to current language arrays (set by set_language) */
static const char **g_type = NULL;
static const char **g_storage = NULL;
static const char **g_statement = NULL;
static const char **g_conditional = NULL;
static const char **g_repeat = NULL;
static const char **g_operator = NULL;
static const char **g_constant = NULL;
static const char **g_typedef = NULL;
static const char **g_structure = NULL;
/* current language name for classifiers */
static char g_langname[64] = "";

static int arr_contains(const char *id, const char *arr[]) {
    if (!arr) return 0;
    for (int i = 0; arr[i]; i++) if (strcmp(id, arr[i]) == 0) return 1;
    return 0;
}

#include "keyword_highlight/generated_index.h"

int set_language(const char *lang) {
    if (!lang) return -1;
    const struct KH *k = kh_find(lang);
    if (!k) return -1;
    g_type = k->type;
    g_storage = k->storage;
    g_statement = k->statement;
    g_conditional = k->conditional;
    g_repeat = k->repeat;
    g_operator = k->operator;
    g_constant = k->constant;
    g_typedef = k->typedef_;
    g_structure = k->structure;
    /* store language for classifiers */
    strncpy(g_langname, lang, sizeof(g_langname)-1);
    return 0;
}

    #include "include_classifier.h"
/* Use standard 8/16 ANSI color codes for broad terminal compatibility. */
#define C_RESET "\x1b[0m"
/* bright colors (ANSI bright variants) */
#define C_KEYWORD "\x1b[93m"       /* bright yellow */
#define C_TYPE "\x1b[96m"          /* bright cyan */
#define C_STORAGE "\x1b[95m"       /* bright magenta */
#define C_CONST "\x1b[96m"         /* bright cyan */
#define C_NUMBER "\x1b[92m"        /* bright green */
#define C_STRING "\x1b[32m"        /* green */
#define C_COMMENT "\x1b[90m"       /* bright black / gray */
#define C_PREPROC "\x1b[94m"       /* bright blue */
#define C_FUNCTION "\x1b[91m"      /* bright red */
/* Include-specific colors */
#define C_INCLUDE_STD "\x1b[94m"   /* bright blue */
#define C_INCLUDE_THIRD "\x1b[95m" /* magenta */
#define C_INCLUDE_LOCAL "\x1b[96m" /* cyan */
/* Symbol/punctuation */
#define C_SYMBOL "\x1b[97m"        /* bright white */
/* Newline color */
#define C_NEWLINE "\x1b[90m"        /* dim */

static int is_in_list(const char *s, const char *arr[]) {
    if (!arr) return 0;
    for (int i = 0; arr[i]; i++) if (strcmp(s, arr[i]) == 0) return 1;
    return 0;
}

/* No default language is selected at startup. The editor should call
 * `set_language()` based on the current file's type/extension. This keeps
 * the highlighter language-agnostic until the user opens or creates a file.
 */

/* Small list of common C standard includes (used to classify <...> includes) */
static const char *c_std_includes[] = {
    "stdio.h", "stdlib.h", "string.h", "stdint.h", "stddef.h", "stdbool.h",
    "inttypes.h", "math.h", "time.h", "assert.h", "errno.h", NULL
};

static int is_std_include(const char *path, size_t len) {
    /* find basename after last '/' (if any) */
    const char *p = path + len - 1;
    while (p >= path && *p != '/') p--;
    const char *base = (p >= path && *p == '/') ? p + 1 : path;
    size_t baselen = (path + len) - base;
    for (int i = 0; c_std_includes[i]; i++) {
        if (strlen(c_std_includes[i]) == baselen && strncmp(base, c_std_includes[i], baselen) == 0) return 1;
    }
    return 0;
}

/* Determine if identifier at s is followed by '(' (function heuristic) */
static int looks_like_function(const char *s, const char *line_end) {
    const char *p = s;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
    while (p < line_end && isspace((unsigned char)*p)) p++;
    return (p < line_end && *p == '(');
}

int highlight_line_state(const char *line, struct HLToken *tokens, int max_tokens, struct HLState *state) {
    const char *p = line;
    const char *end = line + strlen(line);
    int tcount = 0;
    (void)max_tokens; (void)tokens; /* silence unused warnings in different build configs */

    /* If we're already inside a block comment, find its end */
    if (state && state->in_comment) {
        const char *q = strstr(p, "*/");
        if (q) {
            size_t len = (size_t)(q + 2 - p);
            tokens[tcount++] = (struct HLToken){HL_COMMENT, (size_t)(p - line), len};
            p = q + 2;
            state->in_comment = 0;
        } else {
            size_t len = (size_t)(end - p);
            tokens[tcount++] = (struct HLToken){HL_COMMENT, (size_t)(p - line), len};
            return tcount;
        }
    }
    /* If we're inside a string (rare across lines), look for close */
    if (state && state->in_string) {
        const char qch = state->quote;
        const char *q = p;
        while (q < end) {
            if (*q == '\\' && q + 1 < end) q += 2;
            else if (*q == qch) { q++; break; }
            else q++;
        }
        size_t len = (size_t)(q - p);
        tokens[tcount++] = (struct HLToken){HL_STRING, (size_t)(p - line), len};
        if (q < end && *q == qch) state->in_string = 0;
        p = q;
    }

    while (p < end && tcount < max_tokens) {
        /* Preprocessor at line start */
        if ((p == line) && (*p == '#')) {
                /* Try to classify include directives more granularly: split prefix, path and trailing text */
                const char *inc = strstr(p, "include");
                if (inc) {
                    const char *q = inc + strlen("include");
                    while (q < end && isspace((unsigned char)*q)) q++;
                    if (q < end && (*q == '<' || *q == '"')) {
                        char delim = *q;
                        const char *path_start = q + 1;
                        const char *path_end = (delim == '<') ? strchr(path_start, '>') : strchr(path_start, '"');
                        if (path_end) {
                            /* '#include ' prefix */
                            size_t prefix_len = (size_t)(path_start - p - 1);
                            if (prefix_len > 0) tokens[tcount++] = (struct HLToken){HL_PREPROC, (size_t)(p - line), prefix_len};
                            size_t path_len = (size_t)(path_end - path_start);
                            /* classify */
                            enum HLType itype = HL_INCLUDE_SYSTEM;
                            if (delim == '"') itype = HL_INCLUDE_LOCAL;
                            else {
                                if (is_std_include(path_start, path_len)) itype = HL_INCLUDE_SYSTEM;
                                else {
                                    /* heuristic: treat includes with '/' or with uppercase letters as third-party */
                                    int third = 0;
                                    for (const char *r = path_start; r < path_end; r++) { if (*r == '/' || (*r >= 'A' && *r <= 'Z')) { third = 1; break; } }
                                    if (third) itype = HL_INCLUDE_THIRD_PARTY; else itype = HL_INCLUDE_SYSTEM;
                                }
                            }
                            tokens[tcount++] = (struct HLToken){itype, (size_t)(path_start - line), path_len};
                            /* trailing content after the include path */
                            const char *rest = path_end + 1;
                            if (rest < end) {
                                /* If the only trailing character is a newline, emit as HL_NEWLINE */
                                if ((end - rest) == 1 && *rest == '\n') {
                                    tokens[tcount++] = (struct HLToken){HL_NEWLINE, (size_t)(rest - line), 1};
                                } else {
                                    tokens[tcount++] = (struct HLToken){HL_PREPROC, (size_t)(rest - line), (size_t)(end - rest)};
                                }
                            }
                            break;
                        }
                    }
                }
                /* fallback: whole line is preprocessor */
                size_t len = end - p;
                tokens[tcount++] = (struct HLToken){HL_PREPROC, (size_t)(p - line), len};
                break; /* rest of line is preprocessor */
            }
        /* Skip whitespace */
        if (isspace((unsigned char)*p)) { p++; continue; }
        /* Comment */
        if (p[0] == '/' && p[1] == '/') {
            size_t len = (size_t)(end - p);
            tokens[tcount++] = (struct HLToken){HL_COMMENT, (size_t)(p - line), len};
            break;
        }
        if (p[0] == '/' && p[1] == '*') {
            /* find */
            const char *q = strstr(p + 2, "*/");
            if (q) {
                size_t len = (size_t)(q + 2 - p);
                tokens[tcount++] = (struct HLToken){HL_COMMENT, (size_t)(p - line), len};
                p += len;
                continue;
            } else {
                /* runs to next line */
                size_t len = (size_t)(end - p);
                tokens[tcount++] = (struct HLToken){HL_COMMENT, (size_t)(p - line), len};
                if (state) state->in_comment = 1;
                return tcount;
            }
        }
        /* String literal */
        if (*p == '"' || *p == '\'') {
            char qch = *p;
            const char *q = p + 1;
            while (q < end) {
                if (*q == '\\' && q + 1 < end) q += 2;
                else if (*q == qch) { q++; break; }
                else q++;
            }
            size_t len = (size_t)(q - p);
            tokens[tcount++] = (struct HLToken){HL_STRING, (size_t)(p - line), len};
            if (q >= end && state) { state->in_string = 1; state->quote = qch; }
            p = q;
            continue;
        }
        /* Number */
        if (isdigit((unsigned char)*p) || ((*p == '.') && isdigit((unsigned char)p[1]))) {
            const char *q = p;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) q += 2;
            while (q < end && (isalnum((unsigned char)*q) || *q == '.' || *q == '_')) q++;
            size_t len = (size_t)(q - p);
            tokens[tcount++] = (struct HLToken){HL_NUMBER, (size_t)(p - line), len};
            p = q;
            continue;
        }
        /* Identifier or keyword */
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *q = p + 1;
            while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
            size_t len = (size_t)(q - p);
            char *id = (char *)malloc(len + 1);
            if (!id) return tcount;
            memcpy(id, p, len);
            id[len] = '\0';
            if (arr_contains(id, g_statement) || arr_contains(id, g_conditional) || arr_contains(id, g_repeat) || arr_contains(id, g_operator) || arr_contains(id, g_typedef) || arr_contains(id, g_structure)) {
                tokens[tcount++] = (struct HLToken){HL_KEYWORD, (size_t)(p - line), len};
            } else if (arr_contains(id, g_type)) {
                tokens[tcount++] = (struct HLToken){HL_TYPE, (size_t)(p - line), len};
            } else if (arr_contains(id, g_storage)) {
                tokens[tcount++] = (struct HLToken){HL_STORAGE_CLASS, (size_t)(p - line), len};
            } else if (arr_contains(id, g_constant)) {
                tokens[tcount++] = (struct HLToken){HL_CONSTANT, (size_t)(p - line), len};
            } else if (looks_like_function(p, end)) {
                tokens[tcount++] = (struct HLToken){HL_FUNCTION, (size_t)(p - line), len};
            }
            free(id);
            p = q;
            continue;
        }
        /* anything else: single char or punctuation - mark symbols explicitly */
        if (ispunct((unsigned char)*p)) {
            /* single-character symbol token */
            tokens[tcount++] = (struct HLToken){HL_SYMBOL, (size_t)(p - line), 1};
            p++;
            continue;
        }
        p++;
    }
    return tcount;
}

int highlight_line(const char *line, struct HLToken *tokens, int max_tokens) {
    struct HLState st = {0, 0, 0};
    return highlight_line_state(line, tokens, max_tokens, &st);
}

/* Emit colored line using tokens from highlight_line */
void colorize_line(const char *line) {
    struct HLToken tokens[256];
    int n = highlight_line(line, tokens, 256);
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        /* print text before token */
        if (tokens[i].start > pos) {
            fwrite(line + pos, 1, tokens[i].start - pos, stdout);
        }
        /* set color */
        const char *col = C_RESET;
        switch (tokens[i].type) {
            case HL_KEYWORD: col = C_KEYWORD; break;
            case HL_TYPE: col = C_TYPE; break;
            case HL_STORAGE_CLASS: col = C_STORAGE; break;
            case HL_CONSTANT: col = C_CONST; break;
            case HL_NUMBER: col = C_NUMBER; break;
            case HL_STRING: col = C_STRING; break;
            case HL_COMMENT: col = C_COMMENT; break;
            case HL_INCLUDE_SYSTEM: col = C_INCLUDE_STD; break;
            case HL_INCLUDE_THIRD_PARTY: col = C_INCLUDE_THIRD; break;
            case HL_INCLUDE_LOCAL: col = C_INCLUDE_LOCAL; break;
            case HL_PREPROC: col = C_PREPROC; break;
            case HL_FUNCTION: col = C_FUNCTION; break;
            case HL_SYMBOL: col = C_SYMBOL; break;
            default: col = C_RESET; break;
        }
        printf("%s", col);
        fwrite(line + tokens[i].start, 1, tokens[i].len, stdout);
        printf("%s", C_RESET);
        pos = tokens[i].start + tokens[i].len;
    }
    /* remainder -- color newline specially if present at end */
    size_t llen = strlen(line);
    if (pos < llen) {
        if (line[llen - 1] == '\n') {
            /* write text except final newline */
            if (llen - 1 > pos) fwrite(line + pos, 1, (llen - 1) - pos, stdout);
            /* newline with color */
            printf("%s", C_NEWLINE);
            fwrite("\n", 1, 1, stdout);
            printf("%s", C_RESET);
        } else {
            fwrite(line + pos, 1, llen - pos, stdout);
        }
    }
}

void colorize_line_state(const char *line, struct HLState *state) {
    struct HLToken tokens[256];
    int n = highlight_line_state(line, tokens, 256, state);
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        if (tokens[i].start > pos) {
            fwrite(line + pos, 1, tokens[i].start - pos, stdout);
        }
        const char *col = C_RESET;
        switch (tokens[i].type) {
            case HL_KEYWORD: col = C_KEYWORD; break;
            case HL_TYPE: col = C_TYPE; break;
            case HL_STORAGE_CLASS: col = C_STORAGE; break;
            case HL_CONSTANT: col = C_CONST; break;
            case HL_NUMBER: col = C_NUMBER; break;
            case HL_STRING: col = C_STRING; break;
            case HL_COMMENT: col = C_COMMENT; break;
            case HL_INCLUDE_SYSTEM: col = C_INCLUDE_STD; break;
            case HL_INCLUDE_THIRD_PARTY: col = C_INCLUDE_THIRD; break;
            case HL_INCLUDE_LOCAL: col = C_INCLUDE_LOCAL; break;
            case HL_PREPROC: col = C_PREPROC; break;
            case HL_FUNCTION: col = C_FUNCTION; break;            case HL_SYMBOL: col = C_SYMBOL; break;            default: col = C_RESET; break;
        }
        printf("%s", col);
        fwrite(line + tokens[i].start, 1, tokens[i].len, stdout);
        printf("%s", C_RESET);
        pos = tokens[i].start + tokens[i].len;
    }
    size_t llen2 = strlen(line);
    if (pos < llen2) {
        if (line[llen2 - 1] == '\n') {
            if (llen2 - 1 > pos) fwrite(line + pos, 1, (llen2 - 1) - pos, stdout);
            printf("%s", C_NEWLINE);
            fwrite("\n", 1, 1, stdout);
            printf("%s", C_RESET);
        } else {
            fwrite(line + pos, 1, llen2 - pos, stdout);
        }
    }
}
