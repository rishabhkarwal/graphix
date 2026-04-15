#ifndef RENDERER_H
#define RENDERER_H

#include "types.h"

// window properties
int init_renderer(int width, int height, const char *title);
void quit_renderer(void);
int events_quit(void);
int key_down(int key);
float get_scroll_offset(void);

// drawing functions
void fill_background(int r, int g, int b);
void begin_triangle_batch(void);
void end_triangle_batch(void);
void begin_line_batch(void);
void end_line_batch(void);
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
);
void draw_aaline(point start, point end, int r, int g, int b);
void update_display(void);

#endif
