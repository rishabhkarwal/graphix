#include "renderer.h"
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GLFW/glfw3.h>

#define WIDTH 600
#define HEIGHT 600
#define PI 3.14159265359f
#define TAU (2.0f * PI)
#define MAX_SUBDIVIDE_LEVEL 7
#define MAX_CACHED_TRIANGLES 3000000

int background[3] = {0, 0, 0};
int accent[3]     = {255, 255, 255};
int face_base_colour[3] = {200, 200, 200};

typedef struct {
    float m[4][4];
} mat4;

typedef struct {
    point *points;
    int point_count;
    triangle *triangles;
    int triangle_count;
    edge *edges;
    int edge_count;
    int gpu_mesh_id;
    int built;
} lod_mesh;

typedef struct {
    int a;
    int b;
} sorted_edge;

point subtract(point a, point b);
point cross(point a, point b);
float dot(point a, point b);
point midpoint(point a, point b);

void orient_triangles_outward(const point *points, triangle *triangles, int triangle_count) {
    for (int i = 0; i < triangle_count; i++) {
        triangle t = triangles[i];

        point a = points[t.a];
        point b = points[t.b];
        point c = points[t.c];

        point ab = subtract(b, a);
        point ac = subtract(c, a);
        point normal = cross(ab, ac);

        // meshes are centred before LOD build, so outward normals align with triangle centroid direction.
        point centroid = (point){
            (a.x + b.x + c.x) / 3.0f,
            (a.y + b.y + c.y) / 3.0f,
            (a.z + b.z + c.z) / 3.0f,
            1.0f
        };

        if (dot(normal, centroid) < 0.0f) {
            int tmp = triangles[i].b;
            triangles[i].b = triangles[i].c;
            triangles[i].c = tmp;
        }
    }
}

mat4 mat4_identity(void) {
    mat4 out = {{{0}}};
    out.m[0][0] = 1.0f;
    out.m[1][1] = 1.0f;
    out.m[2][2] = 1.0f;
    out.m[3][3] = 1.0f;
    return out;
}

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

void mat4_to_array(const mat4 *m, float out[16]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            out[row * 4 + col] = m->m[row][col];
        }
    }
}

int compare_sorted_edges(const void *lhs, const void *rhs) {
    const sorted_edge *a = (const sorted_edge *)lhs;
    const sorted_edge *b = (const sorted_edge *)rhs;

    if (a->a != b->a) return a->a - b->a;
    return a->b - b->b;
}

int build_unique_edges_from_triangles(const triangle *triangles, int triangle_count, edge **out_edges, int *out_edge_count) {
    *out_edges = NULL;
    *out_edge_count = 0;
    if (triangle_count <= 0) return 1;

    int pair_count = triangle_count * 3;
    sorted_edge *pairs = malloc((size_t)pair_count * sizeof(sorted_edge));
    if (!pairs) return 0;

    int p = 0;
    for (int i = 0; i < triangle_count; i++) {
        triangle t = triangles[i];

        int a0 = t.a < t.b ? t.a : t.b;
        int b0 = t.a < t.b ? t.b : t.a;
        pairs[p++] = (sorted_edge){a0, b0};

        int a1 = t.b < t.c ? t.b : t.c;
        int b1 = t.b < t.c ? t.c : t.b;
        pairs[p++] = (sorted_edge){a1, b1};

        int a2 = t.c < t.a ? t.c : t.a;
        int b2 = t.c < t.a ? t.a : t.c;
        pairs[p++] = (sorted_edge){a2, b2};
    }

    qsort(pairs, (size_t)pair_count, sizeof(sorted_edge), compare_sorted_edges);

    int unique_count = 0;
    for (int i = 0; i < pair_count; i++) {
        if (i == 0 || pairs[i].a != pairs[i - 1].a || pairs[i].b != pairs[i - 1].b) {
            unique_count++;
        }
    }

    edge *unique_edges = malloc((size_t)unique_count * sizeof(edge));
    if (!unique_edges) {
        free(pairs);
        return 0;
    }

    int out_index = 0;
    for (int i = 0; i < pair_count; i++) {
        if (i > 0 && pairs[i].a == pairs[i - 1].a && pairs[i].b == pairs[i - 1].b) continue;
        unique_edges[out_index++] = (edge){pairs[i].a, pairs[i].b};
    }

    free(pairs);
    *out_edges = unique_edges;
    *out_edge_count = unique_count;
    return 1;
}

int find_edge_index(const edge *edges, int edge_count, int start, int end) {
    int low = 0;
    int high = edge_count - 1;

    while (low <= high) {
        int mid = (low + high) / 2;
        edge e = edges[mid];

        if (e.start == start && e.end == end) {
            return mid;
        }

        if (e.start < start || (e.start == start && e.end < end)) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    return -1;
}

void free_lod_mesh(lod_mesh *lod) {
    if (!lod) return;

    if (lod->gpu_mesh_id >= 0) {
        renderer_free_gpu_mesh(lod->gpu_mesh_id);
    }

    if (lod->points) free(lod->points);
    if (lod->triangles) free(lod->triangles);
    if (lod->edges) free(lod->edges);

    *lod = (lod_mesh){0};
    lod->gpu_mesh_id = -1;
}

int build_base_lod(const mesh *source, lod_mesh *base) {
    base->gpu_mesh_id = -1;
    base->built = 0;
    base->point_count = source->point_count;
    base->triangle_count = source->triangle_count;

    base->points = malloc((size_t)base->point_count * sizeof(point));
    base->triangles = malloc((size_t)base->triangle_count * sizeof(triangle));
    if (!base->points || !base->triangles) return 0;

    for (int i = 0; i < base->point_count; i++) {
        base->points[i] = source->points[i];
    }
    for (int i = 0; i < base->triangle_count; i++) {
        base->triangles[i] = source->triangles[i];
    }

    orient_triangles_outward(base->points, base->triangles, base->triangle_count);

    if (!build_unique_edges_from_triangles(base->triangles, base->triangle_count, &base->edges, &base->edge_count)) return 0;

    base->gpu_mesh_id = renderer_upload_mesh(
        base->points,
        base->point_count,
        base->triangles,
        base->triangle_count,
        base->edges,
        base->edge_count
    );

    if (base->gpu_mesh_id < 0) return 0;
    base->built = 1;
    return 1;
}

int build_next_lod(const lod_mesh *previous, lod_mesh *next) {
    next->gpu_mesh_id = -1;
    next->built = 0;

    if (previous->triangle_count > MAX_CACHED_TRIANGLES / 4) {
        return 0;
    }

    if (!previous->edges || previous->edge_count <= 0) {
        return 0;
    }

    const edge *source_edges = previous->edges;
    int source_edge_count = previous->edge_count;

    next->point_count = previous->point_count + source_edge_count;
    next->triangle_count = previous->triangle_count * 4;

    next->points = malloc((size_t)next->point_count * sizeof(point));
    next->triangles = malloc((size_t)next->triangle_count * sizeof(triangle));
    if (!next->points || !next->triangles) {
        return 0;
    }

    for (int i = 0; i < previous->point_count; i++) {
        next->points[i] = previous->points[i];
    }

    for (int i = 0; i < source_edge_count; i++) {
        int a = source_edges[i].start;
        int b = source_edges[i].end;
        next->points[previous->point_count + i] = midpoint(previous->points[a], previous->points[b]);
    }

    int out_triangle_index = 0;
    for (int i = 0; i < previous->triangle_count; i++) {
        triangle t = previous->triangles[i];

        int ab_start = t.a < t.b ? t.a : t.b;
        int ab_end = t.a < t.b ? t.b : t.a;
        int bc_start = t.b < t.c ? t.b : t.c;
        int bc_end = t.b < t.c ? t.c : t.b;
        int ca_start = t.c < t.a ? t.c : t.a;
        int ca_end = t.c < t.a ? t.a : t.c;

        int ab = find_edge_index(source_edges, source_edge_count, ab_start, ab_end);
        int bc = find_edge_index(source_edges, source_edge_count, bc_start, bc_end);
        int ca = find_edge_index(source_edges, source_edge_count, ca_start, ca_end);

        if (ab < 0 || bc < 0 || ca < 0) {
            return 0;
        }

        int ab_index = previous->point_count + ab;
        int bc_index = previous->point_count + bc;
        int ca_index = previous->point_count + ca;

        next->triangles[out_triangle_index++] = (triangle){t.a, ab_index, ca_index};
        next->triangles[out_triangle_index++] = (triangle){ab_index, t.b, bc_index};
        next->triangles[out_triangle_index++] = (triangle){ca_index, bc_index, t.c};
        next->triangles[out_triangle_index++] = (triangle){ab_index, bc_index, ca_index};
    }

    if (!build_unique_edges_from_triangles(next->triangles, next->triangle_count, &next->edges, &next->edge_count)) {
        return 0;
    }

    next->gpu_mesh_id = renderer_upload_mesh(
        next->points,
        next->point_count,
        next->triangles,
        next->triangle_count,
        next->edges,
        next->edge_count
    );

    if (next->gpu_mesh_id < 0) {
        return 0;
    }

    next->built = 1;
    return 1;
}

point subtract(point a, point b) {
    return (point){a.x - b.x, a.y - b.y, a.z - b.z, 0.0f};
}

point cross(point a, point b) {
    return (point){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
        0.0f
    };
}

float dot(point a, point b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

point midpoint(point a, point b) {
    return (point){
        (a.x + b.x) * 0.5f,
        (a.y + b.y) * 0.5f,
        (a.z + b.z) * 0.5f,
        1.0f
    };
}

void print_lod_mesh_stats(int level, const lod_mesh *lod) {
    printf(
        "Subdivision level: %d | vertices: %d | faces: %d | triangles: %d\n",
        level,
        lod->point_count,
        lod->triangle_count,
        lod->triangle_count
    );
}

int main(int argc, char *argv[]) {
    // fallback to a default model if none presented
    const char *model_name = (argc > 1) ? argv[1] : "cube";
    
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "models/%s.obj", model_name);

    // read model from disk
    mesh m = load_obj(filepath);
    if (!m.point_count) return 1;

    // standardise position
    centre(&m);

    // initialise rotation angles
    float delta = 8.0f;
    float angle_x = 0.5f; // pitch
    float angle_y = 0.5f; // yaw
    float angle_z = 0;    // roll

    // setup the display window
    if (!init_renderer(WIDTH, HEIGHT, "Graphix")) return 1;

    lod_mesh lods[MAX_SUBDIVIDE_LEVEL + 1];
    for (int i = 0; i <= MAX_SUBDIVIDE_LEVEL; i++) {
        lods[i] = (lod_mesh){0};
        lods[i].gpu_mesh_id = -1;
    }

    if (!build_base_lod(&m, &lods[0])) {
        free_lod_mesh(&lods[0]);
        quit_renderer();
        free_mesh(&m);
        printf("Error: failed to build base GPU mesh\n");
        return 1;
    }
    int max_built_lod = 0;
    print_lod_mesh_stats(0, &lods[0]);

    // track frame time for frame-rate independent rotation
    double last_time = glfwGetTime();

    const char *autoshot_ppm_path = getenv("GRAPHIX_AUTOSHOT_PPM");
    int autoshot_done = 0;

    // toggles pure wireframe mode (off by default)
    int wireframe_view = 0;
    int was_space_down = 0;
    int subdivision_level = 0;
    int was_plus_down = 0;
    int was_minus_down = 0;

    // camera position (screen space offsets and zoom)
    float camera_x = 0.0f;
    float camera_y = 0.0f;
    float camera_distance = delta;
    int was_r_down = 0;

    // primary game loop
    while (1) {
        double current_time = glfwGetTime();
        float dt = (float)(current_time - last_time);
        last_time = current_time;
        // handle quit events like closing window or pressing escape
        if (events_quit()) {
            break;
        }

        // toggle pure wireframe view on space key press edge
        int is_space_down = key_down(GLFW_KEY_SPACE);
        if (is_space_down && !was_space_down) {
            wireframe_view = !wireframe_view;
        }
        was_space_down = is_space_down;

        // adjust triangle subdivision level for performance testing
        int is_plus_down = key_down(GLFW_KEY_EQUAL);
        int is_minus_down = key_down(GLFW_KEY_MINUS);

        if (is_plus_down && !was_plus_down && subdivision_level < MAX_SUBDIVIDE_LEVEL) {
            int requested_level = subdivision_level + 1;
            int build_ok = 1;

            while (max_built_lod < requested_level) {
                int next_level = max_built_lod + 1;
                free_lod_mesh(&lods[next_level]);

                if (!build_next_lod(&lods[max_built_lod], &lods[next_level])) {
                    free_lod_mesh(&lods[next_level]);
                    build_ok = 0;
                    break;
                }

                max_built_lod = next_level;
            }

            if (build_ok) {
                subdivision_level = requested_level;
                print_lod_mesh_stats(subdivision_level, &lods[subdivision_level]);
            } else {
                printf(
                    "Subdivision level %d could not be built; staying on level %d\n",
                    requested_level,
                    subdivision_level
                );
            }
        }
        if (is_minus_down && !was_minus_down && subdivision_level > 0) {
            subdivision_level--;
            print_lod_mesh_stats(subdivision_level, &lods[subdivision_level]);
        }

        was_plus_down = is_plus_down;
        was_minus_down = is_minus_down;

        // handle camera movement (WASD) and reset (R)
        float camera_speed = 180.0f;  // pixels per second
        if (key_down(GLFW_KEY_W)) camera_y -= camera_speed * dt;
        if (key_down(GLFW_KEY_S)) camera_y += camera_speed * dt;
        if (key_down(GLFW_KEY_A)) camera_x += camera_speed * dt;
        if (key_down(GLFW_KEY_D)) camera_x -= camera_speed * dt;

        // reset camera on R key press edge
        int is_r_down = key_down(GLFW_KEY_R);
        if (is_r_down && !was_r_down) {
            camera_x = 0.0f;
            camera_y = 0.0f;
            camera_distance = delta;
        }
        was_r_down = is_r_down;

        // handle camera zoom (scroll wheel)
        float scroll = get_scroll_offset();
        camera_distance -= scroll * 0.5f;
        if (camera_distance < 0.0f) camera_distance = 0.0f; // clamp at z = 0

        // wipe previous frame with solid colour
        fill_background(background[0], background[1], background[2]);

        // build one matrix from pitch/yaw/roll and translation, then apply in one step
        mat4 model_to_camera = build_transform_matrix(
            angle_x,
            angle_y,
            angle_z,
            (point){0.0f, 0.0f, camera_distance, 1.0f}
        );

        int active_level = subdivision_level;
        if (active_level > max_built_lod) active_level = max_built_lod;

        float model_to_camera_array[16];
        mat4_to_array(&model_to_camera, model_to_camera_array);

        renderer_draw_mesh(
            lods[active_level].gpu_mesh_id,
            model_to_camera_array,
            camera_x,
            camera_y,
            wireframe_view,
            face_base_colour[0],
            face_base_colour[1],
            face_base_colour[2],
            accent[0],
            accent[1],
            accent[2]
        );

        if (autoshot_ppm_path && !autoshot_done) {
            if (renderer_capture_framebuffer_ppm(autoshot_ppm_path)) {
                printf("Saved framebuffer snapshot to %s\n", autoshot_ppm_path);
            } else {
                printf("Error: failed to save framebuffer snapshot to %s\n", autoshot_ppm_path);
            }
            autoshot_done = 1;
        }

        // render to the physical screen
        update_display();

        if (autoshot_done) {
            break;
        }

        // rotate for next frame (scaled by delta time for frame-rate independence)
        angle_x += 0.5f * dt;
        angle_y += 1.0f * dt;
        angle_z += 0.5f * dt;
        
        // wrap angles back to 0-2 PI range
        angle_x = fmod(angle_x, TAU);
        angle_y = fmod(angle_y, TAU);
        angle_z = fmod(angle_z, TAU);
    }

    // prevent memory leaks
    for (int i = 0; i <= MAX_SUBDIVIDE_LEVEL; i++) {
        free_lod_mesh(&lods[i]);
    }

    quit_renderer();
    free_mesh(&m);
    return 0;
}
