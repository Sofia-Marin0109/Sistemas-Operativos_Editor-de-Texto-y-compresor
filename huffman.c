#include "huffman.h"

#include <stdio.h>    /* perror, fprintf */
#include <stdlib.h>   /* malloc, free */
#include <string.h>   /* memset */

/* ====================================================================================
 * COLA DE PRIORIDAD (montículo binario de mínimos)
 * ====================================================================================
 * Hace falta sacar repetidamente los dos nodos de menor frecuencia. Un montículo lo
 * hace en O(log n) por operación, contra O(n) de una búsqueda lineal. Con solo 256
 * símbolos la diferencia es imperceptible, pero es la estructura correcta y el costo
 * de escribirla es bajo.
 *
 * El montículo nunca crece más allá de HUF_NSIMBOLOS: se insertan como máximo 256
 * hojas y cada unión saca dos nodos y mete uno, así que el tamaño solo baja.
 */

typedef struct {
    NodoHuf *v[HUF_NSIMBOLOS];
    size_t   n;
} Monticulo;

/**
 * Orden estricto entre nodos: primero por frecuencia, y en empate por 'orden'.
 *
 * Esta función es el corazón del determinismo del formato. Como no hay dos nodos con
 * el mismo 'orden' (las hojas usan su símbolo 0..255, los internos 256 en adelante),
 * la comparación nunca queda indecisa y dos ejecuciones cualesquiera producen
 * exactamente el mismo árbol.
 */
static int menor_que(const NodoHuf *a, const NodoHuf *b) {
    if (a->freq != b->freq) return a->freq < b->freq;
    return a->orden < b->orden;
}

static void subir(Monticulo *m, size_t i) {
    while (i > 0) {
        size_t padre = (i - 1) / 2;
        if (!menor_que(m->v[i], m->v[padre])) break;
        NodoHuf *t = m->v[i]; m->v[i] = m->v[padre]; m->v[padre] = t;
        i = padre;
    }
}

static void bajar(Monticulo *m, size_t i) {
    for (;;) {
        size_t izq = 2 * i + 1, der = 2 * i + 2, min = i;
        if (izq < m->n && menor_que(m->v[izq], m->v[min])) min = izq;
        if (der < m->n && menor_que(m->v[der], m->v[min])) min = der;
        if (min == i) break;
        NodoHuf *t = m->v[i]; m->v[i] = m->v[min]; m->v[min] = t;
        i = min;
    }
}

static void mont_insertar(Monticulo *m, NodoHuf *nodo) {
    m->v[m->n] = nodo;
    subir(m, m->n);
    m->n++;
}

static NodoHuf *mont_extraer(Monticulo *m) {
    NodoHuf *min = m->v[0];
    if (m->n > 1) {
        m->v[0] = m->v[--m->n];
        bajar(m, 0);
    } else {
        m->n = 0;
    }
    return min;
}

/* ====================================================================================
 * FRECUENCIAS
 * ==================================================================================== */

void huf_freq_init(uint64_t freq[HUF_NSIMBOLOS]) {
    memset(freq, 0, HUF_NSIMBOLOS * sizeof(uint64_t));
}

void huf_contar(const unsigned char *datos, size_t n, uint64_t freq[HUF_NSIMBOLOS]) {
    /* Acumula sin tocar nada fuera de 'freq'. Al no haber estado compartido, varios
       hilos pueden ejecutarla en paralelo sobre bloques distintos y arreglos propios
       sin un solo mutex. */
    for (size_t i = 0; i < n; i++) freq[datos[i]]++;
}

void huf_acumular(uint64_t dst[HUF_NSIMBOLOS], const uint64_t src[HUF_NSIMBOLOS]) {
    for (int i = 0; i < HUF_NSIMBOLOS; i++) dst[i] += src[i];
}

/* ====================================================================================
 * CONSTRUCCIÓN DEL ÁRBOL
 * ==================================================================================== */

/* Recorrido en profundidad que rellena la tabla de códigos.
   'prefijo' lleva los bits acumulados desde la raíz y 'prof' cuántos son.
   Retorna -1 si algún código excede HUF_MAX_LONGITUD. */
static int asignar_codigos(Huffman *h, const NodoHuf *n, uint64_t prefijo, int prof) {
    if (n == NULL) return 0;

    if (n->simbolo >= 0) {                      /* hoja: aquí termina un código */
        if (prof > HUF_MAX_LONGITUD) {
            fprintf(stderr,
                    "huffman: código de %d bits excede el máximo de %d\n",
                    prof, HUF_MAX_LONGITUD);
            return -1;
        }
        h->tabla[n->simbolo].codigo   = prefijo;
        h->tabla[n->simbolo].longitud = (uint8_t)prof;
        return 0;
    }

    /* Izquierda añade un 0, derecha un 1. El desplazamiento a la izquierda hace
       hueco para el bit nuevo al final del prefijo. */
    if (asignar_codigos(h, n->izq, (prefijo << 1) | 0u, prof + 1) == -1) return -1;
    if (asignar_codigos(h, n->der, (prefijo << 1) | 1u, prof + 1) == -1) return -1;
    return 0;
}

int huf_construir(Huffman *h, const uint64_t freq[HUF_NSIMBOLOS]) {
    h->nodos       = NULL;
    h->n_nodos     = 0;
    h->raiz        = NULL;
    h->n_distintos = 0;
    memset(h->tabla, 0, sizeof(h->tabla));

    /* --- Arena de nodos -------------------------------------------------
       Un árbol binario con k hojas tiene exactamente k-1 nodos internos, así que
       2*256-1 = 511 nodos es una cota dura. Se reservan de un solo malloc en vez de
       uno por nodo: una llamada al allocator en lugar de 511, los nodos quedan
       contiguos (mejor localidad al recorrer) y liberar es un único free, sin
       recorrido recursivo que pueda dejar fugas. */
    h->nodos = malloc(HUF_MAX_NODOS * sizeof(NodoHuf));
    if (h->nodos == NULL) { perror("malloc"); return -1; }

    /* --- Hojas ----------------------------------------------------------
       Solo los símbolos que aparecen. Meter los de frecuencia 0 les daría códigos
       que nadie usa y alargaría los de los demás. */
    Monticulo m;
    m.n = 0;
    for (int s = 0; s < HUF_NSIMBOLOS; s++) {
        if (freq[s] == 0) continue;
        NodoHuf *nodo = &h->nodos[h->n_nodos++];
        nodo->freq    = freq[s];
        nodo->orden   = (uint64_t)s;   /* hojas: 0..255 */
        nodo->simbolo = s;
        nodo->izq     = nodo->der = NULL;
        mont_insertar(&m, nodo);
        h->n_distintos++;
    }

    /* Archivo vacío: no hay árbol ni códigos. No es un error. */
    if (h->n_distintos == 0) return 0;

    /* --- Uniones --------------------------------------------------------
       Cada vuelta saca los dos menores y mete su padre. El montículo se reduce en
       uno por vuelta, así que termina en n_distintos-1 pasos. */
    uint64_t siguiente_orden = HUF_NSIMBOLOS;   /* internos: 256, 257, ... */
    while (m.n > 1) {
        NodoHuf *a = mont_extraer(&m);   /* menor  -> hijo izquierdo (bit 0) */
        NodoHuf *b = mont_extraer(&m);   /* segundo -> hijo derecho  (bit 1) */

        NodoHuf *padre = &h->nodos[h->n_nodos++];
        padre->freq    = a->freq + b->freq;
        padre->orden   = siguiente_orden++;
        padre->simbolo = -1;
        padre->izq     = a;
        padre->der     = b;
        mont_insertar(&m, padre);
    }

    h->raiz = mont_extraer(&m);

    /* --- Caso especial: un único símbolo distinto -----------------------
       Con una sola hoja, esa hoja ES la raíz y el camino hasta ella tiene longitud
       cero: el código sería la cadena vacía y no se podría escribir ni distinguir
       nada. Se le fuerza el código de 1 bit '0'. Un archivo de un millón de 'a'
       ocupará un millón de bits (125 KB) en vez de 1 MB. huf_decodificar() maneja
       este árbol de forma simétrica. */
    if (h->raiz->simbolo >= 0) {
        h->tabla[h->raiz->simbolo].codigo   = 0;
        h->tabla[h->raiz->simbolo].longitud = 1;
        return 0;
    }

    if (asignar_codigos(h, h->raiz, 0, 0) == -1) {
        huf_liberar(h);
        return -1;
    }
    return 0;
}

void huf_liberar(Huffman *h) {
    free(h->nodos);          /* un solo free: la arena se lleva los 511 nodos */
    h->nodos       = NULL;
    h->n_nodos     = 0;
    h->raiz        = NULL;
    h->n_distintos = 0;
    memset(h->tabla, 0, sizeof(h->tabla));
}

/* ====================================================================================
 * USO
 * ==================================================================================== */

int huf_emitir(const Huffman *h, BitWriter *w, unsigned char c) {
    const Codigo *cod = &h->tabla[c];
    if (cod->longitud == 0) return -1;   /* símbolo ausente de las frecuencias */
    return bw_bits(w, cod->codigo, cod->longitud);
}

int huf_decodificar(const Huffman *h, BitReader *r) {
    if (h->raiz == NULL) return -1;

    /* Árbol de un solo símbolo: no hay ramas que recorrer, pero sí hay que consumir
       el bit que se escribió, o el contador de bits se desincroniza. */
    if (h->raiz->simbolo >= 0) {
        if (br_bit(r) < 0) return -1;
        return h->raiz->simbolo;
    }

    const NodoHuf *n = h->raiz;
    while (n->simbolo < 0) {
        int b = br_bit(r);
        if (b < 0) return -1;            /* se agotaron los bits a media letra */
        n = b ? n->der : n->izq;
        if (n == NULL) return -1;        /* árbol corrupto */
    }
    return n->simbolo;
}

uint64_t huf_bits_totales(const Huffman *h, const uint64_t freq[HUF_NSIMBOLOS]) {
    uint64_t total = 0;
    for (int s = 0; s < HUF_NSIMBOLOS; s++) {
        total += freq[s] * h->tabla[s].longitud;
    }
    return total;
}
