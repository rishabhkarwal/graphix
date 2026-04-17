#ifndef MATH3D_UTILS_H
#define MATH3D_UTILS_H

#include "types.h"

typedef struct {
    float m[4][4];
} mat4;

point subtract(point a, point b);
point cross(point a, point b);
float dot(point a, point b);
point midpoint(point a, point b);

mat4 mat4_identity(void);
mat4 build_transform_matrix(float pitch, float yaw, float roll, point translation);
void mat4_to_array(const mat4 *m, float out[16]);

float vector_length(point v);
point normalise_vector(point v, point fallback);
mat4 mat4_multiply(const mat4 *a, const mat4 *b);
mat4 build_look_at_matrix(point camera_position, point target);

float compute_mesh_radius(const mesh *m);

#endif
