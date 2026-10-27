//
// Created by marti on 8/08/2026.
//
#include "shell.h"
#include <dirent.h> //nos permite usar las funciones opendir(), readdir(), closedir(), y el struct dirent.
#include <errno.h> //convierte los errores en texto legible en pantalla para que el usuario sepa que está pasando
#include <string.h>//esta se concatena con errno.h, pues el anterior nos dice cual es el error y esta lo traduce a "palabras"

/**
 * ====================================================================================
 * COMANDO: l_dir [<carpeta>]
 * ====================================================================================
 * Demuestra el listado de contenido de un directorio a bajo nivel.
 *
 * Syscalls explicadas:
 * 1. opendir(3): Abre un directorio y devuelve un manejador DIR*.
 *    Por debajo, la librería C usa la syscall open(2) sobre el directorio.
 * 2. readdir(3): Lee UNA entrada del directorio en cada llamada.
 *    Por debajo usa la syscall getdents64(2), que trae varias entradas
 *    de una vez a un buffer interno que la librería te va entregando
 *    de a una. Devuelve NULL cuando ya no hay más entradas.
 * 3. closedir(3): Libera el manejador y cierra el descriptor asociado.
 */
int cmd_l_dir(int argc, char **argv) { //Aquí estoy creando el comando l_dir que basicamente lo que hace es el ls de linux
    const char *path = (argc >= 2) ? argv[1] : "."; //el if que nos lee el comando
    // 1. LLAMADA: opendir  (lo printeamos en la pantalla
    LOG_SYSCALL("opendir", "\"%s\"", path);
    DIR *dir = opendir(path);
    if (dir == NULL) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return 1;
    }
    printf("= " COLOR_RESULT "%p" COLOR_RESET "\n", (void *)dir);

    printf(COLOR_TITLE "\nContenido de '%s':\n" COLOR_RESET, path);

    /* 2. LLAMADA: readdir, en bucle hasta que devuelva NULL */
    struct dirent *entry;
    int count = 0;
    errno = 0; /* Se limpia antes del bucle: readdir usa errno para distinguir
                  "fin del directorio" de "error real" cuando devuelve NULL */
    while ((entry = readdir(dir)) != NULL) {
        printf("  " COLOR_PARAM "%s" COLOR_RESET "\n", entry->d_name);
        count++;
    }
    if (errno != 0) {
        LOG_SYSCALL_ERROR(strerror(errno));
        closedir(dir);
        return 1;
    }
    printf(COLOR_INFO "(%d entradas)\n" COLOR_RESET, count);

    /* 3. LLAMADA: closedir */
    LOG_SYSCALL("closedir", "%p", (void *)dir);
    int res = closedir(dir);
    LOG_SYSCALL_RESULT(res);
    //despues de entrar al directorio y leerlo con el while estas de aquí arribita nos lo cierran y así poder liberar el
    //espacio y recursos que ocupo el kernell para "entrar" a leer el directorio

    return 0;
}