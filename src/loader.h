#ifndef LOADER_H
#define LOADER_H

#include "types.h"

// parses obj file into a mesh
mesh load_obj(const char *filename);

// shifts points so the mesh revolves around the origin
void centre(mesh *m);

// frees allocated memory
void free_mesh(mesh *m);

#endif
