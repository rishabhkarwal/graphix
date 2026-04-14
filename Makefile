CC = clang
CFLAGS = -Wall -Wextra -O2 -std=c99 -I/opt/homebrew/include
LDFLAGS = -L/opt/homebrew/lib -lglfw -framework OpenGL -lm

SOURCES = src/main.c src/renderer.c src/loader.c
OBJECTS = $(SOURCES:.c=.o)
OUTPUT = graphix

all: $(OUTPUT)

$(OUTPUT): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(OUTPUT)
	./$(OUTPUT)

clean:
	rm -f $(OBJECTS) $(OUTPUT)

.PHONY: all run clean
