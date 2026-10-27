# ====================================================================================
# Makefile del Editor de Texto (Wordsito) basado en llamadas al sistema
# ====================================================================================
# make          -> compila ./eafitOS (el shell con el editor) y ./huf (compresor CLI)
# make run      -> compila y además ejecuta ./eafitOS
# make test     -> compila y corre las pruebas unitarias (bitio, huffman)
# make clean    -> borra los .o y los binarios
# ====================================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -std=gnu99 -O2 -g -D_GNU_SOURCE -pthread
TARGET  = eafitOS

# Módulos del compresor, compartidos por el shell, el binario huf y las pruebas.
COMP_SRCS = bitio.c huffman.c huf_formato.c comprimir.c

# Módulos del shell. cat_editor.c es el que implementa el editor interactivo.
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_listado.c cat_editor.c buffer.c $(COMP_SRCS)
OBJS = $(SRCS:.c=.o)

COMP_OBJS = $(COMP_SRCS:.c=.o)

all: $(TARGET) huf

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Binario independiente del compresor; lo usan pruebas_huf.sh y pruebas_hilos.sh.
huf: huf_main.o $(COMP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

test_bitio: test_bitio.o bitio.o
	$(CC) $(CFLAGS) -o $@ $^

test_huffman: test_huffman.o huffman.o bitio.o
	$(CC) $(CFLAGS) -o $@ $^

# Cada .o depende de las cabeceras: si cambia una, se recompila todo.
%.o: %.c shell.h buffer.h bitio.h huffman.h huf_formato.h comprimir.h
	$(CC) $(CFLAGS) -c $< -o $@

# Antes 'all' ejecutaba el binario automáticamente, lo que rompía 'make' en
# entornos no interactivos. Ahora ejecutar es un objetivo aparte.
run: $(TARGET)
	./$(TARGET)

test: test_bitio test_huffman
	./test_bitio
	./test_huffman

clean:
	rm -f $(TARGET) huf test_bitio test_huffman *.o

.PHONY: all run test clean
