#define _POSIX_C_SOURCE 200809L

#include "comprimir.h"
#include "huf_formato.h"
#include "huffman.h"
#include "bitio.h"

#include <pthread.h>
#include <unistd.h>   /* sysconf, lseek, close */
#include <fcntl.h>    /* open */
#include <time.h>     /* clock_gettime */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================================
 * NÚMERO DE HILOS
 * ====================================================================================
 * sysconf(_SC_NPROCESSORS_ONLN) NO dice cuántos hilos se pueden crear (serían miles),
 * sino cuántos núcleos hay disponibles. Para trabajo limitado por CPU como este,
 * lanzar más hilos que núcleos solo añade cambios de contexto y desalojos de caché.
 *
 * La variable de entorno HILOS permite forzarlo desde fuera. No es un capricho: sin
 * ella no se podría ejecutar la prueba HILOS=1 contra HILOS=8 que verifica la
 * ausencia de condiciones de carrera.
 */
int comp_num_hilos(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;

    const char *env = getenv("HILOS");
    if (env != NULL) {
        long forzado = strtol(env, NULL, 10);
        if (forzado > 0) n = forzado;
    }
    if (n > 256) n = 256;
    return (int)n;
}

static double ahora_seg(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);   /* monótono: inmune a cambios de hora */
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ====================================================================================
 * FASE 1: CONTEO PARALELO CON REDUCCIÓN LOCAL
 * ==================================================================================== */

/**
 * Argumentos de cada hilo contador.
 *
 * Cada hilo necesita SU PROPIA copia. El error clásico es pasarle a todos la
 * dirección de la misma variable del bucle (&i): como comparten la dirección, todos
 * acaban leyendo el valor final y procesan el mismo tramo.
 *
 * 'local' va DENTRO de la estructura. Como la estructura mide más de 2 KB, los
 * arreglos de hilos distintos nunca caen en la misma línea de caché (64 bytes). Eso
 * evita el FALSO COMPARTIMIENTO: si dos hilos escribieran en la misma línea, el
 * procesador tendría que sincronizarla entre núcleos en cada escritura y el
 * rendimiento se desplomaría sin que exista ningún error lógico.
 */
typedef struct {
    const unsigned char *datos;
    size_t               inicio;    /* rango [inicio, fin) */
    size_t               fin;
    uint64_t             local[HUF_NSIMBOLOS];

    uint64_t        *global;
    pthread_mutex_t *mtx;
} ArgConteo;

static void *hilo_contar(void *p) {
    ArgConteo *a = (ArgConteo *)p;

    /* Fase sin sincronización: lee de 'datos' (que nadie modifica) y escribe en
       'local' (que es solo suyo). Corre a velocidad plena. Es el 99.99% del trabajo. */
    huf_freq_init(a->local);
    huf_contar(a->datos + a->inicio, a->fin - a->inicio, a->local);

    /* Sección crítica: un único bloqueo por hilo, con 256 sumas dentro. Bloquear el
       arreglo global en cada byte haría millones de lock/unlock y sería MÁS LENTO
       que no paralelizar. */
    pthread_mutex_lock(a->mtx);
    huf_acumular(a->global, a->local);
    pthread_mutex_unlock(a->mtx);

    return NULL;
}

int comp_contar(const unsigned char *datos, size_t n,
                uint64_t freq[HUF_NSIMBOLOS], int n_hilos) {
    huf_freq_init(freq);

    if (n == 0) return 0;
    if (n_hilos < 1) n_hilos = 1;
    if ((size_t)n_hilos > n) n_hilos = (int)n;

    /* Con un solo hilo se evita la maquinaria: crearlo y unirse cuesta más que
       contar unos pocos bytes. El resultado es idéntico. */
    if (n_hilos == 1) {
        huf_contar(datos, n, freq);
        return 0;
    }

    ArgConteo *args  = calloc((size_t)n_hilos, sizeof(ArgConteo));
    pthread_t *hilos = calloc((size_t)n_hilos, sizeof(pthread_t));
    if (args == NULL || hilos == NULL) {
        perror("calloc");
        free(args); free(hilos);
        return -1;
    }

    pthread_mutex_t mtx;
    if (pthread_mutex_init(&mtx, NULL) != 0) {
        perror("pthread_mutex_init");
        free(args); free(hilos);
        return -1;
    }

    /* Tramos contiguos; el último absorbe el resto de la división. Contiguos y no
       entrelazados a propósito: cada hilo recorre memoria secuencial, que es el
       patrón que el prefetcher del procesador predice bien. */
    size_t tam = n / (size_t)n_hilos;
    int creados = 0, error = 0;

    for (int i = 0; i < n_hilos; i++) {
        args[i].datos  = datos;
        args[i].inicio = (size_t)i * tam;
        args[i].fin    = (i == n_hilos - 1) ? n : (size_t)(i + 1) * tam;
        args[i].global = freq;
        args[i].mtx    = &mtx;

        if (pthread_create(&hilos[i], NULL, hilo_contar, &args[i]) != 0) {
            perror("pthread_create");
            error = 1;
            break;
        }
        creados++;
    }

    /* Unirse SIEMPRE a los hilos creados, incluso si hubo error. Un hilo sin join
       podría seguir escribiendo en 'freq' y en 'args' después de que esta función
       los libere: use-after-free con hilos corriendo encima. */
    for (int i = 0; i < creados; i++) pthread_join(hilos[i], NULL);

    pthread_mutex_destroy(&mtx);
    free(args);
    free(hilos);
    return error ? -1 : 0;
}

/* ====================================================================================
 * FASE 3 y 4: CODIFICACIÓN PARALELA CON ESCRITOR ORDENADO
 * ==================================================================================== */

/**
 * Estado compartido entre los trabajadores y el escritor.
 *
 * Un solo mutex protege TODOS los campos mutables. Con un único candado no puede
 * haber interbloqueo por orden de adquisición: la causa clásica -el hilo 1 tiene A y
 * pide B mientras el hilo 2 tiene B y pide A- necesita al menos dos mutex.
 *
 * Dos variables de condición sobre ese mismo mutex, cada una con su significado:
 *   cond_listo   trabajador -> escritor : "terminé un bloque, revisa si es el tuyo"
 *   cond_espacio escritor -> trabajadores: "ya escribí uno, hay hueco en la ventana"
 */
typedef struct {
    /* --- Solo lectura durante la fase (no necesitan protección) --- */
    const unsigned char *datos;
    size_t               n;
    const Huffman       *huf;
    uint64_t             n_bloques;
    uint64_t             ventana;

    /* --- Protegido por 'mtx' --- */
    pthread_mutex_t mtx;
    pthread_cond_t  cond_listo;
    pthread_cond_t  cond_espacio;

    uint64_t  proximo_a_tomar;     /* siguiente bloque sin asignar a nadie */
    uint64_t  proximo_a_escribir;  /* el que el escritor está esperando */
    BitWriter *bw;                 /* resultado de cada bloque */
    unsigned char *listo;          /* 1 cuando bw[b] está completo */
    int       error;               /* algo falló: todos deben salir */
    int       cancelado;           /* el callback pidió parar (solo lo toca el escritor) */
} TrabajoCodif;

/**
 * Hilo trabajador: toma bloques de la cola y los codifica.
 *
 * El trabajo pesado (bw_init, huf_emitir, bw_flush) se hace SIN sostener el mutex.
 * Sostenerlo durante la codificación serializaría a todos los hilos y el paralelismo
 * desaparecería: el mutex solo protege los contadores, no el cómputo.
 */
static void *hilo_codificar(void *p) {
    TrabajoCodif *t = (TrabajoCodif *)p;

    for (;;) {
        /* ---------- Tomar el siguiente bloque ---------- */
        pthread_mutex_lock(&t->mtx);

        /* La ventana: no adelantarse demasiado al escritor, o los bloques
           comprimidos se acumularían en memoria sin límite.
           while y no if: pthread_cond_wait puede despertar de forma espuria, y
           además al despertar hay que reevaluar porque el estado pudo cambiar
           entre el broadcast y la readquisición del mutex. */
        while (!t->error &&
               t->proximo_a_tomar < t->n_bloques &&
               t->proximo_a_tomar >= t->proximo_a_escribir + t->ventana) {
            pthread_cond_wait(&t->cond_espacio, &t->mtx);
        }

        if (t->error || t->proximo_a_tomar >= t->n_bloques) {
            pthread_mutex_unlock(&t->mtx);
            break;                       /* no queda trabajo o alguien falló */
        }

        uint64_t b = t->proximo_a_tomar++;
        pthread_mutex_unlock(&t->mtx);

        /* ---------- Codificar (fuera del mutex) ---------- */
        size_t off = (size_t)b * HUF_TAM_BLOQUE;
        size_t len = (off + HUF_TAM_BLOQUE <= t->n) ? HUF_TAM_BLOQUE : t->n - off;

        int fallo = 0;
        if (bw_init(&t->bw[b], len + 16) != 0) {
            fallo = 1;
        } else {
            for (size_t i = 0; i < len && !fallo; i++)
                if (huf_emitir(t->huf, &t->bw[b], t->datos[off + i]) != 0) fallo = 1;
            if (!fallo && bw_flush(&t->bw[b]) != 0) fallo = 1;
        }

        /* ---------- Publicar el resultado ---------- */
        pthread_mutex_lock(&t->mtx);
        if (fallo) t->error = 1;
        t->listo[b] = 1;
        /* broadcast y no signal: puede haber varios hilos dormidos en cond_listo
           (el escritor) y en cond_espacio; despertar a todos y que cada uno
           reevalúe su condición es lo seguro. */
        pthread_cond_broadcast(&t->cond_listo);
        pthread_mutex_unlock(&t->mtx);

        if (fallo) break;
    }

    /* Al salir, despertar a quien esté esperando: si este hilo era el último, el
       escritor podría quedarse dormido para siempre. */
    pthread_mutex_lock(&t->mtx);
    pthread_cond_broadcast(&t->cond_listo);
    pthread_cond_broadcast(&t->cond_espacio);
    pthread_mutex_unlock(&t->mtx);
    return NULL;
}

/* ====================================================================================
 * COORDINADOR
 * ====================================================================================
 * El hilo que llama a esta función hace de escritor ordenado. No se crea un hilo
 * aparte para eso: el que ya está aquí no tendría nada más que hacer, y un hilo
 * menos es un join menos y una ruta de error menos.
 */
int comp_comprimir_a_fd(const unsigned char *datos, size_t n, int fd) {
    return comp_comprimir_a_fd_cb(datos, n, fd, NULL, NULL);
}

int comp_comprimir_a_fd_cb(const unsigned char *datos, size_t n, int fd,
                           HufProgreso cb, void *ctx) {
    int n_hilos = comp_num_hilos();
    int ret = -1;

    double t0 = ahora_seg();

    /* ---------- Fase 1: conteo paralelo ---------- */
    uint64_t freq[HUF_NSIMBOLOS];
    if (comp_contar(datos, n, freq, n_hilos) != 0) return -1;

    double t1 = ahora_seg();

    /* ---------- Fase 2: el árbol (secuencial por necesidad) ----------
       Los códigos tienen que ser los mismos en todos los bloques, así que el árbol
       se construye una sola vez con las frecuencias completas. Es el cuello de
       botella secuencial inevitable del algoritmo (ley de Amdahl), pero es O(256
       log 256): despreciable frente a recorrer el archivo. */
    Huffman h;
    if (huf_construir(&h, freq) != 0) return -1;

    uint64_t n_bloques = (n + HUF_TAM_BLOQUE - 1) / HUF_TAM_BLOQUE;
    size_t   tam_indice = (size_t)n_bloques * 8;

    /* ---------- Cabecera + hueco del índice ----------
       El índice no se conoce todavía: se reserva con ceros y se rellena al final
       volviendo con lseek. */
    size_t pre = huf_tam_preambulo(freq);   /* depende de cuántos símbolos aparecen */
    unsigned char *preambulo = calloc(1, pre);
    unsigned char *hueco     = tam_indice ? calloc(1, tam_indice) : NULL;
    if (preambulo == NULL || (tam_indice && hueco == NULL)) {
        perror("calloc");
        free(preambulo); free(hueco); huf_liberar(&h);
        return -1;
    }
    huf_escribir_preambulo(preambulo, (uint64_t)n, n_bloques, freq);

    int fallo_io = (huf_escribir_fd(fd, preambulo, pre) != 0) ||
                   (tam_indice && huf_escribir_fd(fd, hueco, tam_indice) != 0);
    free(preambulo);
    free(hueco);
    if (fallo_io) { perror("write"); huf_liberar(&h); return -1; }

    uint64_t *bits = NULL;
    if (n_bloques > 0) {
        bits = calloc((size_t)n_bloques, sizeof(uint64_t));
        if (bits == NULL) { perror("calloc"); huf_liberar(&h); return -1; }
    }

    double t2 = ahora_seg();

    /* ---------- Caso trivial: nada que codificar ---------- */
    if (n_bloques == 0) {
        huf_liberar(&h);
        free(bits);
        return 0;
    }

    /* ---------- Fases 3 y 4 ---------- */
    TrabajoCodif t;
    memset(&t, 0, sizeof(t));
    t.datos     = datos;
    t.n         = n;
    t.huf       = &h;
    t.n_bloques = n_bloques;
    t.ventana   = (uint64_t)n_hilos * COMP_VENTANA_POR_HILO;
    if (t.ventana < 2) t.ventana = 2;

    t.bw    = calloc((size_t)n_bloques, sizeof(BitWriter));
    t.listo = calloc((size_t)n_bloques, 1);
    pthread_t *hilos = calloc((size_t)n_hilos, sizeof(pthread_t));
    if (t.bw == NULL || t.listo == NULL || hilos == NULL) {
        perror("calloc");
        goto limpiar_sin_hilos;
    }

    if (pthread_mutex_init(&t.mtx, NULL) != 0 ||
        pthread_cond_init(&t.cond_listo, NULL) != 0 ||
        pthread_cond_init(&t.cond_espacio, NULL) != 0) {
        perror("pthread_init");
        goto limpiar_sin_hilos;
    }

    int creados = 0;
    for (int i = 0; i < n_hilos; i++) {
        if (pthread_create(&hilos[i], NULL, hilo_codificar, &t) != 0) {
            perror("pthread_create");
            pthread_mutex_lock(&t.mtx);
            t.error = 1;
            pthread_cond_broadcast(&t.cond_espacio);
            pthread_mutex_unlock(&t.mtx);
            break;
        }
        creados++;
    }

    /* ---------- El escritor ordenado ----------
       Recorre los bloques en orden estricto. Para cada uno espera a que esté listo
       durmiendo en cond_listo: cero consumo de CPU mientras espera, a diferencia de
       un bucle `while (!listo[b]) {}` que le robaría un núcleo a los que trabajan. */
    if (creados > 0) {
        for (uint64_t b = 0; b < n_bloques; b++) {
            pthread_mutex_lock(&t.mtx);
            while (!t.listo[b] && !t.error)
                pthread_cond_wait(&t.cond_listo, &t.mtx);
            int hubo_error = t.error;
            pthread_mutex_unlock(&t.mtx);

            if (hubo_error) break;

            /* Escribir fuera del mutex: el write() puede tardar y no hay razón para
               bloquear a los trabajadores mientras tanto. Es seguro leer bw[b] aquí
               porque listo[b] se marcó bajo el mutex, lo que ordena la publicación
               de los datos del bloque respecto a esta lectura. */
            if (huf_escribir_fd(fd, t.bw[b].datos, t.bw[b].nbytes) != 0) {
                perror("write");
                pthread_mutex_lock(&t.mtx);
                t.error = 1;
                pthread_cond_broadcast(&t.cond_espacio);
                pthread_mutex_unlock(&t.mtx);
                break;
            }
            bits[b] = t.bw[b].total_bits;
            bw_free(&t.bw[b]);          /* liberar ya: la ventana depende de esto */

            pthread_mutex_lock(&t.mtx);
            t.proximo_a_escribir = b + 1;
            pthread_cond_broadcast(&t.cond_espacio);   /* hay hueco */
            pthread_mutex_unlock(&t.mtx);

            /* El progreso se informa desde el escritor y no desde los trabajadores:
               aquí los bloques pasan en orden, así que el porcentaje nunca retrocede.
               Informado por los trabajadores (que terminan en desorden) la barra
               daría saltos hacia atrás. */
            if (cb) {
                uint64_t hechos = (b + 1) * HUF_TAM_BLOQUE;
                if (hechos > n) hechos = n;
                if (cb(ctx, hechos, (uint64_t)n) != 0) {
                    /* Cancelación: se sale por el mismo camino que un error de
                       escritura. 'error' hace que los trabajadores terminen y los
                       dos broadcast despiertan a los que estén dormidos. */
                    pthread_mutex_lock(&t.mtx);
                    t.error = 1;
                    pthread_cond_broadcast(&t.cond_espacio);
                    pthread_cond_broadcast(&t.cond_listo);
                    pthread_mutex_unlock(&t.mtx);
                    t.cancelado = 1;
                    break;
                }
            }
        }
    }

    for (int i = 0; i < creados; i++) pthread_join(hilos[i], NULL);

    if (!t.error && creados == n_hilos) {
        /* ---------- Rellenar el índice ----------
           Volver al hueco reservado y sobrescribirlo con los valores reales. */
        unsigned char *idx = malloc(tam_indice);
        if (idx == NULL) { perror("malloc"); goto limpiar; }
        for (uint64_t b = 0; b < n_bloques; b++) huf_poner_u64(idx + b * 8, bits[b]);

        if (lseek(fd, (off_t)pre, SEEK_SET) == (off_t)-1) {
            perror("lseek");
            free(idx);
            goto limpiar;
        }
        if (huf_escribir_fd(fd, idx, tam_indice) != 0) {
            perror("write");
            free(idx);
            goto limpiar;
        }
        free(idx);
        ret = 0;
    }
    if (t.cancelado) ret = HUF_CANCELADO;   /* los hilos ya fueron unidos arriba */

limpiar:
    for (uint64_t b = 0; b < n_bloques; b++) bw_free(&t.bw[b]);   /* idempotente */
    pthread_cond_destroy(&t.cond_espacio);
    pthread_cond_destroy(&t.cond_listo);
    pthread_mutex_destroy(&t.mtx);

limpiar_sin_hilos:
    free(hilos);
    free(t.bw);
    free(t.listo);
    free(bits);
    huf_liberar(&h);

    if (huf_verboso) {
        double t3 = ahora_seg();
        fprintf(stderr,
                "[tiempos] hilos=%d  conteo=%.4f s  arbol=%.4f s  codificacion=%.4f s\n",
                n_hilos, t1 - t0, t2 - t1, t3 - t2);
    }
    return ret;
}

int comp_comprimir_archivo(const char *entrada, const char *salida) {
    unsigned char *datos = NULL;
    size_t n = 0;

    if (huf_leer_archivo(entrada, &datos, &n) != 0) return -1;

    int fd = open(salida, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) { perror("open"); free(datos); return -1; }

    int r = comp_comprimir_a_fd(datos, n, fd);
    free(datos);

    if (close(fd) == -1) { perror("close"); r = -1; }

    /* Nunca dejar un .huf a medio escribir: o está completo o no está. */
    if (r != 0) unlink(salida);
    return r;
}
