# Editor de Texto con Llamadas al Sistema (Linux)

Editor de texto en **C** que trabaja directamente contra el kernel de Linux mediante
**llamadas al sistema POSIX** (`open`, `read`, `write`, `lseek`, `ftruncate`, `fsync`,
`fchmod`, `fchown`, `rename`, `unlink`, `close`), sin usar la capa de `stdio.h` (`fopen`,
`fread`, `fwrite`, `fclose`) para manipular archivos.

El editor vive dentro de un shell educativo (`eafitOS`) que imprime en pantalla, estilo
`strace` simplificado, cada syscall ejecutada con sus parámetros y su valor de retorno.
Sobre el editor se construyó un **compresor Huffman concurrente** (comandos `comp` y
`desc`) que reparte el trabajo entre varios hilos y que el usuario puede **cancelar con
Ctrl+C** sin perder el documento ni dejar archivos a medias. Por último, una auditoría
del código corrigió fallos de robustez del editor y aceleró el compresor entre 4,4 y 9
veces.

> **La decisión central del proyecto:** el editor **no modifica el archivo en disco
> mientras editas**. El documento se carga a memoria al abrir, todas las operaciones
> ocurren sobre una estructura en RAM, y el disco se toca únicamente al guardar (`w`) o
> al comprimir (`comp`). Es el modelo de `vi`, `nano` y VS Code. La sección
> [Decisiones de diseño](#decisiones-de-diseño) explica por qué, y por qué esa misma
> decisión fue la que hizo posible el compresor.

### El proyecto de un vistazo

```
                          ┌──────────────────────────────┐
                          │  eafitOS  (main.c)           │
                          │  prompt: Wordsito>           │
                          └──────────────┬───────────────┘
                                         │  comando "editor"  (categoría nueva)
                                         ▼
                          ┌──────────────────────────────┐
                          │  cat_editor.c                │
                          │  parser + despachador        │
                          │  (no toca el documento en    │
                          │   disco: eso es de buffer.c) │
                          └───────┬──────────────┬───────┘
              o p a i d s w w! q  │              │  comp / desc
                                  ▼              ▼
              ┌─────────────────────────┐  ┌──────────────────────────────────┐
              │  buffer.c               │  │  COMPRESOR HUFFMAN (4 capas)     │
              │  documento en RAM       │  │  comprimir.c   ← hilos           │
              │  + syscalls de archivo  │  │  huf_formato.c ← formato y disco │
              │                         │  │  huffman.c     ← árbol y códigos │
              │  PARCIAL 1              │  │  bitio.c       ← bits ↔ bytes    │
              └─────────────────────────┘  │  PARCIAL 2                       │
                                           └──────────────────────────────────┘
```

| Parcial | Qué se construyó | Archivos principales |
| ------- | ---------------- | -------------------- |
| 1 | Editor con el documento en memoria: `o`, `p`, `a`, `i`, `d`, `s`, `w`, `w!`, `q`, `q!` | `cat_editor.c`, `buffer.c`, `buffer.h` |
| 2 | Compresor Huffman concurrente, comandos `comp` y `desc`, binario independiente `huf` | `bitio.c`, `huffman.c`, `huf_formato.c`, `comprimir.c`, `huf_main.c` |
| 2 | Cancelación de `comp` y `desc` con Ctrl+C | `cat_editor.c`, `comprimir.c`, `huf_formato.c` |
| — | Auditoría: robustez del editor y rendimiento del compresor (ver [`AUDITORIA.md`](AUDITORIA.md)) | `buffer.c`, `cat_editor.c`, `bitio.c`, `huf_formato.c`, `huffman.c`, `Makefile` |

---

## Compilar y ejecutar

Requiere Linux con `gcc`, `make` y soporte de hilos POSIX (`pthread`).

```bash
make              # compila DOS binarios: ./eafitOS (shell + editor) y ./huf (compresor)
./eafitOS         # inicia el shell
make run          # atajo: compila y ejecuta ./eafitOS
make test         # compila y corre las pruebas unitarias (bitio: 18, huffman: 23)
make clean        # borra los .o, los binarios y los binarios de prueba
```

| Objetivo | Qué produce | Para qué sirve |
| -------- | ----------- | -------------- |
| `make` / `make all` | `eafitOS` y `huf` | El shell con el editor, y el compresor como programa aparte |
| `make run` | ejecuta `eafitOS` | Atajo de desarrollo |
| `make test` | `test_bitio` y `test_huffman` | Pruebas unitarias de las dos capas bajas del compresor |
| `make clean` | — | Deja el directorio solo con el código fuente |

**Por qué `make` produce también `huf`.** Los scripts `pruebas_huf.sh` y
`pruebas_hilos.sh` necesitan ese binario. Antes el `Makefile` no lo generaba y esas
pruebas fallaban aunque el código estuviera bien: la falla era del sistema de
construcción, no del compresor.

**Por qué `make` ya no ejecuta el shell.** Antes `all` lanzaba `./eafitOS` al terminar de
compilar, lo que rompía `make` en entornos no interactivos (scripts de prueba, CI). Ahora
ejecutar es un objetivo aparte (`make run`).

La compilación usa `-Wall -Wextra -std=gnu99 -O2 -g -D_GNU_SOURCE -pthread` y no produce
warnings.

| Bandera | Por qué está |
| ------- | ------------ |
| `-O2` | Antes no había ningún nivel de optimización y gcc compilaba en `-O0`. Solo con añadir `-O2`, comprimir 64 MB con 1 hilo pasó de 3,35 s a 1,84 s. Es el cambio de mayor rendimiento por menor esfuerzo |
| `-g` | Se conserva junto a `-O2` para poder seguir depurando con `gdb` y leer las trazas de valgrind |
| `-pthread` | Obligatoria: no solo enlaza la biblioteca de hilos, también activa las macros que hacen segura la biblioteca estándar en programas concurrentes |

Activar `-O2` hizo aparecer dos avisos que en `-O0` no salían: el valor de retorno de
`write` ignorado y un índice que podía desbordarse en `mont_extraer`. Los dos se
corrigieron en lugar de silenciarlos (ver [Decisión 12](#12-qué-cambió-la-auditoría-y-por-qué-se-aplicó-casi-toda)).

Dentro del shell, para entrar al editor:

```
Wordsito> editor
ubuntu:editor>
```

---

## Comandos del editor

| Comando | Qué hace | Syscalls involucradas |
| ------- | -------- | --------------------- |
| `o <archivo>` | Carga el archivo a memoria (lo crea vacío si no existe). Si no tiene permiso de escritura, lo abre en solo lectura | `open(2)` (reintenta con `O_RDONLY` ante `EACCES` o `EROFS`), `lseek(2)`, `read(2)`, `close(2)` |
| `p` | Imprime el documento completo | `write(2)` a FD 1, **uno por cada 64 KB**, no uno por línea |
| `p <n>` | Imprime solo la línea `n` | `write(2)` a FD 1 |
| `a "<texto>"` | Agrega el texto como nueva línea al final | **ninguna**, solo memoria |
| `a` | Sin texto: entra al **modo párrafo** y agrega varias líneas al final | **ninguna**, solo memoria |
| `i <n> "<texto>"` | Inserta el texto en la línea `n` y desplaza el resto hacia abajo | **ninguna**, solo memoria |
| `i <n>` | Sin texto: modo párrafo, insertando a partir de la línea `n` | **ninguna**, solo memoria |
| `d <n>` | Elimina la línea `n` | **ninguna**, solo memoria |
| `s <palabra>` | Busca la subcadena e imprime cada línea donde aparece, con su número | **ninguna**, solo memoria |
| `w` | Guarda en disco reescribiendo el archivo original | `open(2)`, `lseek(2)`, `write(2)`, `ftruncate(2)`, `close(2)` |
| `w!` | Guarda en disco de forma atómica, conservando permisos y dueño | `mkstemp` (`open` con `O_EXCL`), `write(2)`, `fchown(2)`, `fchmod(2)`, `fsync(2)`, `close(2)`, `rename(2)`, `fsync(2)` del directorio |
| `comp <salida.huf>` | Comprime el documento en memoria; el editor espera con barra de progreso. **Ctrl+C cancela** | `sigaction(2)`, `open(2)` de `salida.huf.tmp`, `write(2)`, `lseek(2)`, `close(2)`, `rename(2)`; `unlink(2)` del temporal si falla o se cancela |
| `desc <entrada.huf> <salida>` | Descomprime un `.huf`; el editor espera con barra de progreso. **Ctrl+C cancela** | `sigaction(2)`, `open(2)`, `lseek(2)`, `read(2)`, `write(2)`, `close(2)` |
| `q` | Sale; **avisa si hay cambios sin guardar** | ninguna |
| `q!` | Sale descartando los cambios | ninguna |
| `clear` | Limpia la pantalla | ninguna (secuencia ANSI) |

La columna de syscalls es la parte importante de la tabla: **los comandos que modifican o
consultan el documento (`a`, `i`, `d`, `s`) no tocan el disco**, y `p` solo escribe en la
pantalla. Toda la E/S sobre archivos se concentra en `o`, `w`, `w!`, `comp` y `desc`.

**Los números de línea se validan de forma estricta.** `p`, `i` y `d` aceptan solo un
entero positivo escrito completo. Antes usaban `atoi`, que lee hasta donde puede y
calla el resto: `d 2x` borraba la línea 2 como si el comando estuviera bien escrito, y
`d abc` se convertía en `d 0`. Ahora cualquier entrada que no sea un número válido
muestra `El número de línea debe ser un entero positivo.` y no hace nada:

| Entrada | Antes (`atoi`) | Ahora (`strtoul` estricto) |
| ------- | -------------- | -------------------------- |
| `d 2` | borra la línea 2 | borra la línea 2 |
| `d 2x` | **borra la línea 2** | rechaza |
| `d abc` | `0`: "debe ser positivo" | rechaza |
| `d -3` | `-3`: "debe ser positivo" | rechaza |
| `d 99999999999999999999` | comportamiento indefinido | rechaza (desbordamiento) |

**Las líneas pueden medir hasta 65 536 caracteres.** Si una línea escrita o pegada es
más larga, el editor descarta esa línea completa y avisa `Comando demasiado largo
(máximo 65536 caracteres): se descartó.` Antes el límite era de 4096 y el sobrante se
leía en la vuelta siguiente **como si fuera otro comando**: pegar un texto largo podía
ejecutar instrucciones que el usuario nunca escribió.

### Cómo se escribe el texto: comillas y modo párrafo

El tokenizador `parse_line()` separa los argumentos por espacios, **excepto** lo que va
entre comillas dobles, que entrega como un solo argumento. De ahí salen tres formas de
escribir:

| Lo que escribes | Lo que queda en el documento | Por qué |
| --------------- | ---------------------------- | ------- |
| `a "hola mundo"` | una línea: `hola mundo` | las comillas agrupan las dos palabras en un argumento |
| `a` (sin nada) | las líneas que escribas, hasta un `.` solo | entra al modo párrafo |
| `a hola mundo` | una línea: **`hola`** | sin comillas, `mundo` es un tercer argumento y se ignora |

El **modo párrafo** existe porque escribir un texto de varias líneas con un `a "..."`
por línea es lento y propenso a errores. Al usar `a` o `i <n>` sin texto, el editor
muestra el prompt `...` y toma cada línea tal cual, **con espacios y sin comillas**, hasta
que se escribe un punto solo:

```
ubuntu:notas.txt> a
(Modo párrafo activo: escribe tu contenido libremente. Termina con un solo punto '.' en una línea)
... Querido diario:
... hoy terminé el editor.
... .
Se agregaron 2 líneas al buffer.
```

Con `i <n>` el modo párrafo inserta la primera línea en la posición `n`, la segunda en
`n+1`, y así sucesivamente, de modo que el bloque queda en el mismo orden en que se
escribió.

Si se abren comillas y se presiona Enter sin cerrarlas, el editor no ejecuta el comando:
muestra `quote>` y sigue leyendo hasta encontrar la comilla de cierre, igual que `bash`.

### El prompt informa el estado

```
ubuntu:editor>         ← no hay archivo abierto
ubuntu:notas.txt>      ← notas.txt abierto, sin cambios pendientes
ubuntu:notas.txt*>     ← hay cambios en memoria que NO están en disco
```

El asterisco es la bandera `dirty` del buffer hecha visible. Como el editor trabaja en
memoria, el usuario necesita saber en todo momento si lo que ve coincide con lo que hay
en disco. El nombre de la máquina sale de `gethostname()`, así que cambia según el
equipo.

### Sesión de ejemplo (salida real)

```
Wordsito> editor

--- Editor de Texto (sesión interactiva) ---
  o <archivo>         Carga el archivo a memoria (lo crea si no existe).
  p [n]               Imprime todo el documento, o solo la línea n.
  a ["<texto>"]       Agrega texto al final. Sin comillas entra a modo párrafo.
  i <n> ["<texto>"]   Inserta texto en la línea n desplazando el resto.
  d <n>               Elimina la línea n.
  s <palabra>         Busca subcadenas e imprime las líneas coincidentes.
  w                   Guarda los cambios en disco (en sitio).
  w!                  Guarda de forma atómica (temporal + rename).
  q                   Sale; avisa si hay cambios sin guardar.
  q!                  Sale descartando los cambios.
  comp <sal.huf>      Comprime el documento (el editor espera, con barra de progreso).
  desc <e.huf> <s>    Descomprime un .huf (el editor espera, con barra).

ubuntu:editor> o notas.txt
[syscall] open("notas.txt", O_RDWR|O_CREAT, 0644) ... = 3
[syscall] lseek(3, 0, SEEK_END) ... = 0
[syscall] lseek(3, 0, SEEK_SET) ... = 0
[syscall] close(3) ... = 0
'notas.txt' cargado en memoria (0 líneas).

ubuntu:notas.txt> a "primera linea"
Línea 1 agregada (en memoria).

ubuntu:notas.txt*> a "segunda linea con sol"
Línea 2 agregada (en memoria).

ubuntu:notas.txt*> i 2 "linea insertada"
Línea insertada en posición 2 (en memoria).

ubuntu:notas.txt*> p
primera linea
linea insertada
segunda linea con sol

ubuntu:notas.txt*> s sol
--- Coincidencias para 'sol' ---
[Línea 3]: segunda linea con sol
Total coincidencias: 1

ubuntu:notas.txt*> d 1
Línea 1 eliminada (en memoria). Quedan 2 líneas.

ubuntu:notas.txt*> w
[syscall] open("notas.txt", O_WRONLY|O_CREAT, 0644) ... = 3
[syscall] lseek(3, 0, SEEK_SET) ... = 0
[syscall] write(3, buffer, 38) ... = 38
[syscall] ftruncate(3, 38) ... = 0
[syscall] close(3) ... = 0
'notas.txt' guardado (2 líneas).

ubuntu:notas.txt> comp notas.txt.huf
Comprimiendo 38 bytes hacia 'notas.txt.huf'. El editor espera.
[comprimiendo] [##############################] 100%
Compresión terminada: 'notas.txt.huf'
  Sin comprimir: 38 bytes
  Comprimido:    187 bytes
  Ahorro:        -392.1%

ubuntu:notas.txt> desc notas.txt.huf copia.txt
Descomprimiendo 'notas.txt.huf' hacia 'copia.txt'. El editor espera.
[descomprimiendo] [##############################] 100%
Descompresión terminada: 'copia.txt'

ubuntu:notas.txt> q
Saliendo del editor...
```

**Lo que hay que notar en esa traza:**

1. **Entre `o` y `w` no aparece ni una sola syscall sobre el archivo.** Seis comandos
   (`a`, `a`, `i`, `p`, `s`, `d`) y cero accesos a disco. Todo el costo de E/S se
   concentra en `o` (una lectura) y `w` (una escritura). Ese vacío en la traza es la
   evidencia visible del modelo de diseño.
2. **El asterisco aparece con el primer `a` y desaparece después de `w`.** Es la
   bandera `dirty` siguiendo al documento.
3. **`s sol` encontró la línea 3, no la 2**, porque el `i 2` de antes empujó
   `segunda linea con sol` una posición hacia abajo. La búsqueda trabaja sobre el
   documento actual en memoria, no sobre el archivo en disco.
4. **El ahorro de `comp` sale negativo.** No es un error: con 38 bytes la cabecera del
   `.huf` pesa más que el texto. Con un documento real el resultado se invierte; por
   ejemplo, un archivo Markdown de 48 225 bytes quedó en 30 629 bytes (36,5 % de
   ahorro). La sección [Formato HUF2](#7-por-qué-la-cabecera-guarda-solo-los-símbolos-presentes-formato-huf2)
   explica de dónde sale cada byte.

### Cancelar con Ctrl+C

Como `comp` y `desc` detienen el editor hasta terminar (ver la
[decisión 10](#10-por-qué-el-editor-se-congela-mientras-comprime)), el usuario necesita
una forma de arrepentirse. Durante cualquiera de los dos comandos, **Ctrl+C detiene la
operación de forma ordenada**:

| Comando cancelado | Mensaje | Qué queda en disco | ¿El editor sigue vivo? |
| ----------------- | ------- | ------------------ | ---------------------- |
| `comp salida.huf` | `Compresión cancelada: no se modificó ningún archivo.` | Nada nuevo. Si `salida.huf` ya existía, queda **intacto** | Sí, con el documento y sus cambios sin guardar |
| `desc x.huf salida` | `Descompresión cancelada: no se escribió '…'` | Nada: la salida solo se escribe cuando la decodificación terminó | Sí |

Fuera de `comp` y `desc`, con el prompt esperando un comando, Ctrl+C se comporta como
siempre. La [decisión 11](#11-por-qué-ctrlc-cancela-en-vez-de-matar-el-proceso) explica
cómo funciona y por qué se diseñó así.

### Protecciones de `comp` y `desc`

| Situación | Qué hace el editor | Qué evita |
| --------- | ------------------ | --------- |
| `comp notas.txt` con `notas.txt` abierto | Rechaza y sugiere `comp notas.txt.huf` | `comp` recibe el nombre de **salida**: sobrescribiría el documento original con datos comprimidos |
| `comp ./notas.txt`, un enlace o una ruta con `..` que apunte al archivo abierto | También rechaza | Ver abajo: comparar el texto de las rutas no bastaba |
| `desc x.huf notas.txt` con `notas.txt` abierto | Rechaza y pide otra salida | El disco cambiaría por debajo del documento en memoria, y un `w` posterior lo pisaría sin avisar |
| La compresión falla o se cancela | Borra el temporal `salida.huf.tmp` con `unlink(2)`; `salida.huf` no se toca | Que quede un archivo a medio escribir, o que se destruya una salida anterior |
| La salida no es una terminal (`./eafitOS > log.txt`) | Imprime una línea cada vez que el porcentaje cruza un múltiplo de 10, en vez de redibujar la barra | Que el `\r` de la barra llene el archivo de basura |

**Por qué comparar archivos y no rutas.** La primera versión comparaba los nombres con
`strcmp`. Con `notas.txt` abierto, `comp ./notas.txt` no coincidía como texto, la
protección no actuaba y el `.huf` reemplazaba al documento: se reprodujo, y el contenido
de `notas.txt` pasó a ser `HUF2...`. Lo mismo ocurría con enlaces simbólicos, enlaces
duros y rutas con `..`. Ahora dos rutas se consideran el mismo archivo si `stat(2)`
devuelve el mismo dispositivo y el mismo número de inodo:

```c
static int mismo_archivo(const char *a, const char *b) {
    if (strcmp(a, b) == 0) return 1;
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
```

```
"notas.txt"     ──┐
"./notas.txt"   ──┼──► (st_dev = 2049, st_ino = 4401)  ← un solo archivo
"../x/notas.txt"──┤
enlace → notas  ──┘
```

Es la misma idea de la [decisión 3](#3-por-qué-dos-estrategias-de-guardado): en Unix un
nombre es solo una entrada de directorio; lo que identifica a un archivo es su inodo.
Si la salida todavía no existe, `stat` falla y no puede coincidir con el archivo abierto.

---

## Decisiones de diseño

Esta sección documenta las decisiones arquitectónicas del proyecto, con las alternativas
que se descartaron y el razonamiento detrás de cada una. Las decisiones 1 a 6 son del
editor; las 7 a 10, del compresor.

| # | Decisión | Alternativa descartada | Razón en una línea |
| - | -------- | ---------------------- | ------------------ |
| 1 | Documento en memoria | Editar el archivo en disco | Un proceso que muere a mitad de operación deja el archivo corrupto |
| 2 | Arreglo de descriptores de línea | Lista enlazada, gap buffer, piece table | Todos los comandos usan número de línea: el arreglo da acceso inmediato |
| 3 | Dos estrategias de guardado | Solo una | `ftruncate` es requisito; `rename` es lo correcto. Las dos permiten comparar |
| 4 | Categoría nueva `editor` | Ponerlo en `datos` | El editor abre una sesión; los demás comandos son atómicos |
| 5 | Cerrar el descriptor tras cargar | Mantenerlo abierto | Elimina el `lseek` obligatorio en cada comando |
| 6 | `i` y `s` sobre el mismo arreglo | Estructuras auxiliares | Se reutiliza el `memmove` de `d` y la búsqueda lineal basta |
| 7 | Formato HUF2 con cabecera dispersa | Tabla fija de 256 frecuencias (HUF1) | Los archivos pequeños crecían hasta 3 veces |
| 8 | Compresor en capas, primero secuencial | Escribirlo concurrente desde el inicio | Así un fallo de hilos no se confunde con un fallo del algoritmo |
| 9 | Reducción local, bloques autónomos, ventana acotada | Mutex por byte, flujo continuo de bits | Paralelismo real con memoria acotada |
| 10 | Congelar el editor mientras comprime | Comprimir en un hilo de fondo | Ver la sección 10: elimina copias, estado compartido y carreras |
| 11 | Ctrl+C cancela: bandera + callback que devuelve `int` + temporal con `rename` | Matar el proceso, `pthread_cancel`, trabajar dentro del manejador | Se cancela sin perder el documento, sin hilos cortados y sin archivos a medias |
| 12 | Aplicar la auditoría salvo la descompresión en paralelo | Aplicarla entera, o no aplicarla | Corrige fallos reales comprobados con pruebas; lo opcional se dejó fuera |

### 1. Por qué el documento vive en memoria y no se edita en disco

**La alternativa descartada.** La implementación original mantenía el descriptor de archivo
abierto durante toda la sesión y cada comando modificaba el archivo real de inmediato: `d`
leía el archivo completo, hacía `memmove()` sobre los bytes, reescribía desde el offset 0 y
recortaba con `ftruncate()`. Era la lectura literal del enunciado.

**Los tres problemas de ese modelo.**

*No es atómico.* Borrar una línea son tres syscalls encadenadas: `lseek(0)` → `write(nuevo)`
→ `ftruncate(nuevo_tamaño)`. Entre el `write` y el `ftruncate` el archivo en disco contiene
el contenido nuevo **más la cola vieja pegada al final**. Si el proceso muere en ese instante
(Ctrl+C, un `kill`, un corte de energía, el disco lleno) el usuario se queda con un archivo
que no es ni el viejo ni el nuevo, y sin ninguna copia a la cual volver, porque el original
ya fue sobrescrito.

```
Archivo original:   [línea 1][línea 2][línea 3]
Borrar "línea 1":
  1. write(nuevo) →  [línea 2][línea 3][ea 3]      ← cola vieja todavía en disco
                                       ▲
                     si el proceso muere AQUÍ, el archivo queda así para siempre
  2. ftruncate()  →  [línea 2][línea 3]
```

*No hay reversibilidad.* Cada comando es definitivo. No existe "cancelar", porque no hay
ningún estado anterior guardado en ningún lado.

*El costo de E/S es desproporcionado.* Cada `d` lee el archivo completo y lo reescribe
completo. Borrar 10 líneas de un archivo de 1 MB mueve ~20 MB entre el proceso y el kernel.
Con el documento en memoria, esas 10 ediciones son 10 `memmove()` en RAM y **una** escritura
al final.

**La decisión.** Separar el *modelo del documento* de su *persistencia*. `o` carga, los
comandos de edición operan sobre la estructura en RAM, y `w` persiste. El precio es que
aparece un comando que el enunciado no pedía (`w`) y que `q` debe verificar cambios
pendientes; a cambio, el archivo del usuario nunca queda en un estado intermedio.

**La consecuencia que no se buscó, pero se aprovechó.** Tener el documento entero en RAM
es lo que después hizo sencillo el compresor: el texto ya está en memoria, así que `comp`
no necesita leer el archivo de disco, y comprime exactamente lo que el usuario ve, incluso
cambios que todavía no ha guardado.

### 2. Por qué un arreglo de descriptores y no una lista enlazada

La opción intuitiva para un editor es una lista doblemente enlazada de líneas; es lo que
usa GNU nano. Se descartó tras analizar el patrón de acceso real de los comandos.

**El argumento.** Todos los comandos direccionan por **número absoluto de línea**: `p 3`,
`d 5`, `i 12`. Ninguno tiene concepto de cursor o posición actual. La ventaja de una lista
enlazada es desenlazar en O(1), pero **solo cuando ya se tiene el puntero al nodo**; aquí hay
que llegar recorriendo desde `head`, que es O(n) con *pointer chasing*: cada salto es a una
dirección arbitraria del heap y por tanto un fallo de caché potencial. Se paga el costo sin
cobrar el beneficio.

**La estructura elegida.**

```c
typedef struct {
    char  *texto;   /* la línea, SIN el '\n' */
    size_t len;
} Linea;            /* 16 bytes: un puntero y un size_t */

typedef struct {
    Linea *lineas;  /* arreglo dinámico de DESCRIPTORES */
    size_t num;     /* líneas en uso */
    size_t cap;     /* líneas con espacio reservado */
    ...
} Buffer;
```

```
lineas[]  ->  [ptr,len][ptr,len][ptr,len][ptr,len]   contiguo, 16 B por entrada
                  |         |        |        |
                  v         v        v        v
heap:          "hola"   "mundo"  "tercera"  "final"  disperso, y NUNCA se mueve
```

**Por qué su desventaja aparente no lo es.** Borrar o insertar exige un `memmove()`, que es
O(n). Pero el arreglo guarda **descriptores, no texto**: borrar la línea 5 de un archivo de
10.000 líneas desplaza ~160 KB de descriptores contiguos, a velocidad de ancho de banda de
memoria, con prefetch perfecto y vectorización, en lugar de mover megabytes de contenido.
El texto de cada línea vive aparte en el heap y no se mueve nunca.

**La lección general:** la complejidad asintótica no es lo único que importa. Dos algoritmos
O(n) pueden diferir en un orden de magnitud según su localidad de memoria.

| Operación | Lista enlazada | Arreglo de descriptores |
| --------- | -------------- | ----------------------- |
| `p n` | O(n) con pointer chasing | **O(1)**, una indexación |
| `d n` | O(n) + O(1) desenlazar | O(1) + `memmove` de 16 B/línea |
| `i n` | O(n) + O(1) enlazar | O(1) + `memmove` de 16 B/línea |
| `a` | O(1) con puntero `tail` | O(1) amortizado |
| `s palabra` | O(total de texto) | O(total de texto) |

**Por qué un nodo por línea y no por palabra.** Se consideró también dividir el texto en
palabras. La estructura debe tener la misma unidad que las operaciones: ningún comando
trabaja por palabra, y un nodo por palabra multiplicaba la memoria por unas trece veces
(un descriptor de 16 bytes por cada palabra, más la fragmentación del heap) sin que
ningún comando lo aprovechara.

**Lo que se pierde.** El acceso aleatorio barato tiene como contraparte que insertar al
principio de un archivo enorme desplaza todos los descriptores. Para archivos de texto de
tamaño razonable el intercambio es claramente favorable. Estructuras que optimizan ambos
casos (*gap buffer* de Emacs, *piece table* de VS Code, *rope*) existen, pero su
complejidad no se justifica en este alcance.

### 3. Por qué dos estrategias de guardado

`w` y `w!` producen **exactamente el mismo archivo**. Se diferencian en la ruta por la que
los bytes llegan al disco, y por tanto en qué ocurre si el proceso se interrumpe a la mitad.

| | `w` (en sitio) | `w!` (atómico) |
| --- | --- | --- |
| Escribe sobre | el archivo original | un temporal con nombre único, creado con `mkstemp` en el mismo directorio |
| Syscalls | `open`, `lseek`, `write`, `ftruncate`, `close` | `mkstemp`, `write`, `fchown`, `fchmod`, `fsync`, `close`, `rename`, `fsync` del directorio |
| Si se interrumpe | el original queda **corrupto** | el original queda **intacto** |
| Inodo resultante | el mismo | uno nuevo |
| Conserva permisos y dueño | sí | **sí**, se copian al temporal antes del `rename` |
| Conserva enlaces duros | sí | **no**: el nombre pasa a apuntar a otro inodo |

**Cómo funciona el modo atómico.** En Unix, un archivo no "tiene" un nombre: los datos y
metadatos viven en un **inodo**, y un directorio es una tabla que asocia nombres a números de
inodo. `rename()` **no copia un solo byte de contenido**: solo reescribe esa tabla para que
el nombre `notas.txt` apunte al inodo nuevo, y elimina la entrada del temporal. El inodo
viejo se queda sin enlaces y el sistema de archivos libera sus bloques.

```
ANTES                                  DESPUÉS
notas.txt      ──► inodo 4401          notas.txt  ──► inodo 4402  (contenido nuevo)
temporal       ──► inodo 4402          (inodo 4401: bloques liberados)
```

Esa operación es atómica por dos razones que se refuerzan: ambas modificaciones caen en el
mismo bloque de directorio, y los sistemas de archivos con journal (ext4, XFS, btrfs) la
registran como **una sola transacción**. Tras un corte de energía, la recuperación aplica la
transacción completa o la descarta entera; nunca a medias. Por eso `rename()` tarda lo mismo
en un archivo de 1 KB que en uno de 100 MB.

**Por qué el temporal va en el mismo directorio.** Los números de inodo son únicos *por
sistema de archivos*. Si el temporal estuviera en `/tmp` y el original en `$HOME`, podrían
ser filesystems distintos y no habría inodo que intercambiar: `rename` fallaría con `EXDEV`.

**Por qué el `fsync()` antes del `rename()`.** Cuando `write()` retorna, los datos suelen
estar en el *page cache* del kernel, no en el disco físico. Sin `fsync`, un corte de energía
podría dejar el `rename` ya consolidado pero los datos del temporal aún en RAM: el nombre
correcto apuntando a un inodo vacío. `fsync` fuerza la bajada y no retorna hasta que el disco
confirma, estableciendo el orden correcto.

**Por qué `mkstemp` y no un nombre fijo `notas.txt.tmp`.** La primera versión creaba
siempre `notas.txt.tmp`. Si el usuario ya tenía un archivo propio con ese nombre, `w!` lo
destruía. `mkstemp` inventa un nombre que no existe y lo crea con `O_EXCL`, que hace
fallar la creación si el archivo ya existe en lugar de reutilizarlo. Así el temporal
nunca puede ser un archivo ajeno.

**Por qué se copian dueño y permisos.** Como `rename` deja un inodo nuevo en el lugar del
viejo, el archivo tomaba los permisos del temporal: un archivo con permisos `600`
(privado) o `755` (ejecutable) quedaba en `644` después de `w!`. Ahora, antes del
`rename`, se aplican al temporal el dueño y el grupo del original con `fchown` y sus
permisos con `fchmod`. El orden importa:

| Paso | Por qué en ese orden |
| ---- | -------------------- |
| 1. `fchown` (dueño y grupo) | Cambiar el dueño puede apagar los bits especiales `setuid`/`setgid`. Por eso va primero |
| 2. `fchmod` (permisos) | Al ir después, deja los permisos exactos del original, incluidos esos bits |

`fchown` es "de mejor esfuerzo": un usuario normal no puede regalar archivos a otro
dueño, así que si falla, el guardado continúa. Si no existía un original, el permiso es
`0644` menos lo que quite la `umask` del usuario, como haría cualquier programa.

**Por qué un segundo `fsync`, ahora del directorio.** El primer `fsync` asegura los
*datos* del temporal. Pero el `rename` modifica el *directorio*, que también puede quedar
solo en la caché del kernel. Sin `fsync` del directorio, tras un corte de energía el
archivo podría volver a aparecer con su contenido viejo. Con los dos `fsync`, el orden
queda garantizado de punta a punta:

```
write(temporal) ─► fsync(temporal) ─► rename ─► fsync(directorio)
  datos en caché    datos en disco     nombre     nombre en disco
                                       cambiado
```

**Por qué se implementaron las dos y no solo la mejor.** El enunciado nombra `ftruncate()`
como syscall del proyecto, y esa llamada **solo aparece en la ruta en sitio**: en el modo
atómico el temporal se crea vacío y no hay nada que recortar. Mantener ambas
rutas cumple el requisito y, más importante, permite demostrar empíricamente la diferencia
en la sustentación:

```bash
ls -i notas.txt   # anota el número de inodo
# ... editar y guardar con w   -> el inodo se MANTIENE
# ... editar y guardar con w!  -> el inodo CAMBIA
```

Ese número cambiando es la prueba observable de que son dos mecanismos distintos.

### 4. Por qué el editor es una categoría nueva del shell

El enunciado exige decidir y justificar si el editor entra en una categoría existente del
shell o en una nueva. Se creó la categoría **`editor`** en la tabla `commands[]` de `main.c`.

**El criterio no es temático sino de contrato de ejecución.** Todos los comandos existentes
del shell son **atómicos**: reciben `argc`/`argv`, abren, operan, cierran y devuelven el
control en una sola invocación. El editor abre una **sesión**: toma el control de `stdin`,
corre su propio bucle REPL anidado con prompt propio, y mantiene estado (el documento en
memoria) entre un comando interno y el siguiente.

```
Comando atómico (d_read):            Sesión (editor):
Wordsito> d_read a.txt               Wordsito> editor
  open → read → write → close        ubuntu:editor> o a.txt     ┐
Wordsito>   ← vuelve de inmediato    ubuntu:a.txt> a "..."      │ estado vivo
                                     ubuntu:a.txt*> w           │ entre comandos
                                     ubuntu:a.txt> q            ┘
                                     Wordsito>   ← vuelve solo con q
```

Clasificarlo bajo `datos` (que agrupa `d_create`, `d_read`, `d_copy`) haría que `help datos`
mezclara comandos con dos semánticas de ejecución incompatibles. Un usuario que ejecuta
`d_read archivo` recupera el prompt; uno que ejecuta `editor` queda dentro de otro programa.
Esa diferencia es más relevante para el usuario que el hecho de que ambos manipulen archivos.

**Por qué `comp` y `desc` van dentro del editor y no como comandos del shell.** Por el
mismo criterio, al revés: `comp` comprime **el documento que está en memoria**, que solo
existe dentro de la sesión. Como comando del shell tendría que leer el archivo de disco y
comprimiría la última versión guardada, no la que el usuario está viendo. Para comprimir
archivos sueltos desde la terminal existe el binario independiente `huf`.

### 5. Por qué el descriptor se cierra de inmediato

`buf_cargar()` cierra el `fd` apenas termina de leer, en lugar de mantenerlo abierto durante
la sesión como hacía la versión anterior.

Mantenerlo abierto obligaba a que **cada comando empezara con un `lseek()`** para reposicionar
el cursor, porque ningún comando sabía dónde lo había dejado el anterior. Eso es estado
compartido implícito: cualquier comando nuevo que olvidara el `lseek` corrompería al
siguiente. Además, un descriptor abierto sin usar es un recurso retenido y una fuente de
desincronización si el archivo cambia por fuera.

Con el documento en RAM, el descriptor no aporta nada entre comandos. Cerrarlo hace que cada
operación sea independiente y elimina toda una clase de bugs.

### 6. Por qué `i` y `s` no necesitan estructuras nuevas

Los dos comandos extra del nivel de pareja se montaron sobre el mismo arreglo, sin agregar
ninguna estructura de datos. Eso es una prueba de que la decisión 2 fue correcta.

**`i <n>` es el `memmove` de `d` al revés.** Borrar *cierra* un hueco desplazando los
descriptores hacia arriba; insertar lo *abre* desplazándolos hacia abajo:

```
i 2 "nueva"   sobre   [L1][L2][L3][  ]      (cap = 4, num = 3)

1. memmove: corre L2 y L3 una posición     [L1][  ][L2][L3]
2. escribe el descriptor nuevo en [1]      [L1][nueva][L2][L3]
3. num++                                   num = 4
```

```c
memmove(&b->lineas[n],          /* destino: una posición más abajo   */
        &b->lineas[n - 1],      /* origen: desde la línea n          */
        (b->num - (n - 1)) * sizeof(Linea));
```

El rango válido es de `1` a `num + 1`. La posición `num + 1` significa "después de la
última línea", así que `i` también puede agregar al final y `a` resulta ser un caso
particular de `i`. Fuera de ese rango, el editor rechaza el comando e indica el rango
permitido en lugar de rellenar con líneas vacías.

**`s <palabra>` es una búsqueda lineal con `strstr`.** `buf_buscar(b, palabra, desde)`
devuelve el número de la primera línea, a partir de `desde`, que contiene la subcadena, o
`0` si no hay más. El editor la llama en un bucle, cada vez desde la línea siguiente a la
última coincidencia, e imprime cada resultado con su número de línea:

```
buf_buscar(..., desde = 1)  → 3   imprime [Línea 3]
buf_buscar(..., desde = 4)  → 7   imprime [Línea 7]
buf_buscar(..., desde = 8)  → 0   fin: "Total coincidencias: 2"
```

Se usa `0` como "no encontrado" porque las líneas se numeran desde 1, así que `0` nunca
puede ser una respuesta válida. Separar la búsqueda (en `buffer.c`) de la impresión (en
`cat_editor.c`) respeta la frontera entre los dos archivos: `buffer.c` no imprime nada.

**Por qué no algo más sofisticado.** Un índice invertido o un algoritmo como
Boyer-Moore aceleran búsquedas repetidas sobre textos enormes, pero exigen mantener la
estructura sincronizada con cada `a`, `i` y `d`. Para un documento que cabe en RAM, una
pasada con `strstr` toma microsegundos. Es una búsqueda **de subcadena y sensible a
mayúsculas**: `s sol` encuentra `consola` pero no `Sol` (ver
[Limitaciones](#limitaciones-conocidas)).

### 7. Por qué la cabecera guarda solo los símbolos presentes (formato HUF2)

**La idea de Huffman, en simple.** Normalmente cada carácter ocupa 8 bits, aparezca mucho o
poco. Huffman asigna códigos cortos a los caracteres frecuentes y largos a los raros, como
el código Morse. Con el texto `aaaabbc`:

| Carácter | Frecuencia | Código | Bits usados |
| -------- | ---------- | ------ | ----------- |
| `a` | 4 | `0` | 4 × 1 = 4 |
| `b` | 2 | `10` | 2 × 2 = 4 |
| `c` | 1 | `11` | 1 × 2 = 2 |

Total: **10 bits** en lugar de 7 × 8 = **56 bits**.

**El problema.** Para descomprimir hace falta el mismo árbol con el que se comprimió, así
que el `.huf` tiene que llevar algo que permita reconstruirlo. Se guardan las
**frecuencias**, no el árbol: como `huf_construir()` es determinista (desempata siempre igual),
las mismas frecuencias reproducen exactamente el mismo árbol. Se guarda el dato más simple
y se deja que el algoritmo haga el resto.

La primera versión del formato (HUF1) guardaba **siempre las 256 frecuencias posibles**, de 8
bytes cada una: 2048 bytes fijos de cabecera, aunque el texto usara solo 20 caracteres
distintos. Un texto de 712 bytes se convertía en un `.huf` de 2250 bytes: un "ahorro" de
**menos 216 %**. El compresor hacía crecer justo los archivos más comunes de un editor.

**La decisión.** HUF2 guarda primero cuántos símbolos aparecen (`n_simbolos`, 2 bytes) y
después solo los pares *(byte, frecuencia)* de esos símbolos, 9 bytes cada uno, en orden
creciente de byte:

```
HUF1 (antes)                                HUF2 (ahora)
┌──────────────────────────────┐            ┌──────────────────────────────┐
│ "HUF1" + 3 campos   24 bytes │            │ "HUF2" + 3 campos   24 bytes │
├──────────────────────────────┤            ├──────────────────────────────┤
│ freq[0]   = 0                │            │ n_simbolos = 15      2 bytes │
│ freq[1]   = 0                │            ├──────────────────────────────┤
│ ...                          │            │ ('a', 4120)          9 bytes │
│ freq['a'] = 4120             │ 2048 bytes │ ('e', 3907)          9 bytes │
│ ...                          │ SIEMPRE    │ ...                          │ 9 × 15
│ freq[255] = 0                │            │                              │ = 135 bytes
├──────────────────────────────┤            ├──────────────────────────────┤
│ índice de bloques            │            │ índice de bloques            │
│ bloques comprimidos          │            │ bloques comprimidos          │
└──────────────────────────────┘            └──────────────────────────────┘
cabecera = 2072 bytes, fija                  cabecera = 26 + 9 × n_simbolos
```

| Offset | Tamaño | Contenido |
| ------ | ------ | --------- |
| 0 | 4 | `"HUF2"`, número mágico |
| 4 | 4 | tamaño de bloque (64 KB) |
| 8 | 8 | tamaño original en bytes |
| 16 | 8 | número de bloques |
| 24 | 2 | `n_simbolos`: cuántos valores de byte distintos aparecen |
| 26 | 9 × `n_simbolos` | pares (byte, frecuencia), byte creciente |
| `26 + 9·n` | 8 × bloques | índice: bits útiles de cada bloque |
| ... | resto | los bloques comprimidos, uno tras otro |

**Resultados medidos:**

| Entrada | HUF1 | HUF2 | Observación |
| ------- | ---- | ---- | ----------- |
| Texto repetitivo de 660 bytes | crecía | **378 bytes** | ahora sí comprime |
| Texto de 100 000 bytes (15 símbolos) | 50 214 bytes | **48 303 bytes** | 51,7 % de ahorro |
| Archivo vacío | 2072 bytes | **26 bytes** | solo la cabecera fija |
| Datos aleatorios (256 símbolos) | crece | crece un poco más | se explica abajo |

**Por qué se eligió esto y no un modo "sin comprimir".** La otra salida era detectar que
comprimir no ayuda y guardar el archivo tal cual. Se descartó porque esconde el síntoma
en lugar de corregir la causa: el problema no era Huffman, era una tabla que pagaba por
256 símbolos aunque hubiera 15.

**Lo que sigue sin resolverse, y por qué es aceptable.** La tabla dispersa pierde frente a
la densa con más de 227 símbolos distintos (9 × 228 > 2048). Eso solo pasa con datos casi
aleatorios, que de todos modos no comprimen: si los 256 bytes aparecen con la misma
frecuencia, todos los códigos miden 8 bits y no hay nada que ahorrar. Es el principio del
palomar: ningún compresor sin pérdida puede reducir *todas* las entradas posibles. Y los
archivos diminutos todavía crecen (el ejemplo de 38 bytes de la sesión quedó en 187),
porque cada símbolo distinto cuesta 9 bytes de cabecera; HUF2 bajó mucho el punto a partir
del cual comprimir vale la pena, pero no lo eliminó.

**Por qué se mantuvieron frecuencias de 8 bytes.** Se podían usar 4 bytes para archivos
de menos de 4 GB, pero eso mezclaba tamaños de campo y abría un caso de desbordamiento.
Se prefirió un formato uniforme.

**El lector desconfía de todo.** Un `.huf` puede estar corrupto o no ser un `.huf`. Antes
de usar cualquier campo para indexar memoria, el lector rechaza: `n_simbolos` mayor que 256,
símbolos repetidos o desordenados, frecuencias en cero, frecuencias cuya suma no sea
exactamente el tamaño original, tablas truncadas y archivos del formato anterior HUF1.
Sin estas comprobaciones, un archivo manipulado provocaría lecturas fuera del buffer.

**Costo del cambio:** los `.huf` creados con HUF1 ya no se pueden abrir, y los nuevos no se
abren con la versión anterior. Se aceptó porque no había archivos HUF1 que conservar.

### 8. Por qué el compresor se construyó en capas, y primero sin hilos

El compresor está dividido en cuatro módulos, y cada uno ignora la existencia del de
arriba:

| Capa | Archivo | Responsabilidad | ¿Sabe de hilos? | Prueba |
| ---- | ------- | --------------- | --------------- | ------ |
| 3 | `comprimir.c` | Repartir el trabajo entre hilos, escribir en orden | **Sí** | `pruebas_hilos.sh` |
| 2 | `huf_formato.c` | Formato `.huf`, compresión en memoria, syscalls de archivo | No | `pruebas_huf.sh` |
| 1 | `huffman.c` | Árbol y tabla de códigos | No | `test_huffman` (23) |
| 0 | `bitio.c` | Empaquetar bits sueltos en bytes y viceversa | No | `test_bitio` (18) |

**La decisión: primero correcto, después rápido.** Las capas 0, 1 y 2 se escribieron y
validaron completas en versión secuencial, con el binario independiente `huf`, antes de
crear un solo hilo. Cuando después algo falló al paralelizar, el algoritmo ya estaba
descartado como sospechoso: el error solo podía estar en la capa 3.

**Por qué funcionó.** Las capas bajas no tienen estado global: reciben todo por parámetro
y devuelven todo por parámetro. Eso bastó para que varios hilos pudieran usarlas a la vez:
`bitio.c` y `huffman.c` no cambiaron al paralelizar, y `huf_formato.c` solo recibió
ajustes que no lo atan a los hilos (la función `huf_comprimir_con_freq`, el callback de
progreso, después convertido en la vía de cancelación, y el formato HUF2). Más tarde la
auditoría sí tocó las tres capas, pero por rendimiento y seguridad, no por concurrencia:
`bw_bits` escribe varios bits por iteración, `mont_extraer` es seguro con el montículo
vacío y la descompresión usa una tabla de búsqueda. La concurrencia se montó encima, no se inyectó en cada
módulo. Por eso `bitio.c`, `huffman.c` y `huf_formato.c` no incluyen `pthread.h`.

**Una sola definición del formato.** El compresor secuencial (`huf_comprimir`) cuenta las
frecuencias y llama a `huf_comprimir_con_freq`. El paralelo cuenta con varios hilos y
llama **a la misma función**. Que la salida paralela sea idéntica a la secuencial deja de
ser una coincidencia: es literalmente el mismo código el que define el archivo.

### 9. Cómo se reparte el trabajo entre hilos

La compresión tiene dos fases, y cada una se paraleliza de forma distinta:

```
                datos del documento (bytes contiguos)
     ┌────────────────────────┬────────────────────────┬────────────────────────┐
     │        tramo 1         │        tramo 2         │        tramo 3         │
     └───────────┬────────────┴───────────┬────────────┴───────────┬────────────┘
FASE 1           ▼                        ▼                        ▼
CONTEO      hilo 1: local[256]       hilo 2: local[256]       hilo 3: local[256]
(sin mutex)      │                        │                        │
                 └──────── suma al global, UNA vez por hilo ───────┘
                                          │
                                 pthread_join (barrera)
                                          │
                              construir árbol y tabla
                                          │
FASE 2      bloques de 64 KB:  [b0] [b1] [b2] [b3] [b4] [b5] ...
CODIFICACIÓN      hilos trabajadores codifican en paralelo, cada uno en su BitWriter
                  terminan en DESORDEN:  b2 ✓  b0 ✓  b3 ✓  b1 ✓ ...
                                          │
ESCRITOR    el hilo que llamó escribe EN ORDEN: b0 → b1 → b2 → b3 ...
ORDENADO    si b1 no está listo, DUERME en una variable de condición
                                          │
            al final: lseek al inicio y rellena el índice de bloques
```

| Decisión | Alternativa descartada | Por qué |
| -------- | ---------------------- | ------- |
| **Reducción local**: cada hilo cuenta en su propio arreglo y suma al global una sola vez | Un `freq[256]` global con mutex | Con 10 MB serían 10 millones de bloqueos: los hilos pasarían más tiempo esperándose que contando, y sería **más lento** que sin hilos. Con reducción local son N bloqueos en total |
| **Bloques de 64 KB alineados a byte**, con sus bits útiles en el índice | Un flujo continuo de bits | En un flujo continuo, el bloque *i* empieza donde terminó el *i−1*: cada hilo tendría que esperar al anterior y no habría paralelismo. Alinear cuesta como mucho 7 bits por bloque (0,000013 %) |
| **Ventana acotada**: como mucho 4 bloques por hilo adelantados al escritor | Codificar todo y ensamblar al final | Ensamblar al final mantiene todos los bloques en memoria: con 1 GB serían 16 384 bloques vivos. La ventana deja la memoria en O(hilos), no O(tamaño del archivo) |
| **Dos variables de condición**: `cond_listo` (trabajador → escritor) y `cond_espacio` (escritor → trabajadores) | Espera activa (`while (!listo[b]) {}`) | Un hilo en `pthread_cond_wait` sale de la cola del planificador y usa 0 % de CPU; uno en espera activa le roba un núcleo entero a los que hacen el trabajo |
| **El hilo que llama es el escritor** | Un hilo escritor dedicado | Ahorra un hilo y una ruta de error. Además, como escribe en orden, la barra de progreso nunca retrocede |
| **Índice reservado con ceros y rellenado al final con `lseek`** | Calcular todo antes de escribir | Los bits de cada bloque no se conocen hasta codificarlo; reservar y volver con `lseek(2)` permite escribir los bloques a medida que salen |

**Cuántos hilos.** Se usa `sysconf(_SC_NPROCESSORS_ONLN)`, el número de núcleos
disponibles, porque en trabajo limitado por CPU lanzar más hilos que núcleos solo añade
cambios de contexto. La variable de entorno `HILOS` lo sobrescribe (`HILOS=8 ./huf c ...`),
y no es un capricho: sin ella no se podría ejecutar la prueba de ausencia de carreras.

**El invariante que demuestra que no hay carreras.** El `.huf` sale **idéntico byte por
byte** con 1, 2, 3, 4, 7, 8 y 16 hilos:

```bash
HILOS=1 ./huf c entrada.txt a.huf
HILOS=8 ./huf c entrada.txt b.huf
cmp a.huf b.huf      # debe pasar en silencio
```

Se cumple porque la suma de frecuencias es asociativa, el árbol es determinista y el
escritor impone el orden. A diferencia de "ejecutarlo y ver que no falla", esta prueba
**no depende de la suerte**: si el resultado dependiera del número de hilos, fallaría
siempre. Como refuerzo, ThreadSanitizer (`-fsanitize=thread`) no reportó ninguna carrera
con 2, 4, 8 y 16 hilos sobre 5 MB.

La explicación línea por línea de cada módulo está en
[`README_compresor.md`](README_compresor.md).

### 10. Por qué el editor se congela mientras comprime

Después de una exhaustiva conversación sobre el funcionamiento de los hilos y su mejor
implementación con el profesor, se llegó a la conclusión de que la mejor implementación y
uso para los hilos del editor era que se congelara el editor mientras comprimía el
archivo, debido a que si se despertaba el hilo, había que hacer una copia del archivo,
comprimir esa copia y, mientras tanto, mantener sincronizados dos hilos que compartían
estado. Las razones completas son las siguientes.

**Cómo era la primera versión (en segundo plano).** `comp` lanzaba la compresión en un hilo
aparte, gestionado por un módulo `tarea_comp.c`, y devolvía el prompt de inmediato para que
el usuario siguiera editando. Funcionaba, pero obligaba a resolver una lista de problemas
que solo existían porque el editor seguía vivo:

| Problema del hilo de fondo | Qué había que construir para resolverlo |
| -------------------------- | --------------------------------------- |
| El usuario podía editar mientras se comprimía, y el compresor leería un documento a medio cambiar | Una **copia** del documento al lanzar la tarea, que el hilo de fondo usaba en exclusiva |
| El `.huf` no reflejaba los cambios hechos durante la compresión | Documentar que el resultado es una "foto" del momento del `comp` (como una cola de impresión) |
| El editor y el hilo leían y escribían el estado y el porcentaje al mismo tiempo | Un **estado compartido protegido con mutex** (`INACTIVA → CORRIENDO → OK / ERROR`) |
| El usuario no sabía cómo iba la tarea | Un comando extra, **`st`**, para consultar el porcentaje bajo mutex |
| Si el usuario salía con `q` con el hilo vivo, `buf_liberar` liberaba memoria que el hilo podía estar usando (*use-after-free*) | Prohibir `q` durante una tarea y un **`tarea_esperar()`** (`pthread_join`) en tres lugares distintos |
| El hilo imprimía "45 %" en medio de lo que el usuario estaba tecleando en `fgets` | Avisos solo cada 10 % por `stderr` y el mensaje final aplazado hasta la siguiente vuelta del bucle |
| Dos tareas a la vez multiplicaban los casos de error | Una regla de "solo una tarea a la vez" |

Cada fila es código, estado y una posible carrera. Y nada de eso hacía la compresión más
rápida: el cuello de botella son los núcleos del procesador, que la compresión ya usaba
todos.

**La versión actual (bloqueante).** El hilo principal llama directamente al compresor y se
queda dentro hasta que termina:

```
             ANTES (segundo plano)                         AHORA (bloqueante)
hilo editor  ──comp──► prompt ── a ── p ── st ── q?     ──comp──► [espera, dibuja barra] ──► prompt
                │      (edición concurrente)               │            ▲
hilo tarea      └──► copia ─► comprimir ─► estado+mutex    │            │ escribe bloques en orden
trabajadores        └─► conteo y codificación paralelos    └──► conteo y codificación paralelos
```

| | Segundo plano (descartado) | Bloqueante (actual) |
| --- | --- | --- |
| Hilos que comprimen | N trabajadores + 1 de tarea | N trabajadores (el que llama hace de escritor) |
| Copia del documento | para aislarlo de la edición | solo para entregar bytes contiguos |
| Estado compartido con mutex en el editor | sí | **no** |
| Comando `st` | necesario | **eliminado** |
| Restricción sobre `q` | sí, mientras hubiera tarea | **ninguna** |
| Riesgo de *use-after-free* al salir | sí, mitigado con `tarea_esperar` | **imposible**: cuando `comp` retorna, el trabajo terminó |
| El `.huf` refleja | el documento al momento de lanzar `comp` | el documento actual, siempre |
| Progreso | avisos cada 10 % mezclados con el prompt | barra de 30 celdas que se redibuja con `\r` |

Las razones de la decisión, en orden de peso:

1. **Desaparece la carrera con la edición.** Mientras `comp` corre, nadie puede ejecutar
   `a`, `i` ni `d`. El documento no cambia, así que no hay nada que proteger.
2. **La copia deja de ser una "foto" que haya que aislar.** El compresor sigue necesitando
   un bloque de bytes contiguos y el documento son descriptores de línea dispersos, así que
   `buf_serializar` todavía arma ese bloque en memoria (nunca en disco). Pero ahora es un
   simple formato de entrega que se libera al terminar, no un mecanismo de sincronización.
3. **Se elimina todo un módulo.** Desaparecieron `tarea_comp.c`, el comando `st`, el estado
   con mutex, la restricción de `q` y la espera final antes de liberar memoria. El código
   del editor es más corto y **no tiene carreras de datos propias**.
4. **La barra no necesita mutex.** El único hilo que la dibuja es el mismo que escribe los
   bloques en orden, así que nunca hay dos hilos tocándola a la vez y el porcentaje nunca
   retrocede.
5. **La salida en pantalla es limpia.** Como el usuario no está escribiendo, la barra no se
   mezcla con el texto del prompt.
6. **La concurrencia no se perdió.** Lo que se congela es el bucle de comandos del editor,
   no la compresión: el conteo y la codificación siguen repartidos entre todos los núcleos.
   La concurrencia está donde produce velocidad, y se quitó donde solo producía
   complejidad.

**El costo, dicho honestamente.** El requisito original hablaba de comprimir sin congelar
el editor, así que esta decisión se aparta de su lectura literal y puede afectar el criterio
de integración de la rúbrica. Con archivos muy grandes, el usuario espera sin poder editar.
Se acepta porque (a) los hilos trabajadores siguen siendo concurrentes, (b) la barra
mantiene al usuario informado en todo momento, y (c) para un editor que trabaja con
documentos que caben en RAM, la espera es de fracciones de segundo. La versión con hilo de
fondo se conserva en la rama `backup/antes-cabecera-dispersa` por si hubiera que volver a
ella.

**`desc` se trató igual.** Mantener el hilo de tarea solo para descomprimir habría
conservado toda la maquinaria anterior para un único comando, así que también es
bloqueante y también muestra barra.

**El costo se redujo después.** Al congelarse el editor, el usuario quedaba sin forma de
abortar una compresión larga. Eso se resolvió con la cancelación por Ctrl+C de la
decisión 11: hoy el usuario puede abortar la operación en cualquier momento.

### 11. Por qué Ctrl+C cancela en vez de matar el proceso

**El problema que creó la decisión 10.** Al presionar Ctrl+C, el kernel envía la señal
`SIGINT` al proceso, y si el programa no hace nada, la acción por defecto es **terminarlo
al instante**. Mientras `comp` corría eso tenía tres consecuencias:

| Qué se perdía | Por qué |
| ------------- | ------- |
| Los cambios sin guardar del documento | El proceso entero moría, editor incluido |
| Un `.huf` coherente | El archivo quedaba a medio escribir, con el índice en ceros |
| El cierre ordenado de los hilos | Los trabajadores quedaban cortados a mitad de un bloque |

Y como la decisión 10 congela el editor hasta que `comp` termina, el usuario no tenía
ninguna otra forma de abortar una compresión larga. El criterio de manejo de errores y
recursos de la rúbrica pide justo lo contrario: ni archivos parciales ni hilos sin cerrar.

**Cómo funciona, paso a paso.**

```
  usuario: Ctrl+C
        │
        ▼
1. kernel ──SIGINT──► manejador_sigint:   g_cancelar = 1        (y nada más)
                                                │
2. el escritor ordenado termina el bloque       │
   en curso y llama al callback de progreso ◄───┘
        │
3. barra_cb dibuja la barra y devuelve 1  ("detente")
        │
4. el escritor marca error = 1 y hace broadcast en las dos variables de condición
        │                                  (el MISMO camino que un error de escritura)
        ▼
5. los trabajadores ven error = 1 entre bloques y salen;
   los que dormían esperando espacio en la ventana se despiertan con el broadcast
        │
6. pthread_join de todos los hilos ──► la función devuelve HUF_CANCELADO (-2)
        │
7. comprimir_con_barra: unlink(salida.huf.tmp), libera la copia del documento,
   restaura el manejador anterior ──► "Compresión cancelada: ..." ──► prompt
```

**Por qué cada pieza es como es.**

1. **El manejador solo levanta una bandera.** Una señal puede llegar en cualquier
   instante, incluso a mitad de un `malloc` o con un mutex tomado. Si el manejador
   llamara a `printf` o a `free`, podría reentrar en una función que estaba a medio
   ejecutarse y corromper su estado. Casi ninguna función es segura dentro de un
   manejador; asignar una variable `volatile sig_atomic_t` sí lo es. `volatile` obliga al
   compilador a releerla de memoria cada vez, y `sig_atomic_t` garantiza que se escribe de
   una sola vez. Todo el trabajo real se hace después, fuera del manejador.

2. **El callback de progreso devuelve un entero para pedir la cancelación.** El tipo
   `HufProgreso` pasó de devolver `void` a devolver `int`. Así `comprimir.c` y
   `huf_formato.c` **no saben que existen las señales ni la bandera**: solo saben que el
   callback pidió parar. Es el mismo principio de todo el proyecto, los módulos de abajo
   no dependen del mundo exterior, y además el mismo mecanismo sirve para comprimir y
   para descomprimir sin duplicar código.

3. **La cancelación entra por el camino de error que ya existía.** El compresor ya sabía
   detenerse ante un `write` fallido: marcar `error`, despertar a todos con `broadcast` y
   unirlos sin interbloqueo. Ese camino estaba probado. Reutilizarlo en vez de inventar
   uno nuevo significa menos código nuevo y menos oportunidades de introducir una carrera.

4. **`HUF_CANCELADO` vale -2, no -1.** Así el editor distingue "el usuario canceló" de "el
   programa falló" y muestra el mensaje correcto, en vez de tratar una decisión del
   usuario como un error.

5. **`comp` escribe en `salida.huf.tmp` y solo al final hace `rename` a `salida.huf`.**
   Antes, `open` con `O_TRUNC` vaciaba `salida.huf` en el primer instante: si ya existía
   una versión anterior, se destruía aunque la compresión fallara después. Con el
   temporal, la salida anterior no se toca hasta el último momento, y `rename` la
   reemplaza de forma atómica. Es la misma estrategia de `w!`
   ([decisión 3](#3-por-qué-dos-estrategias-de-guardado)).

6. **El manejador existe solo mientras corre `comp` o `desc`.** Se instala antes de la
   operación y se restaura al terminar. Con el prompt esperando un comando, Ctrl+C
   conserva su significado de siempre.

7. **Se instala antes de crear el temporal, no después.** Si fuera al revés, habría un
   instante en que el temporal ya existe pero Ctrl+C todavía mata el proceso, y el
   archivo quedaría sin borrar.

8. **`sigaction` sin `SA_RESTART`.** `sigaction` tiene un comportamiento definido por el
   estándar; `signal` varía entre sistemas. Sin `SA_RESTART`, un `read` o un `write`
   bloqueado puede volver con `EINTR` al llegar la señal, y `escribir_todo` y `leer_todo`
   ya reintentan en ese caso (ver [Detalles de implementación](#write-no-escribe-todo-lo-que-le-pides)).

9. **`desc` también se puede cancelar.** Tiene el mismo problema, porque también congela
   el editor. Como la salida solo se escribe cuando la decodificación terminó completa,
   cancelar no deja nada en disco.

**Alternativas descartadas.**

| Alternativa | Por qué no |
| ----------- | ---------- |
| Hacer la limpieza dentro del manejador | No es seguro llamar a `unlink`, `free` o `printf` desde un manejador interrumpido en cualquier punto |
| Matar los hilos con `pthread_cancel` | Puede cortar a un hilo con un mutex tomado o con memoria sin liberar |
| Que `comprimir.c` lea la bandera global directamente | Acoplaría el compresor a las señales y no resolvería `desc` |
| Borrar el `.huf` parcial con `unlink`, sin temporal | Destruiría una salida anterior que ya existía |
| Pedir confirmación ("¿seguro que quieres cancelar?") | Un manejador no puede leer la entrada de forma segura, y complicaría el flujo |
| Dejar el manejador instalado siempre | Cambiaría el significado de Ctrl+C en todo el editor |

**Lo que todavía no es perfecto.**

- **La cancelación espera a que termine el bloque en curso.** Con bloques de 64 KB la
  respuesta es casi inmediata.
- **Durante el conteo de frecuencias, la bandera se atiende al terminar el primer bloque
  de la codificación,** porque el callback todavía no se ha llamado.
- **Si el usuario ya tenía un archivo llamado `salida.huf.tmp`, `comp` lo sobrescribe.**
  `w!` ya resolvió ese mismo problema con `mkstemp`; `comp` todavía no.

### 12. Qué cambió la auditoría y por qué se aplicó casi toda

**Qué es la auditoría.** Una revisión sistemática del código que busca fallos, riesgos y
mejoras antes de cambiar nada. Se hizo en una copia del proyecto: se ejecutaron todas las
pruebas, se comprimieron y descomprimieron archivos de tamaños límite con
AddressSanitizer y UBSan, se alteró un byte al azar en 300 archivos `.huf` válidos para
comprobar que el descompresor no se cuelga ni lee fuera de rango, y se midieron tiempos
sobre 64 MB. El compresor no tenía fallos; los problemas estaban en el editor y en el
rendimiento. El detalle completo está en [`AUDITORIA.md`](AUDITORIA.md).

| # | Problema encontrado | Corrección | Dónde se explica |
| - | ------------------- | ---------- | ---------------- |
| 1 | `comp ./notas.txt` destruía el archivo abierto | Comparar inodos con `stat`, no texto | [Protecciones](#protecciones-de-comp-y-desc) |
| 2 | `o` no podía abrir archivos de solo lectura | Reintentar con `O_RDONLY` ante `EACCES` o `EROFS` | Abajo |
| 3 | Se compilaba sin optimización | `-O2 -g` | [Compilar y ejecutar](#compilar-y-ejecutar) |
| 4 | `bw_bits` escribía un bit por iteración | Hasta 8 bits por iteración | [`README_compresor.md`](README_compresor.md) |
| 5 | La descompresión recorría el árbol bit a bit | Tabla de búsqueda de 12 bits | [`README_compresor.md`](README_compresor.md) |
| 6 | `p` hacía dos `write` por línea | Bloques de 64 KB | Abajo |
| 7 | La carga buscaba cada `\n` byte a byte | `memchr` | Abajo |
| 8 | Avisos del compilador con `-O2` | `escribir_stdout` y `mont_extraer` seguro | Abajo |
| 9 | Una línea de más de 4096 caracteres se partía y el resto se ejecutaba como comando | `leer_linea` con límite de 65 536 y aviso | [Comandos](#comandos-del-editor) |
| 10 | `atoi`: `d 2x` borraba la línea 2 | `strtoul` estricto | [Comandos](#comandos-del-editor) |
| 11 | `w!` perdía permisos y dueño, y pisaba un `.tmp` ajeno | `mkstemp`, `fchown`, `fchmod`, `fsync` del directorio | [Decisión 3](#3-por-qué-dos-estrategias-de-guardado) |
| 12 | La descompresión es secuencial | **No se aplicó** | Abajo |
| 13 | Fuga de memoria si `buf_cargar` fallaba a mitad | Liberar lo ya cargado en cada ruta de error | Abajo |

**Los puntos que no tienen sección propia:**

- **Abrir en solo lectura (2).** Leer un archivo no requiere permiso de escritura, pero
  `o` abría siempre con `O_RDWR`, y con un archivo de permisos `444` o un disco de solo
  lectura el usuario no podía ni ver el contenido. Ahora, si `open` falla con `EACCES` o
  `EROFS`, reintenta en solo lectura. El documento se puede leer y editar en memoria, y si
  el usuario intenta guardar, `w` informa el error real (`Permission denied`).
- **Imprimir por bloques (6).** `p` hacía, por cada línea, un `write` con el texto y otro
  con el salto. Un documento de un millón de líneas eran dos millones de entradas al
  kernel. Ahora las líneas se acumulan en un bloque de 64 KB que se vacía con un solo
  `write`; una línea más larga que el bloque se imprime aparte. Con un millón de líneas,
  de 0,31 s a 0,07 s, incluida la carga. Además el enunciado pide volcar el contenido con
  una llamada al sistema, no con una por línea.
- **`memchr` al cargar (7).** La biblioteca estándar implementa `memchr` con instrucciones
  vectoriales, que revisan muchos bytes a la vez. El resultado es idéntico, incluida la
  línea final sin salto.
- **Avisos del compilador (8).** El primero, el valor de retorno de `write` ignorado,
  escondía un fallo real: una escritura parcial perdía datos en silencio. Se resolvió con
  `escribir_stdout`, que reintenta igual que `escribir_todo`. El segundo, en
  `mont_extraer`, era un índice que podía salirse del arreglo con el montículo vacío. Hoy
  no es alcanzable, pero se reescribió para que no pueda ocurrir nunca, sin cambiar el
  orden de los elementos ni, por tanto, el formato.
- **La fuga en `buf_cargar` (13).** Si la memoria se agotaba a mitad de la carga, las
  líneas ya creadas quedaban sin liberar. Solo pasa con memoria agotada, pero se
  comprobó: con un `malloc` que falla a propósito bajo valgrind, la versión anterior
  perdía 70 251 bytes y la nueva pierde 0.

**Por qué no se aplicó el punto 12.** Descomprimir en paralelo es posible, porque cada
bloque es autónomo, pero no es una corrección sino una mejora, y es el cambio más grande
de la lista: habría que repetir toda la verificación con ThreadSanitizer. Tras los
cambios 3 y 5, descomprimir 64 MB ya toma unos 0,55 s, prácticamente lo mismo que
comprimir con 4 hilos (0,54 s). La ganancia no justificaba el riesgo.

**Por qué se confió en estos cambios.** Las 11 pruebas nuevas de `pruebas.sh` (permisos
`600` y `755` en `w!`, temporal ajeno no sobrescrito, números inválidos en `d`, `i` y `p`,
línea de 5000 caracteres guardada completa y línea de 70 000 rechazada sin ejecutar el
resto) **fallan con el código anterior y pasan con el nuevo**. Una prueba que pasa con
ambas versiones no demuestra que la corrección sirva para algo. Además, los cambios de
rendimiento producen un `.huf` **idéntico byte a byte** al de antes.

**Lo que se descartó en las correcciones.** Para las líneas largas se descartó `getline`,
que elimina el límite pero obligaba a reescribir `parse_line` sin un beneficio real.
65 536 caracteres es mucho más de lo que una persona escribe en una línea, y el aviso
evita el problema grave, que era ejecutar texto como comandos.

---

## Detalles de implementación que importan

Seis puntos que no son obvios y que son fuente frecuente de errores. Los cinco primeros
están en `buffer.c`; el último, en `comprimir.c`.

### `write()` no escribe todo lo que le pides

```c
static ssize_t escribir_todo(int fd, const char *buf, size_t n) {
    size_t hechos = 0;
    while (hechos < n) {
        ssize_t w = write(fd, buf + hechos, n - hechos);
        if (w == -1) {
            if (errno == EINTR) continue;   /* señal, no error: reintentar */
            return -1;
        }
        hechos += (size_t)w;
    }
    return (ssize_t)hechos;
}
```

POSIX permite que `write()` transfiera **menos bytes de los solicitados**, por una señal, por
disco lleno o por límites del sistema de archivos. Sin este bucle, escribir 800 de 1000 bytes
dejaría el archivo truncado y el editor reportaría éxito. `EINTR` se trata aparte porque no
es un fallo: significa que una señal interrumpió la llamada **antes de transferir nada**.
`leer_todo()` es el espejo, con `if (r == 0) break;` porque en `read` un 0 significa fin de
archivo, no error. El compresor (`huf_formato.c`) usa exactamente la misma pareja de
funciones.

### La capacidad se duplica, no crece de a uno

```c
size_t nueva = b->cap ? b->cap : 16;
while (nueva < necesarias) nueva *= 2;

Linea *tmp = realloc(b->lineas, nueva * sizeof(Linea));
if (tmp == NULL) { perror("realloc"); return -1; }
b->lineas = tmp;
```

Creciendo de a una posición, agregar *n* líneas costaría *n* reallocs con posible copia
completa: O(n²). Duplicando, son log₂(n) reallocs y el costo se reparte: **O(1) amortizado**
por línea.

La variable `tmp` no es cosmética. Si `realloc` falla devuelve `NULL` **sin liberar el bloque
anterior**; escribir `b->lineas = realloc(b->lineas, ...)` habría machacado el único puntero
al arreglo viejo, perdiendo el documento completo y filtrando la memoria.

### `memmove`, nunca `memcpy`

```c
memmove(&b->lineas[n - 1], &b->lineas[n], (b->num - n) * sizeof(Linea));
```

`memcpy` asume que origen y destino **no se solapan** y puede copiar en cualquier orden. Aquí
sí se solapan: se mueve `lineas[2..3]` sobre `lineas[1..2]`. Usar `memcpy` es comportamiento
indefinido, y lo peligroso es que suele *parecer* correcto hasta que cambia el nivel de
optimización del compilador. El solapamiento es igual de real en `i`, donde el
desplazamiento va en sentido contrario.

### El orden de los `free()`

```c
free(b->lineas[n - 1].texto);   /* PRIMERO el texto de la línea */
memmove(...);                   /* después se desplazan los descriptores */
```

Una vez desplazados los descriptores, la dirección del texto borrado ya no está en ninguna
parte: sería una fuga irrecuperable. Lo mismo aplica en `buf_liberar()`, que libera cada
`texto` antes que el arreglo `lineas`.

### El `\n` final que el usuario no escribió

Un archivo puede terminar o no en salto de línea, y **eso es contenido del usuario**. El
buffer registra el hecho al cargar:

```c
if (leidos > 0 && inicio < (size_t)leidos) {
    /* quedaron bytes tras el último '\n': son una línea válida */
    b->newline_final = 0;
}
```

Sin este flag, abrir un archivo sin salto final y guardarlo sin editar nada le agregaría un
byte. Un editor que modifica lo que no se le pidió modificar está roto, aunque el cambio
parezca inofensivo. Cuando el usuario agrega o inserta una línea, el flag vuelve a 1: a
partir de ese momento el documento termina en una línea que escribió el usuario.

### `while`, nunca `if`, alrededor de `pthread_cond_wait`

```c
pthread_mutex_lock(&t.mtx);
while (!t.listo[b] && !t.error)
    pthread_cond_wait(&t.cond_listo, &t.mtx);   /* DUERME: 0 % de CPU */
pthread_mutex_unlock(&t.mtx);
```

Dos razones. El escritor se despierta con `broadcast` cada vez que **cualquier** bloque
termina, no necesariamente el que él espera; con un `if`, saldría y escribiría un bloque que
no está listo. Además POSIX permite **despertares espurios**: `pthread_cond_wait` puede
retornar sin que nadie haya señalado nada. La regla es absoluta: la condición se vuelve a
evaluar en un `while`.

El mismo cuidado aplica al manejo de errores: el bucle de `pthread_join` usa el contador
`creados`, no `n_hilos`. Si la creación del hilo 3 falla, los hilos 1 y 2 ya están vivos y
usando memoria que la función está a punto de liberar; unirse a ellos es obligatorio
incluso en la ruta de error.

---

## Estructura del proyecto

```
Editor-de-texto--Sistemas-Operativos/
├── Makefile            # objetivos: all (eafitOS + huf), run, test, clean
├── README.md           # este documento
├── README_compresor.md # documentación técnica del compresor, módulo por módulo
├── AVANCES.md          # bitácora de cambios, decisiones y pendientes
├── AUDITORIA.md        # revisión del código: 13 puntos, problema, corrección y medición
│
│   ── SHELL ──────────────────────────────────────────────────────────────
├── shell.h             # macros de color, LOG_SYSCALL, struct Command, prototipos
├── main.c              # REPL principal, tabla de comandos, parse_line(), help
├── cat_datos.c         # d_create, d_read, d_info, d_copy, d_append
├── cat_memoria.c       # m_sbrk, m_mmap, m_info
├── cat_monitoreo.c     # p_fork, p_exec, p_kill, p_monitor
├── cat_listado.c       # l_dir
├── cat_util.c          # saludar, despedir, hora, fecha
│
│   ── EDITOR (parcial 1) ─────────────────────────────────────────────────
├── buffer.h            # EL CONTRATO: tipos del buffer y API pública
├── buffer.c            # LA ESTRUCTURA DE DATOS + todas las syscalls del editor
├── cat_editor.c        # EL EDITOR: parser, despachador, barra y manejador de Ctrl+C
│
│   ── COMPRESOR (parcial 2) ──────────────────────────────────────────────
├── bitio.h / bitio.c             # capa 0: bits ↔ bytes
├── huffman.h / huffman.c         # capa 1: árbol y tabla de códigos
├── huf_formato.h / huf_formato.c # capa 2: formato HUF2 y syscalls de archivo
├── comprimir.h / comprimir.c     # capa 3: hilos (ÚNICO archivo con pthread)
├── huf_main.c                    # binario independiente ./huf
│
│   ── PRUEBAS ────────────────────────────────────────────────────────────
├── test_bitio.c        # 18 pruebas unitarias de bitio
├── test_huffman.c      # 23 pruebas unitarias de huffman
├── pruebas.sh          # 32 pruebas del editor, incluida la cancelación con Ctrl+C
├── pruebas_huf.sh      # 23 pruebas de compresión real (md5sum)
├── pruebas_hilos.sh    # 6 pruebas del invariante de concurrencia
│
│   ── HERRAMIENTAS ───────────────────────────────────────────────────────
├── CLAUDE.md           # indica consultar el grafo antes de leer el código
└── graphify-out/       # grafo de conocimiento del proyecto (GRAPH_REPORT.md, graph.html)
```

**La separación `buffer.c` / `cat_editor.c` es deliberada.** `cat_editor.c` no contiene ni
una llamada al sistema sobre el archivo del documento: es parser, despachador y la barra de
progreso. Toda la manipulación de datos y el acceso a disco del documento están en
`buffer.c`. Esto permite que el módulo del buffer se pruebe de forma independiente y que dos
personas trabajen en paralelo sin tocar los mismos archivos: `buffer.h` es la frontera
contractual entre ambos.

**La excepción, y por qué se acepta.** Con el compresor, `cat_editor.c` pasó a hacer
syscalls propias: en `comprimir_con_barra()`, `open` del temporal `salida.huf.tmp`, `lseek`
para leer su tamaño final, `close`, y después `rename` si todo salió bien o `unlink` si
falló o se canceló; además `sigaction` para instalar y restaurar el manejador de Ctrl+C,
y `stat` en `mismo_archivo()` para comparar inodos. La frontera original decía "cero
syscalls de archivo en `cat_editor.c`" y ya no se cumple al pie de la letra. Se aceptó
porque ninguna de esas llamadas toca el documento: todas trabajan sobre el archivo de
salida o sobre la señal, y moverlas a `buffer.c` habría obligado al buffer a conocer el
compresor. Si se quisiera restaurar la regla completa, el lugar natural sería una función
envoltorio en `huf_formato.c`.

**El compresor repite la misma idea en vertical.** Cada capa se comunica con la de abajo
solo a través de su `.h`. `cat_editor.c` solo conoce `comprimir.h` y `huf_formato.h`: no
sabe cómo se cuentan las frecuencias ni cuántos hilos hay.

```
cat_editor.c ──► comprimir.h ──► huf_formato.h ──► huffman.h ──► bitio.h
  (editor)       (hilos)         (formato/disco)   (árbol)       (bits)
```

**El grafo de conocimiento.** `graphify-out/` contiene un grafo del proyecto (funciones,
archivos, conceptos y sus relaciones) generado con la herramienta graphify. Sirve para
orientarse sin releer todo el código: por ejemplo, confirma que `cmd_editor()` es la función
con más conexiones del proyecto, porque es el punto donde el editor y el compresor se
encuentran. La última reconstrucción, después de la auditoría, tiene 306 nodos y 681
aristas. Después de cambiar código se actualiza con `graphify update . --force`: sin
`--force`, la actualización normal a veces conserva nodos de archivos ya borrados. Los
documentos Markdown, incluido este, necesitan una extracción semántica completa para
entrar al grafo. `.graphifyignore` excluye del grafo los `.zip`, `.pdf`, `.huf` y los
archivos de prueba manual.

---

## Pruebas

```bash
./pruebas.sh              # 32 casos del editor, incluida la cancelación
./pruebas.sh --valgrind   # 33: añade verificación de fugas de memoria
./pruebas_huf.sh          # 23 casos de compresión real
./pruebas_hilos.sh        # 6 casos del invariante de concurrencia + tiempos
make test                 # 18 + 23 pruebas unitarias
```

| Suite | Qué valida | Resultado |
| ----- | ---------- | --------- |
| `pruebas.sh` | Los comandos del editor, la cancelación y las correcciones de la auditoría | **32 / 32** (33 / 33 con `--valgrind`) |
| `test_bitio` | Empaquetado y desempaquetado de bits | **18 / 18** |
| `test_huffman` | Árbol, códigos, determinismo, casos borde | **23 / 23** |
| `pruebas_huf.sh` | Comprimir → descomprimir → comparar con `md5sum` | **23 / 23** |
| `pruebas_hilos.sh` | Mismo `.huf` con 1 a 16 hilos | **6 / 6** |

### `pruebas.sh`: el editor

**Comandos base**: `a` agrega al final; `d` borra la primera, una del medio y la última
línea; `p n` imprime la línea correcta.

**Casos de borde**: archivo cuya última línea no termina en `\n` (guardar no le agrega uno);
archivo inexistente (`o` lo crea); archivo vacío (`p` no rompe); borrar todas las líneas
(`ftruncate` deja 0 bytes, sin bytes fantasma); **el disco permanece intacto si no se ejecuta
`w`**; `q` advierte con cambios pendientes; `w!` produce el mismo resultado que `w` y no deja
el `.tmp` huérfano.

**Manejo de errores**: línea fuera de rango; comandos sin archivo abierto; argumentos
faltantes, no numéricos o negativos; comando desconocido.

**Cancelación (3 pruebas).** Comprimen un archivo de unos 80 MB con un solo hilo, para que
la compresión dure lo suficiente, y envían un `SIGINT` real en cuanto aparece el archivo
temporal. Comprueban que sale el aviso de cancelación, que el editor sigue vivo y responde
a `q`, y que no queda ni el `.huf` ni el temporal. La sesión del editor se alimenta por una
tubería con nombre (`mkfifo`) para poder seguir enviándole comandos después de la señal:
con una entrada normal, el editor habría leído todos los comandos de una vez.

**Auditoría (11 pruebas).** Permisos `600` y `755` conservados por `w!`; un `.tmp` ajeno
que `w!` no sobrescribe; números inválidos en `d`, `i` y `p`; una línea de 5000
caracteres guardada completa; y una de 70 000 rechazada sin ejecutar el sobrante como
comando. Todas **fallan con el código anterior a la auditoría y pasan con el actual**: es
la evidencia de que cada corrección arregla algo real.

| Grupo | Pruebas |
| ----- | ------- |
| Comandos base, casos de borde y errores | 18 |
| Cancelación con Ctrl+C | 3 |
| Correcciones de la auditoría | 11 |
| **Total** | **32** (+1 de fugas con `--valgrind`) |

### `test_bitio` y `test_huffman`: las capas bajas

No son pruebas decorativas; cada una atrapa un error concreto. Algunas de las más
importantes:

| Prueba | Error que atrapa |
| ------ | ---------------- |
| Los 8 bits `10110010` dan el byte `0xB2` | La convención de bit más significativo primero está invertida |
| 3 bits escritos dan `total_bits = 3`, no 8 | El relleno del último byte se cuenta como contenido |
| 3 bloques independientes se decodifican por separado | Los bloques no son autónomos (y los hilos no podrían funcionar) |
| Propiedad de prefijo en 200 árboles aleatorios | Algún código es prefijo de otro y la decodificación sería ambigua |
| Igualdad de Kraft | El árbol desperdicia patrones de bits |
| Dos construcciones con empates dan tablas idénticas | El árbol no es determinista: el descompresor reconstruiría otro árbol |
| Contar por bloques y sumar = contar de una pasada | La reducción local de los hilos daría frecuencias distintas |

### `pruebas_huf.sh`: compresión real

La verificación central es siempre la misma: comprimir, descomprimir y comparar contra el
original. Cubre contenido típico (texto repetitivo, texto natural, binario aleatorio),
casos borde (archivo vacío, un solo byte, un solo símbolo repetido, los 256 valores de
byte, las fronteras de bloque en 65 535 / 65 536 / 65 537 bytes, varios bloques completos,
bytes nulos) y el rechazo de entradas inválidas (archivo que no es `.huf`, `.huf` truncado o
corrupto, archivo inexistente).

Siete de las 23 pruebas se añadieron con HUF2 y atacan directamente la cabecera dispersa.
Dos miden el beneficio: un archivo de 660 bytes queda en 378 y el archivo vacío ocupa solo
los 26 bytes de la cabecera fija. Las otras cinco comprueban que el lector rechaza
`n_simbolos` mayor que 256, símbolos duplicados, frecuencias que no suman el tamaño
original, una tabla truncada y el formato anterior HUF1.

### `pruebas_hilos.sh`: ausencia de carreras

Comprueba que el `.huf` sale idéntico con 1, 2, 3, 4, 7, 8 y 16 hilos sobre seis tipos de
archivo, y que además se recupera el original. Es la única evidencia objetiva de ausencia
de condiciones de carrera que un evaluador puede reproducir, porque las carreras no se ven
leyendo el código ni ejecutándolo una vez. También imprime la tabla de tiempos de la fase
de conteo con 1, 2, 4 y 8 hilos. En el equipo de desarrollo, con 18 núcleos y un archivo de
20 MB:

| Hilos | Conteo de frecuencias | Aceleración |
| ----- | --------------------- | ----------- |
| 1 | 0,0226 s | 1,0× |
| 2 | 0,0119 s | 1,9× |
| 4 | 0,0072 s | **3,1×** |
| 8 | 0,0095 s | 2,4× |

Con 8 hilos es **más lento** que con 4. No es un error: el conteo de 20 MB dura
centésimas de segundo, y crear y unir cada hilo tiene un costo fijo que no se reparte. Con
8 hilos cada uno cuenta solo 2,5 MB, y ese costo fijo ya pesa más que el trabajo que se
ahorra. Añadir hilos solo ayuda mientras cada uno tenga trabajo suficiente. Esta tabla mide solo el conteo, no la
compresión completa.

### Verificación de memoria y de hilos

| Herramienta | Escenario | Resultado |
| ----------- | --------- | --------- |
| valgrind | `./pruebas.sh --valgrind` | 33 / 33, sin fugas |
| valgrind | Sesión con `o`, `a`, `comp` y `desc` sobre 400 KB con 4 hilos | 0 bytes en uso al salir, 0 errores, ida y vuelta correcta |
| valgrind | `comp` cancelado sobre 24 MB con 2 hilos | 0 bytes en uso, 0 errores, salida previa intacta, sin temporal |
| valgrind | `buf_cargar` con un `malloc` que falla a propósito | Antes de la auditoría se perdían 70 251 bytes; ahora 0 |
| ThreadSanitizer | `comp` cancelado sobre 40 MB con 4 hilos y una salida previa existente | 0 advertencias; la salida previa quedó idéntica byte a byte |
| ThreadSanitizer | `comp` de 40 MB con 8 hilos, sin cancelar | 0 advertencias |
| ThreadSanitizer | `desc` cancelado | 0 advertencias |
| AddressSanitizer + UBSan | Todas las suites después de la auditoría | Sin errores |

La compilación usa `-Wall -Wextra` y no produce warnings.

---

## Limitaciones conocidas

Documentadas explícitamente porque conocer los límites de una decisión es parte de haberla
tomado bien.

### Del editor

**El `!` tiene dos significados distintos.** En `q!` significa "forzar, ignorando la
advertencia"; en `w!` significa "usar la otra estrategia de escritura". Es una inconsistencia
de nomenclatura detectada después de implementar ambos comandos. La corrección propuesta es
renombrar `w!` a `wa` ("write atómico"), dejando el `!` con un único significado en todo el
editor.

**`a` sin comillas guarda solo la primera palabra.** `a hola mundo` agrega `hola`, porque
el tokenizador entrega `mundo` como un tercer argumento que `a` no usa. Lo correcto es
escribir `a "hola mundo"` o usar el modo párrafo. Una mejora sería unir todos los argumentos
restantes, o rechazar el comando cuando sobran.

**La búsqueda es de subcadena y sensible a mayúsculas.** `s sol` encuentra `consola` y no
encuentra `Sol`. Una búsqueda por palabra completa o sin distinguir mayúsculas requeriría
comparar con `strcasestr` o revisar los bordes de cada coincidencia.

**El modo atómico rompe los enlaces duros.** Desde la auditoría, `w!` conserva permisos,
dueño y grupo. Pero como `rename()` deja un inodo nuevo en el lugar del viejo, otro nombre
que apuntara al mismo archivo (un enlace duro) sigue viendo el contenido viejo, y los
atributos extendidos no se copian. Es inherente a la estrategia de temporal más
`rename`; `w` sí los conserva porque escribe sobre el mismo inodo.

**El documento completo vive en RAM.** Un archivo más grande que la memoria disponible no se
puede abrir ni comprimir desde el editor. Los editores que manejan archivos arbitrariamente
grandes cargan por demanda (*mmap* o paginación manual), lo cual está fuera del alcance de
este proyecto.

**Sin undo.** El arreglo de descriptores no guarda historial. Añadirlo requeriría snapshots
del arreglo o migrar a un *piece table*, donde los buffers de texto son inmutables y deshacer
cuesta solo restaurar la lista de piezas anterior.

**Sin control de concurrencia entre procesos.** Si otro proceso modifica el archivo mientras
está cargado en memoria, `w` sobrescribe esos cambios sin avisar. Detectarlo requeriría
comparar el `mtime` del inodo antes de guardar, o bloqueo con `fcntl(F_SETLK)`.

### Del compresor

**El editor no responde mientras comprime.** Es la consecuencia directa de la decisión 10.
Con documentos que caben en RAM la espera es corta, y desde la decisión 11 el usuario puede
cancelar con Ctrl+C, pero no puede seguir editando mientras tanto.

**La cancelación no es instantánea.** Se atiende al terminar el bloque de 64 KB en curso,
lo que en la práctica es casi inmediato. Si Ctrl+C llega durante el conteo de
frecuencias, se atiende al terminar el primer bloque de la codificación, porque hasta ese
momento no se ha llamado al callback.

**`comp` sobrescribe un `salida.huf.tmp` que ya existiera.** `w!` resolvió ese problema con
`mkstemp`, pero el temporal de `comp` todavía tiene un nombre fijo. La corrección sería la
misma: crear el temporal con `mkstemp` en el directorio de la salida.

**La cancelación de `desc` no se ha probado bajo valgrind.** Sí se probó con
ThreadSanitizer, y la cancelación de `comp` sí pasó por valgrind.

**Los archivos diminutos todavía crecen.** HUF2 resolvió el caso de los archivos de
cientos de bytes, pero un texto de unas pocas decenas de bytes sigue saliendo más grande
(38 → 187 bytes en la sesión de ejemplo), porque cada símbolo distinto cuesta 9 bytes de
cabecera. Los códigos canónicos, que guardan por cada símbolo solo el byte y la longitud de
su código (2 bytes) en vez del byte y su frecuencia (9 bytes), reducirían la tabla a menos
de la cuarta parte.

**La descompresión es secuencial.** El formato ya lo permitiría (cada bloque es autónomo y
el índice da su posición). Se decidió no implementarla en la auditoría (ver la
[decisión 12](#12-qué-cambió-la-auditoría-y-por-qué-se-aplicó-casi-toda)): con la tabla
de búsqueda, descomprimir 64 MB ya toma unos 0,55 s.

**Formatos incompatibles.** Los `.huf` creados con el formato HUF1 ya no se pueden abrir.

---

## Estado actual y pendientes

**Estado del repositorio.** Todo está en la rama `main` (último commit `f265de1`): el
editor, el compresor con hilos, los comandos bloqueantes con barra, el formato HUF2, la
cancelación con Ctrl+C y la auditoría. Las ramas `feat/comp-bloqueante-barra`,
`feat/cabecera-dispersa`, `feat/cancelacion` y `feat/auditoria` ya están fusionadas y
pueden borrarse. La rama `backup/antes-cabecera-dispersa` y la etiqueta
`backup-hasta-cabecera-fija` guardan el estado anterior a HUF2, con la versión del hilo
de fondo. No se ha hecho push a GitHub: todo es local. `AVANCES.md`, `AUDITORIA.md`,
`CLAUDE.md` y `graphify-out/` no están versionados a propósito.

```
main ──●──────●──────────●────────────●───────────● f265de1
       │      │          │            │           │
   editor   comp/desc   HUF2     cancelación   auditoría
            bloqueante              Ctrl+C     (1–11, 13)
```

| Prioridad | Pendiente | Por qué importa |
| --------- | --------- | --------------- |
| 1 | Regenerar el PDF de `README_compresor.md` | Que el PDF coincida con el Markdown actualizado |
| 2 | Grabar el video de 5 a 7 minutos | Entregable de la rúbrica. Debe incluir, en una frase, por qué el editor se bloquea mientras comprime y cómo se cancela |
| 3 | Confirmar con el equipo la modalidad de entrega y cuál carpeta es la oficial | Hay otras copias del proyecto fuera de este repositorio |
| 4 | Opcional: medir la compresión completa, no solo el conteo, con 1, 2, 4 y 8 hilos | Reforzar en la sustentación el argumento de concurrencia |
| 5 | Opcional: borrar las ramas fusionadas y decidir si se hace push a GitHub | Ordenar el repositorio |
| 6 | Mejoras opcionales: `mkstemp` para el temporal de `comp`, cancelación de `desc` bajo valgrind, `w!` → `wa`, descompresión paralela, códigos canónicos | Ver [Limitaciones](#limitaciones-conocidas) |
