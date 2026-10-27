#ifndef COMPRIMIR_H
#define COMPRIMIR_H

#include <stddef.h>
#include <stdint.h>
#include "huf_formato.h"   /* HufProgreso */

/**
 * ====================================================================================
 * COMPRESOR CONCURRENTE
 * ====================================================================================
 * ESTE ES EL ÚNICO ARCHIVO DEL PROYECTO QUE USA HILOS.
 *
 * bitio.c, huffman.c y huf_formato.c no se modificaron para paralelizar: siguen
 * siendo código secuencial puro. Eso fue posible porque se diseñaron sin estado
 * global, recibiendo y devolviendo todo por parámetro. La concurrencia es una capa
 * que se monta encima, no algo que haya que inyectar en cada módulo.
 *
 * ARQUITECTURA (productor / consumidor con ventana acotada)
 * ---------------------------------------------------------
 *   1. FASE DE CONTEO. N hilos cuentan frecuencias sobre tramos disjuntos, cada uno
 *      en su propio arreglo local. Al terminar, cada uno funde su conteo en el global
 *      una sola vez, bajo mutex. Es la "reducción local": un bloqueo por hilo en vez
 *      de uno por byte.
 *
 *   2. BARRERA. El árbol debe construirse con las frecuencias de TODO el archivo, así
 *      que el conteo tiene que terminar por completo antes de empezar a codificar.
 *      Aquí la barrera es el pthread_join de los hilos de conteo.
 *
 *   3. FASE DE CODIFICACIÓN. N hilos toman bloques de una cola compartida y los
 *      codifican, cada uno en su propio BitWriter. Terminan en desorden.
 *
 *   4. ESCRITOR ORDENADO. El hilo coordinador escribe el bloque b en cuanto está
 *      listo, en orden estricto. Si el que le toca aún no está, DUERME en una
 *      variable de condición (0% de CPU), nunca en un bucle de espera activa.
 *
 * POR QUÉ UNA VENTANA ACOTADA
 * ---------------------------
 * Si los trabajadores pudieran adelantarse sin límite, todos los bloques comprimidos
 * se acumularían en memoria: con un archivo de 1 GB serían 16384 bloques vivos a la
 * vez. La ventana impide codificar más allá de VENTANA bloques por delante del
 * escritor, lo que deja el uso de memoria en O(hilos) en vez de O(tamaño del
 * archivo). Es el clásico búfer acotado, y es la razón de que hagan falta DOS
 * variables de condición: una para avisar "hay un bloque listo" y otra para avisar
 * "se liberó espacio".
 *
 * EL ÍNDICE SE RELLENA AL FINAL
 * -----------------------------
 * El índice con los bits de cada bloque va en la cabecera, pero no se conoce hasta
 * haber codificado todo. Se reserva su espacio con ceros, se escriben los bloques, y
 * al terminar se vuelve con lseek(2) a sobrescribirlo con los valores reales.
 *
 * INVARIANTE QUE DEFINE LA CORRECCIÓN
 * -----------------------------------
 * El .huf debe salir IDÉNTICO byte por byte sin importar cuántos hilos se usen:
 *
 *     HILOS=1 ./huf c entrada.txt a.huf
 *     HILOS=8 ./huf c entrada.txt b.huf
 *     cmp a.huf b.huf      # debe pasar en silencio
 *
 * Se cumple porque la suma de frecuencias es asociativa, el árbol es determinista y
 * el escritor impone el orden. Es un detector de carreras que NO depende de la
 * suerte, a diferencia de ejecutar el programa y esperar a que falle.
 */

/* Cuántos bloques pueden ir por delante del escritor, por hilo. */
#define COMP_VENTANA_POR_HILO 4

/* Hilos a usar: núcleos disponibles (sysconf), sobreescribible con la variable de
   entorno HILOS. Nunca menos de 1. */
int comp_num_hilos(void);

/* Cuenta las frecuencias de datos[0..n) repartiendo el trabajo entre n_hilos,
   con reducción local. freq[] se inicializa dentro. Retorna 0 / -1. */
int comp_contar(const unsigned char *datos, size_t n,
                uint64_t freq[256], int n_hilos);

/* Comprime datos[0..n) escribiendo el .huf completo en 'fd'.
   'fd' debe admitir lseek: un archivo normal, no una tubería. Retorna 0 / -1. */
int comp_comprimir_a_fd(const unsigned char *datos, size_t n, int fd);

/* Igual, informando el avance tras escribir cada bloque. cb puede ser NULL.
   El callback se invoca desde el hilo escritor, no desde los trabajadores. Si devuelve
   distinto de 0, la compresión se detiene, los hilos terminan y retorna HUF_CANCELADO;
   el contenido ya escrito en 'fd' queda incompleto y lo debe descartar el llamador.
   Retorna 0, -1 (error) o HUF_CANCELADO. */
int comp_comprimir_a_fd_cb(const unsigned char *datos, size_t n, int fd,
                           HufProgreso cb, void *ctx);

/* Envoltorio archivo -> archivo. Borra la salida parcial si algo falla. */
int comp_comprimir_archivo(const char *entrada, const char *salida);

#endif /* COMPRIMIR_H */
