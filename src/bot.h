#ifndef BOT_H
#define BOT_H

#include "json.h"
#include "map.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int slot;
    uint64_t rng;
    WorldMap map;
    double last_aim_yaw;
    double last_aim_pitch;
    double last_damage_t;
    double last_damage_bearing;
    double last_pos[3];
    int stuck_ticks;
    double scan_yaw;
} BotState;

void bot_init(BotState *b, const JsonValue *hello);
void bot_free(BotState *b);
void bot_step(BotState *b, const JsonValue *obs, char *out_buf, size_t out_cap);

#endif
