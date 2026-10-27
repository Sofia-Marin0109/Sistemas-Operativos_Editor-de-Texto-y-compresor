#!/bin/bash
# ====================================================================================
# Script de pruebas del editor de texto -- entregable 2 del enunciado.
# Valida los comandos base (o, p, a, d, q) y los casos borde del modelo en memoria.
#
#   ./pruebas.sh            ejecuta la batería completa
#   ./pruebas.sh --valgrind ejecuta además la verificación de fugas de memoria
# ====================================================================================

set -u
BIN=./eafitOS
DIR=$(mktemp -d)
OK=0
FALLOS=0

VERDE='\033[1;32m'; ROJO='\033[1;31m'; GRIS='\033[0;90m'; NC='\033[0m'

# Ejecuta una sesión del editor. $1 = guion de comandos, resto = ignorado.
correr() {
    printf '%b' "$1" | $BIN > "$DIR/salida.log" 2>&1
}

# verificar <descripcion> <archivo> <contenido esperado>
verificar() {
    local desc="$1" archivo="$2" esperado="$3"
    local real
    real=$(cat "$archivo" 2>/dev/null)
    if [ "$real" == "$esperado" ]; then
        printf "${VERDE}  PASA${NC}  %s\n" "$desc"
        OK=$((OK+1))
    else
        printf "${ROJO}  FALLA${NC} %s\n" "$desc"
        printf "${GRIS}         esperado: %q\n         obtenido: %q${NC}\n" "$esperado" "$real"
        FALLOS=$((FALLOS+1))
    fi
}

# verificar_salida <descripcion> <patron esperado en la salida>
verificar_salida() {
    local desc="$1" patron="$2"
    if grep -q "$patron" "$DIR/salida.log"; then
        printf "${VERDE}  PASA${NC}  %s\n" "$desc"
        OK=$((OK+1))
    else
        printf "${ROJO}  FALLA${NC} %s (no se encontró: %s)\n" "$desc" "$patron"
        FALLOS=$((FALLOS+1))
    fi
}

echo
echo "=== Pruebas del editor de texto ==="
echo "Directorio temporal: $DIR"
echo

# ------------------------------------------------------------------ 1. Básicos
echo "-- Comandos base --"

printf 'uno\ndos\ntres\n' > "$DIR/a.txt"
correr "editor\no $DIR/a.txt\na \"cuatro\"\nw\nq\nexit\n"
verificar "a: agrega al final" "$DIR/a.txt" $'uno\ndos\ntres\ncuatro'

printf 'uno\ndos\ntres\n' > "$DIR/b.txt"
correr "editor\no $DIR/b.txt\nd 2\nw\nq\nexit\n"
verificar "d: borra línea del medio" "$DIR/b.txt" $'uno\ntres'

printf 'uno\ndos\ntres\n' > "$DIR/c.txt"
correr "editor\no $DIR/c.txt\nd 1\nw\nq\nexit\n"
verificar "d: borra la primera línea" "$DIR/c.txt" $'dos\ntres'

printf 'uno\ndos\ntres\n' > "$DIR/d.txt"
correr "editor\no $DIR/d.txt\nd 3\nw\nq\nexit\n"
verificar "d: borra la última línea" "$DIR/d.txt" $'uno\ndos'

printf 'uno\ndos\ntres\n' > "$DIR/e.txt"
correr "editor\no $DIR/e.txt\np 2\nq\nexit\n"
verificar_salida "p n: imprime la línea pedida" "dos$"

# ------------------------------------------------- 2. Casos borde del documento
echo
echo "-- Casos borde --"

# Archivo cuya última línea NO termina en '\n': el editor debe conservarlo así.
printf 'uno\ndos' > "$DIR/f.txt"
correr "editor\no $DIR/f.txt\nw\nq\nexit\n"
if [ "$(od -c "$DIR/f.txt" | grep -c 'o\s*$')" -ge 0 ] && [ "$(tail -c1 "$DIR/f.txt" | od -An -c | tr -d ' \n')" != "\\n" ]; then
    printf "${VERDE}  PASA${NC}  guardar no agrega un '\\\\n' que no existía\n"; OK=$((OK+1))
else
    printf "${ROJO}  FALLA${NC} guardar agregó un '\\\\n' inexistente\n"; FALLOS=$((FALLOS+1))
fi

# Archivo inexistente: 'o' debe crearlo vacío, no fallar.
rm -f "$DIR/nuevo.txt"
correr "editor\no $DIR/nuevo.txt\na \"linea\"\nw\nq\nexit\n"
verificar "o: crea el archivo si no existe" "$DIR/nuevo.txt" "linea"

# Archivo vacío: 'p' no debe romper ni imprimir basura.
: > "$DIR/vacio.txt"
correr "editor\no $DIR/vacio.txt\np\nq\nexit\n"
verificar_salida "p sobre archivo vacío" "documento vacío"

# Borrar todas las líneas debe dejar el archivo en 0 bytes (prueba de ftruncate).
printf 'uno\ndos\n' > "$DIR/g.txt"
correr "editor\no $DIR/g.txt\nd 1\nd 1\nw\nq\nexit\n"
if [ ! -s "$DIR/g.txt" ]; then
    printf "${VERDE}  PASA${NC}  ftruncate deja el archivo en 0 bytes\n"; OK=$((OK+1))
else
    printf "${ROJO}  FALLA${NC} quedaron bytes fantasma tras borrar todo\n"; FALLOS=$((FALLOS+1))
fi

# El disco NO debe cambiar hasta ejecutar 'w'.
printf 'original\n' > "$DIR/h.txt"
correr "editor\no $DIR/h.txt\na \"agregada\"\nd 1\nq!\nexit\n"
verificar "sin 'w', el disco queda intacto" "$DIR/h.txt" "original"

# 'q' con cambios pendientes debe advertir en vez de salir en silencio.
printf 'original\n' > "$DIR/i.txt"
correr "editor\no $DIR/i.txt\na \"x\"\nq\nq!\nexit\n"
verificar_salida "q avisa si hay cambios sin guardar" "cambios sin guardar"

# Guardado atómico: mismo resultado que el guardado en sitio.
printf 'uno\ndos\n' > "$DIR/j.txt"
correr "editor\no $DIR/j.txt\na \"tres\"\nw!\nq\nexit\n"
verificar "w!: guardado atómico correcto" "$DIR/j.txt" $'uno\ndos\ntres'
if ! ls "$DIR"/j.txt.* >/dev/null 2>&1; then
    printf "${VERDE}  PASA${NC}  w! no deja el archivo temporal atrás\n"; OK=$((OK+1))
else
    printf "${ROJO}  FALLA${NC} quedó un temporal huérfano\n"; FALLOS=$((FALLOS+1))
fi

# comprobar <descripción> <comando de prueba...>: pasa si el comando devuelve 0.
comprobar() {
    local desc="$1"; shift
    if "$@"; then
        printf "${VERDE}  PASA${NC}  %s\n" "$desc"; OK=$((OK+1))
    else
        printf "${ROJO}  FALLA${NC} %s\n" "$desc"; FALLOS=$((FALLOS+1))
    fi
}

# w! debe conservar los permisos del original (antes dejaba todo en 0644).
printf 'a\n' > "$DIR/p600.txt"; chmod 600 "$DIR/p600.txt"
correr "editor\no $DIR/p600.txt\na \"b\"\nw!\nq\nexit\n"
comprobar "w! conserva los permisos 600" test "$(stat -c %a "$DIR/p600.txt")" = "600"

printf 'a\n' > "$DIR/p755.txt"; chmod 755 "$DIR/p755.txt"
correr "editor\no $DIR/p755.txt\na \"b\"\nw!\nq\nexit\n"
comprobar "w! conserva el bit de ejecución (755)" test "$(stat -c %a "$DIR/p755.txt")" = "755"

# w! no debe pisar un archivo ajeno que se llame 'nombre.tmp'.
printf 'a\n' > "$DIR/ajeno.txt"; printf 'no me toques\n' > "$DIR/ajeno.txt.tmp"
correr "editor\no $DIR/ajeno.txt\na \"b\"\nw!\nq\nexit\n"
comprobar "w! no pisa un .tmp ajeno" test "$(cat "$DIR/ajeno.txt.tmp")" = "no me toques"

# ------------------------------------------------------------- 3. Robustez
echo
echo "-- Manejo de errores --"

printf 'uno\n' > "$DIR/k.txt"
correr "editor\no $DIR/k.txt\np 99\nq\nexit\n"
verificar_salida "p con línea fuera de rango" "no existe"

correr "editor\np\nq\nexit\n"
verificar_salida "p sin archivo abierto" "No hay archivo abierto"

correr "editor\no\nq\nexit\n"
verificar_salida "o sin argumento" "Uso: o"

printf 'uno\n' > "$DIR/l.txt"
correr "editor\no $DIR/l.txt\nd abc\nd -3\nq\nexit\n"
verificar_salida "d con argumento no numérico o negativo" "entero positivo"

correr "editor\nzzz\nq\nexit\n"
verificar_salida "comando desconocido" "no reconocido"

# Números de línea: solo dígitos, positivos y que quepan (antes atoi aceptaba "2x").
printf 'uno\ndos\ntres\n' > "$DIR/num.txt"
correr "editor\no $DIR/num.txt\nd 2x\nd 99999999999999999999\nd 0\nd +2\nw\nq\nexit\n"
verificar "d rechaza basura, números enormes, cero y signo" "$DIR/num.txt" $'uno\ndos\ntres'

correr "editor\no $DIR/num.txt\ni 2x \"x\"\ni abc \"x\"\nw\nq\nexit\n"
verificar "i rechaza números inválidos" "$DIR/num.txt" $'uno\ndos\ntres'

correr "editor\no $DIR/num.txt\np 1x\nq\nexit\n"
verificar_salida "p rechaza un número con basura" "entero positivo"

correr "editor\no $DIR/num.txt\nd 2\nw\nq\nexit\n"
verificar "d con un número válido sigue funcionando" "$DIR/num.txt" $'uno\ntres'

# Líneas largas: antes se partían o se truncaban en silencio.
LARGO=$(head -c 5000 /dev/zero | tr '\0' 'x')
printf 'uno\n' > "$DIR/largo.txt"
correr "editor\no $DIR/largo.txt\na \"$LARGO\"\nw\nq\nexit\n"
comprobar "una línea de 5000 caracteres se guarda completa" \
    test "$(tail -n 1 "$DIR/largo.txt" | tr -d '\n' | wc -c)" = "5000"

MUY_LARGO=$(head -c 70000 /dev/zero | tr '\0' 'y')
printf 'uno\n' > "$DIR/muylargo.txt"
correr "editor\no $DIR/muylargo.txt\na \"$MUY_LARGO\"\na \"final\"\nw\nq\nexit\n"
verificar_salida "una línea desmesurada se rechaza con aviso" "demasiado largo"
verificar "tras rechazarla, lo que sigue no se ejecuta como comando" "$DIR/muylargo.txt" $'uno\nfinal'
comprobar "el resto de la línea larga no genera comandos desconocidos" \
    bash -c "! grep -q 'no reconocido' '$DIR/salida.log'"

# ------------------------------------------------------------- 3b. Cancelación con Ctrl+C
echo
echo "-- Cancelación con Ctrl+C --"
# Se comprime un archivo grande con un solo hilo (para que dure) y se envía SIGINT en
# cuanto aparece el temporal. La sesión se alimenta por una tubería con nombre para poder
# seguir escribiéndole comandos después de la señal.
head -c 60000000 /dev/urandom | base64 -w 76 > "$DIR/grande.txt"
mkfifo "$DIR/in"
HILOS=1 $BIN < "$DIR/in" > "$DIR/cancel.log" 2>&1 &
PID_EDITOR=$!
exec 3> "$DIR/in"
printf 'editor\no %s\ncomp %s\n' "$DIR/grande.txt" "$DIR/grande.huf" >&3
for _ in $(seq 1 300); do [ -e "$DIR/grande.huf.tmp" ] && break; sleep 0.1; done
kill -INT "$PID_EDITOR"
sleep 1
printf 'q\nexit\n' >&3
exec 3>&-
wait "$PID_EDITOR"

verificar_en() {   # verificar_en <descripción> <patrón> <archivo>
    if grep -q "$2" "$3"; then
        printf "${VERDE}  PASA${NC}  %s\n" "$1"; OK=$((OK+1))
    else
        printf "${ROJO}  FALLA${NC} %s\n" "$1"; FALLOS=$((FALLOS+1))
    fi
}
verificar_en "Ctrl+C cancela la compresión y lo avisa" "Compresión cancelada" "$DIR/cancel.log"
verificar_en "el editor sigue vivo tras cancelar (q responde)" "Saliendo del editor" "$DIR/cancel.log"
if [ ! -e "$DIR/grande.huf" ] && [ ! -e "$DIR/grande.huf.tmp" ]; then
    printf "${VERDE}  PASA${NC}  al cancelar no queda ni el .huf ni el temporal\n"; OK=$((OK+1))
else
    printf "${ROJO}  FALLA${NC} quedó un archivo tras cancelar\n"; FALLOS=$((FALLOS+1))
fi
rm -f "$DIR/grande.txt"

# ------------------------------------------------------------- 4. Fugas
if [ "${1:-}" == "--valgrind" ]; then
    echo
    echo "-- Fugas de memoria --"
    printf 'uno\ndos\ntres\n' > "$DIR/v.txt"
    printf "editor\no $DIR/v.txt\na \"x\"\nd 1\np\nw\nq\nexit\n" \
        | valgrind --leak-check=full --error-exitcode=9 $BIN > "$DIR/vg.log" 2>&1
    if [ $? -eq 9 ]; then
        printf "${ROJO}  FALLA${NC} valgrind reportó fugas o errores\n"; FALLOS=$((FALLOS+1))
        grep -E "definitely lost|indirectly lost|Invalid" "$DIR/vg.log" | head -5
    else
        printf "${VERDE}  PASA${NC}  sin fugas de memoria\n"; OK=$((OK+1))
    fi
fi

echo
echo "=================================="
printf "  %d pruebas superadas, %d fallidas\n" "$OK" "$FALLOS"
echo "=================================="
rm -rf "$DIR"
exit $((FALLOS > 0))
