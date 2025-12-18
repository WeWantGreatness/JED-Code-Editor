#ifndef GENERATED_KEYWORD_INDEX_H
#define GENERATED_KEYWORD_INDEX_H

struct KH {
    const char *name;
    const char **type;
    const char **storage;
    const char **statement;
    const char **conditional;
    const char **repeat;
    const char **operator;
    const char **constant;
    const char **typedef_;
    const char **structure;
};

extern const struct KH kh_languages[];

const struct KH *kh_find(const char *name);

#endif
