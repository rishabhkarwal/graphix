#include "renderer.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
} gpu_vertex;

static GLFWwindow *screen = NULL;
static int WIDTH = 0;
static int HEIGHT = 0;
static double last_time = 0;
static int frames = 0;
static double current_fps = 60.0;
static int triangle_batch_active = 0;
static int line_batch_active = 0;
static float scroll_offset = 0.0f;

static gpu_vertex *triangle_vertices = NULL;
static int triangle_vertex_count = 0;
static int triangle_vertex_capacity = 0;

static gpu_vertex *line_vertices = NULL;
static int line_vertex_count = 0;
static int line_vertex_capacity = 0;

static GLuint shader_program = 0;
static GLuint triangle_vbo = 0;
static GLuint line_vbo = 0;
static GLint position_attr = -1;
static GLint colour_attr = -1;
static GLint viewport_half_uniform = -1;
static GLint depth_range_uniform = -1;

static const char *vertex_shader_source =
    "#version 120\n"
    "attribute vec3 aPosition;\n"
    "attribute vec3 aColour;\n"
    "uniform vec2 uViewportHalf;\n"
    "uniform vec2 uDepthRange;\n"
    "varying vec3 vColour;\n"
    "void main() {\n"
    "  float clipX = aPosition.x / uViewportHalf.x;\n"
    "  float clipY = aPosition.y / uViewportHalf.y;\n"
    "  float depth = clamp((aPosition.z - uDepthRange.x) / (uDepthRange.y - uDepthRange.x), 0.0, 1.0);\n"
    "  float clipZ = depth * 2.0 - 1.0;\n"
    "  gl_Position = vec4(clipX, clipY, clipZ, 1.0);\n"
    "  vColour = aColour;\n"
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
    glBindAttribLocation(program, 1, "aColour");
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

static int ensure_vertex_capacity(gpu_vertex **vertices, int *capacity, int required_count) {
    if (required_count <= *capacity) return 1;

    int new_capacity = (*capacity > 0) ? *capacity : 1024;
    while (new_capacity < required_count) {
        new_capacity *= 2;
    }

    gpu_vertex *resized = realloc(*vertices, (size_t)new_capacity * sizeof(gpu_vertex));
    if (!resized) return 0;

    *vertices = resized;
    *capacity = new_capacity;
    return 1;
}

static void append_vertex(gpu_vertex *vertices, int index, point p, int r, int g, int b) {
    vertices[index].x = p.x;
    vertices[index].y = p.y;
    vertices[index].z = p.z;
    vertices[index].r = r / 255.0f;
    vertices[index].g = g / 255.0f;
    vertices[index].b = b / 255.0f;
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
    colour_attr = glGetAttribLocation(shader_program, "aColour");
    viewport_half_uniform = glGetUniformLocation(shader_program, "uViewportHalf");
    depth_range_uniform = glGetUniformLocation(shader_program, "uDepthRange");

    glGenBuffers(1, &triangle_vbo);
    glGenBuffers(1, &line_vbo);
    
    // set scroll callback for camera zoom
    glfwSetScrollCallback(screen, scroll_callback);
    
    last_time = glfwGetTime();
    return 1;
}

void quit_renderer(void) {
    // cleans up glfw resources
    if (triangle_vbo) glDeleteBuffers(1, &triangle_vbo);
    if (line_vbo) glDeleteBuffers(1, &line_vbo);
    if (shader_program) glDeleteProgram(shader_program);

    if (triangle_vertices) free(triangle_vertices);
    if (line_vertices) free(line_vertices);

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

void fill_background(int r, int g, int b) {
    // clears screen using the given RGB colour
    triangle_vertex_count = 0;
    line_vertex_count = 0;

    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void begin_triangle_batch(void) {
    triangle_batch_active = 1;
}

void end_triangle_batch(void) {
    triangle_batch_active = 0;
}

void begin_line_batch(void) {
    line_batch_active = 1;
}

void end_line_batch(void) {
    line_batch_active = 0;
}

void draw_triangle(
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
    int blue_c
) {
    if (!triangle_batch_active) {
        // support call sites that submit triangles without explicit batch scopes
    }

    int required_count = triangle_vertex_count + 3;
    if (!ensure_vertex_capacity(&triangle_vertices, &triangle_vertex_capacity, required_count)) return;

    append_vertex(triangle_vertices, triangle_vertex_count + 0, a, red_a, green_a, blue_a);
    append_vertex(triangle_vertices, triangle_vertex_count + 1, b, red_b, green_b, blue_b);
    append_vertex(triangle_vertices, triangle_vertex_count + 2, c, red_c, green_c, blue_c);
    triangle_vertex_count = required_count;
}

void draw_aaline(point start, point end, int r, int g, int b) {
    if (!line_batch_active) {
        // support call sites that submit lines without explicit batch scopes
    }

    int required_count = line_vertex_count + 2;
    if (!ensure_vertex_capacity(&line_vertices, &line_vertex_capacity, required_count)) return;

    append_vertex(line_vertices, line_vertex_count + 0, start, r, g, b);
    append_vertex(line_vertices, line_vertex_count + 1, end, r, g, b);
    line_vertex_count = required_count;
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

    glUseProgram(shader_program);
    glUniform2f(viewport_half_uniform, WIDTH * 0.5f, HEIGHT * 0.5f);
    glUniform2f(depth_range_uniform, 0.05f, 200.0f);

    glEnableVertexAttribArray((GLuint)position_attr);
    glEnableVertexAttribArray((GLuint)colour_attr);

    if (triangle_vertex_count > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, triangle_vbo);
        glBufferData(GL_ARRAY_BUFFER, (size_t)triangle_vertex_count * sizeof(gpu_vertex), triangle_vertices, GL_DYNAMIC_DRAW);

        glVertexAttribPointer((GLuint)position_attr, 3, GL_FLOAT, GL_FALSE, sizeof(gpu_vertex), (const void *)0);
        glVertexAttribPointer((GLuint)colour_attr, 3, GL_FLOAT, GL_FALSE, sizeof(gpu_vertex), (const void *)(3 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, triangle_vertex_count);
    }

    if (line_vertex_count > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, line_vbo);
        glBufferData(GL_ARRAY_BUFFER, (size_t)line_vertex_count * sizeof(gpu_vertex), line_vertices, GL_DYNAMIC_DRAW);

        glVertexAttribPointer((GLuint)position_attr, 3, GL_FLOAT, GL_FALSE, sizeof(gpu_vertex), (const void *)0);
        glVertexAttribPointer((GLuint)colour_attr, 3, GL_FLOAT, GL_FALSE, sizeof(gpu_vertex), (const void *)(3 * sizeof(float)));
        glDrawArrays(GL_LINES, 0, line_vertex_count);
    }

    glDisableVertexAttribArray((GLuint)position_attr);
    glDisableVertexAttribArray((GLuint)colour_attr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(0);

    glfwSwapBuffers(screen);
}
