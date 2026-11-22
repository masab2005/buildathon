#include "combat.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double wrap_angle_c(double a) {
    while (a > M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

static double clampd_c(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double dist2d_c(double ax, double ay, double bx, double by) {
    return hypot(bx - ax, by - ay);
}

static double dist3d_c(double ax, double ay, double az, double bx, double by, double bz) {
    double dx = bx - ax, dy = by - ay, dz = bz - az;
    return sqrt(dx * dx + dy * dy + dz * dz);
}

static bool solve_mortar_pitch(double horiz_dist, double dz, double v, double g, double *pitch_out) {
    double v2 = v * v;
    double disc = v2 * v2 - g * (g * horiz_dist * horiz_dist + 2.0 * dz * v2);
    if (disc < 0.0 || horiz_dist < 1.0) return false;
    double sq = sqrt(disc);
    double ang = atan2(v2 - sq, g * horiz_dist);
    *pitch_out = clampd_c(ang, -1.2, 1.35);
    return true;
}

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
) {
    out->aim_yaw = cur_yaw;
    out->aim_pitch = 0.0;
    out->fire[0] = false;
    out->fire[1] = false;
    out->reload = false;

    bool have_aim_target = false;
    double tx = 0, ty = 0, tz = 0, tvx = 0, tvy = 0, tvz = 0;
    bool use_lead = true;

    if (best_threat_id >= 0) {
        ThreatMech *tm = &b->threats[best_threat_id];
        have_aim_target = true;
        tx = tm->pos[0]; ty = tm->pos[1]; tz = tm->pos[2];
        tvx = tm->vel[0]; tvy = tm->vel[1]; tvz = tm->vel[2];
        use_lead = (tm->err_lead <= tm->err_static + 0.5);

        double d3 = dist3d_c(px, py, pz + muzzle_h, tx, ty, tz + 4.5);
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
        double d3 = dist3d_c(px, py, pz + muzzle_h, tx, ty, tz + 4.5);
        double d2 = dist2d_c(px, py, tx, ty);
        double t_lead = use_lead ? (d3 / 252.0 + 0.07) : 0.0;
        double lx = tx + tvx * t_lead;
        double ly = ty + tvy * t_lead;
        double lz = tz + 4.5 + tvz * t_lead;

        out->aim_yaw = atan2(ly - py, lx - px);
        out->aim_pitch = atan2(lz - (pz + muzzle_h), hypot(lx - px, ly - py));

        bool los = map_los_clear(&b->map, px, py, pz + muzzle_h, lx, ly, lz);
        double aim_err = fabs(wrap_angle_c(out->aim_yaw - cur_yaw));
        double ang_size = atan2(3.2, fmax(d3, 5.0)) + 0.14;
        bool target_low = (best_threat_id >= 0 && b->threats[best_threat_id].hp < b->threats[best_threat_id].max_hp * 0.35);

        for (int s = 0; s < 2; s++) {
            if (!w_kind[s]) continue;
            if (strcmp(w_kind[s], "gun") == 0) {
                double max_gun_d = (w_kind[1] != NULL || target_low) ? w_range[s] : 150.0;
                if (los && d3 < max_gun_d && aim_err < ang_size && w_cd[s] <= 0.08 && w_ammo[s] > 0 && w_reload[s] <= 0.0) {
                    out->fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "laser") == 0) {
                if (los && d3 < w_range[s]) {
                    if (w_charge[s] < 0.72) out->fire[s] = true;
                    else if (aim_err < 0.06) out->fire[s] = false;
                    else out->fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "lightning") == 0) {
                if (los && d3 < 43.0 && aim_err < 0.45 && energy > 15.0) {
                    out->fire[s] = true;
                }
            } else if (strcmp(w_kind[s], "mortar") == 0) {
                double mpitch = 0.0;
                if (!out->fire[0] && d2 > 18.0 && d2 < 215.0 && solve_mortar_pitch(d2, (tz + 2.0) - (pz + 9.5), w_speed[s], 22.0, &mpitch)) {
                    if (aim_err < 0.15 && fabs(mpitch - cur_pitch) < 0.25) {
                        out->fire[s] = true;
                    }
                }
            }
        }
    } else if (t - b->last_damage_t < 2.0) {
        out->aim_yaw = b->last_damage_bearing;
        out->aim_pitch = 0.0;
    } else {
        b->scan_yaw = wrap_angle_c(cur_yaw + 0.18);
        out->aim_yaw = b->scan_yaw;
        out->aim_pitch = 0.0;
    }

    if ((t - b->last_los_t > 1.0) && w_kind[0] && strcmp(w_kind[0], "gun") == 0 && w_ammo[0] < w_mag[0] && w_reload[0] <= 0.0) {
        out->reload = true;
    }

    b->last_aim_yaw = out->aim_yaw;
    b->last_aim_pitch = clampd_c(out->aim_pitch, -1.3, 1.3);
}
