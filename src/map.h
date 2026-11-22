#ifndef MAP_H
#define MAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double *pts; // [x0, y0, x1, y1, ...]
    int n_pts;
} Polygon2D;

typedef struct {
    double base;
    double top;
    double min_x, max_x, min_y, max_y;
    Polygon2D ring;
    Polygon2D *holes;
    int n_holes;
    bool is_bridge;
} Prism;

typedef struct {
    bool loaded;
    double min_x, min_y, step;
    int nx, ny;
    int16_t *terrain;

    Prism *prisms;
    int n_prisms;

    // 4m navigation grid
    int nav_nx, nav_ny;
    double nav_step;
    uint8_t *nav_blocked;
    uint8_t *nav_cover;
    float *nav_z;
} WorldMap;

bool map_load(WorldMap *m, const char *map_dir);
void map_free(WorldMap *m);

double map_terrain_height(const WorldMap *m, double x, double y);
bool map_point_in_prism(const Prism *p, double x, double y);
bool map_blocked_at(const WorldMap *m, double x, double y, double z_feet, double height);
bool map_los_clear(const WorldMap *m, double ax, double ay, double az, double bx, double by, double bz);
bool map_next_waypoint(const WorldMap *m, double sx, double sy, double gx, double gy, double *wx, double *wy);

#endif
