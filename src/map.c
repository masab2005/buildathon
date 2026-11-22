#include "map.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static void parse_poly(const JsonValue *arr, Polygon2D *poly, double *min_x, double *max_x, double *min_y, double *max_y, bool update_bb) {
    size_t n = json_arr_len(arr);
    poly->n_pts = (int)(n / 2);
    if (poly->n_pts <= 0) { poly->pts = NULL; return; }
    poly->pts = (double *)malloc((size_t)(poly->n_pts * 2) * sizeof(double));
    for (int i = 0; i < poly->n_pts; i++) {
        double x = json_num(json_arr_get(arr, (size_t)(i * 2)), 0.0);
        double y = json_num(json_arr_get(arr, (size_t)(i * 2 + 1)), 0.0);
        poly->pts[i * 2] = x;
        poly->pts[i * 2 + 1] = y;
        if (update_bb) {
            if (x < *min_x) *min_x = x;
            if (x > *max_x) *max_x = x;
            if (y < *min_y) *min_y = y;
            if (y > *max_y) *max_y = y;
        }
    }
}

bool map_load(WorldMap *m, const char *map_dir) {
    memset(m, 0, sizeof(*m));
    if (!map_dir || !*map_dir) return false;

    char path[512];
    snprintf(path, sizeof(path), "%s/v2/world.json", map_dir);
    char *wsrc = read_entire_file(path, NULL);
    if (!wsrc) return false;

    JsonValue *root = json_parse(wsrc);
    free(wsrc);
    if (!root) return false;

    const JsonValue *terr = json_get(root, "terrain");
    if (terr) {
        m->min_x = json_num(json_get(terr, "minX"), -535.0);
        m->min_y = json_num(json_get(terr, "minY"), -362.0);
        m->step = json_num(json_get(terr, "step"), 1.0);
        m->nx = json_int(json_get(terr, "nx"), 1070);
        m->ny = json_int(json_get(terr, "ny"), 724);
    }

    const JsonValue *prisms = json_get(root, "prisms");
    size_t np = json_arr_len(prisms);
    if (np > 0) {
        m->prisms = (Prism *)calloc(np, sizeof(Prism));
        m->n_prisms = (int)np;
        for (size_t i = 0; i < np; i++) {
            const JsonValue *pj = json_arr_get(prisms, i);
            Prism *p = &m->prisms[i];
            p->base = json_num(json_get(pj, "base"), 0.0);
            p->top = json_num(json_get(pj, "top"), 0.0);
            const char *kind = json_str(json_get(pj, "kind"), "");
            p->is_bridge = (strcmp(kind, "bridge") == 0);
            p->min_x = 1e9; p->max_x = -1e9;
            p->min_y = 1e9; p->max_y = -1e9;
            parse_poly(json_get(pj, "ring"), &p->ring, &p->min_x, &p->max_x, &p->min_y, &p->max_y, true);

            const JsonValue *holes = json_get(pj, "holes");
            size_t nh = json_arr_len(holes);
            if (nh > 0) {
                p->holes = (Polygon2D *)calloc(nh, sizeof(Polygon2D));
                p->n_holes = (int)nh;
                for (size_t h = 0; h < nh; h++) {
                    parse_poly(json_arr_get(holes, h), &p->holes[h], NULL, NULL, NULL, NULL, false);
                }
            }
        }
    }
    json_free(root);

    snprintf(path, sizeof(path), "%s/v2/terrain.i16", map_dir);
    size_t tlen = 0;
    char *tbuf = read_entire_file(path, &tlen);
    if (tbuf && (int)tlen >= m->nx * m->ny * 2) {
        m->terrain = (int16_t *)tbuf;
    } else {
        free(tbuf);
    }

    m->loaded = true;

    // Build 4m navigation grid
    m->nav_step = 4.0;
    m->nav_nx = (int)(m->nx * m->step / m->nav_step);
    m->nav_ny = (int)(m->ny * m->step / m->nav_step);
    if (m->nav_nx > 0 && m->nav_ny > 0) {
        m->nav_blocked = (uint8_t *)calloc((size_t)(m->nav_nx * m->nav_ny), 1);
        m->nav_cover = (uint8_t *)calloc((size_t)(m->nav_nx * m->nav_ny), 1);
        m->nav_z = (float *)calloc((size_t)(m->nav_nx * m->nav_ny), sizeof(float));
        for (int iy = 0; iy < m->nav_ny; iy++) {
            double y = m->min_y + (iy + 0.5) * m->nav_step;
            for (int ix = 0; ix < m->nav_nx; ix++) {
                double x = m->min_x + (ix + 0.5) * m->nav_step;
                double z = map_terrain_height(m, x, y);
                double surf_z = z;
                for (int p = 0; p < m->n_prisms; p++) {
                    const Prism *pr = &m->prisms[p];
                    if (!pr->is_bridge && pr->top > surf_z && pr->top - z < 50.0 && map_point_in_prism(pr, x, y)) {
                        surf_z = pr->top;
                    }
                }
                int idx = iy * m->nav_nx + ix;
                m->nav_z[idx] = (float)surf_z;
                if (map_blocked_at(m, x, y, surf_z + 1.2, 7.5)) {
                    m->nav_blocked[idx] = 1;
                }
            }
        }
        // Precompute O(1) cover score (0..8) = number of adjacent/nearby blocked directions
        static const int cdx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
        static const int cdy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
        for (int iy = 2; iy < m->nav_ny - 2; iy++) {
            for (int ix = 2; ix < m->nav_nx - 2; ix++) {
                int idx = iy * m->nav_nx + ix;
                if (m->nav_blocked[idx]) continue;
                uint8_t cov = 0;
                for (int k = 0; k < 8; k++) {
                    if (m->nav_blocked[(iy + cdy[k]) * m->nav_nx + (ix + cdx[k])] ||
                        m->nav_blocked[(iy + 2 * cdy[k]) * m->nav_nx + (ix + 2 * cdx[k])]) {
                        cov++;
                    }
                }
                m->nav_cover[idx] = cov;
            }
        }
    }

    return true;
}

void map_free(WorldMap *m) {
    if (!m) return;
    free(m->terrain);
    free(m->nav_blocked);
    free(m->nav_cover);
    free(m->nav_z);
    for (int i = 0; i < m->n_prisms; i++) {
        free(m->prisms[i].ring.pts);
        for (int h = 0; h < m->prisms[i].n_holes; h++) {
            free(m->prisms[i].holes[h].pts);
        }
        free(m->prisms[i].holes);
    }
    free(m->prisms);
    memset(m, 0, sizeof(*m));
}

double map_terrain_height(const WorldMap *m, double x, double y) {
    if (!m || !m->terrain || m->nx <= 1 || m->ny <= 1) return 0.0;
    double fx = (x - m->min_x) / m->step;
    double fy = (y - m->min_y) / m->step;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    if (fx >= m->nx - 1) fx = m->nx - 1.0001;
    if (fy >= m->ny - 1) fy = m->ny - 1.0001;
    int ix = (int)fx;
    int iy = (int)fy;
    double tx = fx - ix;
    double ty = fy - iy;
    double h00 = m->terrain[iy * m->nx + ix] * 0.01;
    double h10 = m->terrain[iy * m->nx + (ix + 1)] * 0.01;
    double h01 = m->terrain[(iy + 1) * m->nx + ix] * 0.01;
    double h11 = m->terrain[(iy + 1) * m->nx + (ix + 1)] * 0.01;
    double h0 = h00 + (h10 - h00) * tx;
    double h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}

static bool point_in_poly(const Polygon2D *poly, double x, double y) {
    if (!poly || poly->n_pts < 3) return false;
    bool inside = false;
    int n = poly->n_pts;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        double xi = poly->pts[i * 2], yi = poly->pts[i * 2 + 1];
        double xj = poly->pts[j * 2], yj = poly->pts[j * 2 + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

bool map_point_in_prism(const Prism *p, double x, double y) {
    if (x < p->min_x || x > p->max_x || y < p->min_y || y > p->max_y) return false;
    if (!point_in_poly(&p->ring, x, y)) return false;
    for (int h = 0; h < p->n_holes; h++) {
        if (point_in_poly(&p->holes[h], x, y)) return false;
    }
    return true;
}

bool map_blocked_at(const WorldMap *m, double x, double y, double z_feet, double height) {
    if (!m || !m->loaded) return false;
    double z_top = z_feet + height;
    for (int i = 0; i < m->n_prisms; i++) {
        const Prism *p = &m->prisms[i];
        if (z_top <= p->base + 0.1 || z_feet >= p->top - 1.2) continue;
        if (map_point_in_prism(p, x, y)) return true;
    }
    return false;
}

static bool seg_intersects_seg(double ax, double ay, double bx, double by,
                               double cx, double cy, double dx, double dy,
                               double *t_out) {
    double r_x = bx - ax, r_y = by - ay;
    double s_x = dx - cx, s_y = dy - cy;
    double denom = r_x * s_y - r_y * s_x;
    if (fabs(denom) < 1e-12) return false;
    double qpx = cx - ax, qpy = cy - ay;
    double t = (qpx * s_y - qpy * s_x) / denom;
    double u = (qpx * r_y - qpy * r_x) / denom;
    if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0) {
        if (t_out) *t_out = t;
        return true;
    }
    return false;
}

bool map_los_clear(const WorldMap *m, double ax, double ay, double az, double bx, double by, double bz) {
    if (!m || !m->loaded) return true;
    double min_x = fmin(ax, bx), max_x = fmax(ax, bx);
    double min_y = fmin(ay, by), max_y = fmax(ay, by);
    double min_z = fmin(az, bz), max_z = fmax(az, bz);

    for (int i = 0; i < m->n_prisms; i++) {
        const Prism *p = &m->prisms[i];
        if (max_z < p->base || min_z > p->top) continue;
        if (max_x < p->min_x || min_x > p->max_x || max_y < p->min_y || min_y > p->max_y) continue;

        // Check midpoint & endpoints
        if (map_point_in_prism(p, ax, ay) && az >= p->base && az <= p->top) return false;
        if (map_point_in_prism(p, bx, by) && bz >= p->base && bz <= p->top) return false;

        int n = p->ring.n_pts;
        for (int k = 0, j = n - 1; k < n; j = k++) {
            double cx = p->ring.pts[j * 2], cy = p->ring.pts[j * 2 + 1];
            double dx = p->ring.pts[k * 2], dy = p->ring.pts[k * 2 + 1];
            double t = 0.0;
            if (seg_intersects_seg(ax, ay, bx, by, cx, cy, dx, dy, &t)) {
                double z_hit = az + (bz - az) * t;
                if (z_hit >= p->base && z_hit <= p->top) return false;
            }
        }
    }
    return true;
}

typedef struct {
    int idx;
    float f;
} AStarNode;

bool map_next_waypoint(const WorldMap *m, double sx, double sy, double gx, double gy, double *wx, double *wy) {
    if (!m || !m->loaded || !m->nav_blocked) {
        *wx = gx; *wy = gy;
        return false;
    }
    int six = (int)((sx - m->min_x) / m->nav_step);
    int siy = (int)((sy - m->min_y) / m->nav_step);
    int gix = (int)((gx - m->min_x) / m->nav_step);
    int giy = (int)((gy - m->min_y) / m->nav_step);
    if (six < 0) six = 0;
    if (siy < 0) siy = 0;
    if (six >= m->nav_nx) six = m->nav_nx - 1;
    if (siy >= m->nav_ny) siy = m->nav_ny - 1;
    if (gix < 0) gix = 0;
    if (giy < 0) giy = 0;
    if (gix >= m->nav_nx) gix = m->nav_nx - 1;
    if (giy >= m->nav_ny) giy = m->nav_ny - 1;

    if (six == gix && siy == giy) {
        *wx = gx; *wy = gy;
        return true;
    }

    int total = m->nav_nx * m->nav_ny;
    float *gscore = (float *)malloc((size_t)total * sizeof(float));
    int *parent = (int *)malloc((size_t)total * sizeof(int));
    uint8_t *closed = (uint8_t *)calloc((size_t)total, 1);
    AStarNode *heap = (AStarNode *)malloc(2048 * sizeof(AStarNode));
    if (!gscore || !parent || !closed || !heap) {
        free(gscore); free(parent); free(closed); free(heap);
        *wx = gx; *wy = gy;
        return false;
    }
    for (int i = 0; i < total; i++) { gscore[i] = 1e9f; parent[i] = -1; }

    int sidx = siy * m->nav_nx + six;
    int gidx = giy * m->nav_nx + gix;
    gscore[sidx] = 0.0f;
    int hsz = 0;
    heap[hsz++] = (AStarNode){ sidx, (float)hypot(gix - six, giy - siy) };

    int best_idx = sidx;
    float best_h = (float)hypot(gix - six, giy - siy);
    int expansions = 0;

    static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    static const int dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    static const float dc[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.414f, 1.414f, 1.414f, 1.414f };

    while (hsz > 0 && expansions < 1500) {
        AStarNode cur = heap[0];
        heap[0] = heap[--hsz];
        int p = 0;
        while (1) {
            int l = 2 * p + 1, r = 2 * p + 2, sm = p;
            if (l < hsz && heap[l].f < heap[sm].f) sm = l;
            if (r < hsz && heap[r].f < heap[sm].f) sm = r;
            if (sm == p) break;
            AStarNode tmp = heap[p]; heap[p] = heap[sm]; heap[sm] = tmp;
            p = sm;
        }

        if (closed[cur.idx]) continue;
        closed[cur.idx] = 1;
        expansions++;

        if (cur.idx == gidx) { best_idx = gidx; break; }

        int cx = cur.idx % m->nav_nx;
        int cy = cur.idx / m->nav_nx;
        float h = (float)hypot(gix - cx, giy - cy);
        if (h < best_h) { best_h = h; best_idx = cur.idx; }

        for (int k = 0; k < 8; k++) {
            int nx = cx + dx[k], ny = cy + dy[k];
            if (nx < 0 || ny < 0 || nx >= m->nav_nx || ny >= m->nav_ny) continue;
            int nidx = ny * m->nav_nx + nx;
            if (closed[nidx] || (m->nav_blocked[nidx] && nidx != gidx)) continue;
            float dz = m->nav_z ? (m->nav_z[nidx] - m->nav_z[cur.idx]) : 0.0f;
            float climb_cost = (dz > 1.2f) ? dz * 0.4f : 0.0f;
            float ng = gscore[cur.idx] + dc[k] + climb_cost;
            if (ng < gscore[nidx]) {
                gscore[nidx] = ng;
                parent[nidx] = cur.idx;
                float nf = ng + (float)hypot(gix - nx, giy - ny);
                if (hsz < 2048) {
                    int c = hsz++;
                    heap[c] = (AStarNode){ nidx, nf };
                    while (c > 0) {
                        int par = (c - 1) / 2;
                        if (heap[par].f <= heap[c].f) break;
                        AStarNode tmp = heap[par]; heap[par] = heap[c]; heap[c] = tmp;
                        c = par;
                    }
                }
            }
        }
    }

    // Walk back from best_idx to find the 2nd or 3rd step from start
    int cur = best_idx;
    int prev = cur;
    int prev2 = cur;
    while (cur != -1 && cur != sidx) {
        prev2 = prev;
        prev = cur;
        cur = parent[cur];
    }
    int target_cell = (prev2 != sidx) ? prev2 : prev;
    *wx = m->min_x + (target_cell % m->nav_nx + 0.5) * m->nav_step;
    *wy = m->min_y + (target_cell / m->nav_nx + 0.5) * m->nav_step;

    free(gscore); free(parent); free(closed); free(heap);
    return true;
}
