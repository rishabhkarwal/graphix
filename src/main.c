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
#define EPSILON 0.000001f
#define NORMAL_SIMILARITY_THRESHOLD 0.70710678f // cos(45) - only smooth across edges where angle is at most about 45
#define MAX_SUBDIVIDE_LEVEL 7
#define SUBDIVISION_EDGE_THRESHOLD 12.0f // stop subdividing once the triangle is this small on screen
#define SUBDIVISION_EDGE_THRESHOLD_SQUARED (SUBDIVISION_EDGE_THRESHOLD * SUBDIVISION_EDGE_THRESHOLD)

int background[3] = {0, 0, 0};
int accent[3]     = {255, 255, 255};
int face_base_colour[3] = {200, 200, 200};

point project(point position, float scale) {
    // projects 3D co-ordinate to 2D plane
    float x = position.x, y = position.y, z = position.z;
    if (z == 0) z = 0.1f;
    return (point){x * scale / z, y * scale / z, 0};
}

point convert(point position) {
    // scale to screen size
    return project(position, 800.0f);
}

point rotate_x(point position, float theta) {
    // rotates around the x-axis
    float x = position.x, y = position.y, z = position.z;
    float sin_theta = sin(theta), cos_theta = cos(theta);
    return (point){x, y * cos_theta - z * sin_theta, y * sin_theta + z * cos_theta};
}

point rotate_y(point position, float theta) {
    // rotates around the y-axis
    float x = position.x, y = position.y, z = position.z;
    float sin_theta = sin(theta), cos_theta = cos(theta);
    return (point){x * cos_theta + z * sin_theta, y, -x * sin_theta + z * cos_theta};
}

point rotate_z(point position, float theta) {
    // rotates around the z-axis
    float x = position.x, y = position.y, z = position.z;
    float sin_theta = sin(theta), cos_theta = cos(theta);
    return (point){x * cos_theta - y * sin_theta, x * sin_theta + y * cos_theta, z};
}

point subtract(point a, point b) {
    return (point){a.x - b.x, a.y - b.y, a.z - b.z};
}

point cross(point a, point b) {
    return (point){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

float dot(point a, point b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

point normalise(point v) {
    float length = sqrtf(dot(v, v));
    if (length <= EPSILON) return (point){0.0f, 0.0f, 0.0f};
    return (point){v.x / length, v.y / length, v.z / length};
}

int triangle_contains_edge(triangle t, int start, int end) {
    return
        (t.a == start && t.b == end) || (t.a == end && t.b == start) ||
        (t.b == start && t.c == end) || (t.b == end && t.c == start) ||
        (t.c == start && t.a == end) || (t.c == end && t.a == start);
}

int triangle_contains_vertex(triangle t, int vertex_index) {
    return t.a == vertex_index || t.b == vertex_index || t.c == vertex_index;
}

point averaged_vertex_normal(
    int vertex_index,
    point reference_normal,
    const mesh *m,
    const point *triangle_normal
) {
    point sum = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < m->triangle_count; i++) {
        if (!triangle_contains_vertex(m->triangles[i], vertex_index)) continue;

        float normal_similarity = dot(triangle_normal[i], reference_normal);
        if (normal_similarity < NORMAL_SIMILARITY_THRESHOLD) continue;

        sum.x += triangle_normal[i].x;
        sum.y += triangle_normal[i].y;
        sum.z += triangle_normal[i].z;
    }

    point averaged = normalise(sum);
    if (dot(averaged, averaged) <= EPSILON) return reference_normal;
    return averaged;
}

float lambert_intensity(point normal, point position) {
    // light source is fixed at origin
    point to_light = normalise((point){-position.x, -position.y, -position.z});

    float diffuse = dot(normal, to_light);
    if (diffuse < 0.0f) diffuse = 0.0f;

    const float ambient = 0.15f;
    const float diffuse_strength = 0.85f;
    float intensity = ambient + diffuse_strength * diffuse;
    if (intensity > 1.0f) intensity = 1.0f;
    return intensity;
}

point midpoint(point a, point b) {
    return (point){
        (a.x + b.x) * 0.5f,
        (a.y + b.y) * 0.5f,
        (a.z + b.z) * 0.5f
    };
}

int midpoint_channel(int a, int b) {
    return (a + b) / 2;
}

float point_distance_squared(point a, point b) {
    // compare distances without paying for a square root
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

int triangle_front_facing(point a, point b, point c) {
    // use the triangle normal and centroid to decide whether it faces the camera
    point ab = subtract(b, a);
    point ac = subtract(c, a);
    point normal = cross(ab, ac);

    point centroid = (point){
        (a.x + b.x + c.x) / 3.0f,
        (a.y + b.y + c.y) / 3.0f,
        (a.z + b.z + c.z) / 3.0f
    };

    point to_camera = (point){-centroid.x, -centroid.y, -centroid.z};
    return dot(normal, to_camera) > 0.0f;
}

float triangle_max_projected_edge_length_squared(point a, point b, point c) {
    // stop subdividing once the triangle is already small in screen space
    point projected_a = convert(a);
    point projected_b = convert(b);
    point projected_c = convert(c);

    float ab = point_distance_squared(projected_a, projected_b);
    float bc = point_distance_squared(projected_b, projected_c);
    float ca = point_distance_squared(projected_c, projected_a);

    float max_length = ab;
    if (bc > max_length) max_length = bc;
    if (ca > max_length) max_length = ca;
    return max_length;
}

void draw_subdivided_triangle(
    point a,
    point b,
    point c,
    int red_a,
    int green_a,
    int blue_a,
    int red_b,
    int green_b,
    int blue_b,
    int red_c,
    int green_c,
    int blue_c,
    int level,
    float camera_x,
    float camera_y
) {
    typedef struct {
        point a;
        point b;
        point c;
        int red_a;
        int green_a;
        int blue_a;
        int red_b;
        int green_b;
        int blue_b;
        int red_c;
        int green_c;
        int blue_c;
        int level;
    } subdivided_triangle_task;

    static subdivided_triangle_task tasks[1 << (MAX_SUBDIVIDE_LEVEL * 2)];
    int task_count = 0;

    // explicit task stack avoids recursive subdivision and keeps call overhead down
    tasks[task_count++] = (subdivided_triangle_task){
        a,
        b,
        c,
        red_a,
        green_a,
        blue_a,
        red_b,
        green_b,
        blue_b,
        red_c,
        green_c,
        blue_c,
        level
    };

    while (task_count > 0) {
        subdivided_triangle_task task = tasks[--task_count];

        // project once for the current task so both the size test and final draw reuse it
        point projected_a = convert(task.a);
        point projected_b = convert(task.b);
        point projected_c = convert(task.c);

        projected_a.x += camera_x;
        projected_a.y += camera_y;
        projected_b.x += camera_x;
        projected_b.y += camera_y;
        projected_c.x += camera_x;
        projected_c.y += camera_y;

        float max_edge_length_squared = triangle_max_projected_edge_length_squared(task.a, task.b, task.c);

        if (task.level <= 0 || max_edge_length_squared <= SUBDIVISION_EDGE_THRESHOLD_SQUARED) {

            draw_triangle(
                projected_a,
                projected_b,
                projected_c,
                task.red_a,
                task.green_a,
                task.blue_a,
                task.red_b,
                task.green_b,
                task.blue_b,
                task.red_c,
                task.green_c,
                task.blue_c
            );
            continue;
        }

        point ab = midpoint(task.a, task.b);
        point bc = midpoint(task.b, task.c);
        point ca = midpoint(task.c, task.a);

        int red_ab = midpoint_channel(task.red_a, task.red_b);
        int green_ab = midpoint_channel(task.green_a, task.green_b);
        int blue_ab = midpoint_channel(task.blue_a, task.blue_b);

        int red_bc = midpoint_channel(task.red_b, task.red_c);
        int green_bc = midpoint_channel(task.green_b, task.green_c);
        int blue_bc = midpoint_channel(task.blue_b, task.blue_c);

        int red_ca = midpoint_channel(task.red_c, task.red_a);
        int green_ca = midpoint_channel(task.green_c, task.green_a);
        int blue_ca = midpoint_channel(task.blue_c, task.blue_a);

        tasks[task_count++] = (subdivided_triangle_task){
            ab,
            bc,
            ca,
            red_ab,
            green_ab,
            blue_ab,
            red_bc,
            green_bc,
            blue_bc,
            red_ca,
            green_ca,
            blue_ca,
            task.level - 1
        };
        tasks[task_count++] = (subdivided_triangle_task){
            ca,
            bc,
            task.c,
            red_ca,
            green_ca,
            blue_ca,
            red_bc,
            green_bc,
            blue_bc,
            task.red_c,
            task.green_c,
            task.blue_c,
            task.level - 1
        };
        tasks[task_count++] = (subdivided_triangle_task){
            ab,
            task.b,
            bc,
            red_ab,
            green_ab,
            blue_ab,
            task.red_b,
            task.green_b,
            task.blue_b,
            red_bc,
            green_bc,
            blue_bc,
            task.level - 1
        };
        tasks[task_count++] = (subdivided_triangle_task){
            task.a,
            ab,
            ca,
            task.red_a,
            task.green_a,
            task.blue_a,
            red_ab,
            green_ab,
            blue_ab,
            red_ca,
            green_ca,
            blue_ca,
            task.level - 1
        };
    }
}

void draw_subdivided_wireframe_triangle(point a, point b, point c, int level, float camera_x, float camera_y) {
    typedef struct {
        point a;
        point b;
        point c;
        int level;
    } subdivided_wireframe_task;

    static subdivided_wireframe_task tasks[1 << (MAX_SUBDIVIDE_LEVEL * 2)];
    int task_count = 0;

    // wireframe uses the same explicit stack structure as shaded subdivision
    tasks[task_count++] = (subdivided_wireframe_task){a, b, c, level};

    while (task_count > 0) {
        subdivided_wireframe_task task = tasks[--task_count];

        // project once for both the cutoff test and the final line draw
        point projected_a = convert(task.a);
        point projected_b = convert(task.b);
        point projected_c = convert(task.c);

        projected_a.x += camera_x;
        projected_a.y += camera_y;
        projected_b.x += camera_x;
        projected_b.y += camera_y;
        projected_c.x += camera_x;
        projected_c.y += camera_y;

        float max_edge_length_squared = triangle_max_projected_edge_length_squared(task.a, task.b, task.c);

        if (task.level <= 0 || max_edge_length_squared <= SUBDIVISION_EDGE_THRESHOLD_SQUARED) {

            draw_aaline(projected_a, projected_b, accent[0], accent[1], accent[2]);
            draw_aaline(projected_b, projected_c, accent[0], accent[1], accent[2]);
            draw_aaline(projected_c, projected_a, accent[0], accent[1], accent[2]);
            continue;
        }

        point ab = midpoint(task.a, task.b);
        point bc = midpoint(task.b, task.c);
        point ca = midpoint(task.c, task.a);

        tasks[task_count++] = (subdivided_wireframe_task){ab, bc, ca, task.level - 1};
        tasks[task_count++] = (subdivided_wireframe_task){ca, bc, task.c, task.level - 1};
        tasks[task_count++] = (subdivided_wireframe_task){ab, task.b, bc, task.level - 1};
        tasks[task_count++] = (subdivided_wireframe_task){task.a, ab, ca, task.level - 1};
    }
}

void print_subdivision_stats(const mesh *m, int level) {
    unsigned long long vertices = (unsigned long long)m->point_count;
    unsigned long long edges = (unsigned long long)m->edge_count;
    unsigned long long triangles = (unsigned long long)m->triangle_count;

    for (int i = 0; i < level; i++) {
        unsigned long long next_vertices = vertices + edges;
        unsigned long long next_edges = 2ULL * edges + 3ULL * triangles;
        unsigned long long next_triangles = 4ULL * triangles;

        vertices = next_vertices;
        edges = next_edges;
        triangles = next_triangles;
    }

    // subdivision yields triangular faces, so face and triangle counts are equivalent
    unsigned long long faces = triangles;

    printf(
        "Subdivision level: %d | vertices: %llu | faces: %llu | triangles: %llu\n",
        level,
        vertices,
        faces,
        triangles
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

    // temporary storage for projected points each frame
    point *projected = malloc(sizeof(point) * m.point_count);
    point *camera_space = malloc(sizeof(point) * m.point_count);
    point *triangle_normal = malloc(sizeof(point) * m.triangle_count);
    int *triangle_front = malloc(sizeof(int) * m.triangle_count);
    int *triangle_order = malloc(sizeof(int) * m.triangle_count);
    float *triangle_depth = malloc(sizeof(float) * m.triangle_count);

    // track frame time for frame-rate independent rotation
    double last_time = glfwGetTime();

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
            quit_renderer();
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
            subdivision_level++;
            print_subdivision_stats(&m, subdivision_level);
        }
        if (is_minus_down && !was_minus_down && subdivision_level > 0) {
            subdivision_level--;
            print_subdivision_stats(&m, subdivision_level);
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

        // process math for each point in 3D space
        for (int i = 0; i < m.point_count; i++) {
            point p = m.points[i];
            point rotated = rotate_x(p, angle_x);
            rotated = rotate_y(rotated, angle_y);
            rotated = rotate_z(rotated, angle_z);

            rotated.z += camera_distance; // apply camera distance offset

            camera_space[i] = rotated;

            // map to flat 2D screen co-ordinates and apply camera pan
            point proj = convert(rotated);
            proj.x += camera_x;
            proj.y += camera_y;
            projected[i] = proj;
        }

        // classify triangles by camera-facing direction
        point object_centre = (point){0.0f, 0.0f, camera_distance};

        for (int i = 0; i < m.triangle_count; i++) {
            triangle t = m.triangles[i];
            point pa = camera_space[t.a];
            point pb = camera_space[t.b];
            point pc = camera_space[t.c];

            point ab = subtract(pb, pa);
            point ac = subtract(pc, pa);
            point normal = cross(ab, ac);

            point centroid = (point){
                (pa.x + pb.x + pc.x) / 3.0f,
                (pa.y + pb.y + pc.y) / 3.0f,
                (pa.z + pb.z + pc.z) / 3.0f
            };

            triangle_depth[i] = centroid.z;

            // keep normals pointing away from the object centre before doing lighting
            point outward = subtract(centroid, object_centre);
            if (dot(normal, outward) < 0.0f) {
                normal.x = -normal.x;
                normal.y = -normal.y;
                normal.z = -normal.z;
            }

            normal = normalise(normal);
            triangle_normal[i] = normal;

            point to_camera = (point){-centroid.x, -centroid.y, -centroid.z};
            float facing = dot(normal, to_camera);
            triangle_front[i] = facing > 0.0f;
        }

        // draw either shaded faces or pure wireframe
        if (!wireframe_view) {
            // draw front-facing triangles from back to front
            int front_count = 0;
            for (int i = 0; i < m.triangle_count; i++) {
                if (!triangle_front[i]) continue;
                triangle_order[front_count++] = i;
            }

            // painter sort by depth (larger z means farther away)
            for (int i = 1; i < front_count; i++) {
                int key = triangle_order[i];
                float key_depth = triangle_depth[key];
                int j = i - 1;
                while (j >= 0 && triangle_depth[triangle_order[j]] < key_depth) {
                    triangle_order[j + 1] = triangle_order[j];
                    j--;
                }
                triangle_order[j + 1] = key;
            }

            begin_triangle_batch();
            for (int i = 0; i < front_count; i++) {
                int t_index = triangle_order[i];
                triangle t = m.triangles[t_index];
                point projected_a = projected[t.a];
                point projected_b = projected[t.b];
                point projected_c = projected[t.c];

                point base_normal = triangle_normal[t_index];

                point normal_a = averaged_vertex_normal(
                    t.a,
                    base_normal,
                    &m,
                    triangle_normal
                );
                point normal_b = averaged_vertex_normal(
                    t.b,
                    base_normal,
                    &m,
                    triangle_normal
                );
                point normal_c = averaged_vertex_normal(
                    t.c,
                    base_normal,
                    &m,
                    triangle_normal
                );

                float shade_a = lambert_intensity(normal_a, camera_space[t.a]);
                float shade_b = lambert_intensity(normal_b, camera_space[t.b]);
                float shade_c = lambert_intensity(normal_c, camera_space[t.c]);

                int red_a = (int)(face_base_colour[0] * shade_a);
                int green_a = (int)(face_base_colour[1] * shade_a);
                int blue_a = (int)(face_base_colour[2] * shade_a);

                int red_b = (int)(face_base_colour[0] * shade_b);
                int green_b = (int)(face_base_colour[1] * shade_b);
                int blue_b = (int)(face_base_colour[2] * shade_b);

                int red_c = (int)(face_base_colour[0] * shade_c);
                int green_c = (int)(face_base_colour[1] * shade_c);
                int blue_c = (int)(face_base_colour[2] * shade_c);

                if (subdivision_level <= 0) {
                    draw_triangle(
                        projected_a,
                        projected_b,
                        projected_c,
                        red_a,
                        green_a,
                        blue_a,
                        red_b,
                        green_b,
                        blue_b,
                        red_c,
                        green_c,
                        blue_c
                    );
                } else {
                    point camera_a = camera_space[t.a];
                    point camera_b = camera_space[t.b];
                    point camera_c = camera_space[t.c];

                    draw_subdivided_triangle(
                        camera_a,
                        camera_b,
                        camera_c,
                        red_a,
                        green_a,
                        blue_a,
                        red_b,
                        green_b,
                        blue_b,
                        red_c,
                        green_c,
                        blue_c,
                        subdivision_level,
                        camera_x,
                        camera_y
                    );
                }
            }
            end_triangle_batch();

        } else {
            begin_line_batch();
            for (int i = 0; i < m.triangle_count; i++) {
                if (!triangle_front[i]) continue;
                triangle t = m.triangles[i];

                point camera_a = camera_space[t.a];
                point camera_b = camera_space[t.b];
                point camera_c = camera_space[t.c];
                draw_subdivided_wireframe_triangle(camera_a, camera_b, camera_c, subdivision_level, camera_x, camera_y);
            }
            end_line_batch();
        }

        // render to the physical screen
        update_display();

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
    free(projected);
    free(camera_space);
    free(triangle_normal);
    free(triangle_front);
    free(triangle_order);
    free(triangle_depth);
    free_mesh(&m);
    return 0;
}
