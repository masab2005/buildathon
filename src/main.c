#define _POSIX_C_SOURCE 200809L
#include "bot.h"
#include "json.h"
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *read_line_buf(FILE *f, char **buf, size_t *cap) {
    if (!*buf) {
        *cap = 65536;
        *buf = (char *)malloc(*cap);
        if (!*buf) return NULL;
    }
    size_t len = 0;
    while (fgets(*buf + len, (int)(*cap - len), f)) {
        len += strlen(*buf + len);
        if (len > 0 && (*buf)[len - 1] == '\n') {
            return *buf;
        }
        if (len + 1 >= *cap) {
            *cap *= 2;
            char *nb = (char *)realloc(*buf, *cap);
            if (!nb) return NULL;
            *buf = nb;
        }
    }
    return len > 0 ? *buf : NULL;
}

static bool stdin_has_more(void) {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

int main(void) {
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOFBF, 65536);

    char *line = NULL;
    size_t line_cap = 0;
    BotState bot;
    bool initialized = false;
    char out_buf[4096];

    while (read_line_buf(stdin, &line, &line_cap)) {
        // Drain any newer queued lines so we always answer the newest obs
        while (stdin_has_more()) {
            if (!read_line_buf(stdin, &line, &line_cap)) break;
        }

        JsonValue *msg = json_parse(line);
        if (!msg) continue;

        const char *type = json_str(json_get(msg, "type"), "");
        if (strcmp(type, "hello") == 0) {
            bot_init(&bot, msg);
            initialized = true;
            printf("{\"type\":\"ready\",\"name\":\"apex-mech\"}\n");
            fflush(stdout);
        } else if (strcmp(type, "obs") == 0) {
            if (!initialized) {
                memset(&bot, 0, sizeof(bot));
                initialized = true;
            }
            bot_step(&bot, msg, out_buf, sizeof(out_buf));
            fputs(out_buf, stdout);
            fflush(stdout);
        } else if (strcmp(type, "end") == 0) {
            json_free(msg);
            break;
        }
        json_free(msg);
    }

    if (initialized) bot_free(&bot);
    free(line);
    return 0;
}
