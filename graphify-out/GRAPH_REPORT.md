# Graph Report - Editor-de-texto--Sistemas-Operativos  (2026-10-26)

## Corpus Check
- 31 files · ~64,802 words
- Verdict: corpus is large enough that graph structure adds value.
- Unclassified: 3 file(s) not represented in the graph (top: (none) 3)

## Summary
- 328 nodes · 736 edges · 15 communities (11 shown, 4 thin omitted)
- Extraction: 72% EXTRACTED · 28% INFERRED · 0% AMBIGUOUS · INFERRED: 206 edges (avg confidence: 0.88)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `f265de1c`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- cat_datos.c
- Buffer
- test_huffman.c
- Formato .huf
- bitio.c
- huf_formato.c
- Decisiones de diseño
- pruebas.sh
- Estados de tarea INACTIVA/CORRIENDO/OK/ERROR
- pruebas_huf.sh
- pruebas_hilos.sh
- Propiedad de prefijo / Kraft
- Grafo del proyecto (graphify)
- comprimir.c
- cat_editor.c

## God Nodes (most connected - your core abstractions)
1. `cmd_editor()` - 24 edges
2. `huf_construir()` - 17 edges
3. `buf_cargar()` - 15 edges
4. `comp_comprimir_a_fd_cb()` - 14 edges
5. `huf_descomprimir_cb()` - 14 edges
6. `ida_y_vuelta()` - 14 edges
7. `12. Aplicación de la auditoría (`AUDITORIA.md`)` - 14 edges
8. `bw_flush()` - 13 edges
9. `huf_liberar()` - 13 edges
10. `prueba_binario()` - 13 edges

## Surprising Connections (you probably didn't know these)
- `Alternativas y otras perspectivas` --references--> `bw_bits()`  [INFERRED]
  AUDITORIA.md → bitio.c
- `El orden de los `free()`` --references--> `buf_liberar()`  [INFERRED]
  README.md → buffer.c
- `5. Por qué el descriptor se cierra de inmediato` --references--> `buf_cargar()`  [INFERRED]
  README.md → buffer.c
- `Verificación de memoria y de hilos` --references--> `buf_cargar()`  [INFERRED]
  README.md → buffer.c
- `5. Formato HUF2 con cabecera dispersa` --references--> `huf_tam_preambulo()`  [INFERRED]
  AVANCES.md → huf_formato.c

## Import Cycles
- None detected.

## Hyperedges (group relationships)
- **Pipeline de compresión paralela** — readme_compresor_local_reduction_count, readme_compresor_producer_consumer_window, readme_compresor_ordered_writer, readme_compresor_index_backfill_lseek [EXTRACTED 0.95]
- **Cadena de determinismo del formato** — readme_compresor_menor_que_tiebreak, readme_compresor_freq_table_not_tree, readme_compresor_thread_invariant [INFERRED 0.85]

## Communities (15 total, 4 thin omitted)

### Community 1 - "Buffer"
Cohesion: 0.08
Nodes (46): 10. `atoi` acepta entradas inválidas, 11. `w!` no conserva permisos ni dueño, 12. Descompresión en paralelo, 13. Fuga de memoria si `buf_cargar` falla a mitad, 1. `comp` y `desc` pueden destruir el archivo abierto, 2. No se pueden abrir archivos de solo lectura, 3. El proyecto se compilaba sin optimización, 5. La descompresión recorría el árbol bit a bit (+38 more)

### Community 2 - "test_huffman.c"
Cohesion: 0.16
Nodes (30): hilo_contar(), asignar_codigos(), bajar(), huf_acumular(), huf_bits_totales(), huf_construir(), huf_contar(), huf_decodificar() (+22 more)

### Community 3 - "Formato .huf"
Cohesion: 0.07
Nodes (22): BitReader, BitWriter acumula en buffer de memoria, Duplicación de capacidad del buffer, escribir_todo / leer_todo con EINTR, Montículo binario en arreglo, huf_comprimir_con_freq, huf_contar + huf_acumular (reducción), Formato .huf (+14 more)

### Community 4 - "bitio.c"
Cohesion: 0.30
Nodes (20): 4. `bw_bits` escribía un bit por iteración, asegurar_capacidad(), br_bit(), br_bits(), br_init(), br_restantes(), bw_bit(), bw_bits() (+12 more)

### Community 5 - "huf_formato.c"
Cohesion: 0.12
Nodes (26): construir_tabla_rapida(), contar_simbolos(), escribir_todo(), get_u16(), get_u32(), get_u64(), huf_comprimir(), huf_comprimir_archivo() (+18 more)

### Community 6 - "Decisiones de diseño"
Cohesion: 0.06
Nodes (36): 11. Por qué Ctrl+C cancela en vez de matar el proceso, 1. Por qué el documento vive en memoria y no se edita en disco, 2. Por qué un arreglo de descriptores y no una lista enlazada, 3. Por qué dos estrategias de guardado, 4. Por qué el editor es una categoría nueva del shell, 5. Por qué el descriptor se cierra de inmediato, 6. Por qué `i` y `s` no necesitan estructuras nuevas, 7. Por qué la cabecera guarda solo los símbolos presentes (formato HUF2) (+28 more)

### Community 7 - "pruebas.sh"
Cohesion: 0.52
Nodes (6): comprobar(), correr(), pruebas.sh script, verificar(), verificar_en(), verificar_salida()

### Community 8 - "Estados de tarea INACTIVA/CORRIENDO/OK/ERROR"
Cohesion: 0.50
Nodes (4): Comandos del editor comp, desc, st, Join con contador creados, tarea_esperar, Estados de tarea INACTIVA/CORRIENDO/OK/ERROR

### Community 9 - "pruebas_huf.sh"
Cohesion: 0.70
Nodes (4): ciclo(), debe_fallar(), modificar(), pruebas_huf.sh script

### Community 13 - "comprimir.c"
Cohesion: 0.19
Nodes (9): ahora_seg(), comp_comprimir_a_fd(), comp_comprimir_a_fd_cb(), comp_comprimir_archivo(), comp_contar(), comp_num_hilos(), main(), mostrar_info() (+1 more)

### Community 14 - "cat_editor.c"
Cohesion: 0.10
Nodes (27): 10. Cancelación con Control más C (la novedad principal), 11. Verificación de la cancelación, del código actual y de la memoria, 1. Editor con el documento en memoria, 2. Compresor Huffman por módulos, 3. Compresión concurrente, 4. Comandos `comp` y `desc` bloqueantes con barra de progreso, 5. Formato HUF2 con cabecera dispersa, 6. Makefile y limpieza del repositorio (+19 more)

## Knowledge Gaps
- **13 isolated node(s):** `Propiedad de prefijo / Kraft`, `Duplicación de capacidad del buffer`, `escribir_todo / leer_todo con EINTR`, `huf_contar + huf_acumular (reducción)`, `Binario huf (c/d/i/-v)` (+8 more)
  These have ≤1 connection - possible missing edges or undocumented components. (Counts symbols only; 94 node(s) total have ≤1 connection when file, concept and rationale nodes are included.)
- **4 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Editor de Texto con Llamadas al Sistema (Linux)` connect `Decisiones de diseño` to `Buffer`, `cat_editor.c`?**
  _High betweenness centrality (0.073) - this node is a cross-community bridge._
- **Are the 14 inferred relationships involving `cmd_editor()` (e.g. with `9. Líneas de más de 4096 caracteres se parten` and `buf_agregar()`) actually correct?**
  _`cmd_editor()` has 14 INFERRED edges - model-reasoned connections that need verification._
- **What connects `Propiedad de prefijo / Kraft`, `Duplicación de capacidad del buffer`, `escribir_todo / leer_todo con EINTR` to the rest of the system?**
  _13 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `cat_datos.c` be split into smaller, more focused modules?**
  _Cohesion score 0.06620209059233449 - nodes in this community are weakly interconnected._
- **Why does `12. Aplicación de la auditoría (`AUDITORIA.md`)` connect `Buffer` to `test_huffman.c`, `bitio.c`, `huf_formato.c`, `cat_editor.c`?**
  _High betweenness centrality (0.069) - this node is a cross-community bridge._
- **Are the 12 inferred relationships involving `huf_construir()` (e.g. with `comp_comprimir_a_fd_cb()` and `huf_comprimir_con_freq()`) actually correct?**
  _`huf_construir()` has 12 INFERRED edges - model-reasoned connections that need verification._
- **Should `Buffer` be split into smaller, more focused modules?**
  _Cohesion score 0.08235294117647059 - nodes in this community are weakly interconnected._