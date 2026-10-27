#ifndef HUF_FORMATO_H
#define HUF_FORMATO_H

#include <stddef.h>
#include <stdint.h>

/**
 * ====================================================================================
 * FORMATO DE ARCHIVO .huf  Y  COMPRESOR
 * ====================================================================================
 *
 * DISPOSICIÓN DEL ARCHIVO
 * -----------------------
 *   offset  tamaño            contenido
 *   ------  ----------------  --------------------------------------------------
 *        0  4                 "HUF2"  (número mágico)
 *        4  4                 tam_bloque en bytes
 *        8  8                 tam_original en bytes
 *       16  8                 n_bloques
 *       24  2                 n_simbolos: cuántos valores de byte distintos aparecen
 *       26  9 * n_simbolos    pares (byte 1 B, frecuencia 8 B), byte creciente
 *      pre  n_bloques * 8     índice: bits útiles de cada bloque
 *     ....  resto             los bloques comprimidos, uno tras otro
 *   donde pre = 26 + 9 * n_simbolos (ver huf_tam_preambulo).
 *
 * POR QUÉ LAS FRECUENCIAS Y NO EL ÁRBOL, Y POR QUÉ SOLO LAS PRESENTES
 * -------------------------------------------------------------------
 * Serializar el árbol sería más compacto, pero las frecuencias bastan: como
 * huf_construir() es determinista (desempata por el campo 'orden'), el descompresor
 * reconstruye EXACTAMENTE el mismo árbol a partir de los mismos números. Se guarda el
 * dato más simple y se deja que el algoritmo haga el resto.
 *
 * La versión anterior (HUF1) guardaba siempre las 256 frecuencias: 2048 bytes fijos
 * aunque el archivo usara 20 valores de byte, y un archivo pequeño salía MÁS GRANDE
 * que el original. Ahora solo se guardan los símbolos que aparecen, 9 bytes cada uno,
 * así que un texto típico paga unos cientos de bytes. La tabla dispersa pierde frente
 * a la densa solo con más de 227 símbolos distintos (datos casi aleatorios), que de
 * todos modos no comprimen.
 *
 * Los pares van en orden estrictamente creciente de byte: la cabecera es determinista
 * (el .huf sale idéntico con cualquier número de hilos) y el lector detecta duplicados.
 * El lector también exige que las frecuencias sumen exactamente tam_original.
 *
 * POR QUÉ CADA BLOQUE ESTÁ ALINEADO A BYTE
 * ----------------------------------------
 * Un bloque termina casi siempre a mitad de byte. Si el siguiente continuara en ese
 * mismo byte, para codificar el bloque i habría que saber en qué bit terminó el i-1:
 * los hilos tendrían que esperarse en cadena y no habría paralelismo. Rellenando cada
 * bloque hasta completar su último byte, cada uno es AUTÓNOMO. El índice guarda los
 * bits útiles para que el lector ignore el relleno.
 *
 * Se desperdician a lo sumo 7 bits por bloque. Con bloques de 64 KB, es un 0.000013%.
 *
 * ORDEN DE BYTES EXPLÍCITO
 * ------------------------
 * Los enteros se serializan a mano en little-endian, campo por campo, en vez de volcar
 * una struct con memcpy. Volcar structs es frágil por dos motivos: el compilador puede
 * insertar relleno entre campos, y el archivo dejaría de ser legible en una máquina de
 * endianness distinta. El formato queda así definido por el estándar, no por el
 * compilador.
 */

#define HUF_MAGICO          "HUF2"
#define HUF_MAGICO_LEN      4
#define HUF_CABECERA_FIJA   26   /* magia + 3 campos + n_simbolos (u16) */
#define HUF_BYTES_POR_SIMBOLO 9  /* byte (1) + frecuencia (8) */

/* Tamaño de bloque por defecto. Es el grano de paralelismo: bloques pequeños reparten
   mejor la carga entre hilos, bloques grandes amortizan mejor el relleno y el índice.
   64 KB es un punto medio habitual. */
#define HUF_TAM_BLOQUE (64 * 1024)

/* Si vale 1, los envoltorios de archivo imprimen la traza de llamadas al sistema.
   Por defecto 0 para que las pruebas automatizadas no se llenen de ruido. */
extern int huf_verboso;

/**
 * Callback de progreso.
 *
 * Se usa una función en vez de una estructura con mutex para que NINGÚN módulo bajo
 * tenga que conocer pthread: huf_formato.c y comprimir.c solo invocan el callback, y
 * quien lo implementa (cat_editor.c) decide cómo sincronizar. Pasar NULL lo desactiva.
 *
 * Desde qué hilo se invoca: al comprimir, SIEMPRE el escritor ordenado (el hilo que
 * llamó a comp_comprimir_a_fd_cb), nunca los trabajadores, y en orden de bloque, así
 * que el avance no retrocede. Al descomprimir lo invoca el único hilo que hay. Por eso
 * el callback no necesita mutex. Si alguna vez se llamara desde los trabajadores, la
 * implementación tendría que ser segura entre hilos y el avance podría ir a saltos.
 *
 * Cancelación: el callback devuelve 0 para continuar y distinto de 0 para pedir que la
 * operación se detenga. Así los módulos de abajo no conocen señales ni banderas: quien
 * implementa el callback (cat_editor.c) decide cuándo cancelar, y la operación termina
 * ordenadamente devolviendo HUF_CANCELADO.
 */
typedef int (*HufProgreso)(void *ctx, uint64_t hechos, uint64_t total);

/* Valor de retorno de comp_comprimir_a_fd_cb y huf_descomprimir_cb cuando el callback
   pidió cancelar. Es distinto de -1 para poder distinguir "cancelado" de "falló". */
#define HUF_CANCELADO (-2)

/* ------------------------------------------------------- NÚCLEO (solo memoria) */
/* Estas dos funciones no tocan el disco. Reciben y devuelven bloques de memoria, que
   es justo lo que necesita el editor: le pasará la copia de su documento. Son también
   las que se paralelizarán, sin cambiar su firma. */

/* Comprime datos[0..n). Deja en *salida un bloque recién reservado (el llamador lo
   libera con free) y su tamaño en *salida_n. Retorna 0 / -1. */
int huf_comprimir(const unsigned char *datos, size_t n,
                  unsigned char **salida, size_t *salida_n);

/* Igual que huf_comprimir pero con las frecuencias YA calculadas.
   Existe para que el compresor paralelo (comprimir.c) cuente con varios hilos y luego
   reutilice EXACTAMENTE este mismo ensamblado del archivo. Que la salida paralela sea
   idéntica a la secuencial deja así de ser una coincidencia: es el mismo código. */
int huf_comprimir_con_freq(const unsigned char *datos, size_t n,
                           const uint64_t freq[256],
                           unsigned char **salida, size_t *salida_n);

/* Inverso. Valida el número mágico y la coherencia de la cabecera.
   Retorna 0 / -1 (formato inválido, archivo corrupto o sin memoria). */
int huf_descomprimir(const unsigned char *datos, size_t n,
                     unsigned char **salida, size_t *salida_n);

/* Igual, informando el avance tras cada bloque. cb puede ser NULL. Retorna 0, -1, o
   HUF_CANCELADO si el callback pidió detenerse (no se entrega ninguna salida). */
int huf_descomprimir_cb(const unsigned char *datos, size_t n,
                        unsigned char **salida, size_t *salida_n,
                        HufProgreso cb, void *ctx);

/* ------------------------------------------------------- ARCHIVOS (syscalls) */
/* Syscalls usadas: open(2), lseek(2), read(2), write(2), close(2). */

int huf_leer_archivo(const char *ruta, unsigned char **datos, size_t *n);
int huf_escribir_archivo(const char *ruta, const unsigned char *datos, size_t n);

int huf_comprimir_archivo(const char *entrada, const char *salida);
int huf_descomprimir_archivo(const char *entrada, const char *salida);

/* ------------------------------------------------- PIEZAS PARA EL COMPRESOR */
/* Expuestas para que comprimir.c escriba el mismo formato sin duplicar su
   definición: la disposición de la cabecera se declara UNA sola vez, aquí. */

/* Tamaño de la cabecera para estas frecuencias: 26 + 9 * (símbolos con freq > 0).
   El índice de bloques empieza justo en ese offset. */
size_t huf_tam_preambulo(const uint64_t freq[256]);

/* Rellena 'buf' (huf_tam_preambulo(freq) bytes) con la magia, los campos y los pares
   (byte, frecuencia) de los símbolos presentes. */
void huf_escribir_preambulo(unsigned char *buf, uint64_t tam_original,
                            uint64_t n_bloques, const uint64_t freq[256]);

/* Serializa un entero de 64 bits en little-endian (para el índice de bloques). */
void huf_poner_u64(unsigned char *p, uint64_t v);

/* write() reintentando ante transferencias parciales y EINTR. Retorna 0 / -1. */
int huf_escribir_fd(int fd, const unsigned char *buf, size_t n);

/* ------------------------------------------------------- INSPECCIÓN */

typedef struct {
    uint64_t tam_original;
    uint64_t tam_comprimido;
    uint64_t n_bloques;
    uint32_t tam_bloque;
    int      n_simbolos;
} HufInfo;

/* Lee solo la cabecera de un .huf ya cargado en memoria. Retorna 0 / -1. */
int huf_info(const unsigned char *datos, size_t n, HufInfo *info);

#endif /* HUF_FORMATO_H */
