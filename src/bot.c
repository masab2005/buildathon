#include "bot.h"
#include "combat.h"
#include "move.h"
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

static double dist2d(double ax, double ay, double bx, double by) {
    return hypot(bx - ax, by - ay);
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

    if (alive < b->prev_alive - kills_this_tick) {
        b->stance = STANCE_BRAWL;
    } else if (b->our_kills == 0 && __builtin_popcount((unsigned int)b->attackers_mask) >= 2 && t > 20.0) {
        b->stance = STANCE_SQUAD;
    }
    b->prev_alive = alive;

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

    int pick_idx = -1;
    const JsonValue *offer = json_get(obs, "offer");
    if (offer && offer->type == JSON_OBJECT) {
        pick_idx = pick_upgrade(offer);
    }

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

    int engaged_count = 0;
    bool enemy_charging = false;
    int best_threat_id = -1;
    double best_threat_score = 1e9;
    bool any_los = false;

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
        if (engaged && d < 120.0) engaged_count++;
        if (tm->charging && d < 220.0) enemy_charging = true;
        if (!los) continue;

        double hp_frac = tm->hp / fmax(tm->max_hp, 1.0);
        double score = d + hp_frac * 90.0 - (engaged ? 15.0 : 0.0);
        if (id == b->committed_target_id) score -= 45.0;
        if (score < best_threat_score) {
            best_threat_score = score;
            best_threat_id = id;
        }
    }
    if (best_threat_id >= 0) b->committed_target_id = best_threat_id;
    if (any_los) b->last_los_t = t;

    double kite_hp_thresh = (b->stance == STANCE_SQUAD) ? 0.48 : 0.38;
    if (!zone_critical && ((engaged_count >= 2 && hp < max_hp * 0.72) || (best_threat_id >= 0 && hp < max_hp * kite_hp_thresh))) {
        if (t > b->kite_until) {
            b->kite_until = t + 6.0;
            double retreat_r = fmin(35.0, fmax(8.0, z_nr * 0.4));
            double best_retreat_score = -1e9;
            double rx = px, ry = py;
            for (int k = 0; k < 16; k++) {
                double ang = k * (2.0 * M_PI / 16.0);
                double cx = px + retreat_r * cos(ang), cy = py + retreat_r * sin(ang);
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
                if (sc > best_retreat_score) { best_retreat_score = sc; rx = cx; ry = cy; }
            }
            double dl = dist2d(px, py, rx, ry);
            if (dl > 0.5) {
                b->kite_dir[0] = (rx - px) / dl;
                b->kite_dir[1] = (ry - py) / dl;
            } else {
                double zd = dist2d(px, py, z_nx, z_ny) + 1e-6;
                b->kite_dir[0] = (z_nx - px) / zd;
                b->kite_dir[1] = (z_ny - py) / zd;
            }
        }
    }
    if (t < b->kite_until && (t - b->last_damage_t >= 5.2) && hp >= max_hp * 0.78) {
        b->kite_until = t;
    }

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

    MoveOut mout;
    move_decide(b, obs, tick, t, px, py, pz, energy, grounded, z_nx, z_ny, z_nr, zone_urgent, zone_critical, best_threat_id, enemy_charging, w_kind, w_reload, w_charge, &mout);

    CombatOut cout;
    combat_decide(b, t, px, py, pz, cur_yaw, cur_pitch, muzzle_h, energy, w_kind, w_cd, w_ammo, w_reload, w_charge, w_speed, w_range, w_mag, best_threat_id, has_npc, npc_x, npc_y, npc_z, npc_vx, npc_vy, npc_vz, zone_urgent, &cout);

    int pos = snprintf(out_buf, out_cap,
        "{\"tick\":%d,\"move\":[%.3f,%.3f],\"boost\":%s,\"jump\":%s,\"aim\":[%.4f,%.4f],\"fire\":[%s,%s],\"reload\":%s",
        tick, mout.move_x, mout.move_y,
        mout.boost ? "true" : "false",
        mout.jump ? "true" : "false",
        b->last_aim_yaw, b->last_aim_pitch,
        cout.fire[0] ? "true" : "false",
        cout.fire[1] ? "true" : "false",
        cout.reload ? "true" : "false");

    if (pick_idx >= 0 && pos < (int)out_cap - 32) {
        pos += snprintf(out_buf + pos, out_cap - (size_t)pos, ",\"pick\":%d", pick_idx);
    }
    if (take_id >= 0 && pos < (int)out_cap - 32) {
        pos += snprintf(out_buf + pos, out_cap - (size_t)pos, ",\"take\":%d", take_id);
    }
    snprintf(out_buf + pos, out_cap - (size_t)pos, "}\n");
}
