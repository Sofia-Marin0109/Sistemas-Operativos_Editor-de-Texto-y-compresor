#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stddef.h>
#include <stdint.h>
#include "bitio.h"

/**
 * ====================================================================================
 * CODIFICACIÓN DE HUFFMAN
 * ====================================================================================
 * Decide QUÉ bits le corresponde a cada byte. El cómo escribirlos es de bitio.c.
 *
 * Idea: a los bytes frecuentes, códigos cortos; a los raros, códigos largos. El árbol
 * se construye uniendo repetidamente los dos nodos menos frecuentes bajo un padre,
 * hasta que queda uno solo. El código de un símbolo es el camino desde la raíz hasta
 * su hoja: izquierda = 0, derecha = 1.
 *
 * PROPIEDAD DE PREFIJO: como todos los símbolos quedan en HOJAS, ningún código es
 * prefijo de otro. Por eso la secuencia de bits se decodifica sin separadores ni
 * longitudes: se baja por el árbol y al tocar una hoja termina un símbolo.
 *
 * ESTE MÓDULO NO SABE NADA DE HILOS. Es puro algoritmo, sin estado global y sin
 * llamadas al sistema, y por eso se puede probar de forma aislada. La concurrencia
 * vive una capa más arriba, en el compresor.
 */

#define HUF_NSIMBOLOS  256                     /* un byte: 0..255 */
#define HUF_MAX_NODOS  (2 * HUF_NSIMBOLOS - 1) /* un árbol binario lleno con 256 hojas */
#define HUF_MAX_LONGITUD 64                    /* cabe en el uint64_t de 'codigo' */

/**
 * Código asignado a un símbolo.
 * 'codigo' guarda los bits alineados a la derecha: el código 101 se almacena como 5.
 * 'longitud' dice cuántos bits son significativos; sin ella, 101 y 0101 serían
 * indistinguibles. longitud == 0 significa que el símbolo no aparece en el archivo.
 */
typedef struct {
    uint64_t codigo;
    uint8_t  longitud;
} Codigo;

/**
 * Nodo del árbol.
 *
 * 'orden' es la clave del DETERMINISMO. Cuando dos nodos empatan en frecuencia, la
 * cola de prioridad debe sacarlos siempre en el mismo orden, o el compresor y el
 * descompresor construirían árboles distintos con códigos distintos y el archivo
 * saldría como basura. Las hojas usan su propio símbolo (0..255) y los nodos internos
 * un contador creciente (256, 257, ...), así que no hay dos 'orden' iguales y el
 * desempate es total.
 */
typedef struct NodoHuf {
    uint64_t freq;
    uint64_t orden;
    int      simbolo;   /* 0..255 en hojas; -1 en nodos internos */
    struct NodoHuf *izq, *der;
} NodoHuf;

typedef struct {
    NodoHuf *nodos;       /* arena: TODOS los nodos en un solo bloque */
    size_t   n_nodos;     /* nodos usados de la arena */
    NodoHuf *raiz;        /* NULL si el archivo está vacío */
    Codigo   tabla[HUF_NSIMBOLOS];
    int      n_distintos; /* cuántos símbolos distintos aparecen */
} Huffman;

/* ------------------------------------------------------------------ FRECUENCIAS */

/* Pone freq[] en ceros. */
void huf_freq_init(uint64_t freq[HUF_NSIMBOLOS]);

/* Cuenta las apariciones de cada byte de datos[0..n) sobre freq[] (acumula).
   Sin estado global: cada hilo puede llamarla con SU PROPIO arreglo local, sin mutex.
   Esa es la "reducción local" que pide el enunciado. */
void huf_contar(const unsigned char *datos, size_t n, uint64_t freq[HUF_NSIMBOLOS]);

/* dst[i] += src[i]. Es el paso de reducción: cada hilo funde su conteo local en el
   global UNA sola vez, en vez de bloquear un mutex por cada byte leído. */
void huf_acumular(uint64_t dst[HUF_NSIMBOLOS], const uint64_t src[HUF_NSIMBOLOS]);

/* ------------------------------------------------------------------ ÁRBOL */

/* Construye el árbol y la tabla de códigos a partir de las frecuencias.
   Con 0 símbolos distintos deja raiz = NULL (archivo vacío) y retorna 0.
   Retorna -1 si falta memoria o si algún código superara HUF_MAX_LONGITUD bits. */
int huf_construir(Huffman *h, const uint64_t freq[HUF_NSIMBOLOS]);

/* Libera la arena. Deja la estructura reutilizable. */
void huf_liberar(Huffman *h);

/* ------------------------------------------------------------------ USO */

/* Escribe el código del símbolo c en el escritor de bits. Retorna 0 / -1. */
int huf_emitir(const Huffman *h, BitWriter *w, unsigned char c);

/* Lee bits hasta completar un símbolo bajando por el árbol.
   Retorna 0..255, o -1 si se acabaron los bits o el árbol está vacío. */
int huf_decodificar(const Huffman *h, BitReader *r);

/* Bits que ocupará el contenido comprimido con estas frecuencias. Sirve para
   reservar buffers y para reportar la tasa de compresión. */
uint64_t huf_bits_totales(const Huffman *h, const uint64_t freq[HUF_NSIMBOLOS]);

#endif /* HUFFMAN_H */
