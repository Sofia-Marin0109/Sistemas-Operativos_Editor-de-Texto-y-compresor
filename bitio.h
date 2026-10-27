#ifndef BITIO_H
#define BITIO_H

#include <stddef.h>
#include <stdint.h>

/**
 * ====================================================================================
 * EMPAQUETADO DE BITS
 * ====================================================================================
 * Huffman produce códigos de longitud variable en BITS ("a" = 0, "b" = 10), pero los
 * archivos se escriben en BYTES. Este módulo hace de traductor entre ambos mundos.
 *
 * DECISIÓN DE DISEÑO: el escritor acumula en un buffer de MEMORIA, no en un descriptor
 * de archivo.
 *
 * Es lo que hace posible la concurrencia posterior. Cada hilo trabajador tendrá su
 * propio BitWriter privado y codificará su bloque sin compartir absolutamente nada con
 * los demás: cero mutex, cero contención. Un único hilo escritor toma después esos
 * buffers ya terminados y los vuelca al archivo en el orden correcto.
 *
 * Si este módulo recibiera un 'fd', todos los hilos competirían por la posición del
 * archivo y habría que serializarlos: el paralelismo desaparecería.
 *
 * CONVENCIÓN DE ORDEN: los bits se empaquetan de MÁS significativo a MENOS
 * significativo dentro de cada byte. El primer bit escrito ocupa la posición 7 (0x80),
 * el segundo la 6, y así. Es la convención habitual y hace que un volcado hexadecimal
 * sea legible. Lo único que importa de verdad es que el lector use la MISMA
 * convención; si divergen, el archivo se descomprime como basura.
 *
 *   bits escritos:  1 0 1 1 0 0 1 0
 *   byte resultante: 0xB2
 *                    ^ primer bit escrito
 */

/* ------------------------------------------------------------------ ESCRITOR */

typedef struct {
    unsigned char *datos;      /* buffer de salida, propiedad del writer */
    size_t         cap;        /* bytes reservados */
    size_t         nbytes;     /* bytes ya cerrados en 'datos' */
    unsigned char  acum;       /* byte en construcción, aún incompleto */
    int            nbits_acum; /* cuántos bits lleva 'acum' (0..7) */
    uint64_t       total_bits; /* bits escritos en total; va a la cabecera del .huf */
} BitWriter;

/* Inicializa el escritor. cap_inicial en bytes (0 => se usa un valor por defecto).
   Retorna 0 en éxito, -1 si no hay memoria. */
int bw_init(BitWriter *w, size_t cap_inicial);

/* Escribe un bit (cualquier valor != 0 cuenta como 1). Retorna 0 / -1. */
int bw_bit(BitWriter *w, int bit);

/* Escribe los n bits menos significativos de 'valor', del más significativo al menos.
   n debe estar entre 0 y 64. Retorna 0 / -1. */
int bw_bits(BitWriter *w, uint64_t valor, int n);

/* Cierra el byte en construcción rellenando con ceros a la derecha.
   OBLIGATORIO antes de leer w->datos. Idempotente.
   El relleno es inofensivo: el lector se detiene en total_bits y nunca lo ve. */
int bw_flush(BitWriter *w);

/* Libera el buffer y deja el escritor reutilizable. */
void bw_free(BitWriter *w);

/* ------------------------------------------------------------------ LECTOR */

typedef struct {
    const unsigned char *datos;
    uint64_t             total_bits; /* límite real; ignora el relleno del último byte */
    uint64_t             pos_bit;    /* siguiente bit a leer */
} BitReader;

/* El lector NO toma propiedad de 'datos': solo lo apunta. */
void br_init(BitReader *r, const unsigned char *datos, uint64_t total_bits);

/* Retorna 0 o 1, o -1 si ya se consumieron los total_bits. */
int br_bit(BitReader *r);

/* Lee n bits (0..64) y los deja en *out. Retorna 0, o -1 si no quedan suficientes. */
int br_bits(BitReader *r, int n, uint64_t *out);

/* Bits que faltan por leer. */
uint64_t br_restantes(const BitReader *r);

#endif /* BITIO_H */
