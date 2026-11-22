#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;
    JsonValue *val;
} JsonKV;

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            size_t count;
            size_t cap;
        } array;
        struct {
            JsonKV *items;
            size_t count;
            size_t cap;
        } object;
    } u;
};

JsonValue *json_parse(const char *src);
void json_free(JsonValue *v);

const JsonValue *json_get(const JsonValue *obj, const char *key);
const char *json_str(const JsonValue *v, const char *def);
double json_num(const JsonValue *v, double def);
int json_int(const JsonValue *v, int def);
bool json_bool(const JsonValue *v, bool def);
const JsonValue *json_arr_get(const JsonValue *arr, size_t idx);
size_t json_arr_len(const JsonValue *arr);

#endif
