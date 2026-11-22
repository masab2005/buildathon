#ifndef COMBAT_H
#define COMBAT_H

#include "bot.h"
#include <stdbool.h>

typedef struct {
    double aim_yaw;
    double aim_pitch;
    bool fire[2];
    bool reload;
} CombatOut;

void combat_decide(
    BotState *b,
    double t,
    double px, double py, double pz,
    double cur_yaw, double cur_pitch,
    double muzzle_h, double energy,
    const char *w_kind[2], const double w_cd[2], const int w_ammo[2],
    const double w_reload[2], const double w_charge[2],
    const double w_speed[2], const double w_range[2], const int w_mag[2],
    int best_threat_id,
    bool has_npc, double npc_x, double npc_y, double npc_z,
    double npc_vx, double npc_vy, double npc_vz,
    bool zone_urgent,
    CombatOut *out
);

#endif
