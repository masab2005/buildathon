#include "move.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double dist2d_m(double ax, double ay, double bx, double by) {
    return hypot(bx - ax, by - ay);
}

static double dist3d_m(double ax, double ay, double az, double bx, double by, double bz) {
    double dx = bx - ax, dy = by - ay, dz = bz - az;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

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
) {
    out->move_x = 0.0;
    out->move_y = 0.0;
    out->boost = false;
    out->jump = false;

    double d_next = dist2d_m(px, py, z_nx, z_ny);
    double goal_vx = 0.0, goal_vy = 0.0;

    bool going_for_drop = false;
    double drop_x = 0.0, drop_y = 0.0;
    const JsonValue *drops = json_get(obs, "drops");
    for (size_t i = 0; i < json_arr_len(drops); i++) {
        const JsonValue *dr = json_arr_get(drops, i);
        double lands_in = json_num(json_get(dr, "lands_in"), 999.0);
        const JsonValue *dp = json_get(dr, "pos");
        double dx = json_num(json_arr_get(dp, 0), 0.0);
        double dy = json_num(json_arr_get(dp, 1), 0.0);
        double d_drop = dist2d_m(px, py, dx, dy);
        const JsonValue *holder = json_get(dr, "holder");
        bool free_or_ours = (!holder || holder->type == JSON_NULL || json_int(holder, -1) == b->slot);
        if (lands_in <= 12.0 && free_or_ours && d_drop < 120.0 && dist2d_m(dx, dy, z_nx, z_ny) < z_nr * 0.92) {
            if (best_threat_id < 0 || dist2d_m(px, py, b->threats[best_threat_id].pos[0], b->threats[best_threat_id].pos[1]) > 50.0) {
                going_for_drop = true;
                drop_x = dx; drop_y = dy;
                break;
            }
        }
    }

    bool reloading_or_charging = (w_reload[0] > 0.0) || (w_kind[1] && strcmp(w_kind[1], "laser") == 0 && w_charge[1] > 0.0 && w_charge[1] < 0.65);

    if (zone_urgent) {
        double ang = atan2(z_ny - py, z_nx - px);
        double gx = z_nx - 0.6 * z_nr * cos(ang);
        double gy = z_ny - 0.6 * z_nr * sin(ang);
        if (tick - b->cached_wp_tick >= 5) {
            map_next_waypoint(&b->map, px, py, gx, gy, &b->cached_wp[0], &b->cached_wp[1]);
            b->cached_wp_tick = tick;
        }
        double d = dist2d_m(px, py, b->cached_wp[0], b->cached_wp[1]) + 1e-6;
        goal_vx = (b->cached_wp[0] - px) / d;
        goal_vy = (b->cached_wp[1] - py) / d;
        if (zone_critical && energy > 25.0) out->boost = true;
    } else if (t < b->kite_until) {
        double zx = z_nx - px, zy = z_ny - py;
        double zl = hypot(zx, zy) + 1e-6;
        goal_vx = 0.75 * b->kite_dir[0] + 0.25 * (zx / zl);
        goal_vy = 0.75 * b->kite_dir[1] + 0.25 * (zy / zl);
        if (energy > 35.0) out->boost = true;
    } else if (going_for_drop) {
        if (tick - b->cached_wp_tick >= 5) {
            map_next_waypoint(&b->map, px, py, drop_x, drop_y, &b->cached_wp[0], &b->cached_wp[1]);
            b->cached_wp_tick = tick;
        }
        double d = dist2d_m(px, py, b->cached_wp[0], b->cached_wp[1]) + 1e-6;
        goal_vx = (b->cached_wp[0] - px) / d;
        goal_vy = (b->cached_wp[1] - py) / d;
    } else if (best_threat_id >= 0 && reloading_or_charging) {
        ThreatMech *tm = &b->threats[best_threat_id];
        double best_c_score = -1e9;
        double cx_best = px, cy_best = py;
        for (int k = 0; k < 8; k++) {
            double ang = k * (M_PI / 4.0);
            double cx = px + 14.0 * cos(ang), cy = py + 14.0 * sin(ang);
            if (dist2d_m(cx, cy, z_nx, z_ny) > z_nr * 0.95) continue;
            double cz = map_terrain_height(&b->map, cx, cy);
            if (map_blocked_at(&b->map, cx, cy, cz + 1.2, 7.5)) continue;
            bool vis = map_los_clear(&b->map, cx, cy, cz + 6.0, tm->pos[0], tm->pos[1], tm->pos[2] + 6.0);
            double sc = vis ? -50.0 : 50.0;
            if (sc > best_c_score) { best_c_score = sc; cx_best = cx; cy_best = cy; }
        }
        double d = dist2d_m(px, py, cx_best, cy_best) + 1e-6;
        goal_vx = (cx_best - px) / d;
        goal_vy = (cy_best - py) / d;
    } else if (best_threat_id >= 0) {
        ThreatMech *tm = &b->threats[best_threat_id];
        double dx = tm->pos[0] - px, dy = tm->pos[1] - py;
        double d = hypot(dx, dy) + 1e-6;
        double dir = ((tick / 25) % 2 == 0) ? 1.0 : -1.0;
        double sx = -dy / d * dir, sy = dx / d * dir;
        double desired_d = (w_kind[1] && strcmp(w_kind[1], "lightning") == 0) ? 35.0 : 65.0;
        double radial = (d > desired_d + 12.0) ? 0.4 : (d < desired_d - 12.0 ? -0.45 : 0.0);
        goal_vx = sx + radial * (dx / d);
        goal_vy = sy + radial * (dy / d);
        if (enemy_charging && energy > 25.0) out->boost = true;
    } else {
        double best_loot_d = (b->stance == STANCE_SQUAD) ? 35.0 : 50.0;
        bool found_loot = false;
        const JsonValue *cubes = json_get(obs, "cubes");
        for (size_t i = 0; i < json_arr_len(cubes); i++) {
            const JsonValue *cp = json_get(json_arr_get(cubes, i), "pos");
            double cx = json_num(json_arr_get(cp, 0), 0.0);
            double cy = json_num(json_arr_get(cp, 1), 0.0);
            double d = dist2d_m(px, py, cx, cy);
            if (d < best_loot_d) {
                best_loot_d = d;
                found_loot = true;
                goal_vx = (cx - px) / (d + 1e-6);
                goal_vy = (cy - py) / (d + 1e-6);
            }
        }
        if (!found_loot && w_kind[1] == NULL) {
            const JsonValue *pickups = json_get(obs, "pickups");
            for (size_t i = 0; i < json_arr_len(pickups); i++) {
                const JsonValue *pp = json_get(json_arr_get(pickups, i), "pos");
                double cx = json_num(json_arr_get(pp, 0), 0.0);
                double cy = json_num(json_arr_get(pp, 1), 0.0);
                double d = dist2d_m(px, py, cx, cy);
                if (d < best_loot_d) {
                    best_loot_d = d;
                    found_loot = true;
                    goal_vx = (cx - px) / (d + 1e-6);
                    goal_vy = (cy - py) / (d + 1e-6);
                }
            }
        }
        if (!found_loot) {
            if (d_next > z_nr * 0.5) {
                goal_vx = (z_nx - px) / (d_next + 1e-6);
                goal_vy = (z_ny - py) / (d_next + 1e-6);
            } else {
                double ang = atan2(py - z_ny, px - z_nx) + 1.4;
                goal_vx = cos(ang);
                goal_vy = sin(ang);
            }
        }
    }

    double glen = hypot(goal_vx, goal_vy);
    if (glen > 1e-6) {
        double base_ang = atan2(goal_vy, goal_vx);
        const JsonValue *projs = json_get(obs, "projectiles");
        size_t nproj = json_arr_len(projs);
        double best_h_score = -1e9;
        for (int k = 0; k < 8; k++) {
            double ang = base_ang + k * (M_PI / 4.0);
            double hx = cos(ang), hy = sin(ang);
            double score = cos(ang - base_ang) * 10.0;
            if (map_blocked_at(&b->map, px + hx * 4.5, py + hy * 4.5, pz, 8.5)) {
                score -= 100.0;
            } else if (b->map.nav_cover) {
                int cix = (int)((px + hx * 4.0 - b->map.min_x) / b->map.nav_step);
                int ciy = (int)((py + hy * 4.0 - b->map.min_y) / b->map.nav_step);
                if (cix >= 0 && ciy >= 0 && cix < b->map.nav_nx && ciy < b->map.nav_ny) {
                    uint8_t cov = b->map.nav_cover[ciy * b->map.nav_nx + cix];
                    bool reloading = (w_reload[0] > 0.0);
                    double cov_w = (t < b->kite_until || reloading) ? 2.5 : (b->stance == STANCE_SQUAD ? 1.2 : 0.4);
                    score += cov * cov_w;
                }
            }
            double cx = px + hx * 4.0, cy = py + hy * 4.0;
            for (int id = 0; id < 16; id++) {
                if (!b->threats[id].active) continue;
                if (map_los_clear(&b->map, cx, cy, pz + 6.0, b->threats[id].pos[0], b->threats[id].pos[1], b->threats[id].pos[2] + 6.0)) {
                    score -= (t < b->kite_until) ? 12.0 : 3.5;
                }
            }
            for (size_t p = 0; p < nproj; p++) {
                const JsonValue *pr = json_arr_get(projs, p);
                const JsonValue *pp = json_get(pr, "pos");
                const JsonValue *pv = json_get(pr, "vel");
                double prx = json_num(json_arr_get(pp, 0), 0.0);
                double pry = json_num(json_arr_get(pp, 1), 0.0);
                double pvx = json_num(json_arr_get(pv, 0), 0.0);
                double pvy = json_num(json_arr_get(pv, 1), 0.0);
                double nx = px + hx * 2.0, ny = py + hy * 2.0;
                double d_proj = dist2d_m(nx, ny, prx + pvx * 0.1, pry + pvy * 0.1);
                if (d_proj < 5.0) score -= (5.0 - d_proj) * 4.0;
            }
            if (score > best_h_score) {
                best_h_score = score;
                out->move_x = hx;
                out->move_y = hy;
            }
        }
        if (best_h_score < -50.0 && grounded) {
            out->jump = true;
            if (energy > 30.0) out->boost = true;
        }
    }

    if (glen > 0.2 && dist3d_m(px, py, pz, b->last_pos[0], b->last_pos[1], b->last_pos[2]) < 0.12) {
        b->stuck_ticks++;
        if (b->stuck_ticks > 8) {
            if (grounded) out->jump = true;
            if (energy > 25.0) out->boost = true;
        }
    } else {
        b->stuck_ticks = 0;
    }
    b->last_pos[0] = px; b->last_pos[1] = py; b->last_pos[2] = pz;
}
