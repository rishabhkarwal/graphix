#ifndef MESH_BUILD_H
#define MESH_BUILD_H

#include "types.h"

typedef struct {
    point *points;
    int point_count;
    triangle *triangles;
    int triangle_count;
    edge *edges;
    int edge_count;
    int gpu_mesh_id;
    int built;
} mesh_buffers;

void orient_mesh_triangles_outward(const point *points, triangle *triangles, int triangle_count);
int build_mesh_unique_edges(const triangle *triangles, int triangle_count, edge **out_edges, int *out_edge_count);
void free_mesh_buffers(mesh_buffers *buffers);
int build_mesh_buffers(const mesh *source, mesh_buffers *buffers);

#endif
