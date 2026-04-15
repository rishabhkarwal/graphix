#include "renderer.h"
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <limits.h>
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

point subtract(point a, point b);
point cross(point a, point b);
float dot(point a, point b);
point midpoint(point a, point b);

// ensures triangle winding is outward after centring
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
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            out[row * 4 + col] = m->m[row][col];
        }
    }
}

// hashes a canonical edge pair for bucket lookup
static unsigned int edge_hash(int start, int end) {
    return ((unsigned int)start * 73856093u) ^ ((unsigned int)end * 19349663u);
}

// selects a conservative bucket count for edge hash tables
static int edge_bucket_count_for_items(int item_count) {
    if (item_count < 0) return 0;

    long long suggested = ((long long)item_count * 3LL) / 2LL + 1LL;
    if (suggested < 17LL) suggested = 17LL;
    if (suggested > INT_MAX) return 0;

    return (int)suggested;
}

// looks up an edge index from a chained hash table
static int find_edge_index_hashed(
    const edge *edges,
    const int *buckets,
    const int *next_indices,
    int bucket_count,
    int start,
    int end
) {
    if (!edges || !buckets || !next_indices || bucket_count <= 0) return -1;

    int bucket = (int)(edge_hash(start, end) % (unsigned int)bucket_count);
    int edge_index = buckets[bucket];

    while (edge_index >= 0) {
        if (edges[edge_index].start == start && edges[edge_index].end == end) {
            return edge_index;
        }
        edge_index = next_indices[edge_index];
    }

    return -1;
}

// builds a unique undirected edge list from triangle indices
int build_unique_edges_from_triangles(const triangle *triangles, int triangle_count, edge **out_edges, int *out_edge_count) {
    *out_edges = NULL;
    *out_edge_count = 0;
    if (triangle_count <= 0) return 1;

    if (triangle_count > INT_MAX / 3) return 0;
    int max_edge_count = triangle_count * 3;
    int bucket_count = edge_bucket_count_for_items(max_edge_count);
    if (bucket_count <= 0) return 0;

    edge *unique_edges = malloc((size_t)max_edge_count * sizeof(edge));
    int *next_indices = malloc((size_t)max_edge_count * sizeof(int));
    int *buckets = malloc((size_t)bucket_count * sizeof(int));
    if (!unique_edges || !next_indices || !buckets) {
        free(unique_edges);
        free(next_indices);
        free(buckets);
        return 0;
    }

    for (int i = 0; i < bucket_count; i++) {
        buckets[i] = -1;
    }

    int unique_count = 0;
    for (int i = 0; i < triangle_count; i++) {
        triangle t = triangles[i];

        int edge_starts[3];
        int edge_ends[3];

        edge_starts[0] = t.a < t.b ? t.a : t.b;
        edge_ends[0] = t.a < t.b ? t.b : t.a;

        edge_starts[1] = t.b < t.c ? t.b : t.c;
        edge_ends[1] = t.b < t.c ? t.c : t.b;

        edge_starts[2] = t.c < t.a ? t.c : t.a;
        edge_ends[2] = t.c < t.a ? t.a : t.c;

        for (int edge_i = 0; edge_i < 3; edge_i++) {
            int start = edge_starts[edge_i];
            int end = edge_ends[edge_i];
            int bucket = (int)(edge_hash(start, end) % (unsigned int)bucket_count);
            int existing = buckets[bucket];
            int found = 0;

            while (existing >= 0) {
                if (unique_edges[existing].start == start && unique_edges[existing].end == end) {
                    found = 1;
                    break;
                }
                existing = next_indices[existing];
            }

            if (!found) {
                int new_index = unique_count++;
                unique_edges[new_index] = (edge){start, end};
                next_indices[new_index] = buckets[bucket];
                buckets[bucket] = new_index;
            }
        }
    }

    free(next_indices);
    free(buckets);

    if (unique_count > 0) {
        edge *shrunk = realloc(unique_edges, (size_t)unique_count * sizeof(edge));
        if (shrunk) {
            unique_edges = shrunk;
        }
    }

    *out_edges = unique_edges;
    *out_edge_count = unique_count;
    return 1;
}

// frees cpu and gpu resources owned by one lod mesh
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

// creates lod level 0 from the loaded mesh and uploads it
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

// builds the next subdivision level from an existing lod mesh
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

    int source_bucket_count = edge_bucket_count_for_items(source_edge_count);
    if (source_bucket_count <= 0) {
        return 0;
    }

    int *source_next_indices = malloc((size_t)source_edge_count * sizeof(int));
    int *source_buckets = malloc((size_t)source_bucket_count * sizeof(int));
    if (!source_next_indices || !source_buckets) {
        free(source_next_indices);
        free(source_buckets);
        return 0;
    }

    for (int i = 0; i < source_bucket_count; i++) {
        source_buckets[i] = -1;
    }

    for (int i = 0; i < source_edge_count; i++) {
        int start = source_edges[i].start;
        int end = source_edges[i].end;
        int bucket = (int)(edge_hash(start, end) % (unsigned int)source_bucket_count);

        source_next_indices[i] = source_buckets[bucket];
        source_buckets[bucket] = i;
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

        int ab = find_edge_index_hashed(source_edges, source_buckets, source_next_indices, source_bucket_count, ab_start, ab_end);
        int bc = find_edge_index_hashed(source_edges, source_buckets, source_next_indices, source_bucket_count, bc_start, bc_end);
        int ca = find_edge_index_hashed(source_edges, source_buckets, source_next_indices, source_bucket_count, ca_start, ca_end);

        if (ab < 0 || bc < 0 || ca < 0) {
            free(source_next_indices);
            free(source_buckets);
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

    free(source_next_indices);
    free(source_buckets);

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
        (a.x + b.x) * 0.5f,
        (a.y + b.y) * 0.5f,
        (a.z + b.z) * 0.5f,
        1.0f
    };
}

// prints lod mesh counts for quick inspection
void print_lod_mesh_stats(int level, const lod_mesh *lod) {
    printf(
        "Subdivision level: %d | vertices: %d | faces: %d | triangles: %d\n",
        level,
        lod->point_count,
        lod->triangle_count,
        lod->triangle_count
    );
}

// runs input, lod selection, and rendering for the active model
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

    // set up the display window
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
    float mouse_pan_sensitivity = -1.0f;
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

        // pan camera by dragging while left mouse button is held
        float drag_delta_x = 0.0f;
        float drag_delta_y = 0.0f;
        get_left_mouse_drag_delta(&drag_delta_x, &drag_delta_y);
        camera_x += drag_delta_x * mouse_pan_sensitivity;
        camera_y -= drag_delta_y * mouse_pan_sensitivity;

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
