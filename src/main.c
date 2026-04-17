#include "renderer.h"
#include "loader.h"
#include "math_3D.h"
#include "mesh_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <GLFW/glfw3.h>

#define WINDOW_WIDTH 600
#define WINDOW_HEIGHT 600
#define MODEL_PATH_BUFFER_SIZE 256
#define DEFAULT_MODEL_NAME "cube"
#define MODEL_PATH_FORMAT "models/%s.obj"
#define AUTOSHOT_ENV_VAR "GRAPHIX_AUTOSHOT_PPM"

#define PI 3.14159265359f
#define TAU (2.0f * PI)

#define MIN_CAMERA_DISTANCE_PADDING 0.1f
#define MIN_CAMERA_ORBIT_DISTANCE 8.0f
#define LIGHT_DISTANCE_OFFSET 0.5f

#define INITIAL_MODEL_ANGLE_X 0.5f
#define INITIAL_MODEL_ANGLE_Y 0.5f
#define INITIAL_MODEL_ANGLE_Z 0.0f

#define CAMERA_MOVE_SPEED 6.0f
#define CAMERA_ZOOM_SPEED 0.5f
#define MOUSE_LOOK_SENSITIVITY 0.0025f
#define MAX_CAMERA_PITCH_FACTOR 0.49f

#define MODEL_ROTATE_SPEED_X 0.5f
#define MODEL_ROTATE_SPEED_Y 1.0f
#define MODEL_ROTATE_SPEED_Z 0.5f

#define MODEL_TO_CAMERA_ARRAY_SIZE 16
#define ZERO_PAN_OFFSET 0.0f

static const int BACKGROUND_RGB[3] = {0, 0, 0};
static const int ACCENT_RGB[3] = {255, 255, 255};
static const int FACE_BASE_RGB[3] = {200, 200, 200};

static const point WORLD_UP = {0.0f, 1.0f, 0.0f, 0.0f};
static const point FORWARD_FALLBACK = {0.0f, 0.0f, 1.0f, 0.0f};
static const point RIGHT_FALLBACK = {1.0f, 0.0f, 0.0f, 0.0f};
static const point CAMERA_CLAMP_FALLBACK = {0.0f, 0.0f, -1.0f, 0.0f};
static const point ORIGIN_POSITION = {0.0f, 0.0f, 0.0f, 1.0f};

// runs input and rendering for the active model
int main(int argc, char *argv[]) {
    // fallback to a default model if none presented
    const char *model_name = (argc > 1) ? argv[1] : DEFAULT_MODEL_NAME;

    char model_filepath[MODEL_PATH_BUFFER_SIZE];
    snprintf(model_filepath, sizeof(model_filepath), MODEL_PATH_FORMAT, model_name);

    // read model from disk
    mesh loaded_mesh = load_obj(model_filepath);
    if (!loaded_mesh.point_count) return 1;

    // standardise position
    centre(&loaded_mesh);

    // keep the camera outside the model bounds to avoid near-plane warping artifacts
    float model_radius = compute_mesh_radius(&loaded_mesh);
    float minimum_camera_distance = model_radius + MIN_CAMERA_DISTANCE_PADDING;

    // set default camera and light distance from the model centre
    float camera_orbit_distance = fmaxf(MIN_CAMERA_ORBIT_DISTANCE, minimum_camera_distance + LIGHT_DISTANCE_OFFSET);
    point default_camera_position = (point){0.0f, 0.0f, -camera_orbit_distance, 1.0f};
    point fixed_world_light_position = (point){0.0f, 0.0f, -camera_orbit_distance, 1.0f};
    float model_angle_x = INITIAL_MODEL_ANGLE_X;
    float model_angle_y = INITIAL_MODEL_ANGLE_Y;
    float model_angle_z = INITIAL_MODEL_ANGLE_Z;

    // set up the display window
    if (!init_renderer(WINDOW_WIDTH, WINDOW_HEIGHT, "Graphix")) return 1;

    mesh_buffers drawable_mesh = {0};
    drawable_mesh.gpu_mesh_id = -1;

    if (!build_mesh_buffers(&loaded_mesh, &drawable_mesh)) {
        free_mesh_buffers(&drawable_mesh);
        quit_renderer();
        free_mesh(&loaded_mesh);
        printf("Error: failed to build base GPU mesh\n");
        return 1;
    }

    // track frame time for frame-rate independent camera movement
    double last_time = glfwGetTime();

    const char *autoshot_ppm_path = getenv(AUTOSHOT_ENV_VAR);
    int autoshot_done = 0;

    // toggles pure wireframe mode (off by default)
    int wireframe_view = 0;
    int was_space_down = 0;

    // camera controls in first-person style
    point camera_position = default_camera_position;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float max_camera_pitch = PI * MAX_CAMERA_PITCH_FACTOR;
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

        // reset camera on R key press edge
        int is_r_down = key_down(GLFW_KEY_R);
        if (is_r_down && !was_r_down) {
            camera_position = default_camera_position;
            camera_yaw = 0.0f;
            camera_pitch = 0.0f;
        }
        was_r_down = is_r_down;

        // rotate camera while left mouse button is held and dragged
        float drag_delta_x = 0.0f;
        float drag_delta_y = 0.0f;
        get_left_mouse_drag_delta(&drag_delta_x, &drag_delta_y);
        camera_yaw += drag_delta_x * MOUSE_LOOK_SENSITIVITY;
        camera_pitch -= drag_delta_y * MOUSE_LOOK_SENSITIVITY;

        if (camera_pitch > max_camera_pitch) camera_pitch = max_camera_pitch;
        if (camera_pitch < -max_camera_pitch) camera_pitch = -max_camera_pitch;
        camera_yaw = fmodf(camera_yaw, TAU);

        point camera_forward = normalise_vector(
            (point){
                cosf(camera_pitch) * sinf(camera_yaw),
                sinf(camera_pitch),
                cosf(camera_pitch) * cosf(camera_yaw),
                0.0f
            },
            FORWARD_FALLBACK
        );
        point move_forward = normalise_vector(
            (point){camera_forward.x, 0.0f, camera_forward.z, 0.0f},
            FORWARD_FALLBACK
        );
        point move_right = normalise_vector(
            cross(WORLD_UP, move_forward),
            RIGHT_FALLBACK
        );

        // move camera in first-person style with wasdqe
        if (key_down(GLFW_KEY_W)) {
            camera_position.x += move_forward.x * CAMERA_MOVE_SPEED * dt;
            camera_position.z += move_forward.z * CAMERA_MOVE_SPEED * dt;
        }
        if (key_down(GLFW_KEY_S)) {
            camera_position.x -= move_forward.x * CAMERA_MOVE_SPEED * dt;
            camera_position.z -= move_forward.z * CAMERA_MOVE_SPEED * dt;
        }
        if (key_down(GLFW_KEY_A)) {
            camera_position.x -= move_right.x * CAMERA_MOVE_SPEED * dt;
            camera_position.z -= move_right.z * CAMERA_MOVE_SPEED * dt;
        }
        if (key_down(GLFW_KEY_D)) {
            camera_position.x += move_right.x * CAMERA_MOVE_SPEED * dt;
            camera_position.z += move_right.z * CAMERA_MOVE_SPEED * dt;
        }
        if (key_down(GLFW_KEY_Q)) camera_position.y += CAMERA_MOVE_SPEED * dt;
        if (key_down(GLFW_KEY_E)) camera_position.y -= CAMERA_MOVE_SPEED * dt;

        // handle camera zoom (scroll wheel)
        float scroll = get_scroll_offset();
        if (scroll != 0.0f) {
            camera_position.x += camera_forward.x * scroll * CAMERA_ZOOM_SPEED;
            camera_position.y += camera_forward.y * scroll * CAMERA_ZOOM_SPEED;
            camera_position.z += camera_forward.z * scroll * CAMERA_ZOOM_SPEED;
        }

        // keep the camera outside the model volume
        float camera_distance = vector_length(camera_position);
        if (camera_distance < minimum_camera_distance) {
            point from_origin = normalise_vector(
                camera_position,
                CAMERA_CLAMP_FALLBACK
            );
            camera_position.x = from_origin.x * minimum_camera_distance;
            camera_position.y = from_origin.y * minimum_camera_distance;
            camera_position.z = from_origin.z * minimum_camera_distance;
        }
        camera_position.w = 1.0f;

        // wipe previous frame with solid colour
        fill_background(BACKGROUND_RGB[0], BACKGROUND_RGB[1], BACKGROUND_RGB[2]);

        // build model and camera matrices, then combine to model-to-camera
        mat4 model_matrix = build_transform_matrix(
            model_angle_x,
            model_angle_y,
            model_angle_z,
            ORIGIN_POSITION
        );
        mat4 view_matrix = build_look_at_matrix(
            camera_position,
            (point){
                camera_position.x + camera_forward.x,
                camera_position.y + camera_forward.y,
                camera_position.z + camera_forward.z,
                1.0f
            }
        );
        mat4 model_to_camera = mat4_multiply(&view_matrix, &model_matrix);
        point light_camera_position = (point){
            view_matrix.m[0][0] * fixed_world_light_position.x + view_matrix.m[0][1] * fixed_world_light_position.y + view_matrix.m[0][2] * fixed_world_light_position.z + view_matrix.m[0][3] * fixed_world_light_position.w,
            view_matrix.m[1][0] * fixed_world_light_position.x + view_matrix.m[1][1] * fixed_world_light_position.y + view_matrix.m[1][2] * fixed_world_light_position.z + view_matrix.m[1][3] * fixed_world_light_position.w,
            view_matrix.m[2][0] * fixed_world_light_position.x + view_matrix.m[2][1] * fixed_world_light_position.y + view_matrix.m[2][2] * fixed_world_light_position.z + view_matrix.m[2][3] * fixed_world_light_position.w,
            view_matrix.m[3][0] * fixed_world_light_position.x + view_matrix.m[3][1] * fixed_world_light_position.y + view_matrix.m[3][2] * fixed_world_light_position.z + view_matrix.m[3][3] * fixed_world_light_position.w
        };

        float model_to_camera_array[MODEL_TO_CAMERA_ARRAY_SIZE];
        mat4_to_array(&model_to_camera, model_to_camera_array);

        renderer_draw_mesh(
            drawable_mesh.gpu_mesh_id,
            model_to_camera_array,
            ZERO_PAN_OFFSET,
            ZERO_PAN_OFFSET,
            light_camera_position.x,
            light_camera_position.y,
            light_camera_position.z,
            wireframe_view,
            FACE_BASE_RGB[0],
            FACE_BASE_RGB[1],
            FACE_BASE_RGB[2],
            ACCENT_RGB[0],
            ACCENT_RGB[1],
            ACCENT_RGB[2]
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

        // rotate the model so the form stays readable as 3D
        model_angle_x += MODEL_ROTATE_SPEED_X * dt;
        model_angle_y += MODEL_ROTATE_SPEED_Y * dt;
        model_angle_z += MODEL_ROTATE_SPEED_Z * dt;

        model_angle_x = fmodf(model_angle_x, TAU);
        model_angle_y = fmodf(model_angle_y, TAU);
        model_angle_z = fmodf(model_angle_z, TAU);
    }

    // prevent memory leaks
    free_mesh_buffers(&drawable_mesh);

    quit_renderer();
    free_mesh(&loaded_mesh);
    return 0;
}
