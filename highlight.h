#ifndef HIGHLIGHT_H
#define HIGHLIGHT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Token types returned by the highlighter */
enum HLType {
    HL_NORMAL = 0,
    HL_KEYWORD,
    HL_TYPE,
    HL_STORAGE_CLASS,
    HL_CONSTANT,
    HL_NUMBER,
    HL_STRING,
    HL_COMMENT,
    HL_PREPROC,
    HL_INCLUDE_SYSTEM,
    HL_INCLUDE_THIRD_PARTY,
    HL_INCLUDE_LOCAL,
    HL_SYMBOL,
    HL_NEWLINE,
    HL_FUNCTION,
    HL_ESCAPE,
    HL_FORMAT,
};

struct HLToken {
    enum HLType type;
    size_t start; /* byte offset in the line */
    size_t len;   /* length in bytes */
};

/* Stateful highlighter state (for multi-line comments/strings) */
struct HLState {
    int in_comment; /* inside a C-style block comment */
    int in_string;  /* inside a multi-line string (rare in C, but for other langs) */
    char quote;     /* ' or " for string delim */
};

/* Highlight a single line. Returns number of tokens filled (<= max_tokens).
   The tokens are non-overlapping and cover only recognized tokens (not the whole line). */
int highlight_line(const char *line, struct HLToken *tokens, int max_tokens);

/* Stateful version: provides and updates `state` across lines */
int highlight_line_state(const char *line, struct HLToken *tokens, int max_tokens, struct HLState *state);

/* Utility to write a colorized version of a line to stdout */
void colorize_line(const char *line);
void colorize_line_state(const char *line, struct HLState *state);

/* Set current language for highlighting ("c", "python", "javascript", ...). Returns 0 on success. */
int set_language(const char *lang);

#ifdef __cplusplus
}
#endif

#endif /* HIGHLIGHT_H */
