#include "mesh_build.h"
#include "renderer.h"
#include "math_3D.h"
#include <stdlib.h>
#include <limits.h>

#define TRIANGLE_EDGE_COUNT 3
#define MESH_CENTROID_DIVISOR 3.0f
#define EDGE_HASH_MULTIPLIER_A 73856093u
#define EDGE_HASH_MULTIPLIER_B 19349663u
#define EDGE_BUCKET_LOAD_NUMERATOR 3LL
#define EDGE_BUCKET_LOAD_DENOMINATOR 2LL
#define EDGE_BUCKET_MIN_COUNT 17LL

// ensures triangle winding is outward after centring
void orient_mesh_triangles_outward(const point *points, triangle *triangles, int triangle_count) {
    for (int i = 0; i < triangle_count; i++) {
        triangle t = triangles[i];

        point a = points[t.a];
        point b = points[t.b];
        point c = points[t.c];

        point ab = subtract(b, a);
        point ac = subtract(c, a);
        point normal = cross(ab, ac);

        // meshes are centred before buffer build, so outward normals align with triangle centroid direction.
        point centroid = (point){
            (a.x + b.x + c.x) / MESH_CENTROID_DIVISOR,
            (a.y + b.y + c.y) / MESH_CENTROID_DIVISOR,
            (a.z + b.z + c.z) / MESH_CENTROID_DIVISOR,
            1.0f
        };

        if (dot(normal, centroid) < 0.0f) {
            int tmp = triangles[i].b;
            triangles[i].b = triangles[i].c;
            triangles[i].c = tmp;
        }
    }
}

// hashes a canonical edge pair for bucket lookup
static unsigned int edge_hash(int start, int end) {
    return ((unsigned int)start * EDGE_HASH_MULTIPLIER_A) ^ ((unsigned int)end * EDGE_HASH_MULTIPLIER_B);
}

// selects a conservative bucket count for edge hash tables
static int edge_bucket_count_for_items(int item_count) {
    if (item_count < 0) return 0;

    long long suggested = ((long long)item_count * EDGE_BUCKET_LOAD_NUMERATOR) / EDGE_BUCKET_LOAD_DENOMINATOR + 1LL;
    if (suggested < EDGE_BUCKET_MIN_COUNT) suggested = EDGE_BUCKET_MIN_COUNT;
    if (suggested > INT_MAX) return 0;

    return (int)suggested;
}

// builds a unique undirected edge list from triangle indices
int build_mesh_unique_edges(const triangle *triangles, int triangle_count, edge **out_edges, int *out_edge_count) {
    *out_edges = NULL;
    *out_edge_count = 0;
    if (triangle_count <= 0) return 1;

    if (triangle_count > INT_MAX / TRIANGLE_EDGE_COUNT) return 0;
    int max_edge_count = triangle_count * TRIANGLE_EDGE_COUNT;
    int bucket_count = edge_bucket_count_for_items(max_edge_count);
    if (bucket_count <= 0) return 0;

    edge *unique_edges = malloc((size_t)max_edge_count * sizeof(edge));
    int *next_indices = malloc((size_t)max_edge_count * sizeof(int));
    int *buckets = malloc((size_t)bucket_count * sizeof(int));
    if (!unique_edges || !next_indices || !buckets) {
        free(unique_edges);
        free(next_indices);
        free(buckets);
        return 0;
    }

    for (int i = 0; i < bucket_count; i++) {
        buckets[i] = -1;
    }

    int unique_count = 0;
    for (int i = 0; i < triangle_count; i++) {
        triangle t = triangles[i];

        int edge_starts[TRIANGLE_EDGE_COUNT];
        int edge_ends[TRIANGLE_EDGE_COUNT];

        edge_starts[0] = t.a < t.b ? t.a : t.b;
        edge_ends[0] = t.a < t.b ? t.b : t.a;

        edge_starts[1] = t.b < t.c ? t.b : t.c;
        edge_ends[1] = t.b < t.c ? t.c : t.b;

        edge_starts[2] = t.c < t.a ? t.c : t.a;
        edge_ends[2] = t.c < t.a ? t.a : t.c;

        for (int edge_i = 0; edge_i < TRIANGLE_EDGE_COUNT; edge_i++) {
            int start = edge_starts[edge_i];
            int end = edge_ends[edge_i];
            int bucket = (int)(edge_hash(start, end) % (unsigned int)bucket_count);
            int existing = buckets[bucket];
            int found = 0;

            while (existing >= 0) {
                if (unique_edges[existing].start == start && unique_edges[existing].end == end) {
                    found = 1;
                    break;
                }
                existing = next_indices[existing];
            }

            if (!found) {
                int new_index = unique_count++;
                unique_edges[new_index] = (edge){start, end};
                next_indices[new_index] = buckets[bucket];
                buckets[bucket] = new_index;
            }
        }
    }

    free(next_indices);
    free(buckets);

    if (unique_count > 0) {
        edge *shrunk = realloc(unique_edges, (size_t)unique_count * sizeof(edge));
        if (shrunk) {
            unique_edges = shrunk;
        }
    }

    *out_edges = unique_edges;
    *out_edge_count = unique_count;
    return 1;
}

// frees cpu and gpu resources owned by one mesh buffer set
void free_mesh_buffers(mesh_buffers *buffers) {
    if (!buffers) return;

    if (buffers->gpu_mesh_id >= 0) {
        renderer_free_gpu_mesh(buffers->gpu_mesh_id);
    }

    if (buffers->points) free(buffers->points);
    if (buffers->triangles) free(buffers->triangles);
    if (buffers->edges) free(buffers->edges);

    *buffers = (mesh_buffers){0};
    buffers->gpu_mesh_id = -1;
}

// creates GPU-ready mesh buffers from the loaded mesh
int build_mesh_buffers(const mesh *source, mesh_buffers *buffers) {
    buffers->gpu_mesh_id = -1;
    buffers->built = 0;
    buffers->point_count = source->point_count;
    buffers->triangle_count = source->triangle_count;

    buffers->points = malloc((size_t)buffers->point_count * sizeof(point));
    buffers->triangles = malloc((size_t)buffers->triangle_count * sizeof(triangle));
    if (!buffers->points || !buffers->triangles) return 0;

    for (int i = 0; i < buffers->point_count; i++) {
        buffers->points[i] = source->points[i];
    }
    for (int i = 0; i < buffers->triangle_count; i++) {
        buffers->triangles[i] = source->triangles[i];
    }

    orient_mesh_triangles_outward(buffers->points, buffers->triangles, buffers->triangle_count);

    if (!build_mesh_unique_edges(buffers->triangles, buffers->triangle_count, &buffers->edges, &buffers->edge_count)) return 0;

    buffers->gpu_mesh_id = renderer_upload_mesh(
        buffers->points,
        buffers->point_count,
        buffers->triangles,
        buffers->triangle_count,
        buffers->edges,
        buffers->edge_count
    );

    if (buffers->gpu_mesh_id < 0) return 0;
    buffers->built = 1;
    return 1;
}
