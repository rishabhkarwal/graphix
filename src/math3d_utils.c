#include "math3d_utils.h"
#include <math.h>

#define MAT4_DIMENSION 4
#define VECTOR_NORMALISE_EPSILON 0.000001f
#define MIDPOINT_SCALE 0.5f

static const point WORLD_UP_VECTOR = {0.0f, 1.0f, 0.0f, 0.0f};
static const point FORWARD_FALLBACK_VECTOR = {0.0f, 0.0f, 1.0f, 0.0f};
static const point RIGHT_FALLBACK_VECTOR = {1.0f, 0.0f, 0.0f, 0.0f};

// returns a 4x4 identity matrix
mat4 mat4_identity(void) {
    mat4 out = {{{0}}};
    out.m[0][0] = 1.0f;
    out.m[1][1] = 1.0f;
    out.m[2][2] = 1.0f;
    out.m[3][3] = 1.0f;
    return out;
}

// builds a single model-to-camera transform from euler rotation and translation
mat4 build_transform_matrix(float pitch, float yaw, float roll, point translation) {
    // combined rotation (Rz * Ry * Rx) plus translation in one matrix
    float sx = sinf(pitch), cx = cosf(pitch);
    float sy = sinf(yaw), cy = cosf(yaw);
    float sz = sinf(roll), cz = cosf(roll);

    mat4 out = mat4_identity();

    out.m[0][0] = cz * cy;
    out.m[0][1] = cz * sy * sx - sz * cx;
    out.m[0][2] = cz * sy * cx + sz * sx;

    out.m[1][0] = sz * cy;
    out.m[1][1] = sz * sy * sx + cz * cx;
    out.m[1][2] = sz * sy * cx - cz * sx;

    out.m[2][0] = -sy;
    out.m[2][1] = cy * sx;
    out.m[2][2] = cy * cx;

    out.m[0][3] = translation.x;
    out.m[1][3] = translation.y;
    out.m[2][3] = translation.z;
    return out;
}

// flattens a mat4 into a contiguous float array for uniform upload
void mat4_to_array(const mat4 *m, float out[16]) {
    for (int row = 0; row < MAT4_DIMENSION; row++) {
        for (int col = 0; col < MAT4_DIMENSION; col++) {
            out[row * MAT4_DIMENSION + col] = m->m[row][col];
        }
    }
}

// returns the euclidean length of a 3D vector
float vector_length(point v) {
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

// normalises a vector and falls back if length is near zero
point normalise_vector(point v, point fallback) {
    float length = vector_length(v);
    if (length <= VECTOR_NORMALISE_EPSILON) {
        return fallback;
    }

    return (point){
        v.x / length,
        v.y / length,
        v.z / length,
        0.0f
    };
}

// multiplies two 4x4 matrices (a * b)
mat4 mat4_multiply(const mat4 *a, const mat4 *b) {
    mat4 out = {{{0}}};
    for (int row = 0; row < MAT4_DIMENSION; row++) {
        for (int col = 0; col < MAT4_DIMENSION; col++) {
            float sum = 0.0f;
            for (int i = 0; i < MAT4_DIMENSION; i++) {
                sum += a->m[row][i] * b->m[i][col];
            }
            out.m[row][col] = sum;
        }
    }
    return out;
}

// builds a camera matrix that always looks at the target point
mat4 build_look_at_matrix(point camera_position, point target) {
    point forward = normalise_vector(
        subtract(target, camera_position),
        FORWARD_FALLBACK_VECTOR
    );

    point right = cross(WORLD_UP_VECTOR, forward);
    right = normalise_vector(right, RIGHT_FALLBACK_VECTOR);

    point up = cross(forward, right);
    up = normalise_vector(up, WORLD_UP_VECTOR);

    mat4 out = mat4_identity();

    out.m[0][0] = right.x;
    out.m[0][1] = right.y;
    out.m[0][2] = right.z;
    out.m[0][3] = -dot(right, camera_position);

    out.m[1][0] = up.x;
    out.m[1][1] = up.y;
    out.m[1][2] = up.z;
    out.m[1][3] = -dot(up, camera_position);

    out.m[2][0] = forward.x;
    out.m[2][1] = forward.y;
    out.m[2][2] = forward.z;
    out.m[2][3] = -dot(forward, camera_position);

    return out;
}

// subtracts one point/vector from another
point subtract(point a, point b) {
    return (point){a.x - b.x, a.y - b.y, a.z - b.z, 0.0f};
}

// returns the 3d cross product of two vectors
point cross(point a, point b) {
    return (point){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
        0.0f
    };
}

// returns the 3d dot product of two vectors
float dot(point a, point b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// returns the midpoint between two positions
point midpoint(point a, point b) {
    return (point){
        (a.x + b.x) * MIDPOINT_SCALE,
        (a.y + b.y) * MIDPOINT_SCALE,
        (a.z + b.z) * MIDPOINT_SCALE,
        1.0f
    };
}

// returns the maximum distance from origin to any mesh point
float compute_mesh_radius(const mesh *m) {
    if (!m || !m->points || m->point_count <= 0) return 0.0f;

    float max_radius_squared = 0.0f;
    for (int i = 0; i < m->point_count; i++) {
        point p = m->points[i];
        float radius_squared = p.x * p.x + p.y * p.y + p.z * p.z;
        if (radius_squared > max_radius_squared) {
            max_radius_squared = radius_squared;
        }
    }

    return sqrtf(max_radius_squared);
}
