#ifndef RENDERER_H
#define RENDERER_H

#include "types.h"

// window properties
int init_renderer(int width, int height, const char *title);
void quit_renderer(void);
int events_quit(void);
int key_down(int key);
void get_left_mouse_drag_delta(float *out_delta_x, float *out_delta_y);
float get_scroll_offset(void);
int renderer_capture_framebuffer_ppm(const char *path);

int renderer_upload_mesh(
    const point *positions,
    int point_count,
    const triangle *triangles,
    int triangle_count,
    const edge *edges,
    int edge_count
);
void renderer_free_gpu_mesh(int mesh_id);
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
);

// frame functions
void fill_background(int r, int g, int b);
void update_display(void);

#endif
