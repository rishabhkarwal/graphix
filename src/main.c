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

int background[3] = {0, 0, 0};
int accent[3]     = {255, 255, 255};

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

int triangle_contains_edge(triangle t, int start, int end) {
    return
        (t.a == start && t.b == end) || (t.a == end && t.b == start) ||
        (t.b == start && t.c == end) || (t.b == end && t.c == start) ||
        (t.c == start && t.a == end) || (t.c == end && t.a == start);
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
    int *triangle_front = malloc(sizeof(int) * m.triangle_count);

    // track frame time for frame-rate independent rotation
    double last_time = glfwGetTime();

    // toggles debug draw mode for triangulation
    int triangle_view = 0;
    int was_space_down = 0;

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

        // toggle triangle debug view on space key press edge
        int is_space_down = key_down(GLFW_KEY_SPACE);
        if (is_space_down && !was_space_down) {
            triangle_view = !triangle_view;
        }
        was_space_down = is_space_down;

        // wipe previous frame with solid colour
        fill_background(background[0], background[1], background[2]);

        // process math for each point in 3D space
        for (int i = 0; i < m.point_count; i++) {
            point p = m.points[i];
            point rotated = rotate_x(p, angle_x);
            rotated = rotate_y(rotated, angle_y);
            rotated = rotate_z(rotated, angle_z);

            rotated.z += delta; // apply depth offset

            camera_space[i] = rotated;

            // map to flat 2D screen co-ordinates
            projected[i] = convert(rotated);
        }

        // classify triangles by camera-facing direction
        point object_center = (point){0.0f, 0.0f, delta};
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

            point outward = subtract(centroid, object_center);
            if (dot(normal, outward) < 0.0f) {
                normal.x = -normal.x;
                normal.y = -normal.y;
                normal.z = -normal.z;
            }

            point to_camera = (point){-centroid.x, -centroid.y, -centroid.z};
            float facing = dot(normal, to_camera);
            triangle_front[i] = facing > 0.0f;
        }

        // draw either mesh edges or triangulation edges
        if (!triangle_view) {
            for (int i = 0; i < m.edge_count; i++) {
                edge e = m.edges[i];

                // hidden-line removal for convex wireframe
                int visible = 0;
                for (int j = 0; j < m.triangle_count; j++) {
                    if (!triangle_front[j]) continue;
                    if (triangle_contains_edge(m.triangles[j], e.start, e.end)) {
                        visible = 1;
                        break;
                    }
                }
                if (!visible) continue;

                point start = projected[e.start];
                point end = projected[e.end];
                draw_aaline(start, end, accent[0], accent[1], accent[2]);
            }
        } else {
            for (int i = 0; i < m.triangle_count; i++) {
                if (!triangle_front[i]) continue;
                triangle t = m.triangles[i];

                point a = projected[t.a];
                point b = projected[t.b];
                point c = projected[t.c];

                draw_aaline(a, b, accent[0], accent[1], accent[2]);
                draw_aaline(b, c, accent[0], accent[1], accent[2]);
                draw_aaline(c, a, accent[0], accent[1], accent[2]);
            }
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
    free(triangle_front);
    free_mesh(&m);
    return 0;
}
