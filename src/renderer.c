#include "renderer.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

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
static int WIDTH = 0;
static int HEIGHT = 0;
static double last_time = 0;
static int frames = 0;
static double current_fps = 60.0;
static float scroll_offset = 0.0f;

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
    "attribute vec3 aPosition;\n"
    "attribute vec3 aNormal;\n"
    "uniform mat4 uModelToCamera;\n"
    "uniform vec2 uViewportHalf;\n"
    "uniform vec2 uDepthRange;\n"
    "uniform float uProjectionScale;\n"
    "uniform vec2 uLightingMix;\n"
    "uniform vec2 uPan;\n"
    "uniform int uUseLighting;\n"
    "uniform vec3 uBaseColor;\n"
    "uniform vec3 uWireColor;\n"
    "varying vec3 vColour;\n"
    "void main() {\n"
    "  vec4 cameraPos = uModelToCamera * vec4(aPosition, 1.0);\n"
    "  float z = max(cameraPos.z, uDepthRange.x);\n"
    "  float screenX = cameraPos.x * uProjectionScale / z + uPan.x;\n"
    "  float screenY = cameraPos.y * uProjectionScale / z + uPan.y;\n"
    "  float depth = clamp((cameraPos.z - uDepthRange.x) / (uDepthRange.y - uDepthRange.x), 0.0, 1.0);\n"
    "  float clipZ = depth * 2.0 - 1.0;\n"
    "  gl_Position = vec4(screenX / uViewportHalf.x, screenY / uViewportHalf.y, clipZ, 1.0);\n"
    "  if (uUseLighting == 1) {\n"
    "    vec3 normalCamera = normalize(mat3(uModelToCamera) * aNormal);\n"
    "    vec3 toLight = normalize(-cameraPos.xyz);\n"
    "    float diffuse = max(dot(normalCamera, toLight), 0.0);\n"
    "    float intensity = min(1.0, uLightingMix.x + uLightingMix.y * diffuse);\n"
    "    vColour = uBaseColor * intensity;\n"
    "  } else {\n"
    "    vColour = uWireColor;\n"
    "  }\n"
    "}\n";

static const char *fragment_shader_source =
    "#version 120\n"
    "varying vec3 vColour;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(vColour, 1.0);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        GLsizei log_length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &log_length, log);
        fprintf(stderr, "Shader compile error: %.*s\n", (int)log_length, log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

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
    glBindAttribLocation(program, 0, "aPosition");
    glBindAttribLocation(program, 1, "aNormal");
    glLinkProgram(program);

    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
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

static int ensure_mesh_capacity(int required_count) {
    if (required_count <= mesh_capacity) return 1;

    int new_capacity = (mesh_capacity > 0) ? mesh_capacity : 8;
    while (new_capacity < required_count) {
        new_capacity *= 2;
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
    if (length <= 0.000001f) {
        *nx = 0.0f;
        *ny = 0.0f;
        *nz = 1.0f;
        return;
    }

    *nx = x / length;
    *ny = y / length;
    *nz = z / length;
}

static void free_mesh_gpu_resources(gpu_mesh *mesh) {
    if (!mesh || !mesh->in_use) return;

    if (mesh->vbo_positions) glDeleteBuffers(1, &mesh->vbo_positions);
    if (mesh->vbo_shaded_positions) glDeleteBuffers(1, &mesh->vbo_shaded_positions);
    if (mesh->vbo_shaded_normals) glDeleteBuffers(1, &mesh->vbo_shaded_normals);
    if (mesh->ibo_lines) glDeleteBuffers(1, &mesh->ibo_lines);

    *mesh = (gpu_mesh){0};
}

static int allocate_mesh_slot(void) {
    for (int i = 0; i < mesh_count; i++) {
        if (!meshes[i].in_use) return i;
    }

    if (!ensure_mesh_capacity(mesh_count + 1)) return -1;
    return mesh_count++;
}

static void scroll_callback(GLFWwindow *window, double xoffset, double yoffset) {
    (void)window;
    (void)xoffset;
    // accumulate wheel movement so the main loop can consume it once per frame
    scroll_offset += (float)yoffset;
}

int init_renderer(int width, int height, const char *title) {
    WIDTH = width; HEIGHT = height;
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

    position_attr = glGetAttribLocation(shader_program, "aPosition");
    normal_attr = glGetAttribLocation(shader_program, "aNormal");
    viewport_half_uniform = glGetUniformLocation(shader_program, "uViewportHalf");
    depth_range_uniform = glGetUniformLocation(shader_program, "uDepthRange");
    model_to_camera_uniform = glGetUniformLocation(shader_program, "uModelToCamera");
    pan_uniform = glGetUniformLocation(shader_program, "uPan");
    use_lighting_uniform = glGetUniformLocation(shader_program, "uUseLighting");
    base_colour_uniform = glGetUniformLocation(shader_program, "uBaseColor");
    wire_colour_uniform = glGetUniformLocation(shader_program, "uWireColor");
    projection_scale_uniform = glGetUniformLocation(shader_program, "uProjectionScale");
    lighting_mix_uniform = glGetUniformLocation(shader_program, "uLightingMix");
    
    // set scroll callback for camera zoom
    glfwSetScrollCallback(screen, scroll_callback);
    
    last_time = glfwGetTime();
    return 1;
}

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

int events_quit(void) {
    // handles system events and checks for escape key
    glfwPollEvents();
    if (glfwWindowShouldClose(screen) || glfwGetKey(screen, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        return 1;
    }
    return 0;
}

int key_down(int key) {
    if (!screen) return 0;
    return glfwGetKey(screen, key) == GLFW_PRESS;
}

float get_scroll_offset(void) {
    float result = scroll_offset;
    scroll_offset = 0.0f;
    return result;
}

int renderer_capture_framebuffer_ppm(const char *path) {
    if (!path || !screen) return 0;

    int byte_count = WIDTH * HEIGHT * 3;
    if (byte_count <= 0) return 0;

    unsigned char *pixels = malloc((size_t)byte_count);
    if (!pixels) return 0;

    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGB, GL_UNSIGNED_BYTE, pixels);

    FILE *out = fopen(path, "wb");
    if (!out) {
        free(pixels);
        return 0;
    }

    fprintf(out, "P6\n%d %d\n255\n", WIDTH, HEIGHT);

    // glReadPixels returns bottom-up rows; write top-down for normal image orientation.
    for (int y = HEIGHT - 1; y >= 0; y--) {
        fwrite(pixels + (size_t)y * (size_t)WIDTH * 3, 1, (size_t)WIDTH * 3, out);
    }

    fclose(out);
    free(pixels);
    return 1;
}

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

    float *position_data = malloc((size_t)point_count * 3 * sizeof(float));
    float *shaded_position_data = malloc((size_t)triangle_count * 9 * sizeof(float));
    float *shaded_normal_data = malloc((size_t)triangle_count * 9 * sizeof(float));
    unsigned int *line_indices = malloc((size_t)edge_count * 2 * sizeof(unsigned int));

    if (!position_data || !shaded_position_data || !shaded_normal_data || (!line_indices && edge_count > 0)) {
        if (position_data) free(position_data);
        if (shaded_position_data) free(shaded_position_data);
        if (shaded_normal_data) free(shaded_normal_data);
        if (line_indices) free(line_indices);
        return -1;
    }

    for (int i = 0; i < point_count; i++) {
        position_data[i * 3 + 0] = positions[i].x;
        position_data[i * 3 + 1] = positions[i].y;
        position_data[i * 3 + 2] = positions[i].z;
    }

    for (int i = 0; i < triangle_count; i++) {
        point pa = positions[triangles[i].a];
        point pb = positions[triangles[i].b];
        point pc = positions[triangles[i].c];

        float nx = 0.0f, ny = 0.0f, nz = 1.0f;
        write_triangle_normal(pa.x, pa.y, pa.z, pb.x, pb.y, pb.z, pc.x, pc.y, pc.z, &nx, &ny, &nz);

        int base = i * 9;
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
        line_indices[i * 2 + 0] = (unsigned int)edges[i].start;
        line_indices[i * 2 + 1] = (unsigned int)edges[i].end;
    }

    gpu_mesh mesh = {0};
    glGenBuffers(1, &mesh.vbo_positions);
    glGenBuffers(1, &mesh.vbo_shaded_positions);
    glGenBuffers(1, &mesh.vbo_shaded_normals);
    glGenBuffers(1, &mesh.ibo_lines);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_positions);
    glBufferData(GL_ARRAY_BUFFER, (size_t)point_count * 3 * sizeof(float), position_data, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_shaded_positions);
    glBufferData(GL_ARRAY_BUFFER, (size_t)triangle_count * 9 * sizeof(float), shaded_position_data, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_shaded_normals);
    glBufferData(GL_ARRAY_BUFFER, (size_t)triangle_count * 9 * sizeof(float), shaded_normal_data, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo_lines);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)edge_count * 2 * sizeof(unsigned int), line_indices, GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    free(position_data);
    free(shaded_position_data);
    free(shaded_normal_data);
    free(line_indices);

    mesh.shaded_vertex_count = (GLsizei)(triangle_count * 3);
    mesh.line_index_count = (GLsizei)(edge_count * 2);
    mesh.in_use = 1;
    meshes[mesh_id] = mesh;

    return mesh_id;
}

void renderer_free_gpu_mesh(int mesh_id) {
    if (mesh_id < 0 || mesh_id >= mesh_count) return;
    free_mesh_gpu_resources(&meshes[mesh_id]);
}

void renderer_draw_mesh(
    int mesh_id,
    const float *model_to_camera,
    float pan_x,
    float pan_y,
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
    glUniform2f(viewport_half_uniform, WIDTH * 0.5f, HEIGHT * 0.5f);
    glUniform2f(depth_range_uniform, RENDER_NEAR_DEPTH, RENDER_FAR_DEPTH);
    glUniform1f(projection_scale_uniform, RENDER_PROJECTION_SCALE);
    glUniform2f(lighting_mix_uniform, RENDER_AMBIENT_LIGHT, RENDER_DIFFUSE_LIGHT_SCALE);
    glUniform2f(pan_uniform, pan_x, pan_y);
    glUniform3f(base_colour_uniform, base_r / 255.0f, base_g / 255.0f, base_b / 255.0f);
    glUniform3f(wire_colour_uniform, wire_r / 255.0f, wire_g / 255.0f, wire_b / 255.0f);

    glEnableVertexAttribArray((GLuint)position_attr);
    glEnableVertexAttribArray((GLuint)normal_attr);

    if (wireframe) {
        glUniform1i(use_lighting_uniform, 0);
        glDisable(GL_CULL_FACE);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_positions);
        glVertexAttribPointer((GLuint)position_attr, 3, GL_FLOAT, GL_FALSE, 0, (const void *)0);
        glVertexAttrib3f((GLuint)normal_attr, 0.0f, 0.0f, 1.0f);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo_lines);
        glDrawElements(GL_LINES, mesh->line_index_count, GL_UNSIGNED_INT, (const void *)0);
    } else {
        glUniform1i(use_lighting_uniform, 1);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CW);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_shaded_positions);
        glVertexAttribPointer((GLuint)position_attr, 3, GL_FLOAT, GL_FALSE, 0, (const void *)0);

        glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo_shaded_normals);
        glVertexAttribPointer((GLuint)normal_attr, 3, GL_FLOAT, GL_FALSE, 0, (const void *)0);

        glDrawArrays(GL_TRIANGLES, 0, mesh->shaded_vertex_count);
    }

    glDisableVertexAttribArray((GLuint)position_attr);
    glDisableVertexAttribArray((GLuint)normal_attr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

void fill_background(int r, int g, int b) {
    // clears screen using the given RGB colour
    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void update_display(void) {
    // updates fps in the window title
    double now = glfwGetTime();
    frames++;
    if (now - last_time >= 1.0) {
        current_fps = frames / (now - last_time);
        frames = 0;
        last_time = now;
        
        char title[64];
        snprintf(title, sizeof(title), "FPS: %.0f", current_fps);
        glfwSetWindowTitle(screen, title); // write fps to title bar
    }

    glfwSwapBuffers(screen);
}
