#ifndef BOT_H
#define BOT_H

#include "json.h"
#include "map.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STANCE_BRAWL = 0,
    STANCE_SQUAD = 1
} LobbyStance;

typedef struct {
    bool active;
    double pos[3];
    double vel[3];
    double aim[2];
    double hp;
    double max_hp;
    double last_seen_t;
    double last_hit_us_t;
    bool charging;
    bool has_mortar;
    bool has_laser;
    bool has_lightning;
    // Adaptive lead tracking
    double pred_static[2];
    double pred_lead[2];
    double pred_t;
    double err_static;
    double err_lead;
} ThreatMech;

typedef struct {
    int slot;
    uint64_t rng;
    WorldMap map;
    LobbyStance stance;
    int prev_alive;
    int our_kills;
    int attackers_mask;

    ThreatMech threats[16];

    double last_aim_yaw;
    double last_aim_pitch;
    double last_damage_t;
    double last_damage_bearing;
    double last_los_t;
    double kite_until;
    double kite_dir[2];

    double last_pos[3];
    int stuck_ticks;
    double scan_yaw;
} BotState;

void bot_init(BotState *b, const JsonValue *hello);
void bot_free(BotState *b);
void bot_step(BotState *b, const JsonValue *obs, char *out_buf, size_t out_cap);

#endif
