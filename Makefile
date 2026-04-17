CC = clang
CFLAGS = -Wall -Wextra -O2 -std=c99 -I/opt/homebrew/include
LDFLAGS = -L/opt/homebrew/lib -lglfw -framework OpenGL -lm

SOURCES = src/main.c src/renderer.c src/loader.c src/math_3D.c src/mesh_build.c
OBJECTS = $(SOURCES:.c=.o)
LEGACY_OBJECTS = src/math3d.o src/math3d_utils.o src/lod.o
OUTPUT = graphix

all: $(OUTPUT)

$(OUTPUT): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(OUTPUT)
	./$(OUTPUT)

clean:
	rm -f $(OBJECTS) $(LEGACY_OBJECTS) $(OUTPUT)

.PHONY: all run clean
