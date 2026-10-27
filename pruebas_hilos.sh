#!/bin/bash
# ====================================================================================
# Verificación de la etapa concurrente.
#
# INVARIANTE CENTRAL: el archivo .huf debe salir IDÉNTICO byte por byte sin importar
# cuántos hilos se usen. Si no es así, hay una condición de carrera.
#
# Este es un detector determinista, a diferencia de "ejecutar el programa y ver si se
# cae": las carreras se manifiestan una vez de cada cien ejecuciones, así que probar
# a ojo no sirve. Aquí, si el resultado depende del número de hilos, falla SIEMPRE.
#
#   ./pruebas_hilos.sh
# ====================================================================================

set -u
BIN=./huf
DIR=$(mktemp -d)
OK=0
FALLOS=0
HILOS_PROBAR="1 2 3 4 7 8 16"

VERDE='\033[1;32m'; ROJO='\033[1;31m'; GRIS='\033[0;90m'; NC='\033[0m'

# invariante <descripción> <archivo>
invariante() {
    local desc="$1" f="$2" ref="" todos_ok=1

    for h in $HILOS_PROBAR; do
        if ! HILOS=$h $BIN c "$f" "$f.$h.huf" 2>/dev/null; then
            printf "${ROJO}  FALLA${NC} %s (falló con %s hilos)\n" "$desc" "$h"
            FALLOS=$((FALLOS+1)); return
        fi
        if [ -z "$ref" ]; then
            ref="$f.$h.huf"
        elif ! cmp -s "$ref" "$f.$h.huf"; then
            printf "${ROJO}  FALLA${NC} %s (con %s hilos el .huf difiere)\n" "$desc" "$h"
            todos_ok=0; break
        fi
    done

    [ $todos_ok -eq 0 ] && { FALLOS=$((FALLOS+1)); return; }

    # Y que además se recupere el original.
    if $BIN d "$ref" "$f.out" 2>/dev/null && cmp -s "$f" "$f.out"; then
        printf "${VERDE}  PASA${NC}  %s ${GRIS}(idéntico con 1..16 hilos, md5 ok)${NC}\n" "$desc"
        OK=$((OK+1))
    else
        printf "${ROJO}  FALLA${NC} %s (no se recupera el original)\n" "$desc"
        FALLOS=$((FALLOS+1))
    fi
}

echo
echo "=== Invariante de concurrencia: la salida no depende del número de hilos ==="
echo

python3 -c "print(('los sistemas operativos gestionan procesos y memoria. ')*20000, end='')" \
    > "$DIR/grande.txt"
invariante "texto grande (1 MB, varios bloques)" "$DIR/grande.txt"

head -c 500000 /dev/urandom > "$DIR/aleatorio.bin"
invariante "binario aleatorio" "$DIR/aleatorio.bin"

python3 -c "print('a'*100000, end='')" > "$DIR/unsimbolo.txt"
invariante "un solo símbolo" "$DIR/unsimbolo.txt"

: > "$DIR/vacio.txt"
invariante "archivo vacío" "$DIR/vacio.txt"

printf 'ab' > "$DIR/dosbytes.txt"
invariante "menos bytes que hilos" "$DIR/dosbytes.txt"

python3 -c "
import sys
sys.stdout.buffer.write(bytes(range(256))*2000)
" > "$DIR/todos.bin"
invariante "los 256 valores de byte" "$DIR/todos.bin"

# ----------------------------------------------------------- Medición de ganancia
echo
echo "-- Tiempo de la fase de conteo --"
python3 -c "print(('texto de prueba para medir el conteo de frecuencias. ')*400000, end='')" \
    > "$DIR/medir.txt"
printf "${GRIS}  archivo de %s MB${NC}\n" "$(( $(stat -c%s "$DIR/medir.txt") / 1048576 ))"
for h in 1 2 4 8; do
    t=$(HILOS=$h $BIN -v c "$DIR/medir.txt" "$DIR/medir.huf" 2>&1 >/dev/null \
        | grep -o 'conteo=[0-9.]*' | cut -d= -f2)
    printf "  %2s hilos -> conteo %s s\n" "$h" "$t"
done

echo
echo "=================================="
printf "  %d pruebas superadas, %d fallidas\n" "$OK" "$FALLOS"
echo "=================================="
rm -rf "$DIR"
exit $((FALLOS > 0))
