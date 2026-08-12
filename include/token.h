#ifndef TOKEN_H
#define TOKEN_H

#define MAX_TOKENS 64
#define MAX_TOKEN_LEN 256

typedef enum {
    TOKEN_WORD,
    TOKEN_PIPE,
    TOKEN_INPUT,
    TOKEN_OUTPUT,
    TOKEN_APPEND,
    TOKEN_BACKGROUND,
    TOKEN_END
} token_type_t;

typedef struct {
    token_type_t type;
    char text[MAX_TOKEN_LEN];
} token_t;

typedef struct {
    token_t tokens[MAX_TOKENS];
    int count;
} token_list_t;

void token_list_init(token_list_t *list);
void token_add(token_list_t *list, token_type_t type, const char *text);
const char *token_name(token_type_t type);
void token_print(const token_list_t *list);

#endif
