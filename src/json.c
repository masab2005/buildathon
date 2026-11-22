#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *p;
} Parser;

static void skip_ws(Parser *ps) {
    while (*ps->p && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static JsonValue *new_val(JsonType t) {
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) v->type = t;
    return v;
}

static JsonValue *parse_value(Parser *ps);

static char *parse_string_raw(Parser *ps) {
    if (*ps->p != '"') return NULL;
    ps->p++;
    size_t cap = 32, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    while (*ps->p && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '\\') {
            char esc = *ps->p++;
            if (!esc) break;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned int code = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*ps->p); i++) {
                        char h = *ps->p++;
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                    }
                    c = (unsigned char)(code & 0xFF);
                    break;
                }
                default: c = (unsigned char)esc; break;
            }
        }
        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (*ps->p == '"') ps->p++;
    buf[len] = '\0';
    return buf;
}

static JsonValue *parse_array(Parser *ps) {
    ps->p++; // '['
    JsonValue *arr = new_val(JSON_ARRAY);
    if (!arr) return NULL;
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return arr; }

    while (*ps->p) {
        JsonValue *item = parse_value(ps);
        if (!item) break;
        if (arr->u.array.count == arr->u.array.cap) {
            size_t ncap = arr->u.array.cap ? arr->u.array.cap * 2 : 8;
            JsonValue **ni = (JsonValue **)realloc(arr->u.array.items, ncap * sizeof(JsonValue *));
            if (!ni) { json_free(item); break; }
            arr->u.array.items = ni;
            arr->u.array.cap = ncap;
        }
        arr->u.array.items[arr->u.array.count++] = item;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; skip_ws(ps); }
        else if (*ps->p == ']') { ps->p++; break; }
        else break;
    }
    return arr;
}

static JsonValue *parse_object(Parser *ps) {
    ps->p++; // '{'
    JsonValue *obj = new_val(JSON_OBJECT);
    if (!obj) return NULL;
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return obj; }

    while (*ps->p) {
        skip_ws(ps);
        if (*ps->p != '"') break;
        char *key = parse_string_raw(ps);
        if (!key) break;
        skip_ws(ps);
        if (*ps->p != ':') { free(key); break; }
        ps->p++;
        JsonValue *val = parse_value(ps);
        if (!val) { free(key); break; }

        if (obj->u.object.count == obj->u.object.cap) {
            size_t ncap = obj->u.object.cap ? obj->u.object.cap * 2 : 8;
            JsonKV *ni = (JsonKV *)realloc(obj->u.object.items, ncap * sizeof(JsonKV));
            if (!ni) { free(key); json_free(val); break; }
            obj->u.object.items = ni;
            obj->u.object.cap = ncap;
        }
        obj->u.object.items[obj->u.object.count].key = key;
        obj->u.object.items[obj->u.object.count].val = val;
        obj->u.object.count++;

        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; }
        else if (*ps->p == '}') { ps->p++; break; }
        else break;
    }
    return obj;
}

static JsonValue *parse_value(Parser *ps) {
    skip_ws(ps);
    char c = *ps->p;
    if (!c) return NULL;
    if (c == '"') {
        JsonValue *v = new_val(JSON_STRING);
        if (v) v->u.string = parse_string_raw(ps);
        return v;
    }
    if (c == '{') return parse_object(ps);
    if (c == '[') return parse_array(ps);
    if (c == 't' && strncmp(ps->p, "true", 4) == 0) {
        ps->p += 4;
        JsonValue *v = new_val(JSON_BOOL);
        if (v) v->u.boolean = true;
        return v;
    }
    if (c == 'f' && strncmp(ps->p, "false", 5) == 0) {
        ps->p += 5;
        JsonValue *v = new_val(JSON_BOOL);
        if (v) v->u.boolean = false;
        return v;
    }
    if (c == 'n' && strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4;
        return new_val(JSON_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end = NULL;
        double num = strtod(ps->p, &end);
        if (end != ps->p) {
            ps->p = end;
            JsonValue *v = new_val(JSON_NUMBER);
            if (v) v->u.number = num;
            return v;
        }
    }
    return NULL;
}

JsonValue *json_parse(const char *src) {
    if (!src) return NULL;
    Parser ps = { src };
    return parse_value(&ps);
}

void json_free(JsonValue *v) {
    if (!v) return;
    if (v->type == JSON_STRING) {
        free(v->u.string);
    } else if (v->type == JSON_ARRAY) {
        for (size_t i = 0; i < v->u.array.count; i++) {
            json_free(v->u.array.items[i]);
        }
        free(v->u.array.items);
    } else if (v->type == JSON_OBJECT) {
        for (size_t i = 0; i < v->u.object.count; i++) {
            free(v->u.object.items[i].key);
            json_free(v->u.object.items[i].val);
        }
        free(v->u.object.items);
    }
    free(v);
}

const JsonValue *json_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (size_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.items[i].key, key) == 0) {
            return obj->u.object.items[i].val;
        }
    }
    return NULL;
}

const char *json_str(const JsonValue *v, const char *def) {
    if (v && v->type == JSON_STRING && v->u.string) return v->u.string;
    return def;
}

double json_num(const JsonValue *v, double def) {
    if (v && v->type == JSON_NUMBER) return v->u.number;
    return def;
}

int json_int(const JsonValue *v, int def) {
    if (v && v->type == JSON_NUMBER) return (int)v->u.number;
    return def;
}

bool json_bool(const JsonValue *v, bool def) {
    if (v && v->type == JSON_BOOL) return v->u.boolean;
    return def;
}

const JsonValue *json_arr_get(const JsonValue *arr, size_t idx) {
    if (!arr || arr->type != JSON_ARRAY || idx >= arr->u.array.count) return NULL;
    return arr->u.array.items[idx];
}

size_t json_arr_len(const JsonValue *arr) {
    if (!arr || arr->type != JSON_ARRAY) return 0;
    return arr->u.array.count;
}
