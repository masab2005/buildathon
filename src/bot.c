#include "bot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double wrap_angle(double a) {
    while (a > M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

static double clampd(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double dist2d(double ax, double ay, double bx, double by) {
    return hypot(bx - ax, by - ay);
}

static double dist3d(double ax, double ay, double az, double bx, double by, double bz) {
    double dx = bx - ax, dy = by - ay, dz = bz - az;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

void bot_init(BotState *b, const JsonValue *hello) {
    memset(b, 0, sizeof(*b));
    b->slot = json_int(json_get(hello, "slot"), 0);
    b->rng = (uint64_t)json_num(json_get(hello, "seed"), 12345.0);
    if (b->rng == 0) b->rng = 123456789ULL;
    b->stance = STANCE_BRAWL;
    b->prev_alive = 16;
    b->last_damage_t = -100.0;
    b->last_los_t = -100.0;
    b->kite_until = -100.0;
    b->committed_target_id = -1;
    b->cached_wp_tick = -100;

    const JsonValue *map_obj = json_get(hello, "map");
    const char *map_dir = json_str(json_get(map_obj, "dir"), "/arena");
    map_load(&b->map, map_dir);
    fprintf(stderr, "apex-mech: map loaded=%d prisms=%d\n", b->map.loaded, b->map.n_prisms);
}

void bot_free(BotState *b) {
    map_free(&b->map);
}

static int pick_upgrade(const JsonValue *offer) {
    const JsonValue *choices = json_get(offer, "choices");
    size_t n = json_arr_len(choices);
    if (n == 0) return -1;

    static const char *prio[] = {
        "armor", "max_hp", "hp", "regen", "nanites", "plating",
        "servo", "jet", "speed", "boost",
        "gun_rate", "gun_dmg", "laser_dmg", "laser_charge",
        "mortar_dmg", "lightning_dmg", "sensors"
    };
    int best_idx = 0;
    int best_score = 999;
    for (size_t i = 0; i < n; i++) {
        const char *id = json_str(json_get(json_arr_get(choices, i), "id"), "");
        int score = 100;
        for (size_t p = 0; p < sizeof(prio) / sizeof(prio[0]); p++) {
            if (strstr(id, prio[p]) != NULL) {
                score = (int)p;
                break;
            }
        }
        if (score < best_score) {
            best_score = score;
            best_idx = (int)i;
        }
    }
    return best_idx;
}

static bool solve_mortar_pitch(double horiz_dist, double dz, double v, double g, double *pitch_out) {
    double v2 = v * v;
    double disc = v2 * v2 - g * (g * horiz_dist * horiz_dist + 2.0 * dz * v2);
    if (disc < 0.0 || horiz_dist < 1.0) return false;
    double sq = sqrt(disc);
    double ang = atan2(v2 - sq, g * horiz_dist);
    *pitch_out = clampd(ang, -1.2, 1.35);
    return true;
}

void bot_step(BotState *b, const JsonValue *obs, char *out_buf, size_t out_cap) {
    int tick = json_int(json_get(obs, "tick"), 0);
    double t = json_num(json_get(obs, "t"), 0.0);
    int alive = json_int(json_get(obs, "alive"), 16);

    const JsonValue *self = json_get(obs, "self");
    const JsonValue *pos_arr = json_get(self, "pos");
    double px = json_num(json_arr_get(pos_arr, 0), 0.0);
    double py = json_num(json_arr_get(pos_arr, 1), 0.0);
    double pz = json_num(json_arr_get(pos_arr, 2), 0.0);

    const JsonValue *aim_arr = json_get(self, "aim");
    double cur_yaw = json_num(json_arr_get(aim_arr, 0), b->last_aim_yaw);
    double cur_pitch = json_num(json_arr_get(aim_arr, 1), b->last_aim_pitch);

    double hp = json_num(json_get(self, "hp"), 1400.0);
    double max_hp = json_num(json_get(self, "max_hp"), 1400.0);
    double energy = json_num(json_get(self, "energy"), 100.0);
    bool grounded = json_bool(json_get(self, "grounded"), true);
    bool in_zone = json_bool(json_get(self, "in_zone"), true);

    const JsonValue *stats = json_get(self, "stats");
    double muzzle_h = json_num(json_get(stats, "muzzle_height"), 7.0);

    // Weapons
    const JsonValue *weapons = json_get(self, "weapons");
    const char *w_kind[2] = { NULL, NULL };
    double w_cd[2] = { 0.0, 0.0 };
    int w_ammo[2] = { 0, 0 };
    double w_reload[2] = { 0.0, 0.0 };
    double w_charge[2] = { 0.0, 0.0 };
    double w_speed[2] = { 252.0, 252.0 };
    double w_range[2] = { 230.0, 230.0 };
    int w_mag[2] = { 12, 12 };

    for (int i = 0; i < 2; i++) {
        const JsonValue *w = json_arr_get(weapons, (size_t)i);
        if (w && w->type == JSON_OBJECT) {
            w_kind[i] = json_str(json_get(w, "kind"), NULL);
            w_cd[i] = json_num(json_get(w, "cooldown"), 0.0);
            w_ammo[i] = json_int(json_get(w, "ammo"), 0);
            w_reload[i] = json_num(json_get(w, "reload"), 0.0);
            w_charge[i] = json_num(json_get(w, "charge"), 0.0);
            const JsonValue *ws = json_get(w, "stats");
            if (ws) {
                w_speed[i] = json_num(json_get(ws, "speed"), 252.0);
                w_range[i] = json_num(json_get(ws, "range"), 230.0);
                w_mag[i] = json_int(json_get(ws, "mag"), 12);
            }
        }
    }

    // Events (track damage, kills, lobby stance)
    int kills_this_tick = 0;
    const JsonValue *events = json_get(obs, "events");
    size_t nev = json_arr_len(events);
    for (size_t i = 0; i < nev; i++) {
        const JsonValue *ev = json_arr_get(events, i);
        const char *etype = json_str(json_get(ev, "type"), "");
        if (strcmp(etype, "damaged") == 0) {
            const JsonValue *br = json_get(ev, "bearing");
            if (br && br->type == JSON_NUMBER) {
                b->last_damage_t = t;
                b->last_damage_bearing = br->u.number;
            }
            int by = json_int(json_get(ev, "by"), -1);
            if (by >= 0 && by < 16) {
                b->threats[by].last_hit_us_t = t;
                b->attackers_mask |= (1 << by);
            }
        } else if (strcmp(etype, "kill") == 0) {
            kills_this_tick++;
            b->our_kills++;
        }
    }

    // Lobby detection: if alive dropped without our kill, it's BRAWL; if 2+ attackers before our first kill, SQUAD
    if (alive < b->prev_alive - kills_this_tick) {
        b->stance = STANCE_BRAWL;
    } else if (b->our_kills == 0 && __builtin_popcount((unsigned int)b->attackers_mask) >= 2 && t > 20.0) {
        b->stance = STANCE_SQUAD;
    }
    b->prev_alive = alive;

    // Zone timeline model
    const JsonValue *zone = json_get(obs, "zone");
    const JsonValue *zc = json_get(zone, "center");
    const JsonValue *znc = json_get(zone, "next_center");
    double z_cx = json_num(json_arr_get(zc, 0), 0.0);
    double z_cy = json_num(json_arr_get(zc, 1), 0.0);
    double z_r = json_num(json_get(zone, "radius"), 500.0);
    double z_nx = json_num(json_arr_get(znc, 0), z_cx);
    double z_ny = json_num(json_arr_get(znc, 1), z_cy);
    double z_nr = json_num(json_get(zone, "next_radius"), z_r);
    double shrink_start = json_num(json_get(zone, "shrink_start"), 999.0);
    double shrink_end = json_num(json_get(zone, "shrink_end"), 999.0);

    double d_next = dist2d(px, py, z_nx, z_ny);
    double m_out_next = fmax(0.0, d_next - z_nr * 0.92);
    double travel_s = m_out_next / 11.7 + 2.5;
    double time_to_edge = (t < shrink_start) ? (shrink_start - t + (shrink_end - shrink_start) * 0.3) : fmax(0.0, shrink_end - t);
    bool zone_critical = !in_zone || (dist2d(px, py, z_cx, z_cy) > z_r - 8.0) || (time_to_edge < 2.5 && m_out_next > 0.0);
    bool zone_urgent = zone_critical || (m_out_next > 0.0 && (time_to_edge - travel_s < 9.0));

    // Upgrade offer
    int pick_idx = -1;
    const JsonValue *offer = json_get(obs, "offer");
    if (offer && offer->type == JSON_OBJECT) {
        pick_idx = pick_upgrade(offer);
    }

    // Update threat table from visible mechs
    const JsonValue *mechs = json_get(obs, "mechs");
    size_t nm = json_arr_len(mechs);

    bool has_npc = false;
    double npc_x = 0, npc_y = 0, npc_z = 0, npc_vx = 0, npc_vy = 0, npc_vz = 0;
    double best_npc_d = 90.0;

    for (size_t i = 0; i < nm; i++) {
        const JsonValue *m = json_arr_get(mechs, i);
        const char *kind = json_str(json_get(m, "kind"), "");
        const JsonValue *mp = json_get(m, "pos");
        const JsonValue *mv = json_get(m, "vel");
        double mx = json_num(json_arr_get(mp, 0), 0.0);
        double my = json_num(json_arr_get(mp, 1), 0.0);
        double mz = json_num(json_arr_get(mp, 2), 0.0);
        double mvx = json_num(json_arr_get(mv, 0), 0.0);
        double mvy = json_num(json_arr_get(mv, 1), 0.0);
        double mvz = json_num(json_arr_get(mv, 2), 0.0);

        if (strcmp(kind, "mech") == 0) {
            int id = json_int(json_get(m, "id"), -1);
            if (id >= 0 && id < 16) {
                ThreatMech *tm = &b->threats[id];
                if (tm->active && t - tm->pred_t > 0.15 && t - tm->pred_t < 0.6) {
                    double es = dist2d(mx, my, tm->pred_static[0], tm->pred_static[1]);
                    double el = dist2d(mx, my, tm->pred_lead[0], tm->pred_lead[1]);
                    tm->err_static = tm->err_static * 0.7 + es * 0.3;
                    tm->err_lead = tm->err_lead * 0.7 + el * 0.3;
                }
                tm->active = true;
                tm->pos[0] = mx; tm->pos[1] = my; tm->pos[2] = mz;
                tm->vel[0] = mvx; tm->vel[1] = mvy; tm->vel[2] = mvz;
                const JsonValue *ma = json_get(m, "aim");
                tm->aim[0] = json_num(json_arr_get(ma, 0), 0.0);
                tm->aim[1] = json_num(json_arr_get(ma, 1), 0.0);
                tm->hp = json_num(json_get(m, "hp"), 1400.0);
                tm->max_hp = json_num(json_get(m, "max_hp"), 1400.0);
                tm->last_seen_t = t;
                tm->charging = json_bool(json_get(m, "charging"), false);
                tm->has_mortar = tm->has_laser = tm->has_lightning = false;
                const JsonValue *mw = json_get(m, "weapons");
                for (size_t k = 0; k < json_arr_len(mw); k++) {
                    const char *wk = json_str(json_arr_get(mw, k), "");
                    if (strcmp(wk, "mortar") == 0) tm->has_mortar = true;
                    if (strcmp(wk, "laser") == 0) tm->has_laser = true;
                    if (strcmp(wk, "lightning") == 0) tm->has_lightning = true;
                }
            }
        } else if (strcmp(kind, "npc") == 0) {
            double d = dist2d(px, py, mx, my);
            if (d < best_npc_d) {
                best_npc_d = d;
                has_npc = true;
                npc_x = mx; npc_y = my; npc_z = mz;
                npc_vx = mvx; npc_vy = mvy; npc_vz = mvz;
            }
        }
    }

    // Evaluate active threats (5s memory) with advantage gate & target commitment
    int engaged_count = 0;
    bool enemy_charging = false;
    int best_threat_id = -1;
    double best_threat_score = 1e9;
    bool any_los = false;
    double enemy_dps_on_us = 0.0;

    for (int id = 0; id < 16; id++) {
        ThreatMech *tm = &b->threats[id];
        if (!tm->active || t - tm->last_seen_t > 5.0) {
            tm->active = false;
            if (b->committed_target_id == id) b->committed_target_id = -1;
            continue;
        }
        double d = dist2d(px, py, tm->pos[0], tm->pos[1]);
        double to_us = atan2(py - tm->pos[1], px - tm->pos[0]);
        bool aiming_at_us = fabs(wrap_angle(tm->aim[0] - to_us)) < 0.45;
        bool engaged = aiming_at_us || (t - tm->last_hit_us_t < 3.0);
        bool los = map_los_clear(&b->map, px, py, pz + muzzle_h, tm->pos[0], tm->pos[1], tm->pos[2] + 4.5);
        if (los) any_los = true;
        if (engaged && d < 120.0) {
            engaged_count++;
            if (los) enemy_dps_on_us += (d < 45.0 && tm->has_lightning) ? 220.0 : 160.0;
        }
        if (tm->charging && d < 220.0) enemy_charging = true;

        double hp_frac = tm->hp / fmax(tm->max_hp, 1.0);
        double score = d + hp_frac * 90.0 + (los ? 0.0 : 80.0) - (engaged ? 15.0 : 0.0);
        if (id == b->committed_target_id) score -= 45.0; // Target commitment bonus
        if (score < best_threat_score) {
            best_threat_score = score;
            best_threat_id = id;
        }
    }
    if (best_threat_id >= 0) b->committed_target_id = best_threat_id;
    if (any_los) b->last_los_t = t;

    double our_eff_dps = 185.0;
    if (w_kind[1] && strcmp(w_kind[1], "lightning") == 0 && best_threat_id >= 0 &&
        dist2d(px, py, b->threats[best_threat_id].pos[0], b->threats[best_threat_id].pos[1]) < 45.0) {
        our_eff_dps += 180.0;
    }
    bool target_low = (best_threat_id >= 0 && b->threats[best_threat_id].hp < b->threats[best_threat_id].max_hp * 0.30);
    bool bad_trade = (enemy_dps_on_us > 0.0 && our_eff_dps < enemy_dps_on_us * 1.15 && !target_low);

    // Trigger LOS-breaking retreat + heal cycle when focus-fired, bad trade, or low HP
    double kite_hp_thresh = (b->stance == STANCE_SQUAD) ? 0.55 : 0.45;
    if (!zone_critical && (engaged_count >= 2 || bad_trade || (best_threat_id >= 0 && hp < max_hp * kite_hp_thresh))) {
        if (t > b->kite_until) {
            b->kite_until = t + 6.0;
            // Find a walkable cell that breaks LOS to all active threats
            double best_retreat_score = -1e9;
            double rx = px, ry = py;
            for (int k = 0; k < 16; k++) {
                double ang = k * (2.0 * M_PI / 16.0);
                double cx = px + 35.0 * cos(ang);
                double cy = py + 35.0 * sin(ang);
                if (dist2d(cx, cy, z_nx, z_ny) > z_nr * 0.95) continue;
                double cz = map_terrain_height(&b->map, cx, cy);
                if (map_blocked_at(&b->map, cx, cy, cz + 1.2, 7.5)) continue;
                int visible_to = 0;
                for (int id = 0; id < 16; id++) {
                    if (!b->threats[id].active) continue;
                    if (map_los_clear(&b->map, cx, cy, cz + 6.0, b->threats[id].pos[0], b->threats[id].pos[1], b->threats[id].pos[2] + 6.0)) {
                        visible_to++;
                    }
                }
                double sc = -visible_to * 100.0 - dist2d(cx, cy, z_nx, z_ny) * 0.1;
                if (sc > best_retreat_score) {
                    best_retreat_score = sc;
                    rx = cx; ry = cy;
                }
            }
            double dl = dist2d(px, py, rx, ry) + 1e-6;
            b->kite_dir[0] = (rx - px) / dl;
            b->kite_dir[1] = (ry - py) / dl;
        }
    }
    // End kite early if we healed and are clean for 5.2s
    if (t < b->kite_until && (t - b->last_damage_t >= 5.2) && hp >= max_hp * 0.78) {
        b->kite_until = t;
    }

    // Choose desired movement goal
    double goal_vx = 0.0, goal_vy = 0.0;
    bool boost = false;
    bool jump = false;

    if (zone_urgent) {
        double ang = atan2(z_ny - py, z_nx - px);
        double gx = z_nx - 0.6 * z_nr * cos(ang);
        double gy = z_ny - 0.6 * z_nr * sin(ang);
        if (tick - b->cached_wp_tick >= 5) {
            map_next_waypoint(&b->map, px, py, gx, gy, &b->cached_wp[0], &b->cached_wp[1]);
            b->cached_wp_tick = tick;
        }
        double d = dist2d(px, py, b->cached_wp[0], b->cached_wp[1]) + 1e-6;
        goal_vx = (b->cached_wp[0] - px) / d;
        goal_vy = (b->cached_wp[1] - py) / d;
        if (zone_critical && energy > 25.0) boost = true;
    } else if (t < b->kite_until) {
        double zx = z_nx - px, zy = z_ny - py;
        double zl = hypot(zx, zy) + 1e-6;
        goal_vx = 0.75 * b->kite_dir[0] + 0.25 * (zx / zl);
        goal_vy = 0.75 * b->kite_dir[1] + 0.25 * (zy / zl);
        if (energy > 35.0) boost = true;
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
        if (enemy_charging && energy > 25.0) boost = true;
    } else {
        // Safe economy: cubes or pickups nearby, else patrol toward 0.6 * next_radius
        double best_loot_d = (b->stance == STANCE_SQUAD) ? 35.0 : 50.0;
        bool found_loot = false;
        const JsonValue *cubes = json_get(obs, "cubes");
        for (size_t i = 0; i < json_arr_len(cubes); i++) {
            const JsonValue *cp = json_get(json_arr_get(cubes, i), "pos");
            double cx = json_num(json_arr_get(cp, 0), 0.0);
            double cy = json_num(json_arr_get(cp, 1), 0.0);
            double d = dist2d(px, py, cx, cy);
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
                double d = dist2d(px, py, cx, cy);
                if (d < best_loot_d) {
                    best_loot_d = d;
                    found_loot = true;
                    goal_vx = (cx - px) / (d + 1e-6);
                    goal_vy = (cy - py) / (d + 1e-6);
                }
            }
        }
        if (!found_loot && d_next > z_nr * 0.6) {
            goal_vx = (z_nx - px) / (d_next + 1e-6);
            goal_vy = (z_ny - py) / (d_next + 1e-6);
        }
    }

    // Local 8-heading steering with obstacle & projectile avoidance
    double move_x = 0.0, move_y = 0.0;
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
            }
            // Penalize exposure to active threats (hug cover)
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
                double d_proj = dist2d(nx, ny, prx + pvx * 0.1, pry + pvy * 0.1);
                if (d_proj < 5.0) score -= (5.0 - d_proj) * 4.0;
            }
            if (score > best_h_score) {
                best_h_score = score;
                move_x = hx;
                move_y = hy;
            }
        }
        if (best_h_score < -50.0 && grounded) {
            jump = true;
            if (energy > 30.0) boost = true;
        }
    }

    // Anti-stuck
    if (glen > 0.2 && dist3d(px, py, pz, b->last_pos[0], b->last_pos[1], b->last_pos[2]) < 0.12) {
        b->stuck_ticks++;
        if (b->stuck_ticks > 8) {
            if (grounded) jump = true;
            if (energy > 25.0) boost = true;
        }
    } else {
        b->stuck_ticks = 0;
    }
    b->last_pos[0] = px; b->last_pos[1] = py; b->last_pos[2] = pz;

    // Take weapon pickup if within 3.8m and slot 1 is empty
    int take_id = -1;
    if (w_kind[1] == NULL) {
        const JsonValue *pickups = json_get(obs, "pickups");
        for (size_t i = 0; i < json_arr_len(pickups); i++) {
            const JsonValue *pk = json_arr_get(pickups, i);
            const JsonValue *pp = json_get(pk, "pos");
            double cx = json_num(json_arr_get(pp, 0), 0.0);
            double cy = json_num(json_arr_get(pp, 1), 0.0);
            double cz = json_num(json_arr_get(pp, 2), 0.0);
            if (dist2d(px, py, cx, cy) < 3.8 && fabs(pz - cz) < 3.8) {
                const char *pk_kind = json_str(json_get(pk, "kind"), "");
                if (strcmp(pk_kind, "mortar") != 0 && (!w_kind[0] || strcmp(pk_kind, w_kind[0]) != 0)) {
                    take_id = json_int(json_get(pk, "id"), -1);
                    break;
                }
            }
        }
    }

    // Aiming & Fire Discipline
    double aim_yaw = cur_yaw;
    double aim_pitch = 0.0;
    bool fire[2] = { false, false };
    bool reload = false;

    bool have_aim_target = false;
    double tx = 0, ty = 0, tz = 0, tvx = 0, tvy = 0, tvz = 0;
    bool use_lead = true;

    if (best_threat_id >= 0) {
        ThreatMech *tm = &b->threats[best_threat_id];
        have_aim_target = true;
        tx = tm->pos[0]; ty = tm->pos[1]; tz = tm->pos[2];
        tvx = tm->vel[0]; tvy = tm->vel[1]; tvz = tm->vel[2];
        use_lead = (tm->err_lead <= tm->err_static + 0.5);

        double d3 = dist3d(px, py, pz + muzzle_h, tx, ty, tz + 4.5);
        double tl = d3 / 252.0 + 0.07;
        tm->pred_static[0] = tx;
        tm->pred_static[1] = ty;
        tm->pred_lead[0] = tx + tvx * tl;
        tm->pred_lead[1] = ty + tvy * tl;
        tm->pred_t = t;
    } else if (has_npc && !zone_urgent) {
        have_aim_target = true;
        tx = npc_x; ty = npc_y; tz = npc_z;
        tvx = npc_vx; tvy = npc_vy; tvz = npc_vz;
    }

    if (have_aim_target) {
        double d3 = dist3d(px, py, pz + muzzle_h, tx, ty, tz + 4.5);
        double d2 = dist2d(px, py, tx, ty);
        double t_lead = use_lead ? (d3 / 252.0 + 0.07) : 0.0;
        double lx = tx + tvx * t_lead;
        double ly = ty + tvy * t_lead;
        double lz = tz + 4.5 + tvz * t_lead;

        aim_yaw = atan2(ly - py, lx - px);
        aim_pitch = atan2(lz - (pz + muzzle_h), hypot(lx - px, ly - py));

        bool los = map_los_clear(&b->map, px, py, pz + muzzle_h, lx, ly, lz);
        double aim_err = fabs(wrap_angle(aim_yaw - cur_yaw));
        double ang_size = atan2(2.8, fmax(d3, 5.0)) + 0.05;

        for (int s = 0; s < 2; s++) {
            if (!w_kind[s]) continue;
            if (strcmp(w_kind[s], "gun") == 0) {
                if (los && d3 < w_range[s] && aim_err < ang_size && w_cd[s] <= 0.0 && w_ammo[s] > 0 && w_reload[s] <= 0.0) {
                    fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "laser") == 0) {
                if (los && d3 < w_range[s] && t > b->kite_until) {
                    if (w_charge[s] < 0.72) fire[s] = true;
                    else if (aim_err < 0.06) fire[s] = false;
                    else fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "lightning") == 0) {
                if (los && d3 < 43.0 && aim_err < 0.45 && energy > 15.0) {
                    fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "mortar") == 0) {
                double mpitch = 0.0;
                if (!fire[0] && d2 > 18.0 && d2 < 215.0 && solve_mortar_pitch(d2, (tz + 2.0) - (pz + 9.5), w_speed[s], 22.0, &mpitch)) {
                    aim_pitch = mpitch;
                    if (aim_err < 0.15 && fabs(mpitch - cur_pitch) < 0.15) {
                        fire[s] = true;
                    }
                }
            }
        }
    } else if (t - b->last_damage_t < 2.0) {
        aim_yaw = b->last_damage_bearing;
        aim_pitch = 0.0;
    } else {
        b->scan_yaw = wrap_angle(cur_yaw + 0.18);
        aim_yaw = b->scan_yaw;
        aim_pitch = 0.0;
    }

    if ((t - b->last_los_t > 1.0) && w_kind[0] && strcmp(w_kind[0], "gun") == 0 && w_ammo[0] < w_mag[0] && w_reload[0] <= 0.0) {
        reload = true;
    }

    b->last_aim_yaw = aim_yaw;
    b->last_aim_pitch = clampd(aim_pitch, -1.3, 1.3);

    int pos = snprintf(out_buf, out_cap,
        "{\"tick\":%d,\"move\":[%.3f,%.3f],\"boost\":%s,\"jump\":%s,\"aim\":[%.4f,%.4f],\"fire\":[%s,%s],\"reload\":%s",
        tick, move_x, move_y,
        boost ? "true" : "false",
        jump ? "true" : "false",
        b->last_aim_yaw, b->last_aim_pitch,
        fire[0] ? "true" : "false",
        fire[1] ? "true" : "false",
        reload ? "true" : "false");

    if (pick_idx >= 0 && pos < (int)out_cap - 32) {
        pos += snprintf(out_buf + pos, out_cap - (size_t)pos, ",\"pick\":%d", pick_idx);
    }
    if (take_id >= 0 && pos < (int)out_cap - 32) {
        pos += snprintf(out_buf + pos, out_cap - (size_t)pos, ",\"take\":%d", take_id);
    }
    snprintf(out_buf + pos, out_cap - (size_t)pos, "}\n");
}
