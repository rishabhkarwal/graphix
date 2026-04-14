#ifndef TYPES_H
#define TYPES_H

// 3D point in space
typedef struct {
    float x, y, z;
} point;

// connection between two points
typedef struct {
    int start, end;
} edge;

// 3D object composed of points and edges
typedef struct {
    point *points;
    int point_count;
    
    edge *edges;
    int edge_count;
} mesh;

#endif
