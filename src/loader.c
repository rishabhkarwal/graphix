#include "loader.h"
#include <stdio.h>
#include <stdlib.h>

mesh load_obj(const char *filename) {
    mesh m = {0};
    FILE *f = fopen(filename, "r");
    
    if (!f) {
        printf("Error: could not find or open '%s'\n", filename);
        return m;
    }
    
    // start with a small allocation pool
    int point_cap = 1024;
    int edge_cap = 1024;
    point *points = malloc(point_cap * sizeof(point));
    edge *edges = malloc(edge_cap * sizeof(edge));
    
    int point_count = 0, edge_count = 0, face_count = 0;
    
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            // parse vertex: v ...
            float x, y, z;
            if (sscanf(line, "v %f %f %f", &x, &y, &z) == 3) {
                // dynamically resize points array if full
                if (point_count >= point_cap) {
                    point_cap *= 2;
                    points = realloc(points, point_cap * sizeof(point));
                }
                points[point_count++] = (point){x, y, z};
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            face_count++;
            
            // parse face: f ...
            // note: obj indices are 1-based
            int v[64], count = 0; // 64 is safe enough for complex polygons
            char *token = line + 2;
            
            while (*token != '\0') {
                while (*token == ' ') token++;
                if (*token == '\0' || *token == '\n') break;
                
                int index;
                if (sscanf(token, "%d", &index) == 1) {
                    if (count < 64) v[count++] = index - 1; // convert to 0-based
                }
                while (*token != ' ' && *token != '\0' && *token != '\n') token++;
            }
            
            // convert face to edges
            for (int i = 0; i < count; i++) {
                // dynamically resize edges array
                if (edge_count >= edge_cap) {
                    edge_cap *= 2;
                    edges = realloc(edges, edge_cap * sizeof(edge));
                }
                int start = v[i];
                int end = v[(i + 1) % count]; // wrap around to connect shape
                edges[edge_count++] = (edge){start, end};
            }
        }
    }
    fclose(f);
    
    // trim arrays perfectly to size to save memory
    m.point_count = point_count;
    m.points = realloc(points, point_count * sizeof(point));
        
    m.edge_count = edge_count;
    m.edges = realloc(edges, edge_count * sizeof(edge));
    
    printf("Loaded '%s' successfully: %d vertices, %d faces\n", filename, point_count, face_count);
        
    return m;
}

void centre(mesh *m) {
    if (m->point_count == 0) return;
    
    // define bounding box
    float min_x = m->points[0].x, max_x = m->points[0].x;
    float min_y = m->points[0].y, max_y = m->points[0].y;
    float min_z = m->points[0].z, max_z = m->points[0].z;
    
    for (int i = 1; i < m->point_count; i++) {
        point p = m->points[i];
        if (p.x < min_x) min_x = p.x;
        if (p.x > max_x) max_x = p.x;
        if (p.y < min_y) min_y = p.y;
        if (p.y > max_y) max_y = p.y;
        if (p.z < min_z) min_z = p.z;
        if (p.z > max_z) max_z = p.z;
    }
    
    // calculate the centre co-ordinate
    float centre_x = (min_x + max_x) / 2.0f;
    float centre_y = (min_y + max_y) / 2.0f;
    float centre_z = (min_z + max_z) / 2.0f;
    
    // shift all points so the centre becomes (0, 0, 0)
    for (int i = 0; i < m->point_count; i++) {
        m->points[i].x -= centre_x;
        m->points[i].y -= centre_y;
        m->points[i].z -= centre_z;
    }
}

void free_mesh(mesh *m) {
    // clear dynamic memory to prevent memory leaks
    if (m->points) free(m->points);
    if (m->edges) free(m->edges);
    m->point_count = 0;
    m->edge_count = 0;
}
