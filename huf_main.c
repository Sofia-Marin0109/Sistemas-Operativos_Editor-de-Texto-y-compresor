/* Binario de prueba independiente del compresor.
 *
 * Existe para poder validar el algoritmo SIN pasar por el REPL del editor: el ciclo
 * comprimir -> descomprimir -> md5sum dura dos segundos y se puede automatizar. Probar
 * desde el editor obligaría a teclear comandos a mano en cada intento.
 *
 *   ./huf c <entrada> <salida.huf>     comprimir
 *   ./huf d <entrada.huf> <salida>     descomprimir
 *   ./huf i <archivo.huf>              información
 *   (-v como primer argumento muestra la traza de llamadas al sistema)
 */

#include "huf_formato.h"
#include "comprimir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void uso(const char *prog) {
    fprintf(stderr,
        "uso: %s [-v] c <entrada> <salida.huf>   comprimir\n"
        "     %s [-v] d <entrada.huf> <salida>   descomprimir\n"
        "     %s [-v] i <archivo.huf>            información\n",
        prog, prog, prog);
}

static int mostrar_info(const char *ruta) {
    unsigned char *datos = NULL;
    size_t n = 0;
    if (huf_leer_archivo(ruta, &datos, &n) != 0) return 1;

    HufInfo info;
    if (huf_info(datos, n, &info) != 0) {
        fprintf(stderr, "huf: '%s' no es un archivo .huf válido\n", ruta);
        free(datos);
        return 1;
    }

    double tasa = info.tam_original > 0
                ? 100.0 * (1.0 - (double)info.tam_comprimido / (double)info.tam_original)
                : 0.0;

    printf("archivo:            %s\n", ruta);
    printf("tamaño original:    %llu bytes\n", (unsigned long long)info.tam_original);
    printf("tamaño comprimido:  %llu bytes\n", (unsigned long long)info.tam_comprimido);
    printf("ahorro:             %.2f%%\n", tasa);
    printf("bloques:            %llu de %u bytes\n",
           (unsigned long long)info.n_bloques, info.tam_bloque);
    printf("símbolos distintos: %d\n", info.n_simbolos);
    if (tasa < 0)
        printf("\nnota: el archivo creció. La cabecera y el índice pesan más que lo\n"
               "que ahorra la codificación en un archivo tan pequeño.\n");

    free(datos);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) { huf_verboso = 1; i = 2; }

    if (argc - i < 2) { uso(argv[0]); return 1; }

    const char *modo = argv[i];

    if (strcmp(modo, "i") == 0) {
        return mostrar_info(argv[i + 1]);
    }

    if (argc - i < 3) { uso(argv[0]); return 1; }

    if (strcmp(modo, "c") == 0)
        return comp_comprimir_archivo(argv[i + 1], argv[i + 2]) == 0 ? 0 : 1;

    if (strcmp(modo, "d") == 0)
        return huf_descomprimir_archivo(argv[i + 1], argv[i + 2]) == 0 ? 0 : 1;

    uso(argv[0]);
    return 1;
}
