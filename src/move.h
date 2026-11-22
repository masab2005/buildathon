#ifndef MOVE_H
#define MOVE_H

#include "bot.h"
#include "json.h"
#include <stdbool.h>

typedef struct {
    double move_x;
    double move_y;
    bool boost;
    bool jump;
} MoveOut;

void move_decide(
    BotState *b,
    const JsonValue *obs,
    int tick, double t,
    double px, double py, double pz,
    double energy, bool grounded,
    double z_nx, double z_ny, double z_nr,
    bool zone_urgent, bool zone_critical,
    int best_threat_id, bool enemy_charging,
    const char *w_kind[2], const double w_reload[2], const double w_charge[2],
    MoveOut *out
);

#endif
