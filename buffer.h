#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

/**
 * ====================================================================================
 * MODELO DEL DOCUMENTO EN MEMORIA: ARREGLO DINÁMICO DE DESCRIPTORES DE LÍNEA
 * ====================================================================================
 *
 * DECISIÓN DE DISEÑO CENTRAL DEL PROYECTO
 * ---------------------------------------
 * El editor NO modifica el archivo en disco durante la edición. Al abrir con 'o'
 * el archivo se carga completo a memoria; los comandos de edición (a, d, i)
 * operan solo sobre esta estructura; y el disco se toca únicamente al guardar
 * con 'w'. Es el mismo modelo de vi/vim/nano.
 *
 * Motivos:
 *   1. Atomicidad. Editar en sitio implica secuencias write()+ftruncate() que,
 *      si se interrumpen a la mitad, dejan el archivo en un estado que no es ni
 *      el viejo ni el nuevo, y sin copia de respaldo.
 *   2. Reversibilidad. Si el disco nunca se toca hasta 'w', cancelar los cambios
 *      es simplemente no guardar.
 *   3. Costo de E/S. Borrar 10 líneas de un archivo de 1 MB editando en sitio
 *      mueve ~20 MB entre proceso y kernel. Aquí son 10 memmove() en RAM y una
 *      sola escritura al final.
 *
 * POR QUÉ UN ARREGLO Y NO UNA LISTA ENLAZADA
 * ------------------------------------------
 * Todos los comandos del enunciado direccionan por número ABSOLUTO de línea
 * (p n, d n, i n). Una lista enlazada desenlaza en O(1), pero solo cuando ya se
 * tiene el puntero al nodo; llegar a la línea n exige recorrer n nodos saltando
 * por direcciones arbitrarias del heap (pointer chasing, un fallo de caché por
 * salto). El arreglo indexa lineas[n-1] en O(1).
 *
 * Su aparente desventaja -el memmove() al borrar o insertar- es barata porque
 * el arreglo guarda DESCRIPTORES, no texto: cada entrada son 16 bytes
 * (un char* y un size_t). El texto vive aparte en el heap y NUNCA se mueve.
 * Borrar la línea 5 de un archivo de 10.000 líneas desplaza ~160 KB de
 * descriptores contiguos -velocidad de ancho de banda de memoria, con prefetch
 * perfecto- en lugar de megabytes de texto.
 *
 *      lineas[]  ->  [ptr,len][ptr,len][ptr,len][ptr,len]   (contiguo, 16 B c/u)
 *                        |         |        |        |
 *                        v         v        v        v
 *      heap:          "hola"   "mundo"  "tercera"  "final"  (disperso, inmóvil)
 *
 * FRONTERA DE RESPONSABILIDADES DEL EQUIPO
 * ----------------------------------------
 * Este módulo (buffer.h / buffer.c) implementa la estructura de datos y las
 * primitivas. cat_editor.c implementa el bucle de comandos que las invoca.
 * Las primitivas buf_insertar() y buf_buscar() existen aquí para que los
 * comandos 'i' y 's' se construyan encima sin modificar este archivo.
 */

/* ====================================================================================
 * TIPOS
 * ==================================================================================== */

/**
 * Descriptor de una línea. NO contiene el texto: apunta a él.
 * 'texto' está terminado en '\0' (para poder usar strstr, printf, etc.) y NO
 * incluye el '\n' separador: ese carácter es estructura del archivo, no
 * contenido de la línea, y se reinserta al serializar en buf_guardar().
 */
typedef struct {
    char  *texto;
    size_t len;   /* == strlen(texto), cacheado para no recalcularlo */
} Linea;

/**
 * Estrategia de escritura a disco. Ver buf_guardar() para el análisis completo.
 */
typedef enum {
    GUARDA_EN_SITIO,  /* lseek+write+ftruncate sobre el archivo original */
    GUARDA_ATOMICA    /* write a temporal + fsync + rename */
} ModoGuardado;

typedef struct {
    char filename[256];

    Linea *lineas;  /* arreglo dinámico de descriptores */
    size_t num;     /* líneas en uso */
    size_t cap;     /* líneas con espacio reservado (cap >= num) */

    int cargado;        /* 1 = hay un archivo abierto en la sesión */
    int dirty;          /* 1 = hay cambios en memoria sin guardar */
    int newline_final;  /* 1 = el archivo original terminaba en '\n' */
} Buffer;

/* ====================================================================================
 * CICLO DE VIDA
 * ==================================================================================== */

/* Deja el buffer vacío y consistente. Obligatorio antes de cualquier otra cosa. */
void buf_init(Buffer *b);

/* Carga un archivo a memoria (lo crea vacío si no existe).
   Syscalls: open, lseek, read, close.   Retorna 0 en éxito, -1 en error. */
int buf_cargar(Buffer *b, const char *ruta);

/* Vuelca el documento a disco con la estrategia indicada.
   Syscalls: open, lseek, write, ftruncate, close  (o rename en modo atómico).
   Retorna 0 en éxito, -1 en error. */
int buf_guardar(Buffer *b, ModoGuardado modo);

/* Libera TODA la memoria y deja el buffer como recién inicializado.
   Requisito del enunciado: 'q' debe salir sin fugas. */
void buf_liberar(Buffer *b);

/* ====================================================================================
 * CONSULTA  (no modifican nada, no tocan disco)
 * ==================================================================================== */

/**
 * Vuelca el documento a un bloque contiguo recién reservado (el llamador lo libera).
 * Es la FOTO que se le entrega al compresor en segundo plano: a partir de ahí el
 * usuario puede seguir editando sin que el compresor vea un estado a medias, porque
 * trabaja sobre su propia copia y nunca toca este Buffer.
 * Retorna 0 / -1; deja el tamaño en *tam.
 */
int buf_serializar(const Buffer *b, unsigned char **out, size_t *tam);

size_t buf_num_lineas(const Buffer *b);

/* Texto de la línea n (1-indexada), o NULL si n está fuera de rango.
   Si 'len' no es NULL, recibe la longitud. Costo O(1). */
const char *buf_linea(const Buffer *b, size_t n, size_t *len);

/* ====================================================================================
 * EDICIÓN  (solo memoria: ninguna de estas funciones ejecuta syscalls)
 * ==================================================================================== */

/* Agrega 'texto' como última línea.            Costo: O(1) amortizado. */
int buf_agregar(Buffer *b, const char *texto);

/* Elimina la línea n (1-indexada).             Costo: O(num-n) sobre descriptores. */
int buf_borrar(Buffer *b, size_t n);

/* Inserta 'texto' ANTES de la línea n, desplazando el resto hacia abajo.
   n == num+1 equivale a agregar al final.      Costo: O(num-n) sobre descriptores.
   [Primitiva para el comando 'i'] */
int buf_insertar(Buffer *b, size_t n, const char *texto);

/* Busca 'palabra' como subcadena a partir de la línea 'desde' (1-indexada).
   Retorna el número de la primera línea que la contiene, o 0 si no hay más.
   Llamar en bucle con desde = resultado+1 para recorrer todas las coincidencias.
   [Primitiva para el comando 's'] */
size_t buf_buscar(const Buffer *b, const char *palabra, size_t desde);

#endif /* BUFFER_H */


#ifndef EDITOR_DE_TEXTO_SISTEMAS_OPERATIVOS_BUFFER_H
#define EDITOR_DE_TEXTO_SISTEMAS_OPERATIVOS_BUFFER_H

#endif //EDITOR_DE_TEXTO_SISTEMAS_OPERATIVOS_BUFFER_H