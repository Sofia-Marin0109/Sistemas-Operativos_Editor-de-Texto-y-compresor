#include "huf_formato.h"
#include "huffman.h"
#include "bitio.h"

#include <unistd.h>   /* read, write, close, lseek */
#include <fcntl.h>    /* open */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int huf_verboso = 0;

/* ====================================================================================
 * SERIALIZACIÓN DE ENTEROS (little-endian explícito)
 * ====================================================================================
 * Campo por campo y byte por byte. Así el formato queda definido por el estándar y no
 * por el compilador ni por la arquitectura: nada de relleno sorpresa entre campos de
 * una struct, y el mismo .huf se lee igual en una máquina big-endian.
 */

static void put_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static uint16_t get_u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_u32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static void put_u64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}

static uint32_t get_u32(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static uint64_t get_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Versiones públicas: comprimir.c escribe el mismo formato reutilizando estas,
   en vez de repetir la disposición de la cabecera. */

void huf_poner_u64(unsigned char *p, uint64_t v) { put_u64(p, v); }

/* Cabecera dispersa: solo se guardan los símbolos que aparecen, como pares
   (byte, frecuencia), en orden creciente de byte. El orden fijo hace la cabecera
   determinista y permite detectar duplicados al leer. */

static int contar_simbolos(const uint64_t freq[256]) {
    int k = 0;
    for (int s = 0; s < 256; s++) if (freq[s] > 0) k++;
    return k;
}

size_t huf_tam_preambulo(const uint64_t freq[256]) {
    return HUF_CABECERA_FIJA + (size_t)contar_simbolos(freq) * HUF_BYTES_POR_SIMBOLO;
}

void huf_escribir_preambulo(unsigned char *buf, uint64_t tam_original,
                            uint64_t n_bloques, const uint64_t freq[256]) {
    memcpy(buf, HUF_MAGICO, HUF_MAGICO_LEN);
    put_u32(buf + 4,  (uint32_t)HUF_TAM_BLOQUE);
    put_u64(buf + 8,  tam_original);
    put_u64(buf + 16, n_bloques);
    put_u16(buf + 24, (uint16_t)contar_simbolos(freq));

    unsigned char *p = buf + HUF_CABECERA_FIJA;
    for (int s = 0; s < 256; s++) {
        if (freq[s] == 0) continue;
        *p++ = (unsigned char)s;
        put_u64(p, freq[s]);
        p += 8;
    }
}

/* Cabecera ya validada y decodificada. */
typedef struct {
    uint32_t tam_bloque;
    uint64_t tam_original;
    uint64_t n_bloques;
    uint64_t freq[HUF_NSIMBOLOS];
    int      n_simbolos;
    size_t   tam_preambulo;     /* dónde empieza el índice de bloques */
} Cabecera;

/* Valida y decodifica la cabecera de un .huf en memoria. Todo campo que viene de un
   archivo es sospechoso, así que se comprueba ANTES de usarlo para indexar. Si 'hablar'
   es 1 explica el motivo del rechazo por stderr. Retorna 0 / -1. */
static int leer_preambulo(const unsigned char *d, size_t n, Cabecera *c, int hablar) {
#define RECHAZAR(msg) do { if (hablar) fprintf(stderr, "huf: " msg "\n"); return -1; } while (0)

    if (n < HUF_CABECERA_FIJA) RECHAZAR("archivo demasiado corto para ser un .huf");
    if (memcmp(d, HUF_MAGICO, HUF_MAGICO_LEN) != 0)
        RECHAZAR("número mágico inválido (no es un archivo .huf)");

    c->tam_bloque   = get_u32(d + 4);
    c->tam_original = get_u64(d + 8);
    c->n_bloques    = get_u64(d + 16);

    if (c->tam_bloque == 0 && c->n_bloques > 0) RECHAZAR("tamaño de bloque inválido");

    /* Coherencia entre el número de bloques y el tamaño original. */
    uint64_t esperados = c->tam_bloque
                       ? (c->tam_original + c->tam_bloque - 1) / c->tam_bloque : 0;
    if (c->n_bloques != esperados) RECHAZAR("cabecera incoherente (bloques vs tamaño)");

    unsigned k = get_u16(d + 24);
    if (k > HUF_NSIMBOLOS) RECHAZAR("número de símbolos inválido (más de 256)");
    c->n_simbolos    = (int)k;
    c->tam_preambulo = HUF_CABECERA_FIJA + (size_t)k * HUF_BYTES_POR_SIMBOLO;
    if (n < c->tam_preambulo) RECHAZAR("tabla de símbolos truncada");

    /* Los pares deben venir en orden estrictamente creciente (sin duplicados), con
       frecuencia positiva, y sumar exactamente el tamaño original. */
    memset(c->freq, 0, sizeof(c->freq));
    const unsigned char *p = d + HUF_CABECERA_FIJA;
    int previo = -1;
    uint64_t suma = 0;
    for (unsigned i = 0; i < k; i++, p += HUF_BYTES_POR_SIMBOLO) {
        int s = p[0];
        uint64_t f = get_u64(p + 1);
        if (s <= previo) RECHAZAR("símbolos duplicados o fuera de orden");
        if (f == 0)      RECHAZAR("frecuencia nula en la tabla de símbolos");
        if (f > c->tam_original - suma) RECHAZAR("frecuencias superan el tamaño original");
        c->freq[s] = f;
        suma += f;
        previo = s;
    }
    if (suma != c->tam_original) RECHAZAR("frecuencias no suman el tamaño original");

    /* El índice necesita 8 bytes por bloque; la comparación evita un desbordamiento. */
    if (c->n_bloques > (n - c->tam_preambulo) / 8) RECHAZAR("índice de bloques truncado");

    return 0;
#undef RECHAZAR
}

/* ====================================================================================
 * E/S ROBUSTA
 * ====================================================================================
 * read() y write() pueden transferir menos bytes de los pedidos (señales, disco lleno,
 * límites del sistema de archivos). Sin estos bucles el archivo quedaría truncado en
 * silencio. EINTR no es un error: la llamada se interrumpió antes de transferir nada.
 */

static ssize_t escribir_todo(int fd, const unsigned char *buf, size_t n) {
    size_t hechos = 0;
    while (hechos < n) {
        ssize_t w = write(fd, buf + hechos, n - hechos);
        if (w == -1) { if (errno == EINTR) continue; return -1; }
        hechos += (size_t)w;
    }
    return (ssize_t)hechos;
}

static ssize_t leer_todo(int fd, unsigned char *buf, size_t n) {
    size_t hechos = 0;
    while (hechos < n) {
        ssize_t r = read(fd, buf + hechos, n - hechos);
        if (r == -1) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        hechos += (size_t)r;
    }
    return (ssize_t)hechos;
}

/* ====================================================================================
 * COMPRESIÓN
 * ==================================================================================== */

int huf_comprimir(const unsigned char *datos, size_t n,
                  unsigned char **salida, size_t *salida_n) {
    /* --- Frecuencias de TODO el archivo ----------------------------------
       El árbol debe ser uno solo para todos los bloques: si cada bloque tuviera el
       suyo, habría que guardar 256 frecuencias por bloque y los códigos cambiarían
       a mitad de archivo. Por eso el conteo abarca el archivo completo aunque la
       codificación luego se haga por bloques. */
    uint64_t freq[HUF_NSIMBOLOS];
    huf_freq_init(freq);
    huf_contar(datos, n, freq);
    return huf_comprimir_con_freq(datos, n, freq, salida, salida_n);
}

int huf_comprimir_con_freq(const unsigned char *datos, size_t n,
                           const uint64_t freq[HUF_NSIMBOLOS],
                           unsigned char **salida, size_t *salida_n) {
    *salida   = NULL;
    *salida_n = 0;

    Huffman h;
    if (huf_construir(&h, freq) != 0) return -1;

    /* --- 2. División en bloques ------------------------------------------ */
    uint64_t n_bloques = (n + HUF_TAM_BLOQUE - 1) / HUF_TAM_BLOQUE;  /* techo */

    BitWriter *bw = NULL;
    if (n_bloques > 0) {
        bw = calloc((size_t)n_bloques, sizeof(BitWriter));
        if (bw == NULL) { perror("calloc"); huf_liberar(&h); return -1; }
    }

    /* --- 3. Codificar cada bloque por separado ---------------------------
       Este bucle es LO QUE SE PARALELIZARÁ: cada iteración es independiente de las
       demás, escribe en su propio BitWriter y solo lee de 'datos' y de la tabla de
       'h', que nadie modifica. No hay estado compartido que proteger. */
    for (uint64_t b = 0; b < n_bloques; b++) {
        size_t off = (size_t)b * HUF_TAM_BLOQUE;
        size_t len = (off + HUF_TAM_BLOQUE <= n) ? HUF_TAM_BLOQUE : n - off;

        if (bw_init(&bw[b], len + 16) != 0) goto error;
        for (size_t i = 0; i < len; i++) {
            if (huf_emitir(&h, &bw[b], datos[off + i]) != 0) goto error;
        }
        if (bw_flush(&bw[b]) != 0) goto error;   /* alinea el bloque a byte */
    }

    /* --- 4. Reservar la salida de una sola vez --------------------------- */
    size_t tam_indice = (size_t)n_bloques * 8;
    size_t tam_carga  = 0;
    for (uint64_t b = 0; b < n_bloques; b++) tam_carga += bw[b].nbytes;

    size_t pre   = huf_tam_preambulo(freq);
    size_t total = pre + tam_indice + tam_carga;
    unsigned char *out = malloc(total);
    if (out == NULL) { perror("malloc"); goto error; }

    /* --- 5. Armar el archivo --------------------------------------------- */
    huf_escribir_preambulo(out, (uint64_t)n, n_bloques, freq);

    size_t off_indice = pre;
    size_t off_carga  = pre + tam_indice;
    for (uint64_t b = 0; b < n_bloques; b++) {
        put_u64(out + off_indice + b * 8, bw[b].total_bits);  /* bits ÚTILES */
        memcpy(out + off_carga, bw[b].datos, bw[b].nbytes);
        off_carga += bw[b].nbytes;
    }

    for (uint64_t b = 0; b < n_bloques; b++) bw_free(&bw[b]);
    free(bw);
    huf_liberar(&h);

    *salida   = out;
    *salida_n = total;
    return 0;

error:
    if (bw) {
        for (uint64_t b = 0; b < n_bloques; b++) bw_free(&bw[b]);
        free(bw);
    }
    huf_liberar(&h);
    return -1;
}

/* ====================================================================================
 * DESCOMPRESIÓN
 * ==================================================================================== */

int huf_info(const unsigned char *datos, size_t n, HufInfo *info) {
    Cabecera c;
    if (leer_preambulo(datos, n, &c, 0) != 0) return -1;

    info->tam_bloque     = c.tam_bloque;
    info->tam_original   = c.tam_original;
    info->n_bloques      = c.n_bloques;
    info->tam_comprimido = n;
    info->n_simbolos     = c.n_simbolos;
    return 0;
}

int huf_descomprimir(const unsigned char *datos, size_t n,
                     unsigned char **salida, size_t *salida_n) {
    return huf_descomprimir_cb(datos, n, salida, salida_n, NULL, NULL);
}

/* Tabla de decodificación rápida: indexada por los próximos TABLA_BITS bits del flujo,
   da el símbolo y su longitud de una sola lectura. Recorrer el árbol bit a bit cuesta
   una llamada y varias comparaciones por bit; con la tabla es un acceso por símbolo.
   Los códigos más largos que TABLA_BITS (longitud 0 en la tabla) usan el árbol. */
#define TABLA_BITS 12

typedef struct { unsigned char simbolo, longitud; } EntradaRapida;

static void construir_tabla_rapida(const Huffman *h, EntradaRapida *t) {
    memset(t, 0, sizeof(EntradaRapida) << TABLA_BITS);
    for (int s = 0; s < HUF_NSIMBOLOS; s++) {
        int L = h->tabla[s].longitud;
        if (L == 0 || L > TABLA_BITS) continue;
        uint32_t base = (uint32_t)h->tabla[s].codigo << (TABLA_BITS - L);
        uint32_t cuantos = 1u << (TABLA_BITS - L);
        for (uint32_t k = 0; k < cuantos; k++) {
            t[base + k].simbolo  = (unsigned char)s;
            t[base + k].longitud = (unsigned char)L;
        }
    }
}

/* Los próximos TABLA_BITS bits desde 'pos'; lo que quede más allá del bloque cuenta como
   ceros (el llamador verifica después que el código no se pasó de los bits útiles). */
static inline uint32_t mirar_bits(const unsigned char *d, size_t nbytes, uint64_t pos) {
    size_t i = (size_t)(pos >> 3);
    uint32_t v = (i     < nbytes ? (uint32_t)d[i]     << 16 : 0)
               | (i + 1 < nbytes ? (uint32_t)d[i + 1] <<  8 : 0)
               | (i + 2 < nbytes ? (uint32_t)d[i + 2]       : 0);
    return (v >> (12 - (unsigned)(pos & 7))) & ((1u << TABLA_BITS) - 1u);
}

int huf_descomprimir_cb(const unsigned char *datos, size_t n,
                        unsigned char **salida, size_t *salida_n,
                        HufProgreso cb, void *ctx) {
    *salida   = NULL;
    *salida_n = 0;

    /* --- 1. Validar la cabecera ------------------------------------------
       Todo dato que venga de un archivo es sospechoso: puede estar corrupto o no ser
       un .huf en absoluto. leer_preambulo valida cada campo ANTES de usarlo para
       indexar, o un archivo manipulado provocaría lecturas fuera de rango. */
    Cabecera cab;
    if (leer_preambulo(datos, n, &cab, 1) != 0) return -1;

    uint32_t tam_bloque   = cab.tam_bloque;
    uint64_t tam_original = cab.tam_original;
    uint64_t n_bloques    = cab.n_bloques;
    size_t   pre          = cab.tam_preambulo;
    size_t   tam_indice   = (size_t)n_bloques * 8;

    /* --- 2. Reconstruir el MISMO árbol ------------------------------------
       Aquí se apoya todo el formato: como huf_construir es determinista, las
       frecuencias de los símbolos presentes producen un árbol idéntico al que usó el
       compresor (los símbolos ausentes valen 0 y no participan). */
    Huffman h;
    if (huf_construir(&h, cab.freq) != 0) return -1;

    unsigned char *out = malloc(tam_original ? (size_t)tam_original : 1);
    if (out == NULL) { perror("malloc"); huf_liberar(&h); return -1; }

    /* --- 3. Decodificar bloque por bloque --------------------------------
       Cada bloque arranca en frontera de byte, así que su posición se obtiene sumando
       los tamaños de los anteriores. Es también lo que permitiría descomprimir en
       paralelo: ningún bloque depende del estado del anterior. */
    size_t off_carga = pre + tam_indice;
    size_t escritos  = 0;

    EntradaRapida *rapida = malloc(sizeof(EntradaRapida) << TABLA_BITS);
    if (rapida == NULL) { perror("malloc"); free(out); huf_liberar(&h); return -1; }
    construir_tabla_rapida(&h, rapida);

    for (uint64_t b = 0; b < n_bloques; b++) {
        uint64_t bits  = get_u64(datos + pre + b * 8);
        size_t   bytes = (size_t)((bits + 7) / 8);

        if (off_carga + bytes > n) {
            fprintf(stderr, "huf: datos del bloque %llu fuera de rango\n",
                    (unsigned long long)b);
            goto error;
        }

        /* Símbolos que le tocan a este bloque: todos llenos menos quizá el último. */
        size_t sim = (escritos + tam_bloque <= tam_original)
                   ? tam_bloque
                   : (size_t)(tam_original - escritos);

        BitReader r;
        br_init(&r, datos + off_carga, bits);

        for (size_t i = 0; i < sim; i++) {
            /* Vía rápida: un acceso a la tabla. Solo vale si el código cabe en los bits
               útiles que quedan; si no, o si es largo, decide el árbol (que además
               detecta el bloque corrupto). */
            EntradaRapida e = rapida[mirar_bits(datos + off_carga, bytes, r.pos_bit)];
            int c;
            if (e.longitud != 0 && e.longitud <= br_restantes(&r)) {
                r.pos_bit += e.longitud;
                c = e.simbolo;
            } else {
                c = huf_decodificar(&h, &r);
            }
            if (c < 0) {
                fprintf(stderr, "huf: bloque %llu corrupto (bits insuficientes)\n",
                        (unsigned long long)b);
                goto error;
            }
            out[escritos++] = (unsigned char)c;
        }

        /* Si sobran bits útiles, el archivo no es coherente: el relleno ya quedó
           excluido por 'bits', así que no debería quedar nada. */
        if (br_restantes(&r) != 0) {
            fprintf(stderr, "huf: bloque %llu tiene bits sobrantes\n",
                    (unsigned long long)b);
            goto error;
        }
        off_carga += bytes;
        if (cb && cb(ctx, (uint64_t)escritos, tam_original) != 0) {
            free(out);
            free(rapida);
            huf_liberar(&h);
            return HUF_CANCELADO;      /* *salida ya vale NULL */
        }
    }

    if (escritos != tam_original) {
        fprintf(stderr, "huf: se recuperaron %zu bytes de %llu esperados\n",
                escritos, (unsigned long long)tam_original);
        goto error;
    }

    free(rapida);
    huf_liberar(&h);
    *salida   = out;
    *salida_n = (size_t)tam_original;
    return 0;

error:
    free(out);
    free(rapida);
    huf_liberar(&h);
    return -1;
}

/* ====================================================================================
 * ENVOLTORIOS DE ARCHIVO (aquí sí hay llamadas al sistema)
 * ==================================================================================== */

#define TRAZA(...) do { if (huf_verboso) fprintf(stderr, __VA_ARGS__); } while (0)

int huf_leer_archivo(const char *ruta, unsigned char **datos, size_t *n) {
    *datos = NULL;
    *n     = 0;

    TRAZA("[syscall] open(\"%s\", O_RDONLY)\n", ruta);
    int fd = open(ruta, O_RDONLY);
    if (fd == -1) { perror("open"); return -1; }

    /* El valor de retorno de lseek al final ES el tamaño del archivo. */
    TRAZA("[syscall] lseek(%d, 0, SEEK_END)\n", fd);
    off_t tam = lseek(fd, 0, SEEK_END);
    if (tam == (off_t)-1) { perror("lseek"); close(fd); return -1; }
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) { perror("lseek"); close(fd); return -1; }

    unsigned char *buf = malloc(tam > 0 ? (size_t)tam : 1);
    if (buf == NULL) { perror("malloc"); close(fd); return -1; }

    ssize_t leidos = 0;
    if (tam > 0) {
        TRAZA("[syscall] read(%d, buffer, %ld)\n", fd, (long)tam);
        leidos = leer_todo(fd, buf, (size_t)tam);
        if (leidos == -1) { perror("read"); free(buf); close(fd); return -1; }
    }

    TRAZA("[syscall] close(%d)\n", fd);
    close(fd);

    *datos = buf;
    *n     = (size_t)leidos;
    return 0;
}

int huf_escribir_archivo(const char *ruta, const unsigned char *datos, size_t n) {
    TRAZA("[syscall] open(\"%s\", O_WRONLY|O_CREAT|O_TRUNC, 0644)\n", ruta);
    int fd = open(ruta, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) { perror("open"); return -1; }

    TRAZA("[syscall] write(%d, buffer, %zu)\n", fd, n);
    if (n > 0 && escribir_todo(fd, datos, n) == -1) {
        perror("write");
        close(fd);
        unlink(ruta);          /* no dejar un archivo a medias */
        return -1;
    }

    TRAZA("[syscall] close(%d)\n", fd);
    if (close(fd) == -1) { perror("close"); return -1; }
    return 0;
}

int huf_comprimir_archivo(const char *entrada, const char *salida) {
    unsigned char *datos = NULL, *comp = NULL;
    size_t n = 0, comp_n = 0;

    if (huf_leer_archivo(entrada, &datos, &n) != 0) return -1;
    if (huf_comprimir(datos, n, &comp, &comp_n) != 0) { free(datos); return -1; }
    free(datos);

    int r = huf_escribir_archivo(salida, comp, comp_n);
    free(comp);
    return r;
}

int huf_descomprimir_archivo(const char *entrada, const char *salida) {
    unsigned char *datos = NULL, *plano = NULL;
    size_t n = 0, plano_n = 0;

    if (huf_leer_archivo(entrada, &datos, &n) != 0) return -1;
    if (huf_descomprimir(datos, n, &plano, &plano_n) != 0) { free(datos); return -1; }
    free(datos);

    int r = huf_escribir_archivo(salida, plano, plano_n);
    free(plano);
    return r;
}

int huf_escribir_fd(int fd, const unsigned char *buf, size_t n) {
    if (n == 0) return 0;
    return escribir_todo(fd, buf, n) == -1 ? -1 : 0;
}
