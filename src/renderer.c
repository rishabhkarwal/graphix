#include "renderer.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SHADER_LOG_BUFFER_SIZE 1024
#define DEFAULT_FPS_VALUE 60.0
#define MESH_INITIAL_CAPACITY 8
#define MESH_CAPACITY_GROWTH_FACTOR 2
#define NORMAL_EPSILON 0.000001f
#define NORMAL_FALLBACK_Z 1.0f
#define RGB_COMPONENT_COUNT 3
#define TRIANGLE_VERTEX_COUNT 3
#define TRIANGLE_FLOAT_COMPONENT_COUNT 9
#define EDGE_VERTEX_COUNT 2
#define COLOUR_MAX_COMPONENT 255.0f
#define VIEWPORT_HALF_SCALE 0.5f
#define FPS_UPDATE_INTERVAL_SECONDS 1.0
#define WINDOW_TITLE_BUFFER_SIZE 64

typedef struct {
    GLuint vbo_positions;
    GLuint vbo_shaded_positions;
    GLuint vbo_shaded_normals;
    GLuint ibo_lines;
    GLsizei shaded_vertex_count;
    GLsizei line_index_count;
    int in_use;
} gpu_mesh;

static GLFWwindow *screen = NULL;
static int screen_width = 0;
static int screen_height = 0;
static double last_time = 0;
static int frames = 0;
static double current_fps = DEFAULT_FPS_VALUE;
static float scroll_offset = 0.0f;
static double last_cursor_x = 0.0;
static double last_cursor_y = 0.0;
static int was_left_mouse_down = 0;

static GLuint shader_program = 0;
static GLint position_attr = -1;
static GLint normal_attr = -1;
static GLint viewport_half_uniform = -1;
static GLint depth_range_uniform = -1;
static GLint model_to_camera_uniform = -1;
static GLint pan_uniform = -1;
static GLint use_lighting_uniform = -1;
static GLint base_colour_uniform = -1;
static GLint wire_colour_uniform = -1;
static GLint projection_scale_uniform = -1;
static GLint lighting_mix_uniform = -1;
static GLint light_camera_uniform = -1;

static const float RENDER_NEAR_DEPTH = 0.05f;
static const float RENDER_FAR_DEPTH = 200.0f;
static const float RENDER_PROJECTION_SCALE = 800.0f;
static const float RENDER_AMBIENT_LIGHT = 0.15f;
static const float RENDER_DIFFUSE_LIGHT_SCALE = 0.85f;

static gpu_mesh *meshes = NULL;
static int mesh_count = 0;
static int mesh_capacity = 0;

static const char *vertex_shader_source =
    "#version 120\n"
    "attribute vec3 a_position;\n"
    "attribute vec3 a_normal;\n"
    "uniform mat4 u_model_to_camera;\n"
    "uniform vec2 u_viewport_half;\n"
    "uniform vec2 u_depth_range;\n"
    "uniform float u_projection_scale;\n"
    "uniform vec2 u_lighting_mix;\n"
    "uniform vec3 u_light_camera;\n"
    "uniform vec2 u_pan;\n"
    "uniform int u_use_lighting;\n"
    "uniform vec3 u_base_colour;\n"
    "uniform vec3 u_wire_colour;\n"
    "varying vec3 v_colour;\n"
    "void main() {\n"
    "  vec4 camera_pos = u_model_to_camera * vec4(a_position, 1.0);\n"
    "  float near_depth = u_depth_range.x;\n"
    "  float far_depth = u_depth_range.y;\n"
    "  float camera_z = camera_pos.z;\n"
    "  float clip_w = camera_z;\n"
    "  const float clip_w_epsilon = 0.0001;\n"
    "  if (abs(clip_w) < clip_w_epsilon) {\n"
    "    clip_w = (clip_w < 0.0) ? -clip_w_epsilon : clip_w_epsilon;\n"
    "  }\n"
    "  float clip_x = (camera_pos.x * u_projection_scale + u_pan.x * camera_z) / u_viewport_half.x;\n"
    "  float clip_y = (camera_pos.y * u_projection_scale + u_pan.y * camera_z) / u_viewport_half.y;\n"
    "  float clip_z_ndc = ((camera_z - near_depth) / (far_depth - near_depth)) * 2.0 - 1.0;\n"
    "  float clip_z = clip_z_ndc * clip_w;\n"
    "  gl_Position = vec4(clip_x, clip_y, clip_z, clip_w);\n"
    "  if (u_use_lighting == 1) {\n"
    "    vec3 normal_camera = normalize(mat3(u_model_to_camera) * a_normal);\n"
    "    vec3 to_light_camera = normalize(u_light_camera - camera_pos.xyz);\n"
    "    float diffuse = max(dot(normal_camera, to_light_camera), 0.0);\n"
    "    float intensity = min(1.0, u_lighting_mix.x + u_lighting_mix.y * diffuse);\n"
    "    v_colour = u_base_colour * intensity;\n"
    "  } else {\n"
    "    v_colour = u_wire_colour;\n"
    "  }\n"
    "}\n";

static const char *fragment_shader_source =
    "#version 120\n"
    "varying vec3 v_colour;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(v_colour, 1.0);\n"
    "}\n";

// compiles a single shader stage and reports compile errors
static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[SHADER_LOG_BUFFER_SIZE];
        GLsizei log_length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &log_length, log);
        fprintf(stderr, "Shader compile error: %.*s\n", (int)log_length, log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

// links vertex and fragment stages into one shader program
static GLuint create_shader_program(void) {
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    if (!vertex_shader) return 0;

    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!fragment_shader) {
        glDeleteShader(vertex_shader);
        return 0;
    }

    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return 0;
    }

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_normal");
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[SHADER_LOG_BUFFER_SIZE];
        GLsizei log_length = 0;
        glGetProgramInfoLog(program, sizeof(log), &log_length, log);
        fprintf(stderr, "Program link error: %.*s\n", (int)log_length, log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    return program;
}

// grows the mesh slot array to fit the requested count
static int ensure_mesh_capacity(int required_count) {
    if (required_count <= mesh_capacity) return 1;

    int new_capacity = (mesh_capacity > 0) ? mesh_capacity : MESH_INITIAL_CAPACITY;
    while (new_capacity < required_count) {
        new_capacity *= MESH_CAPACITY_GROWTH_FACTOR;
    }

    gpu_mesh *resized = realloc(meshes, (size_t)new_capacity * sizeof(gpu_mesh));
    if (!resized) return 0;

    for (int i = mesh_capacity; i < new_capacity; i++) {
        resized[i] = (gpu_mesh){0};
    }

    meshes = resized;
    mesh_capacity = new_capacity;
    return 1;
}

// computes a face normal for one triangle
static void write_triangle_normal(
    float ax, float ay, float az,
    float bx, float by, float bz,
    float cx, float cy, float cz,
    float *nx,
    float *ny,
    float *nz
) {
    float abx = bx - ax;
    float aby = by - ay;
    float abz = bz - az;
    float acx = cx - ax;
    float acy = cy - ay;
    float acz = cz - az;

    float x = aby * acz - abz * acy;
    float y = abz * acx - abx * acz;
    float z = abx * acy - aby * acx;

    float length = sqrtf(x * x + y * y + z * z);
    if (length <= NORMAL_EPSILON) {
        *nx = 0.0f;
        *ny = 0.0f;
        *nz = NORMAL_FALLBACK_Z;
        return;
    }

    *nx = x / length;
    *ny = y / length;
    *nz = z / length;
}

// releases opengl buffers associated with one gpu mesh
static void free_mesh_gpu_resources(gpu_mesh *mesh) {
    if (!mesh || !mesh->in_use) return;

    if (mesh->vbo_positions) glDeleteBuffers(1, &mesh->vbo_positions);
    if (mesh->vbo_shaded_positions) glDeleteBuffers(1, &mesh->vbo_shaded_positions);
    if (mesh->vbo_shaded_normals) glDeleteBuffers(1, &mesh->vbo_shaded_normals);
    if (mesh->ibo_lines) glDeleteBuffers(1, &mesh->ibo_lines);

    *mesh = (gpu_mesh){0};
}

// finds or allocates a free gpu mesh slot
static int allocate_mesh_slot(void) {
    for (int i = 0; i < mesh_count; i++) {
        if (!meshes[i].in_use) return i;
    }

    if (!ensure_mesh_capacity(mesh_count + 1)) return -1;
    return mesh_count++;
}

// accumulates scroll input for frame-based consumption
static void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;
    // accumulate wheel movement so the main loop can consume it once per frame
    scroll_offset += (float)yoffset;
}

// initialises glfw, opengl state, and shader handles
int init_renderer(int width, int height, const char *title) {
    screen_width = width;
    screen_height = height;
    if (!glfwInit()) return 0;
    
    // minimal opengl config to lock aspect ratio
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    screen = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!screen) return 0;
    
    glfwMakeContextCurrent(screen);
    glfwSwapInterval(0); // 0 = unlocked fps, 1 = vsync (monitor refresh rate)
    
    // enable anti-aliasing for smooth lines
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);

    shader_program = create_shader_program();
    if (!shader_program) return 0;

    position_attr = glGetAttribLocation(shader_program, "a_position");
    normal_attr = glGetAttribLocation(shader_program, "a_normal");
    viewport_half_uniform = glGetUniformLocation(shader_program, "u_viewport_half");
    depth_range_uniform = glGetUniformLocation(shader_program, "u_depth_range");
    model_to_camera_uniform = glGetUniformLocation(shader_program, "u_model_to_camera");
    pan_uniform = glGetUniformLocation(shader_program, "u_pan");
    use_lighting_uniform = glGetUniformLocation(shader_program, "u_use_lighting");
    base_colour_uniform = glGetUniformLocation(shader_program, "u_base_colour");
    wire_colour_uniform = glGetUniformLocation(shader_program, "u_wire_colour");
    projection_scale_uniform = glGetUniformLocation(shader_program, "u_projection_scale");
    lighting_mix_uniform = glGetUniformLocation(shader_program, "u_lighting_mix");
    light_camera_uniform = glGetUniformLocation(shader_program, "u_light_camera");
    
    // set scroll callback for camera zoom
    glfwSetScrollCallback(screen, scroll_callback);

    // initialise drag tracking from current cursor position
    glfwGetCursorPos(screen, &last_cursor_x, &last_cursor_y);
    was_left_mouse_down = 0;
    
    last_time = glfwGetTime();
    return 1;
}

// tears down gpu state and closes the window
void quit_renderer(void) {
    // cleans up glfw resources
    for (int i = 0; i < mesh_count; i++) {
        free_mesh_gpu_resources(&meshes[i]);
    }

    if (shader_program) glDeleteProgram(shader_program);
    if (meshes) free(meshes);

    meshes = NULL;
    mesh_count = 0;
    mesh_capacity = 0;

    if (screen) glfwDestroyWindow(screen);
    glfwTerminate();
}

// polls events and reports whether the app should exit
int events_quit(void) {
    // handles system events and checks for escape key
    glfwPollEvents();
    if (glfwWindowShouldClose(screen) || glfwGetKey(screen, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        return 1;
    }
    return 0;
}

// returns whether a key is currently pressed
int key_down(int key) {
    if (!screen) return 0;
    return glfwGetKey(screen, key) == GLFW_PRESS;
}

// returns cursor delta while left mouse button is held
void get_left_mouse_drag_delta(float *out_delta_x, float *out_delta_y) {
    if (!out_delta_x || !out_delta_y) return;

    *out_delta_x = 0.0f;
    *out_delta_y = 0.0f;
    if (!screen) return;

    double cursor_x = 0.0;
    double cursor_y = 0.0;
    glfwGetCursorPos(screen, &cursor_x, &cursor_y);

    int is_left_mouse_down = glfwGetMouseButton(screen, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (is_left_mouse_down && was_left_mouse_down) {
        *out_delta_x = (float)(cursor_x - last_cursor_x);
        *out_delta_y = (float)(cursor_y - last_cursor_y);
    }

    last_cursor_x = cursor_x;
    last_cursor_y = cursor_y;
    was_left_mouse_down = is_left_mouse_down;
}

// returns and resets accumulated scroll delta
float get_scroll_offset(void) {
    float result = scroll_offset;
    scroll_offset = 0.0f;
    return result;
}

// captures the back buffer and writes it as a binary ppm image
int renderer_capture_framebuffer_ppm(const char *path) {
    if (!path || !screen) return 0;

    int byte_count = screen_width * screen_height * RGB_COMPONENT_COUNT;
    if (byte_count <= 0) return 0;

    unsigned char *pixels = malloc((size_t)byte_count);
    if (!pixels) return 0;

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, screen_width, screen_height, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    FILE *out = fopen(path, "wb");
    if (!out) {
        free(pixels);
        return 0;
    }

    fprintf(out, "P6\n%d %d\n255\n", screen_width, screen_height);

    // glReadPixels returns bottom-up rows; write top-down for normal image orientation.
    for (int y = screen_height - 1; y >= 0; y--) {
        fwrite(
            pixels + (size_t)y * (size_t)screen_width * RGB_COMPONENT_COUNT,
            1,
            (size_t)screen_width * RGB_COMPONENT_COUNT,
            out
        );
    }

    fclose(out);
    free(pixels);
    return 1;
}

// uploads mesh buffers for shaded and wireframe render paths
int renderer_upload_mesh(
    const point *positions,
    int point_count,
    const triangle *triangles,
    int triangle_count,
    const edge *edges,
    int edge_count
) {
    if (!positions || !triangles || !edges) return -1;
    if (point_count <= 0 || triangle_count <= 0 || edge_count < 0) return -1;

    int mesh_id = allocate_mesh_slot();
    if (mesh_id < 0) return -1;

    float *position_data = malloc((size_t)point_count * RGB_COMPONENT_COUNT * sizeof(float));
    float *shaded_position_data = malloc((size_t)triangle_count * TRIANGLE_FLOAT_COMPONENT_COUNT * sizeof(float));
    float *shaded_normal_data = malloc((size_t)triangle_count * TRIANGLE_FLOAT_COMPONENT_COUNT * sizeof(float));
    unsigned int *line_indices = malloc((size_t)edge_count * EDGE_VERTEX_COUNT * sizeof(unsigned int));

    if (!position_data || !shaded_position_data || !shaded_normal_data || (!line_indices && edge_count > 0)) {
        if (position_data) free(position_data);
        if (shaded_position_data) free(shaded_position_data);
        if (shaded_normal_data) free(shaded_normal_data);
        if (line_indices) free(line_indices);
        return -1;
    }

    for (int i = 0; i < point_count; i++) {
        position_data[i * RGB_COMPONENT_COUNT + 0] = positions[i].x;
        position_data[i * RGB_COMPONENT_COUNT + 1] = positions[i].y;
        position_data[i * RGB_COMPONENT_COUNT + 2] = positions[i].z;
    }

    for (int i = 0; i < triangle_count; i++) {
        point pa = positions[triangles[i].a];
        point pb = positions[triangles[i].b];
        point pc = positions[triangles[i].c];

        float nx = 0.0f, ny = 0.0f, nz = 1.0f;
        write_triangle_normal(pa.x, pa.y, pa.z, pb.x, pb.y, pb.z, pc.x, pc.y, pc.z, &nx, &ny, &nz);

        int base = i * TRIANGLE_FLOAT_COMPONENT_COUNT;
        shaded_position_data[base + 0] = pa.x;
        shaded_position_data[base + 1] = pa.y;
        shaded_position_data[base + 2] = pa.z;
        shaded_position_data[base + 3] = pb.x;
        shaded_position_data[base + 4] = pb.y;
        shaded_position_data[base + 5] = pb.z;
        shaded_position_data[base + 6] = pc.x;
        shaded_position_data[base + 7] = pc.y;
        shaded_position_data[base + 8] = pc.z;

        shaded_normal_data[base + 0] = nx;
        shaded_normal_data[base + 1] = ny;
        shaded_normal_data[base + 2] = nz;
        shaded_normal_data[base + 3] = nx;
        shaded_normal_data[base + 4] = ny;
        shaded_normal_data[base + 5] = nz;
        shaded_normal_data[base + 6] = nx;
        shaded_normal_data[base + 7] = ny;
        shaded_normal_data[base + 8] = nz;
    }

    for (int i = 0; i < edge_count; i++) {
        line_indices[i * EDGE_VERTEX_COUNT + 0] = (unsigned int)edges[i].start;
        line_indices[i * EDGE_VERTEX_COUNT + 1] = (unsigned int)edges[i].end;
    }

    gpu_mesh mesh = {0};
    glGenBuffers(1, &mesh.vbo_positions);
    glGenBuffers(1, &mesh.vbo_shaded_positions);
    glGenBuffers(1, &mesh.vbo_shaded_normals);
    glGenBuffers(1, &mesh.ibo_lines);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_positions);
    glBufferData(GL_ARRAY_BUFFER, (size_t)point_count * RGB_COMPONENT_COUNT * sizeof(float), position_data, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_shaded_positions);
    glBufferData(
        GL_ARRAY_BUFFER,
        (size_t)triangle_count * TRIANGLE_FLOAT_COMPONENT_COUNT * sizeof(float),
        shaded_position_data,
        GL_STATIC_DRAW
    );

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_shaded_normals);
    glBufferData(
        GL_ARRAY_BUFFER,
        (size_t)triangle_count * TRIANGLE_FLOAT_COMPONENT_COUNT * sizeof(float),
        shaded_normal_data,
        GL_STATIC_DRAW
    );

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo_lines);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)edge_count * EDGE_VERTEX_COUNT * sizeof(unsigned int), line_indices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    free(position_data);
    free(shaded_position_data);
    free(shaded_normal_data);
    free(line_indices);

    mesh.shaded_vertex_count = (GLsizei)(triangle_count * TRIANGLE_VERTEX_COUNT);
    mesh.line_index_count = (GLsizei)(edge_count * EDGE_VERTEX_COUNT);
    mesh.in_use = 1;
    meshes[mesh_id] = mesh;

    return mesh_id;
}

// frees one uploaded gpu mesh by id
void renderer_free_gpu_mesh(int mesh_id) {
    if (mesh_id < 0 || mesh_id >= mesh_count) return;
    free_mesh_gpu_resources(&meshes[mesh_id]);
}

// draws one mesh in shaded or wireframe mode
void renderer_draw_mesh(
    int mesh_id,
    const float *model_to_camera,
    float pan_x,
    float pan_y,
    float light_camera_x,
    float light_camera_y,
    float light_camera_z,
    int wireframe,
    int base_r,
    int base_g,
    int base_b,
    int wire_r,
    int wire_g,
    int wire_b
) {
    if (mesh_id < 0 || mesh_id >= mesh_count || !model_to_camera) return;

    gpu_mesh *mesh = &meshes[mesh_id];
    if (!mesh->in_use) return;

    glUseProgram(shader_program);
    glUniformMatrix4fv(model_to_camera_uniform, 1, GL_TRUE, model_to_camera);
    glUniform2f(viewport_half_uniform, screen_width * VIEWPORT_HALF_SCALE, screen_height * VIEWPORT_HALF_SCALE);
    glUniform2f(depth_range_uniform, RENDER_NEAR_DEPTH, RENDER_FAR_DEPTH);
    glUniform1f(projection_scale_uniform, RENDER_PROJECTION_SCALE);
    glUniform2f(lighting_mix_uniform, RENDER_AMBIENT_LIGHT, RENDER_DIFFUSE_LIGHT_SCALE);
    glUniform2f(pan_uniform, pan_x, pan_y);
    glUniform3f(light_camera_uniform, light_camera_x, light_camera_y, light_camera_z);
    glUniform3f(base_colour_uniform, base_r / COLOUR_MAX_COMPONENT, base_g / COLOUR_MAX_COMPONENT, base_b / COLOUR_MAX_COMPONENT);
    glUniform3f(wire_colour_uniform, wire_r / COLOUR_MAX_COMPONENT, wire_g / COLOUR_MAX_COMPONENT, wire_b / COLOUR_MAX_COMPONENT);

    glEnableVertexAttribArray((GLuint)position_attr);
    glEnableVertexAttribArray((GLuint)normal_attr);

    if (wireframe) {
        glUniform1i(use_lighting_uniform, 0);
        glDisable(GL_CULL_FACE);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_positions);
        glVertexAttribPointer((GLuint)position_attr, RGB_COMPONENT_COUNT, GL_FLOAT, GL_FALSE, 0, (const void *)0);
        glVertexAttrib3f((GLuint)normal_attr, 0.0f, 0.0f, NORMAL_FALLBACK_Z);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo_lines);
        glDrawElements(GL_LINES, mesh->line_index_count, GL_UNSIGNED_INT, (const void *)0);
    } else {
        glUniform1i(use_lighting_uniform, 1);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CW);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_shaded_positions);
        glVertexAttribPointer((GLuint)position_attr, RGB_COMPONENT_COUNT, GL_FLOAT, GL_FALSE, 0, (const void *)0);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_shaded_normals);
        glVertexAttribPointer((GLuint)normal_attr, RGB_COMPONENT_COUNT, GL_FLOAT, GL_FALSE, 0, (const void *)0);

        glDrawArrays(GL_TRIANGLES, 0, mesh->shaded_vertex_count);
    }

    glDisableVertexAttribArray((GLuint)position_attr);
    glDisableVertexAttribArray((GLuint)normal_attr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

// clears colour and depth buffers for a new frame
void fill_background(int r, int g, int b) {
    // clears screen using the given RGB colour
    glClearColor(r / COLOUR_MAX_COMPONENT, g / COLOUR_MAX_COMPONENT, b / COLOUR_MAX_COMPONENT, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// presents the frame and updates fps in the title bar
void update_display(void) {
    // updates fps in the window title
    double now = glfwGetTime();
    frames++;
    if (now - last_time >= FPS_UPDATE_INTERVAL_SECONDS) {
        current_fps = frames / (now - last_time);
        frames = 0;
        last_time = now;
        
        char title[WINDOW_TITLE_BUFFER_SIZE];
        snprintf(title, sizeof(title), "FPS: %.0f", current_fps);
        glfwSetWindowTitle(screen, title); // write fps to title bar
    }

    glfwSwapBuffers(screen);
}
