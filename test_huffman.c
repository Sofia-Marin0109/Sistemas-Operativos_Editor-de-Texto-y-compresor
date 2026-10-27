/* Prueba independiente del módulo Huffman.
   gcc -Wall -Wextra -std=gnu99 -g bitio.c huffman.c test_huffman.c -o test_huffman */

#include "huffman.h"
#include "bitio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ok = 0, fallos = 0;

static void check(int cond, const char *desc) {
    if (cond) { printf("  PASA  %s\n", desc); ok++; }
    else      { printf("  FALLA %s\n", desc); fallos++; }
}

/* Devuelve el código de 'c' como texto ("101"), para comparar de forma legible. */
static void codigo_str(const Huffman *h, unsigned char c, char *out) {
    const Codigo *cod = &h->tabla[c];
    for (int i = cod->longitud - 1; i >= 0; i--)
        *out++ = ((cod->codigo >> i) & 1) ? '1' : '0';
    *out = '\0';
}

static void frecuencias_de(const char *s, uint64_t freq[HUF_NSIMBOLOS]) {
    huf_freq_init(freq);
    huf_contar((const unsigned char *)s, strlen(s), freq);
}

/* ---------------------------------------------------------------- 1. Ejemplo fijo */
/* "abracadabra": a=5, b=2, r=2, c=1, d=1. Con el desempate por 'orden' el árbol
   queda determinado y los códigos deben ser EXACTAMENTE estos. Si alguien toca la
   regla de desempate o el orden izquierda/derecha, esta prueba lo atrapa. */
static void prueba_abracadabra(void) {
    uint64_t freq[HUF_NSIMBOLOS];
    frecuencias_de("abracadabra", freq);

    check(freq['a'] == 5 && freq['b'] == 2 && freq['r'] == 2 &&
          freq['c'] == 1 && freq['d'] == 1, "conteo de abracadabra correcto");

    Huffman h;
    check(huf_construir(&h, freq) == 0, "el árbol se construye");
    check(h.n_distintos == 5, "5 símbolos distintos");

    char buf[80];
    codigo_str(&h, 'a', buf); check(strcmp(buf, "0")   == 0, "código de 'a' es 0");
    codigo_str(&h, 'c', buf); check(strcmp(buf, "100") == 0, "código de 'c' es 100");
    codigo_str(&h, 'd', buf); check(strcmp(buf, "101") == 0, "código de 'd' es 101");
    codigo_str(&h, 'b', buf); check(strcmp(buf, "110") == 0, "código de 'b' es 110");
    codigo_str(&h, 'r', buf); check(strcmp(buf, "111") == 0, "código de 'r' es 111");

    check(huf_bits_totales(&h, freq) == 23, "el texto ocupa 23 bits (contra 88)");
    huf_liberar(&h);
}

/* ------------------------------------------------- 2. Propiedad de prefijo */
/* Ningún código puede ser prefijo de otro: si lo fuera, la decodificación sería
   ambigua. Se comprueba por fuerza bruta entre todos los pares presentes. */
static int es_prefijo(const Codigo *a, const Codigo *b) {
    if (a->longitud == 0 || b->longitud == 0 || a->longitud > b->longitud) return 0;
    return (b->codigo >> (b->longitud - a->longitud)) == a->codigo;
}

static void prueba_prefijo(void) {
    srand(4242);
    int limpio = 1;

    for (int intento = 0; intento < 200 && limpio; intento++) {
        uint64_t freq[HUF_NSIMBOLOS];
        huf_freq_init(freq);
        for (int s = 0; s < HUF_NSIMBOLOS; s++)
            if (rand() % 3) freq[s] = (uint64_t)(rand() % 1000) + 1;

        Huffman h;
        if (huf_construir(&h, freq) != 0) { limpio = 0; break; }

        for (int i = 0; i < HUF_NSIMBOLOS && limpio; i++) {
            for (int j = 0; j < HUF_NSIMBOLOS; j++) {
                if (i == j) continue;
                if (es_prefijo(&h.tabla[i], &h.tabla[j])) { limpio = 0; break; }
            }
        }
        huf_liberar(&h);
    }
    check(limpio, "propiedad de prefijo en 200 árboles aleatorios");
}

/* ------------------------------------------------- 3. Igualdad de Kraft */
/* Un código de prefijo es COMPLETO (no desperdicia ningún patrón de bits) si y solo
   si la suma de 2^(-longitud) sobre todas las hojas vale exactamente 1. Huffman
   siempre produce códigos completos, así que esta identidad debe cumplirse al bit.
   Se calcula con enteros para evitar error de redondeo: se escala por 2^L, donde L
   es la longitud máxima, y la suma debe dar 2^L. */
static void prueba_kraft(void) {
    uint64_t freq[HUF_NSIMBOLOS];
    frecuencias_de("los sistemas operativos administran recursos", freq);

    Huffman h;
    huf_construir(&h, freq);

    int L = 0;
    for (int s = 0; s < HUF_NSIMBOLOS; s++)
        if (h.tabla[s].longitud > L) L = h.tabla[s].longitud;

    uint64_t suma = 0;
    for (int s = 0; s < HUF_NSIMBOLOS; s++)
        if (h.tabla[s].longitud > 0)
            suma += 1ull << (L - h.tabla[s].longitud);

    check(suma == (1ull << L), "se cumple la igualdad de Kraft (código completo)");
    huf_liberar(&h);
}

/* ------------------------------------------------- 4. Determinismo */
/* El descompresor reconstruye el árbol desde las frecuencias de la cabecera. Si dos
   construcciones con las mismas frecuencias dieran tablas distintas, el archivo se
   descomprimiría como basura. */
static void prueba_determinismo(void) {
    uint64_t freq[HUF_NSIMBOLOS];
    huf_freq_init(freq);
    /* Muchos empates a propósito: es el escenario que rompe implementaciones
       sin criterio de desempate. */
    for (int s = 0; s < 64; s++) freq[s] = 7;
    freq[200] = 7; freq[201] = 7; freq[5] = 999;

    Huffman h1, h2;
    huf_construir(&h1, freq);
    huf_construir(&h2, freq);

    check(memcmp(h1.tabla, h2.tabla, sizeof(h1.tabla)) == 0,
          "dos construcciones con empates dan tablas idénticas");

    huf_liberar(&h1);
    huf_liberar(&h2);
}

/* ------------------------------------------------- 5. Ida y vuelta real */
/* Codificar con la tabla, decodificar bajando por el árbol, y comparar. Es la
   prueba que de verdad valida que el módulo sirve. */
static int ida_y_vuelta(const char *texto) {
    size_t n = strlen(texto);
    uint64_t freq[HUF_NSIMBOLOS];
    frecuencias_de(texto, freq);

    Huffman h;
    if (huf_construir(&h, freq) != 0) return 0;

    BitWriter w;
    bw_init(&w, 16);
    for (size_t i = 0; i < n; i++)
        if (huf_emitir(&h, &w, (unsigned char)texto[i]) != 0) { bw_free(&w); huf_liberar(&h); return 0; }
    bw_flush(&w);

    int igual = (w.total_bits == huf_bits_totales(&h, freq));

    BitReader r;
    br_init(&r, w.datos, w.total_bits);
    for (size_t i = 0; i < n && igual; i++)
        if (huf_decodificar(&h, &r) != (unsigned char)texto[i]) igual = 0;

    if (br_restantes(&r) != 0) igual = 0;   /* no debe sobrar ni un bit */

    bw_free(&w);
    huf_liberar(&h);
    return igual;
}

static void prueba_ida_y_vuelta(void) {
    check(ida_y_vuelta("abracadabra"), "ida y vuelta: abracadabra");
    check(ida_y_vuelta("Los sistemas operativos gestionan la memoria, "
                       "los procesos y el sistema de archivos."),
          "ida y vuelta: texto largo con espacios y puntuación");
    check(ida_y_vuelta("ab"), "ida y vuelta: dos símbolos");
}

/* ------------------------------------------------- 6. Casos borde */
static void prueba_un_solo_simbolo(void) {
    uint64_t freq[HUF_NSIMBOLOS];
    frecuencias_de("aaaaaaaaaa", freq);

    Huffman h;
    huf_construir(&h, freq);

    check(h.n_distintos == 1, "un solo símbolo distinto");
    check(h.tabla['a'].longitud == 1, "se le fuerza un código de 1 bit (no cero)");
    check(ida_y_vuelta("aaaaaaaaaa"), "ida y vuelta con un solo símbolo");
    huf_liberar(&h);
}

static void prueba_vacio(void) {
    uint64_t freq[HUF_NSIMBOLOS];
    huf_freq_init(freq);

    Huffman h;
    check(huf_construir(&h, freq) == 0, "archivo vacío no es un error");
    check(h.raiz == NULL && h.n_distintos == 0, "árbol vacío, sin raíz");

    BitReader r;
    br_init(&r, (const unsigned char *)"", 0);
    check(huf_decodificar(&h, &r) == -1, "decodificar un árbol vacío retorna -1");
    huf_liberar(&h);
}

/* Huffman trabaja sobre bytes, no sobre texto: debe tragarse los 256 valores,
   incluido el 0, que en C corta las cadenas. */
static void prueba_binario(void) {
    unsigned char datos[1024];
    srand(7);
    for (int i = 0; i < 1024; i++) datos[i] = (unsigned char)(rand() & 0xFF);
    datos[0] = 0; datos[1] = 0; datos[500] = 255;

    uint64_t freq[HUF_NSIMBOLOS];
    huf_freq_init(freq);
    huf_contar(datos, sizeof(datos), freq);

    Huffman h;
    huf_construir(&h, freq);

    BitWriter w;
    bw_init(&w, 64);
    for (size_t i = 0; i < sizeof(datos); i++) huf_emitir(&h, &w, datos[i]);
    bw_flush(&w);

    BitReader r;
    br_init(&r, w.datos, w.total_bits);
    int igual = 1;
    for (size_t i = 0; i < sizeof(datos); i++)
        if (huf_decodificar(&h, &r) != datos[i]) { igual = 0; break; }

    check(igual, "ida y vuelta con datos binarios (incluye 0x00 y 0xFF)");
    bw_free(&w);
    huf_liberar(&h);
}

/* ------------------------------------------------- 7. Reducción */
/* huf_acumular es lo que usarán los hilos para fundir sus conteos locales.
   Contar por bloques y sumar debe dar lo mismo que contar de una sola pasada. */
static void prueba_reduccion(void) {
    const char *texto = "el resultado no puede depender de como se parta el archivo";
    size_t n = strlen(texto);

    uint64_t completo[HUF_NSIMBOLOS];
    huf_freq_init(completo);
    huf_contar((const unsigned char *)texto, n, completo);

    uint64_t global[HUF_NSIMBOLOS];
    huf_freq_init(global);
    size_t tam = n / 4 + 1;
    for (size_t off = 0; off < n; off += tam) {
        size_t len = (off + tam <= n) ? tam : n - off;
        uint64_t local[HUF_NSIMBOLOS];          /* como haría cada hilo */
        huf_freq_init(local);
        huf_contar((const unsigned char *)texto + off, len, local);
        huf_acumular(global, local);            /* la reducción */
    }

    check(memcmp(completo, global, sizeof(completo)) == 0,
          "contar por bloques y reducir == contar de una pasada");
}

int main(void) {
    printf("\n=== Pruebas de huffman ===\n");
    prueba_abracadabra();
    prueba_prefijo();
    prueba_kraft();
    prueba_determinismo();
    prueba_ida_y_vuelta();
    prueba_un_solo_simbolo();
    prueba_vacio();
    prueba_binario();
    prueba_reduccion();
    printf("\n  %d superadas, %d fallidas\n\n", ok, fallos);
    return fallos > 0;
}
