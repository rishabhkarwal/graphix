#include "renderer.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <math.h>

static GLFWwindow *screen = NULL;
static int WIDTH = 0;
static int HEIGHT = 0;
static double last_time = 0;
static int frames = 0;
static double current_fps = 60.0;

int init_renderer(int width, int height, const char *title) {
    WIDTH = width; HEIGHT = height;
    if (!glfwInit()) return 0;
    
    // minimal opengl config to lock aspect ratio
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    screen = glfwCreateWindow(width, height, title, NULL, NULL);
    if (!screen) return 0;
    
    glfwMakeContextCurrent(screen);
    glfwSwapInterval(1); // 0 = unlocked fps, 1 = vsync (monitor refresh rate)
    
    // setup 2D cartesian orthographic view (origin at centre, y points up)
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-width / 2.0, width / 2.0, -height / 2.0, height / 2.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glShadeModel(GL_SMOOTH);
    
    // enable anti-aliasing for smooth lines
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    last_time = glfwGetTime();
    return 1;
}

void quit_renderer(void) {
    // cleans up glfw resources
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

void fill_background(int r, int g, int b) {
    // clears screen using the given RGB colour
    glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
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
    // draws a triangle with per-vertex colours for smooth shading
    glBegin(GL_TRIANGLES);
    glColor3f(red_a / 255.0f, green_a / 255.0f, blue_a / 255.0f);
    glVertex2f(a.x, a.y);

    glColor3f(red_b / 255.0f, green_b / 255.0f, blue_b / 255.0f);
    glVertex2f(b.x, b.y);

    glColor3f(red_c / 255.0f, green_c / 255.0f, blue_c / 255.0f);
    glVertex2f(c.x, c.y);
    glEnd();
}

void draw_aaline(point start, point end, int r, int g, int b) {
    // draws an anti-aliased line between two co-ordinates
    glColor3f(r / 255.0f, g / 255.0f, b / 255.0f);
    glBegin(GL_LINES);
    glVertex2f(start.x, start.y);
    glVertex2f(end.x, end.y);
    glEnd();
}

void update_display(void) {
    // swaps buffers and updates fps in the window title
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
    
    glFlush();
    glfwSwapBuffers(screen);
}
