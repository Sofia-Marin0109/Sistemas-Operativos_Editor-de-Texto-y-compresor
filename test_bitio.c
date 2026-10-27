/* Prueba independiente del módulo bitio.
   Compilar:  gcc -Wall -Wextra -std=gnu99 -g bitio.c test_bitio.c -o test_bitio  */

#include "bitio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ok = 0, fallos = 0;

static void check(int cond, const char *desc) {
    if (cond) { printf("  PASA  %s\n", desc); ok++; }
    else      { printf("  FALLA %s\n", desc); fallos++; }
}

/* 1. La convención MSB primero produce el byte esperado. */
static void prueba_convencion(void) {
    BitWriter w;
    bw_init(&w, 0);
    int bits[] = {1,0,1,1,0,0,1,0};      /* debe dar 0xB2 */
    for (int i = 0; i < 8; i++) bw_bit(&w, bits[i]);
    bw_flush(&w);

    check(w.nbytes == 1 && w.datos[0] == 0xB2, "8 bits producen el byte 0xB2");
    check(w.total_bits == 8, "total_bits cuenta 8");
    bw_free(&w);
}

/* 2. El relleno no altera total_bits ni se lee. */
static void prueba_relleno(void) {
    BitWriter w;
    bw_init(&w, 0);
    for (int i = 0; i < 3; i++) bw_bit(&w, 1);   /* 3 bits: 111 */
    bw_flush(&w);

    check(w.total_bits == 3, "total_bits = 3 pese al relleno");
    check(w.nbytes == 1 && w.datos[0] == 0xE0, "el byte queda 0xE0 (111 + ceros)");

    BitReader r;
    br_init(&r, w.datos, w.total_bits);
    check(br_bit(&r) == 1 && br_bit(&r) == 1 && br_bit(&r) == 1, "se leen los 3 bits");
    check(br_bit(&r) == -1, "el lector se detiene: no ve el relleno");
    bw_free(&w);
}

/* 3. bw_bits / br_bits con anchos variados, incluidos los extremos. */
static void prueba_anchos(void) {
    BitWriter w;
    bw_init(&w, 0);
    bw_bits(&w, 0, 0);                      /* caso borde: cero bits */
    bw_bits(&w, 5, 3);                      /* 101 */
    bw_bits(&w, 0xFFFFFFFFFFFFFFFFull, 64); /* los 64 en uno */
    bw_bits(&w, 1, 1);
    bw_flush(&w);

    check(w.total_bits == 68, "0 + 3 + 64 + 1 = 68 bits");

    BitReader r;
    br_init(&r, w.datos, w.total_bits);
    uint64_t v;
    check(br_bits(&r, 3, &v) == 0 && v == 5,  "recupera 3 bits = 5");
    check(br_bits(&r, 64, &v) == 0 && v == 0xFFFFFFFFFFFFFFFFull, "recupera 64 bits");
    check(br_bits(&r, 1, &v) == 0 && v == 1,  "recupera 1 bit = 1");
    check(br_bits(&r, 1, &v) == -1, "pedir de más falla en vez de inventar");
    bw_free(&w);
}

/* 4. Escritor vacío: no debe romper. */
static void prueba_vacio(void) {
    BitWriter w;
    bw_init(&w, 0);
    bw_flush(&w);
    check(w.total_bits == 0 && w.nbytes == 0, "escritor vacío: 0 bits, 0 bytes");

    BitReader r;
    br_init(&r, w.datos, 0);
    check(br_bit(&r) == -1, "lector vacío retorna -1 de inmediato");
    bw_free(&w);
}

/* 5. Ida y vuelta con 100000 bits aleatorios: la prueba que de verdad importa.
   También fuerza decenas de reallocs, validando el crecimiento del buffer. */
static void prueba_ida_y_vuelta(void) {
    const int N = 100000;
    int *esperados = malloc((size_t)N * sizeof(int));
    if (!esperados) { printf("  FALLA sin memoria\n"); fallos++; return; }

    srand(12345);   /* semilla fija: la prueba es reproducible */
    BitWriter w;
    bw_init(&w, 8);  /* capacidad inicial minúscula a propósito */
    for (int i = 0; i < N; i++) {
        esperados[i] = rand() & 1;
        bw_bit(&w, esperados[i]);
    }
    bw_flush(&w);

    check(w.total_bits == (uint64_t)N, "100000 bits escritos");
    check(w.nbytes == (size_t)((N + 7) / 8), "el tamaño en bytes es el esperado");

    BitReader r;
    br_init(&r, w.datos, w.total_bits);
    int iguales = 1;
    for (int i = 0; i < N; i++) {
        if (br_bit(&r) != esperados[i]) { iguales = 0; break; }
    }
    check(iguales, "los 100000 bits vuelven idénticos");
    check(br_bit(&r) == -1, "no sobra ningún bit");

    free(esperados);
    bw_free(&w);
}

/* 6. Simula el escenario real: varios bloques independientes, cada uno alineado a
   byte, concatenados. Es exactamente lo que harán los hilos trabajadores. */
static void prueba_bloques_independientes(void) {
    const char *textos[3] = {"bloque uno", "dos", "el tercero es mas largo"};
    BitWriter w[3];
    uint64_t bits[3];

    for (int b = 0; b < 3; b++) {
        bw_init(&w[b], 0);
        for (const char *p = textos[b]; *p; p++) bw_bits(&w[b], (uint64_t)*p, 8);
        bw_flush(&w[b]);
        bits[b] = w[b].total_bits;
    }

    int todo_ok = 1;
    for (int b = 0; b < 3; b++) {
        BitReader r;
        br_init(&r, w[b].datos, bits[b]);
        for (const char *p = textos[b]; *p; p++) {
            uint64_t v;
            if (br_bits(&r, 8, &v) != 0 || (char)v != *p) { todo_ok = 0; break; }
        }
    }
    check(todo_ok, "3 bloques independientes se decodifican por separado");

    for (int b = 0; b < 3; b++) bw_free(&w[b]);
}

int main(void) {
    printf("\n=== Pruebas de bitio ===\n");
    prueba_convencion();
    prueba_relleno();
    prueba_anchos();
    prueba_vacio();
    prueba_ida_y_vuelta();
    prueba_bloques_independientes();
    printf("\n  %d superadas, %d fallidas\n\n", ok, fallos);
    return fallos > 0;
}
