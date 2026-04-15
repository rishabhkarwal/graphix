#ifndef TYPES_H
#define TYPES_H

// homogeneous 3D point/vector (w = 1 for positions, w = 0 for direction vectors)
typedef struct {
    float x, y, z, w;
} point;

// connection between two points
typedef struct {
    int start, end;
} edge;

// single triangle face stored as point indices
typedef struct {
    int a, b, c;
} triangle;

// triangles touching a vertex, built once at load time for fast normal lookup
typedef struct {
    int *triangle_indices;
    int triangle_count;
} vertex_adjacency;

// 3D object composed of points and edges
typedef struct {
    point *points;
    int point_count;
    
    edge *edges;
    int edge_count;

    triangle *triangles;
    int triangle_count;

    vertex_adjacency *vertex_adjacencies;
} mesh;

#endif
