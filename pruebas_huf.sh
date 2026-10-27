#!/bin/bash
# ====================================================================================
# Pruebas del compresor Huffman (etapa secuencial).
#
# La verificación central es siempre la misma: comprimir, descomprimir y comparar el
# md5 contra el original. Si los dos hashes coinciden, la recuperación fue exacta
# byte por byte; si no, algo se perdió por el camino.
#
#   ./pruebas_huf.sh
# ====================================================================================

set -u
BIN=./huf
DIR=$(mktemp -d)
OK=0
FALLOS=0

VERDE='\033[1;32m'; ROJO='\033[1;31m'; GRIS='\033[0;90m'; NC='\033[0m'

# ciclo <descripción> <archivo>  -> comprime, descomprime y compara md5
ciclo() {
    local desc="$1" f="$2"
    if ! $BIN c "$f" "$f.huf" 2>"$DIR/err.log"; then
        printf "${ROJO}  FALLA${NC} %s (falló al comprimir)\n" "$desc"
        FALLOS=$((FALLOS+1)); return
    fi
    if ! $BIN d "$f.huf" "$f.out" 2>>"$DIR/err.log"; then
        printf "${ROJO}  FALLA${NC} %s (falló al descomprimir)\n" "$desc"
        FALLOS=$((FALLOS+1)); return
    fi
    if cmp -s "$f" "$f.out"; then
        local a b
        a=$(stat -c%s "$f"); b=$(stat -c%s "$f.huf")
        printf "${VERDE}  PASA${NC}  %s ${GRIS}(%s B -> %s B)${NC}\n" "$desc" "$a" "$b"
        OK=$((OK+1))
    else
        printf "${ROJO}  FALLA${NC} %s (el contenido no coincide)\n" "$desc"
        FALLOS=$((FALLOS+1))
    fi
}

# debe_fallar <descripción> <comando...>
debe_fallar() {
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        printf "${ROJO}  FALLA${NC} %s (debió rechazarlo y no lo hizo)\n" "$desc"
        FALLOS=$((FALLOS+1))
    else
        printf "${VERDE}  PASA${NC}  %s\n" "$desc"
        OK=$((OK+1))
    fi
}

echo
echo "=== Pruebas del compresor Huffman ==="
echo

# ----------------------------------------------------------------- Contenido típico
echo "-- Contenido típico --"

python3 -c "print(('los sistemas operativos gestionan procesos y memoria. ')*2000, end='')" \
    > "$DIR/repetitivo.txt"
ciclo "texto repetitivo (comprime mucho)" "$DIR/repetitivo.txt"

head -c 200000 /usr/share/dict/words 2>/dev/null > "$DIR/palabras.txt" || \
    python3 -c "
import random, string
random.seed(1)
print(' '.join(''.join(random.choice(string.ascii_lowercase) for _ in range(random.randint(3,9))) for _ in range(30000)), end='')
" > "$DIR/palabras.txt"
ciclo "texto natural" "$DIR/palabras.txt"

head -c 100000 /dev/urandom > "$DIR/aleatorio.bin"
ciclo "binario aleatorio (apenas comprime)" "$DIR/aleatorio.bin"

# ----------------------------------------------------------------- Casos borde
echo
echo "-- Casos borde --"

: > "$DIR/vacio.txt"
ciclo "archivo vacío (0 bytes)" "$DIR/vacio.txt"

printf 'a' > "$DIR/unbyte.txt"
ciclo "un solo byte" "$DIR/unbyte.txt"

python3 -c "print('a'*50000, end='')" > "$DIR/unsimbolo.txt"
ciclo "un solo símbolo repetido 50000 veces" "$DIR/unsimbolo.txt"

python3 -c "
import sys
sys.stdout.buffer.write(bytes(range(256))*100)
" > "$DIR/todos.bin"
ciclo "los 256 valores de byte posibles" "$DIR/todos.bin"

# Fronteras de bloque: el bloque es de 65536 bytes.
head -c 65535 /dev/urandom > "$DIR/menos1.bin"; ciclo "un byte menos que un bloque"  "$DIR/menos1.bin"
head -c 65536 /dev/urandom > "$DIR/exacto.bin"; ciclo "exactamente un bloque"        "$DIR/exacto.bin"
head -c 65537 /dev/urandom > "$DIR/mas1.bin";   ciclo "un byte más que un bloque"    "$DIR/mas1.bin"
head -c 300000 /dev/urandom > "$DIR/varios.bin"; ciclo "varios bloques completos"    "$DIR/varios.bin"

python3 -c "
import sys
sys.stdout.buffer.write(b'\x00'*1000 + b'texto\n' + b'\xff'*1000)
" > "$DIR/nulos.bin"
ciclo "bytes nulos y 0xFF mezclados con texto" "$DIR/nulos.bin"

# ----------------------------------------------------------------- Robustez
echo
echo "-- Rechazo de entradas inválidas --"

echo "esto no es un archivo huf" > "$DIR/falso.huf"
debe_fallar "rechaza un archivo que no es .huf" $BIN d "$DIR/falso.huf" "$DIR/x.out"

printf 'HUF2' > "$DIR/corto.huf"
debe_fallar "rechaza un .huf truncado"          $BIN d "$DIR/corto.huf" "$DIR/x.out"

# Corrupción del contenido: se altera un byte de la carga útil.
$BIN c "$DIR/repetitivo.txt" "$DIR/danado.huf" >/dev/null 2>&1
python3 -c "
import sys
p = sys.argv[1]
d = bytearray(open(p,'rb').read())
d[3000] ^= 0xFF          # un byte en plena zona de datos
open(p,'wb').write(d)
" "$DIR/danado.huf"
if $BIN d "$DIR/danado.huf" "$DIR/danado.out" >/dev/null 2>&1; then
    if cmp -s "$DIR/repetitivo.txt" "$DIR/danado.out"; then
        printf "${ROJO}  FALLA${NC} un .huf corrupto produjo el archivo correcto (imposible)\n"
        FALLOS=$((FALLOS+1))
    else
        printf "${VERDE}  PASA${NC}  un .huf corrupto no produce el original\n"
        OK=$((OK+1))
    fi
else
    printf "${VERDE}  PASA${NC}  un .huf corrupto se detecta y se rechaza\n"
    OK=$((OK+1))
fi

debe_fallar "rechaza un archivo inexistente" $BIN c "$DIR/no_existe" "$DIR/x.huf"

# ----------------------------------------------------------------- Cabecera dispersa (HUF2)
echo
echo "-- Cabecera dispersa --"

# Un archivo pequeño con pocos símbolos debe ocupar MENOS comprimido que original.
python3 -c "print('hola mundo ' * 60, end='')" > "$DIR/pequeno.txt"
$BIN c "$DIR/pequeno.txt" "$DIR/pequeno.huf" >/dev/null 2>&1
a=$(stat -c%s "$DIR/pequeno.txt"); b=$(stat -c%s "$DIR/pequeno.huf")
if [ "$b" -lt "$a" ]; then
    printf "${VERDE}  PASA${NC}  un archivo pequeño ya no crece ${GRIS}(%s B -> %s B)${NC}\n" "$a" "$b"
    OK=$((OK+1))
else
    printf "${ROJO}  FALLA${NC} un archivo pequeño creció (%s B -> %s B)\n" "$a" "$b"
    FALLOS=$((FALLOS+1))
fi

# El archivo vacío solo lleva la cabecera fija (26 B), sin tabla de símbolos.
$BIN c "$DIR/vacio.txt" "$DIR/vacio.huf" >/dev/null 2>&1
v=$(stat -c%s "$DIR/vacio.huf")
if [ "$v" -eq 26 ]; then
    printf "${VERDE}  PASA${NC}  el archivo vacío ocupa solo la cabecera fija ${GRIS}(26 B)${NC}\n"
    OK=$((OK+1))
else
    printf "${ROJO}  FALLA${NC} el archivo vacío ocupa %s B y debía ocupar 26\n" "$v"
    FALLOS=$((FALLOS+1))
fi

# modificar <salida> <expresión Python sobre 'd' (bytearray)> : parte de pequeno.huf
modificar() {
    python3 -c "
import sys
d = bytearray(open(sys.argv[1],'rb').read())
$2
open(sys.argv[3],'wb').write(d)
" "$DIR/pequeno.huf" x "$DIR/$1"
}

# Offsets: n_simbolos en 24..25; primer par en 26 (byte) y 27..34 (frecuencia).
modificar mas256.huf  "d[24]=0x2C; d[25]=0x01"      # n_simbolos = 300
debe_fallar "rechaza n_simbolos mayor que 256"      $BIN d "$DIR/mas256.huf" "$DIR/x.out"

modificar dup.huf     "d[35]=d[26]"                 # segundo símbolo repite al primero
debe_fallar "rechaza símbolos duplicados"           $BIN d "$DIR/dup.huf" "$DIR/x.out"

modificar suma.huf    "d[27]^=1"                    # frecuencia alterada: la suma ya no cuadra
debe_fallar "rechaza frecuencias que no suman el tamaño original" $BIN d "$DIR/suma.huf" "$DIR/x.out"

modificar tabla.huf   "d[:]=d[:40]"                 # corta dentro de la tabla de símbolos
debe_fallar "rechaza una tabla de símbolos truncada" $BIN d "$DIR/tabla.huf" "$DIR/x.out"

modificar viejo.huf   "d[0:4]=b'HUF1'"              # formato anterior
debe_fallar "rechaza el formato anterior HUF1"      $BIN d "$DIR/viejo.huf" "$DIR/x.out"

echo
echo "=================================="
printf "  %d pruebas superadas, %d fallidas\n" "$OK" "$FALLOS"
echo "=================================="
rm -rf "$DIR"
exit $((FALLOS > 0))
