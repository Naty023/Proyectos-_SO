# Proyecto 1: grep multiproceso en C

## Descripción
Este proyecto implementa una versión multiproceso del programa `grep` en lenguaje C sobre ambiente Unix. Permite buscar palabras o expresiones regulares en un archivo de texto muy grande, imprimiendo el párrafo donde aparece la coincidencia. Utiliza procesos hijos y comunicación por pipes para dividir el trabajo y mejorar el rendimiento.

## Uso
Compila el programa con:

```sh
gcc -o grep main.c -Wall -Wextra -std=c99
```

Ejecuta el programa con:

```sh
./grep '<expresion_regular>' <archivo> <num_procesos> <logfile>
```

- `<expresion_regular>`: Expresión regular a buscar (usa sintaxis POSIX).
- `<archivo>`: Archivo de texto grande donde buscar.
- `<num_procesos>`: Número de procesos hijos a usar (recomendado probar varios valores).
- `<logfile>`: Archivo CSV donde se guardan los resultados de cada lectura.

Ejemplo:
```sh
./grep 'Don|Quijote' textos/don_quijote.txt 4 logfile.csv
```

## Requisitos
- Sistema operativo Unix/Linux/macOS
- Compilador GCC
- Solo se utiliza la librería estándar de C y `regex.h` (no se permiten otras librerías)

## Consideraciones de diseño
- El buffer de lectura por proceso es de 8K (8192 bytes).
- Los procesos hijos alternan la lectura del archivo, no se calculan las porciones de antemano.
- El procesamiento se realiza sobre un único archivo grande.
- Se utilizan procesos hijos, no hilos.
- La salida se imprime por párrafos (separados por doble salto de línea) donde se encuentra la expresión regular.
- Se genera un archivo de bitácora (CSV) con el tiempo y datos de cada lectura para análisis posterior.

## Análisis de resultados
El archivo de bitácora (`logfile.csv`) contiene las columnas:
- `process_id`: ID del proceso hijo
- `file_offset`: Posición inicial en el archivo
- `bytes_read`: Bytes leídos en la porción
- `elapsed_time`: Tiempo que tomó la lectura
- `found`: 1 si se encontró coincidencia, 0 si no

Puedes analizar el rendimiento y la cantidad óptima de procesos usando Excel, R o Python para graficar los resultados.

## Archivos recomendados para pruebas
Puedes descargar archivos de texto grandes del Proyecto Gutenberg, por ejemplo:
- texts/don_quijote.txt
- texts/divina_comedia.txt
- texts/anna_karenina.txt

## Autoría y restricciones
- Proyecto para el curso IC6600 - Principios de Sistemas Operativos
- [Andres Arias](https://github.com/andco97)
- [Natalia Rodriguez](https://github.com/Naty023)
