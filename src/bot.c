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
    b->last_damage_t = -100.0;

    const JsonValue *map_obj = json_get(hello, "map");
    const char *map_dir = json_str(json_get(map_obj, "dir"), "/arena");
    map_load(&b->map, map_dir);
}

void bot_free(BotState *b) {
    map_free(&b->map);
}

static int pick_upgrade(const JsonValue *offer) {
    const JsonValue *choices = json_get(offer, "choices");
    size_t n = json_arr_len(choices);
    if (n == 0) return -1;

    static const char *prio[] = {
        "armor", "max_hp", "hp", "regen",
        "gun_rate", "gun_dmg", "laser_dmg", "laser_charge",
        "mortar_dmg", "lightning_dmg", "speed", "boost", "sensors"
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
    int w_ammo[2] = { 0, 0 };
    double w_reload[2] = { 0.0, 0.0 };
    double w_charge[2] = { 0.0, 0.0 };
    double w_speed[2] = { 252.0, 252.0 };
    double w_range[2] = { 230.0, 230.0 };

    for (int i = 0; i < 2; i++) {
        const JsonValue *w = json_arr_get(weapons, (size_t)i);
        if (w && w->type == JSON_OBJECT) {
            w_kind[i] = json_str(json_get(w, "kind"), NULL);
            w_ammo[i] = json_int(json_get(w, "ammo"), 0);
            w_reload[i] = json_num(json_get(w, "reload"), 0.0);
            w_charge[i] = json_num(json_get(w, "charge"), 0.0);
            const JsonValue *ws = json_get(w, "stats");
            if (ws) {
                w_speed[i] = json_num(json_get(ws, "speed"), 252.0);
                w_range[i] = json_num(json_get(ws, "range"), 230.0);
            }
        }
    }

    // Events (track damage)
    const JsonValue *events = json_get(obs, "events");
    size_t nev = json_arr_len(events);
    for (size_t i = 0; i < nev; i++) {
        const JsonValue *ev = json_arr_get(events, i);
        if (strcmp(json_str(json_get(ev, "type"), ""), "damaged") == 0) {
            const JsonValue *br = json_get(ev, "bearing");
            if (br && br->type == JSON_NUMBER) {
                b->last_damage_t = t;
                b->last_damage_bearing = br->u.number;
            }
        }
    }

    // Zone
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

    // Upgrade offer
    int pick_idx = -1;
    const JsonValue *offer = json_get(obs, "offer");
    if (offer && offer->type == JSON_OBJECT) {
        pick_idx = pick_upgrade(offer);
    }

    // Analyze visible mechs & NPCs
    const JsonValue *mechs = json_get(obs, "mechs");
    size_t nm = json_arr_len(mechs);
    int nearby_enemies = 0;
    bool enemy_charging = false;

    bool has_target = false;
    bool target_is_mech = false;
    double tgt_x = 0, tgt_y = 0, tgt_z = 0, tgt_vx = 0, tgt_vy = 0, tgt_vz = 0;
    double best_score = 1e9;

    for (size_t i = 0; i < nm; i++) {
        const JsonValue *m = json_arr_get(mechs, i);
        const char *kind = json_str(json_get(m, "kind"), "");
        bool is_mech = (strcmp(kind, "mech") == 0);
        const JsonValue *mp = json_get(m, "pos");
        const JsonValue *mv = json_get(m, "vel");
        double mx = json_num(json_arr_get(mp, 0), 0.0);
        double my = json_num(json_arr_get(mp, 1), 0.0);
        double mz = json_num(json_arr_get(mp, 2), 0.0);
        double d = dist2d(px, py, mx, my);

        if (is_mech) {
            if (d < 95.0) nearby_enemies++;
            if (json_bool(json_get(m, "charging"), false)) enemy_charging = true;
        }

        double score = d + (is_mech ? 0.0 : 120.0);
        if (score < best_score) {
            best_score = score;
            has_target = true;
            target_is_mech = is_mech;
            tgt_x = mx; tgt_y = my; tgt_z = mz;
            tgt_vx = json_num(json_arr_get(mv, 0), 0.0);
            tgt_vy = json_num(json_arr_get(mv, 1), 0.0);
            tgt_vz = json_num(json_arr_get(mv, 2), 0.0);
        }
    }

    // Also check crates if no mech/NPC target
    if (!has_target) {
        const JsonValue *crates = json_get(obs, "crates");
        size_t nc = json_arr_len(crates);
        double best_cd = 60.0;
        for (size_t i = 0; i < nc; i++) {
            const JsonValue *c = json_arr_get(crates, i);
            const JsonValue *cp = json_get(c, "pos");
            double cx = json_num(json_arr_get(cp, 0), 0.0);
            double cy = json_num(json_arr_get(cp, 1), 0.0);
            double cz = json_num(json_arr_get(cp, 2), 0.0);
            double d = dist2d(px, py, cx, cy);
            if (d < best_cd && map_los_clear(&b->map, px, py, pz + muzzle_h, cx, cy, cz + 1.5)) {
                best_cd = d;
                has_target = true;
                target_is_mech = false;
                tgt_x = cx; tgt_y = cy; tgt_z = cz - 3.0;
                tgt_vx = tgt_vy = tgt_vz = 0.0;
            }
        }
    }

    // Movement decision
    double move_x = 0.0, move_y = 0.0;
    bool boost = false;
    bool jump = false;

    double d_next = dist2d(px, py, z_nx, z_ny);
    bool need_zone = !in_zone || (t > shrink_start - 15.0 && d_next > z_nr - 20.0) || (dist2d(px, py, z_cx, z_cy) > z_r - 15.0);

    if (need_zone) {
        double ang = atan2(z_ny - py, z_nx - px);
        move_x = cos(ang);
        move_y = sin(ang);
        if (!in_zone && energy > 30.0) boost = true;
    } else if (has_target && target_is_mech && (hp < max_hp * 0.45 || nearby_enemies >= 2)) {
        // Disengage / kite away toward zone center
        double away_x = px - tgt_x;
        double away_y = py - tgt_y;
        double alen = hypot(away_x, away_y) + 1e-6;
        double zx = z_nx - px, zy = z_ny - py;
        double zlen = hypot(zx, zy) + 1e-6;
        move_x = 0.7 * (away_x / alen) + 0.3 * (zx / zlen);
        move_y = 0.7 * (away_y / alen) + 0.3 * (zy / zlen);
        if (energy > 40.0) boost = true;
    } else if (has_target && target_is_mech) {
        // Strafe perpendicular to target
        double dx = tgt_x - px, dy = tgt_y - py;
        double d = hypot(dx, dy) + 1e-6;
        double dir = ((tick / 25) % 2 == 0) ? 1.0 : -1.0;
        double sx = -dy / d * dir;
        double sy = dx / d * dir;
        double radial = (d > 55.0) ? 0.35 : (d < 22.0 ? -0.45 : 0.0);
        move_x = sx + radial * (dx / d);
        move_y = sy + radial * (dy / d);
        if (enemy_charging && energy > 25.0) boost = true;
    } else {
        // Look for nearby cubes or pickups
        double best_loot_d = 45.0;
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
                move_x = (cx - px) / (d + 1e-6);
                move_y = (cy - py) / (d + 1e-6);
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
                    move_x = (cx - px) / (d + 1e-6);
                    move_y = (cy - py) / (d + 1e-6);
                }
            }
        }
        if (!found_loot) {
            // Patrol toward next zone center
            double d = dist2d(px, py, z_nx, z_ny);
            if (d > 25.0) {
                move_x = (z_nx - px) / d;
                move_y = (z_ny - py) / d;
            }
        }
    }

    // Normalize move
    double mlen = hypot(move_x, move_y);
    if (mlen > 1e-6) {
        move_x /= mlen;
        move_y /= mlen;

        // Obstacle avoidance using map
        if (map_blocked_at(&b->map, px + move_x * 4.5, py + move_y * 4.5, pz, 8.5)) {
            double base_ang = atan2(move_y, move_x);
            double offsets[] = { 0.785, -0.785, 1.57, -1.57 };
            bool steered = false;
            for (int k = 0; k < 4; k++) {
                double a = base_ang + offsets[k];
                double tx = cos(a), ty = sin(a);
                if (!map_blocked_at(&b->map, px + tx * 4.5, py + ty * 4.5, pz, 8.5)) {
                    move_x = tx;
                    move_y = ty;
                    steered = true;
                    break;
                }
            }
            if (!steered && grounded) {
                jump = true;
                if (energy > 30.0) boost = true;
            }
        }
    }

    // Anti-stuck
    if (mlen > 0.2 && dist3d(px, py, pz, b->last_pos[0], b->last_pos[1], b->last_pos[2]) < 0.12) {
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
                if (!w_kind[0] || strcmp(pk_kind, w_kind[0]) != 0) {
                    take_id = json_int(json_get(pk, "id"), -1);
                    break;
                }
            }
        }
    }

    // Aiming & Firing
    double aim_yaw = cur_yaw;
    double aim_pitch = 0.0;
    bool fire[2] = { false, false };
    bool reload = false;

    if (has_target) {
        double d3 = dist3d(px, py, pz + muzzle_h, tgt_x, tgt_y, tgt_z + 4.5);
        double d2 = dist2d(px, py, tgt_x, tgt_y);
        double t_lead = d3 / 252.0;
        double lx = tgt_x + tgt_vx * t_lead;
        double ly = tgt_y + tgt_vy * t_lead;
        double lz = tgt_z + 4.5 + tgt_vz * t_lead;

        aim_yaw = atan2(ly - py, lx - px);
        aim_pitch = atan2(lz - (pz + muzzle_h), hypot(lx - px, ly - py));

        bool los = map_los_clear(&b->map, px, py, pz + muzzle_h, lx, ly, lz);
        double aim_err = fabs(wrap_angle(aim_yaw - cur_yaw));

        for (int s = 0; s < 2; s++) {
            if (!w_kind[s]) continue;
            if (strcmp(w_kind[s], "gun") == 0) {
                if (los && d3 < w_range[s] && aim_err < 0.18 && w_ammo[s] > 0 && w_reload[s] <= 0.0) {
                    fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "laser") == 0) {
                if (los && d3 < w_range[s]) {
                    if (w_charge[s] < 0.72) {
                        fire[s] = true; // hold to charge
                    } else if (aim_err < 0.06) {
                        fire[s] = false; // release to fire!
                    } else {
                        fire[s] = true; // hold until aligned
                    }
                }
            } else if (strcmp(w_kind[s], "lightning") == 0) {
                if (los && d3 < 43.0 && aim_err < 0.45 && energy > 15.0) {
                    fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "mortar") == 0) {
                double mpitch = 0.0;
                if (d2 > 18.0 && d2 < 215.0 && solve_mortar_pitch(d2, (tgt_z + 2.0) - (pz + 9.5), w_speed[s], 22.0, &mpitch)) {
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

    if (!has_target && w_kind[0] && strcmp(w_kind[0], "gun") == 0 && w_ammo[0] < 5 && w_reload[0] <= 0.0) {
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
