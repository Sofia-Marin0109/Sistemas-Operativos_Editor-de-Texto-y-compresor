#include "shell.h"
#include "buffer.h"
#include "comprimir.h"
#include "huf_formato.h"

#include <errno.h>    /* errno, EINTR */
#include <errno.h>    /* errno, ERANGE */
#include <fcntl.h>    /* open */
#include <sys/stat.h> /* stat */
#include <signal.h>   /* sigaction, SIGINT */
#include <unistd.h>   /* write */
#include <stdio.h>    /* printf, fgets, fflush */
#include <stdlib.h>   /* atoi */
#include <string.h>   /* strcmp */

/**
 * ====================================================================================
 * CATEGORÍA: editor        COMANDO DE ENTRADA: editor
 * ====================================================================================
 *
 * JUSTIFICACIÓN DE LA INTEGRACIÓN CON EL SHELL
 * --------------------------------------------
 * Se creó una categoría NUEVA ("editor") en la tabla commands[] de main.c en
 * lugar de ubicarlo bajo "datos". El criterio no es temático sino de contrato de
 * ejecución: todos los comandos existentes del shell son ATÓMICOS -reciben argc/
 * argv, abren, operan, cierran y devuelven el control en una sola invocación.
 * El editor abre una SESIÓN: toma el control de stdin, corre su propio bucle REPL
 * anidado con prompt propio y mantiene estado (el documento en memoria) entre un
 * comando interno y el siguiente. Clasificarlo bajo "datos" haría que 'help datos'
 * mezclara comandos con dos semánticas de ejecución incompatibles.
 *
 * MODELO DE EDICIÓN
 * -----------------
 * Este archivo NO contiene lógica de manipulación de datos ni llamadas al sistema:
 * es exclusivamente el parser y despachador de comandos. Toda la estructura de
 * datos y el acceso a disco están en buffer.c. Esa separación es la que permite
 * que ambos integrantes trabajen en paralelo sin tocar los mismos archivos.
 */

/* Imprime una línea del documento al descriptor 1 (stdout).
 *
 * Se usa write(1, ...) en lugar de printf porque el enunciado exige volcar el
 * contenido por llamada al sistema. Como printf escribe en el buffer de stdio
 * (espacio de usuario) y write va directo al kernel, sin el fflush(stdout) previo
 * los dos flujos salen desordenados: el contenido aparecería ANTES de las trazas.
 */
static void escribir_stdout(const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(1, buf, n);
        if (w == -1) { if (errno == EINTR) continue; return; }
        buf += w;
        n   -= (size_t)w;
    }
}

static void imprimir_linea(const char *texto, size_t len) {
    fflush(stdout);
    escribir_stdout(texto, len);
    escribir_stdout("\n", 1);
}

/* Imprime todo el documento acumulando las líneas en un bloque de 64 KB y vaciándolo con
   un write(1, ...) por bloque, no dos por línea. Con un millón de líneas pasa de dos
   millones de cambios a modo kernel a unos cientos. */
static void imprimir_documento(const Buffer *buf) {
    enum { CAP = 64 * 1024 };
    static char salida[CAP];
    size_t usado = 0;

    fflush(stdout);
    for (size_t i = 1; i <= buf_num_lineas(buf); i++) {
        size_t len;
        const char *txt = buf_linea(buf, i, &len);
        if (len + 1 > CAP) {                    /* línea más grande que el bloque */
            if (usado) { escribir_stdout(salida, usado); usado = 0; }
            imprimir_linea(txt, len);
            continue;
        }
        if (usado + len + 1 > CAP) { escribir_stdout(salida, usado); usado = 0; }
        memcpy(salida + usado, txt, len);
        salida[usado + len] = '\n';
        usado += len + 1;
    }
    if (usado) escribir_stdout(salida, usado);
}

/* ¿Ambas rutas nombran el mismo archivo? strcmp no basta: "a.txt" y "./a.txt" (o un
   enlace) son el mismo archivo y comp/desc lo sobrescribirían. Se compara (dispositivo,
   inodo); si la ruta 'b' aún no existe no puede coincidir con una abierta. */
static int mismo_archivo(const char *a, const char *b) {
    if (strcmp(a, b) == 0) return 1;
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* ====================================================================================
 * COMPRESIÓN / DESCOMPRESIÓN CON BARRA DE PROGRESO
 * ====================================================================================
 * El hilo principal llama al compresor y se queda dentro hasta que termina: el editor
 * está detenido, así que no hay edición concurrente que proteger (ni copia para
 * aislarla, ni hilo de fondo, ni estado compartido). Los hilos trabajadores del
 * compresor siguen existiendo; el hilo que llama es quien escribe el archivo y quien
 * invoca el callback de progreso, por lo que la barra se dibuja SIN mutex.
 */
typedef struct {
    const char *verbo;
    int         tty;        /* stdout es una terminal: se puede redibujar con '\r' */
    int         ultimo;     /* último porcentaje dibujado */
} Barra;

/* 30 celdas. Sin terminal ('\r' ensuciaría un archivo redirigido) se imprime una
   línea cada vez que se CRUZA un múltiplo de 10%, no solo cuando se cae exacto en él:
   el avance es de bloque en bloque y puede pasar de 19% a 21% sin tocar el 20%. */
static void barra_dibujar(Barra *b, int pct) {
    if (pct == b->ultimo) return;
    int previo = b->ultimo;
    b->ultimo = pct;
    if (!b->tty) {
        if (previo < 0 || pct / 10 > previo / 10) {
            printf("[%s] %3d%%\n", b->verbo, pct / 10 * 10);
            fflush(stdout);
        }
        return;
    }
    enum { ANCHO = 30 };
    int lleno = pct * ANCHO / 100;
    printf("\r[%s] [", b->verbo);
    for (int i = 0; i < ANCHO; i++) putchar(i < lleno ? '#' : '-');
    printf("] %3d%%", pct);
    fflush(stdout);
}

/* ------------------------------------------------------------------------------------
 * CANCELACIÓN CON CTRL+C
 * ------------------------------------------------------------------------------------
 * El manejador de SIGINT solo levanta una bandera: dentro de un manejador casi ninguna
 * función es segura de llamar (printf, malloc y los mutex no lo son), y una asignación a
 * un volatile sig_atomic_t sí. Quien la lee es barra_cb, que el escritor invoca tras cada
 * bloque; si la ve levantada devuelve distinto de 0 y el compresor se detiene solo.
 *
 * El manejador se instala únicamente mientras corre comp o desc y se restaura al
 * terminar, así que con el prompt esperando un comando Ctrl+C se comporta como siempre.
 */
static volatile sig_atomic_t g_cancelar = 0;

static void manejador_sigint(int sig) {
    (void)sig;
    g_cancelar = 1;
}

static void sigint_instalar(struct sigaction *anterior) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                /* sin SA_RESTART: que read/write puedan volver con EINTR */
    g_cancelar = 0;
    sigaction(SIGINT, &sa, anterior);
}

static void sigint_restaurar(const struct sigaction *anterior) {
    sigaction(SIGINT, anterior, NULL);
}

static int barra_cb(void *ctx, uint64_t hechos, uint64_t total) {
    int pct = (total > 0) ? (int)((hechos * 100) / total) : 100;
    barra_dibujar((Barra *)ctx, pct > 100 ? 100 : pct);
    return g_cancelar ? 1 : 0;
}

static void barra_fin(Barra *b, int ok) {
    if (ok) barra_dibujar(b, 100);
    if (b->tty) printf("\n");
}

/* Toma 'datos' (la copia serializada del documento) y los libera siempre.
   Retorna 0, -1 (falló) o HUF_CANCELADO (el usuario presionó Ctrl+C).

   Se escribe a '<salida>.tmp' y solo al terminar bien se renombra a '<salida>'. Así una
   cancelación o un fallo no deja un .huf a medias, y si la salida ya existía queda
   intacta hasta el último momento (rename es atómico). */
static int comprimir_con_barra(const char *ruta_salida, unsigned char *datos, size_t n,
                               off_t *tam_salida) {
    Barra b = { "comprimiendo", isatty(STDOUT_FILENO), -1 };
    char tmp[4096];
    int r = -1;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", ruta_salida) >= (int)sizeof(tmp)) {
        fprintf(stderr, "comp: la ruta de salida es demasiado larga\n");
        free(datos);
        return -1;
    }

    /* El manejador se instala ANTES de crear el temporal: así no hay ventana en la que
       exista el archivo pero un Ctrl+C todavía mate el proceso y lo deje. */
    struct sigaction anterior;
    sigint_instalar(&anterior);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("open");
        sigint_restaurar(&anterior);
    } else {
        r = comp_comprimir_a_fd_cb(datos, n, fd, barra_cb, &b);
        sigint_restaurar(&anterior);

        /* El tamaño final se lee del descriptor antes de cerrarlo. */
        if (r == 0 && (*tam_salida = lseek(fd, 0, SEEK_END)) == (off_t)-1) r = -1;
        if (close(fd) == -1) { perror("close"); r = -1; }
        if (r == 0 && rename(tmp, ruta_salida) == -1) { perror("rename"); r = -1; }
        if (r != 0) unlink(tmp);    /* cancelado o fallido: no queda nada a medias */
    }
    free(datos);
    barra_fin(&b, r == 0);
    return r;
}

static int descomprimir_con_barra(const char *ruta_entrada, const char *ruta_salida) {
    Barra b = { "descomprimiendo", isatty(STDOUT_FILENO), -1 };
    unsigned char *entrada = NULL, *plano = NULL;
    size_t entrada_n = 0, plano_n = 0;
    int r = -1;

    if (huf_leer_archivo(ruta_entrada, &entrada, &entrada_n) == 0) {
        struct sigaction anterior;
        sigint_instalar(&anterior);
        int rd = huf_descomprimir_cb(entrada, entrada_n, &plano, &plano_n, barra_cb, &b);
        sigint_restaurar(&anterior);

        /* La salida solo se escribe si la decodificación terminó: al cancelar no se toca. */
        if (rd == 0) {
            r = huf_escribir_archivo(ruta_salida, plano, plano_n);
            free(plano);
        } else if (rd == HUF_CANCELADO) {
            r = HUF_CANCELADO;
        }
        free(entrada);
    }
    barra_fin(&b, r == 0);
    return r;
}

static void mostrar_ayuda(void) {
    printf(COLOR_TITLE "\n--- Editor de Texto (sesión interactiva) ---\n" COLOR_RESET);
    printf("  " COLOR_PROMPT "o <archivo>" COLOR_RESET "         Carga el archivo a memoria (lo crea si no existe).\n");
    printf("  " COLOR_PROMPT "p [n]" COLOR_RESET "               Imprime todo el documento, o solo la línea n.\n");
    printf("  " COLOR_PROMPT "a [\"<texto>\"]" COLOR_RESET "     Agrega texto al final. Sin comillas entra a modo párrafo.\n");
    printf("  " COLOR_PROMPT "i <n> [\"<texto>\"]" COLOR_RESET " Inserta texto en la línea n desplazando el resto.\n");
    printf("  " COLOR_PROMPT "d <n>" COLOR_RESET "               Elimina la línea n.\n");
    printf("  " COLOR_PROMPT "s <palabra>" COLOR_RESET "         Busca subcadenas e imprime las líneas coincidentes.\n");
    printf("  " COLOR_PROMPT "w" COLOR_RESET "                   Guarda los cambios en disco (en sitio).\n");
    printf("  " COLOR_PROMPT "w!" COLOR_RESET "                  Guarda de forma atómica (temporal + rename).\n");
    printf("  " COLOR_PROMPT "q" COLOR_RESET "                   Sale; avisa si hay cambios sin guardar.\n");
    printf("  " COLOR_PROMPT "q!" COLOR_RESET "                  Sale descartando los cambios.\n");
    printf("  " COLOR_PROMPT "comp <sal.huf>" COLOR_RESET "  Comprime el documento (el editor espera, con barra de progreso).\n");
    printf("  " COLOR_PROMPT "desc <e.huf> <s>" COLOR_RESET " Descomprime un .huf (el editor espera, con barra).\n");
    printf(COLOR_INFO "\n Tip: Si usas 'a' o 'i <n>' sin texto, entra a modo multilínea; finaliza con '.' en una línea sola.\n\n" COLOR_RESET);
}

/* Tamaño máximo de una línea leída de la entrada (incluye el '\n' y el '\0'). */
#define LINEA_MAX 65536

/* Lee una línea de stdin sin partirla en silencio.
 *
 * fgets devuelve a lo sumo cap-1 caracteres y deja el resto en la entrada. Ese resto se
 * leería en la vuelta siguiente como si fuera otro comando, o se perdería sin aviso.
 * Aquí, si el buffer se llenó sin ver el '\n', se descarta lo que sobra de ESA línea y
 * se avisa al llamador para que informe al usuario.
 *
 * Retorna 1 si la línea cabe, 0 en fin de archivo, -1 si era demasiado larga. */
static int leer_linea(char *buf, size_t cap) {
    if (fgets(buf, (int)cap, stdin) == NULL) return 0;

    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') return 1;
    if (len + 1 < cap) return 1;            /* última línea del archivo, sin '\n' */

    int c;
    while ((c = getchar()) != EOF && c != '\n') { /* descartar el resto */ }
    return -1;
}

/* Convierte un número de línea. Acepta solo dígitos y exige un entero positivo que
 * quepa en un size_t. atoi no sirve: "3x" da 3, "abc" da 0 y un número enorme tiene
 * comportamiento indefinido, así que 'd 3x' borraba la línea 3 sin avisar.
 * Retorna 0 y deja el valor en *out, o -1 si la entrada no es válida. */
static int parse_linea(const char *s, size_t *out) {
    if (s == NULL || *s < '0' || *s > '9') return -1;    /* sin espacios ni signo */

    errno = 0;
    char *fin;
    unsigned long v = strtoul(s, &fin, 10);
    if (errno == ERANGE || *fin != '\0' || v == 0) return -1;

    *out = (size_t)v;
    return 0;
}

/* Modo interactivo para escribir múltiples líneas hasta que el usuario digite '.' */
static void leer_parrafo_multilinea(Buffer *buf, size_t insertar_en_pos) {
    printf(COLOR_INFO "(Modo párrafo activo: escribe tu contenido libremente. Termina con un solo punto '.' en una línea)\n" COLOR_RESET);
    char block[LINEA_MAX];
    size_t lineas_insertadas = 0;

    while (1) {
        printf(COLOR_PROMPT "... " COLOR_RESET);
        fflush(stdout);

        int rl = leer_linea(block, sizeof(block));
        if (rl == 0) break;
        if (rl < 0) {
            printf(COLOR_ERROR "Línea demasiado larga (máximo %d caracteres): se omitió.\n" COLOR_RESET,
                   LINEA_MAX - 2);
            continue;
        }
        block[strcspn(block, "\r\n")] = '\0';

        if (strcmp(block, ".") == 0) {
            break;
        }

        if (insertar_en_pos > 0) {
            buf_insertar(buf, insertar_en_pos + lineas_insertadas, block);
        } else {
            buf_agregar(buf, block);
        }
        lineas_insertadas++;
    }
    printf(COLOR_RESULT "Se agregaron %zu líneas al buffer.\n" COLOR_RESET, lineas_insertadas);
}

int cmd_editor(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Buffer buf;
    buf_init(&buf);

    char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strncpy(hostname, "eafitOS", sizeof(hostname) - 1);
    }

    mostrar_ayuda();

    char line[LINEA_MAX];
    char *eargv[16];

    while (1) {
        if (buf.cargado && buf.filename[0] != '\0') {
            printf(COLOR_PROMPT "%s:%s%s> " COLOR_RESET, hostname, buf.filename, buf.dirty ? "*" : "");
        } else {
            printf(COLOR_PROMPT "%s:editor> " COLOR_RESET, hostname);
        }
        fflush(stdout);

        int rl = leer_linea(line, sizeof(line));
        if (rl == 0) break;                                     /* Ctrl+D */
        if (rl < 0) {
            printf(COLOR_ERROR "Comando demasiado largo (máximo %d caracteres): se descartó.\n" COLOR_RESET,
                   LINEA_MAX - 2);
            continue;
        }

        /* Soporte para argumentos con comillas multilínea */
        int quote_count = 0;
        for (char *c = line; *c; c++) {
            if (*c == '"') quote_count++;
        }
        int demasiado_largo = 0;
        while ((quote_count % 2) != 0) {
            char extra[LINEA_MAX];
            printf(COLOR_PROMPT "quote> " COLOR_RESET);
            fflush(stdout);
            int re = leer_linea(extra, sizeof(extra));
            if (re == 0) break;
            if (re < 0) { demasiado_largo = 1; break; }
            for (char *c = extra; *c; c++) {
                if (*c == '"') quote_count++;
            }
            if (strlen(line) + strlen(extra) < sizeof(line) - 1) {
                strcat(line, extra);
            } else {
                demasiado_largo = 1;
                break;
            }
        }
        if (demasiado_largo) {
            printf(COLOR_ERROR "Comando demasiado largo (máximo %d caracteres): se descartó.\n" COLOR_RESET,
                   LINEA_MAX - 2);
            continue;
        }

        int eargc = parse_line(line, eargv);
        if (eargc == 0) continue;

        /* ================================================================
         * q / q!  -- salir de la sesión
         * El chequeo de 'dirty' es la contraparte necesaria de editar en
         * memoria: sin él sería trivial perder el trabajo saliendo por
         * accidente. Es el mismo contrato de vi.
         * ================================================================ */
        if (strcmp(eargv[0], "q") == 0 || strcmp(eargv[0], "q!") == 0) {
            if (strcmp(eargv[0], "q") == 0 && buf.dirty) {
                printf(COLOR_ERROR "Hay cambios sin guardar.\n" COLOR_RESET);
                printf(COLOR_INFO "Usa 'w' para guardarlos o 'q!' para descartarlos.\n" COLOR_RESET);
                continue;
            }
            printf(COLOR_INFO "Saliendo del editor...\n" COLOR_RESET);
            break;
        }

        /* ================================================================
         * o <archivo>  -- cargar a memoria
         * ================================================================ */
        else if (strcmp(eargv[0], "o") == 0) {
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: o <archivo>\n" COLOR_RESET);
                continue;
            }
            if (buf.cargado && buf.dirty) {
                printf(COLOR_ERROR "'%s' tiene cambios sin guardar. Usa 'w' primero.\n" COLOR_RESET,
                       buf.filename);
                continue;
            }
            if (buf_cargar(&buf, eargv[1]) == 0) {
                printf(COLOR_RESULT "'%s' cargado en memoria (%zu líneas).\n" COLOR_RESET,
                       buf.filename, buf_num_lineas(&buf));
            }
        }

        /* ================================================================
         * p [n]  -- imprimir todo o una línea
         * ================================================================ */
        else if (strcmp(eargv[0], "p") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }

            if (eargc >= 2) {
                size_t n;
                if (parse_linea(eargv[1], &n) == -1) {
                    printf(COLOR_ERROR "El número de línea debe ser un entero positivo.\n" COLOR_RESET);
                    continue;
                }
                size_t len;
                const char *txt = buf_linea(&buf, n, &len);
                if (txt == NULL) {
                    printf(COLOR_ERROR "La línea %zu no existe (el documento tiene %zu líneas).\n" COLOR_RESET,
                           n, buf_num_lineas(&buf));
                    continue;
                }
                imprimir_linea(txt, len);
            } else {
                size_t total = buf_num_lineas(&buf);
                if (total == 0) {
                    printf(COLOR_INFO "(documento vacío)\n" COLOR_RESET);
                    continue;
                }
                imprimir_documento(&buf);
            }
        }

        /* ================================================================
         * a "<texto>"  -- agregar línea al final
         * ================================================================ */
        else if (strcmp(eargv[0], "a") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                leer_parrafo_multilinea(&buf, 0);
            } else {
                if (buf_agregar(&buf, eargv[1]) == 0) {
                    printf(COLOR_RESULT "Línea %zu agregada (en memoria).\n" COLOR_RESET,
                           buf_num_lineas(&buf));
                }
            }
        }

        /* ================================================================
         * i <n> ["<texto>"]  -- inserción arbitraria (Requisito Pareja)
         * ================================================================ */
        else if (strcmp(eargv[0], "i") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: i <n> [\"<texto>\"]\n" COLOR_RESET);
                continue;
            }
            size_t n;
            if (parse_linea(eargv[1], &n) == -1) {
                printf(COLOR_ERROR "El número de línea debe ser un entero positivo.\n" COLOR_RESET);
                continue;
            }
            if (n > buf_num_lineas(&buf) + 1) {
                printf(COLOR_ERROR "Posición %zu fuera de rango (1 a %zu).\n" COLOR_RESET,
                       n, buf_num_lineas(&buf) + 1);
                continue;
            }

            if (eargc < 3) {
                leer_parrafo_multilinea(&buf, n);
            } else {
                if (buf_insertar(&buf, n, eargv[2]) == 0) {
                    printf(COLOR_RESULT "Línea insertada en posición %zu (en memoria).\n" COLOR_RESET, n);
                }
            }
        }

        /* ================================================================
         * d <n>  -- borrar línea
         * ================================================================ */
        else if (strcmp(eargv[0], "d") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: d <n>\n" COLOR_RESET);
                continue;
            }
            size_t n;
            if (parse_linea(eargv[1], &n) == -1) {
                printf(COLOR_ERROR "El número de línea debe ser un entero positivo.\n" COLOR_RESET);
                continue;
            }
            if (buf_borrar(&buf, n) == -1) {
                printf(COLOR_ERROR "La línea %zu no existe (el documento tiene %zu líneas).\n" COLOR_RESET,
                       n, buf_num_lineas(&buf));
                continue;
            }
            printf(COLOR_RESULT "Línea %zu eliminada (en memoria). Quedan %zu líneas.\n" COLOR_RESET,
                   n, buf_num_lineas(&buf));
        }

        /* ================================================================
         * s <palabra>  -- búsqueda simple
         * ================================================================ */
        else if (strcmp(eargv[0], "s") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: s <palabra>\n" COLOR_RESET);
                continue;
            }
            const char *query = eargv[1];
            size_t coincidencia = 0;
            size_t actual = 1;
            int encontrados = 0;

            printf(COLOR_TITLE "--- Coincidencias para '%s' ---\n" COLOR_RESET, query);
            while ((coincidencia = buf_buscar(&buf, query, actual)) != 0) {
                size_t len;
                const char *txt = buf_linea(&buf, coincidencia, &len);
                printf(COLOR_PARAM "[Línea %zu]: " COLOR_RESET "%s\n", coincidencia, txt ? txt : "");
                actual = coincidencia + 1;
                encontrados++;
            }
            if (encontrados == 0) {
                printf(COLOR_INFO "No se encontraron ocurrencias.\n" COLOR_RESET);
            } else {
                printf(COLOR_RESULT "Total coincidencias: %d\n" COLOR_RESET, encontrados);
            }
        }

        /* ================================================================
         * w / w!  -- persistir a disco
         * ================================================================ */
        else if (strcmp(eargv[0], "w") == 0 || strcmp(eargv[0], "w!") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            ModoGuardado modo = (strcmp(eargv[0], "w!") == 0) ? GUARDA_ATOMICA : GUARDA_EN_SITIO;
            if (buf_guardar(&buf, modo) == 0) {
                printf(COLOR_RESULT "'%s' guardado (%zu líneas).\n" COLOR_RESET,
                       buf.filename, buf_num_lineas(&buf));
            }
        }

        /* ================================================================
         * clear  -- limpiar pantalla en sesión
         * ================================================================ */


        /* ================================================================
         * comp <salida.huf>  -- comprimir el documento (el editor espera)
         * ================================================================ */
        else if (strcmp(eargv[0], "comp") == 0) {
            if (!buf.cargado) {
                printf(COLOR_ERROR "No hay archivo abierto. Usa 'o <archivo>' primero.\n" COLOR_RESET);
                continue;
            }
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: comp <archivo_de_SALIDA.huf>\n" COLOR_RESET);
                continue;
            }
            /* 'comp' recibe el nombre de SALIDA. Si coincide con el archivo abierto,
               el .huf sobrescribiría el original y el usuario perdería su documento. */
            if (mismo_archivo(eargv[1], buf.filename)) {
                printf(COLOR_ERROR "'%s' es el archivo abierto: lo sobrescribirías.\n" COLOR_RESET,
                       eargv[1]);
                printf(COLOR_INFO "'comp' recibe el nombre de SALIDA. Prueba: comp %s.huf\n" COLOR_RESET,
                       buf.filename);
                continue;
            }

            /* El documento son descriptores de línea, no bytes contiguos: el compresor
               necesita un bloque lineal, así que se serializa. La copia ya no sirve
               para evitar carreras (el editor se detiene), solo para dar ese bloque. */
            unsigned char *copia = NULL;
            size_t copia_n = 0;
            if (buf_serializar(&buf, &copia, &copia_n) != 0) {
                printf(COLOR_ERROR "No se pudo copiar el documento.\n" COLOR_RESET);
                continue;
            }
            printf(COLOR_INFO "Comprimiendo %zu bytes hacia '%s'. El editor espera.\n" COLOR_RESET,
                   copia_n, eargv[1]);
            off_t comprimido_n = 0;
            int rc = comprimir_con_barra(eargv[1], copia, copia_n, &comprimido_n);
            if (rc == HUF_CANCELADO) {
                printf(COLOR_INFO "Compresión cancelada: no se modificó ningún archivo.\n" COLOR_RESET);
            } else if (rc == 0) {
                printf(COLOR_RESULT "Compresión terminada: '%s'\n" COLOR_RESET, eargv[1]);
                printf(COLOR_INFO "  Sin comprimir: %zu bytes\n  Comprimido:    %lld bytes\n" COLOR_RESET,
                       copia_n, (long long)comprimido_n);
                if (copia_n > 0)
                    printf(COLOR_INFO "  Ahorro:        %.1f%%\n" COLOR_RESET,
                           100.0 * (1.0 - (double)comprimido_n / (double)copia_n));
            } else
                printf(COLOR_ERROR "La compresión falló; no se dejó archivo parcial.\n" COLOR_RESET);
        }

        /* ================================================================
         * desc <entrada.huf> <salida>  -- descomprimir (el editor espera)
         * ================================================================ */
        else if (strcmp(eargv[0], "desc") == 0) {
            if (eargc < 3) {
                printf(COLOR_ERROR "Uso: desc <entrada.huf> <salida>\n" COLOR_RESET);
                continue;
            }
            /* Escribir sobre el archivo abierto dejaría el documento en memoria
               desincronizado del disco sin que el usuario se entere. */
            if (buf.cargado && mismo_archivo(eargv[2], buf.filename)) {
                printf(COLOR_ERROR "'%s' es el archivo abierto. Elige otra salida.\n" COLOR_RESET,
                       eargv[2]);
                continue;
            }
            printf(COLOR_INFO "Descomprimiendo '%s' hacia '%s'. El editor espera.\n" COLOR_RESET,
                   eargv[1], eargv[2]);
            int rd = descomprimir_con_barra(eargv[1], eargv[2]);
            if (rd == HUF_CANCELADO)
                printf(COLOR_INFO "Descompresión cancelada: no se escribió '%s'.\n" COLOR_RESET, eargv[2]);
            else if (rd == 0)
                printf(COLOR_RESULT "Descompresión terminada: '%s'\n" COLOR_RESET, eargv[2]);
            else
                printf(COLOR_ERROR "La descompresión falló.\n" COLOR_RESET);
        }

        else if (strcmp(eargv[0], "clear") == 0) {
            printf("\033[H\033[J");
            continue;
        }

        else {
            printf(COLOR_ERROR "Comando '%s' no reconocido dentro del editor.\n" COLOR_RESET, eargv[0]);
            printf(COLOR_INFO "Válidos: o <archivo> | p [n] | a [\"<texto>\"] | i <n> [\"<texto>\"] | d <n> | s <palabra> | w | w! | q | q! | clear | comp | desc\n" COLOR_RESET);
        }
    }

    buf_liberar(&buf);
    return 0;
}
