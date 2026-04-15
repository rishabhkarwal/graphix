#include "loader.h"
#include <stdio.h>
#include <stdlib.h>

// loads an obj mesh and builds adjacency data for fast lookups
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
    int triangle_cap = 1024;
    point *points = malloc(point_cap * sizeof(point));
    edge *edges = malloc(edge_cap * sizeof(edge));
    triangle *triangles = malloc(triangle_cap * sizeof(triangle));
    
    int point_count = 0, edge_count = 0, triangle_count = 0, face_count = 0;
    
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
                points[point_count++] = (point){x, y, z, 1.0f};
            }
        } else if (line[0] == 'f' && line[1] == ' ') {
            face_count++;
            
            // parse face: f ...
            int v[64], count = 0; // 64 is safe enough for complex polygons
            char *token = line + 2;
            
            while (*token != '\0') {
                while (*token == ' ') token++;
                if (*token == '\0' || *token == '\n') break;

                char *token_end = token;
                while (*token_end != ' ' && *token_end != '\0' && *token_end != '\n') token_end++;

                char saved = *token_end;
                *token_end = '\0';

                // supports obj token styles: v, v/vt, v//vn, v/vt/vn
                int raw_index = 0;
                if (sscanf(token, "%d", &raw_index) == 1) {
                    int index = -1;

                    // obj index rules: positive = 1-based, negative = relative to current end
                    if (raw_index > 0) {
                        index = raw_index - 1;
                    } else if (raw_index < 0) {
                        index = point_count + raw_index;
                    }

                    if (index >= 0 && index < point_count && count < 64) {
                        v[count++] = index;
                    }
                }

                *token_end = saved;
                token = token_end;
            }
            
            // convert face to edges for wireframe
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

            // triangulate each polygon face using fan triangulation
            for (int i = 1; i + 1 < count; i++) {
                if (triangle_count >= triangle_cap) {
                    triangle_cap *= 2;
                    triangles = realloc(triangles, triangle_cap * sizeof(triangle));
                }

                triangles[triangle_count++] = (triangle){
                    v[0],
                    v[i],
                    v[i + 1]
                };
            }
        }
    }
    fclose(f);
    
    // trim arrays perfectly to size to save memory
    m.point_count = point_count;
    m.points = realloc(points, point_count * sizeof(point));
        
    m.edge_count = edge_count;
    m.edges = realloc(edges, edge_count * sizeof(edge));

    m.triangle_count = triangle_count;
    m.triangles = realloc(triangles, triangle_count * sizeof(triangle));

    // build per-vertex triangle adjacency once so normal lookups do not scan the whole mesh every frame
    if (point_count > 0) {
        m.vertex_adjacencies = calloc(point_count, sizeof(vertex_adjacency));

        int *triangle_counts = calloc(point_count, sizeof(int));
        int *triangle_offsets = calloc(point_count, sizeof(int));

        for (int i = 0; i < triangle_count; i++) {
            triangle t = m.triangles[i];
            triangle_counts[t.a]++;
            triangle_counts[t.b]++;
            triangle_counts[t.c]++;
        }

        for (int i = 0; i < point_count; i++) {
            m.vertex_adjacencies[i].triangle_count = triangle_counts[i];
            if (triangle_counts[i] > 0) {
                m.vertex_adjacencies[i].triangle_indices = malloc(triangle_counts[i] * sizeof(int));
            }
        }

        for (int i = 0; i < triangle_count; i++) {
            triangle t = m.triangles[i];
            m.vertex_adjacencies[t.a].triangle_indices[triangle_offsets[t.a]++] = i;
            m.vertex_adjacencies[t.b].triangle_indices[triangle_offsets[t.b]++] = i;
            m.vertex_adjacencies[t.c].triangle_indices[triangle_offsets[t.c]++] = i;
        }

        free(triangle_counts);
        free(triangle_offsets);
    }
    
    printf("Loaded '%s' successfully: %d vertices, %d faces, %d triangles\n", filename, point_count, face_count, triangle_count);
        
    return m;
}

// recentres mesh points around the origin
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

// releases all allocated mesh memory
void free_mesh(mesh *m) {
    // clear dynamic memory to prevent memory leaks
    if (m->points) free(m->points);
    if (m->edges) free(m->edges);
    if (m->triangles) free(m->triangles);
    if (m->vertex_adjacencies) {
        for (int i = 0; i < m->point_count; i++) {
            if (m->vertex_adjacencies[i].triangle_indices) {
                free(m->vertex_adjacencies[i].triangle_indices);
            }
        }
        free(m->vertex_adjacencies);
    }
    m->point_count = 0;
    m->edge_count = 0;
    m->triangle_count = 0;
    m->vertex_adjacencies = NULL;
}
