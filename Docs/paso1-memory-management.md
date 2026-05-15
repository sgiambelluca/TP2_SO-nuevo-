# Paso 1 - Memory Management: Teoria e Implementacion

## Indice

1. [Contexto: que hay actualmente](#1-contexto-que-hay-actualmente)
2. [Que se pide](#2-que-se-pide)
3. [Teoria: como funciona un memory manager](#3-teoria-como-funciona-un-memory-manager)
4. [Definir la interfaz comun](#4-definir-la-interfaz-comun)
5. [Elegir el bloque de memoria a administrar](#5-elegir-el-bloque-de-memoria-a-administrar)
6. [Implementacion A: Memory Manager con lista enlazada (First Fit)](#6-implementacion-a-memory-manager-con-lista-enlazada-first-fit)
7. [Implementacion B: Buddy System](#7-implementacion-b-buddy-system)
8. [Compilacion condicional](#8-compilacion-condicional)
9. [Agregar las syscalls de memoria](#9-agregar-las-syscalls-de-memoria)
10. [Integrar el test_mm](#10-integrar-el-test_mm)
11. [Checklist final](#11-checklist-final)

---

## 1. Contexto: que hay actualmente

El kernel actual **no tiene ningun tipo de memoria dinamica**. Todo esta asignado estaticamente:

```
Mapa de memoria actual:

0x000000 - 0x0FFFFF   Reservado (BIOS, VBE info en 0x5C00, etc.)
0x100000 - ...         Kernel (.text, .rodata, .data, .bss)
           ...         endOfKernel (simbolo del linker script)
           ...         Stack del kernel (8 paginas despues de endOfKernel)
0x400000               sampleCodeModule (userland code, copiado por loadModules)
0x500000               sampleDataModule (userland data, copiado por loadModules)
```

Las direcciones clave estan en `Kernel/c/kernel.c`:
- `sampleCodeModuleAddress = 0x400000`
- `sampleDataModuleAddress = 0x500000`
- El stack del kernel empieza en `endOfKernel + 8 paginas`

El linker script (`Kernel/kernel.ld`) ubica el kernel en `0x100000` y exporta los simbolos `endOfKernelBinary` y `endOfKernel`.

**Actualmente no existe `malloc` ni `free`.** Todo buffer es estatico (ej. `char buff[256]` en el keyboard driver).

---

## 2. Que se pide

El enunciado del TP2 requiere:

1. **Dos implementaciones** de memory manager: una elegida por el grupo + Buddy System.
2. **Intercambiables en tiempo de compilacion** (no simultaneas). Ej: `make` usa una, `make buddy` usa la otra.
3. **Misma interfaz** para ambas (transparentes).
4. **Syscalls**: reservar memoria, liberar memoria, consultar estado.
5. **Pasar el test `test_mm`** con al menos una de las dos implementaciones.

---

## 3. Teoria: como funciona un memory manager

### Que es

Un memory manager administra un bloque grande de memoria (el "heap") y permite a los programas pedir porciones (`malloc`) y devolverlas (`free`). El desafio es:

- **No solapar** bloques asignados.
- **Reutilizar** bloques liberados.
- **Minimizar fragmentacion** (huecos inutilizables entre bloques).

### Fragmentacion

- **Fragmentacion externa**: hay suficiente memoria libre en total, pero esta dividida en pedazos pequenos no contiguos, asi que no se puede satisfacer un pedido grande.
- **Fragmentacion interna**: se asigna mas memoria de la pedida (ej. se piden 10 bytes pero se dan 16 por alineacion).

### Algoritmo First Fit con lista enlazada

Se mantiene una lista enlazada de bloques libres. Cada bloque tiene un header con su tamano y un puntero al siguiente bloque libre.

```
[Header | ........datos........] -> [Header | ....datos....] -> NULL
  size=64                             size=128
```

**malloc(n)**: recorre la lista y devuelve el **primer** bloque que tenga tamano >= n. Si el bloque es mas grande que lo pedido, lo parte en dos: uno asignado y otro libre con el sobrante.

**free(ptr)**: marca el bloque como libre y lo inserta de vuelta en la lista. Si hay bloques libres contiguos, los **fusiona** (coalescing) para reducir fragmentacion.

Ventajas: simple de implementar, bajo overhead.
Desventajas: puede fragmentarse, busqueda O(n) en el peor caso.

### Algoritmo Buddy System

Divide la memoria en bloques cuyo tamano es siempre una **potencia de 2**. Arranca con un unico bloque del tamano total.

**malloc(n)**: busca el bloque mas chico que sea potencia de 2 y >= n. Si el bloque disponible es demasiado grande, lo parte a la mitad recursivamente hasta llegar al tamano justo. Cada mitad se llama "buddy" (companero) de la otra.

**free(ptr)**: libera el bloque y verifica si su buddy tambien esta libre. Si lo esta, los fusiona en un bloque del doble de tamano, y repite recursivamente hacia arriba.

```
Ejemplo con 256 bytes:

malloc(30):
  256 -> split -> [128][128]
                   split -> [64][64]
                             asigna [64] (32 de header + 30 de datos)

free(ptr):
  libera [64], buddy [64] tambien libre -> merge -> [128]
  buddy [128] libre -> merge -> [256]
```

Ventajas: fusion rapida (O(log n)), poca fragmentacion externa.
Desventajas: fragmentacion interna significativa (siempre redondea a potencia de 2), mas complejo de implementar.

---

## 4. Definir la interfaz comun

Ambas implementaciones deben exponer **exactamente las mismas funciones**. Crear el header:

### Archivo: `Kernel/include/memoryManager.h`

```c
#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <stdint.h>

// Estructura para reportar el estado de la memoria
typedef struct {
    uint64_t total;      // Bytes totales administrados
    uint64_t used;       // Bytes actualmente asignados
    uint64_t free;       // Bytes libres
    uint64_t alloc_count; // Cantidad de bloques asignados actualmente
} MemStatus;

// Inicializa el memory manager sobre el bloque [start, start+size)
void mm_init(void *start, uint64_t size);

// Reserva 'size' bytes. Retorna NULL si no hay espacio.
void *mm_malloc(uint64_t size);

// Libera el bloque apuntado por 'ptr'. Si ptr es NULL, no hace nada.
void mm_free(void *ptr);

// Llena 'status' con el estado actual de la memoria.
void mm_status(MemStatus *status);

#endif
```

Esto es lo unico que el resto del kernel (y las syscalls) van a ver. No importa cual implementacion este detras.

---

## 5. Elegir el bloque de memoria a administrar

Hay que decidir **donde empieza y que tamano tiene** el heap. La memoria disponible en el sistema (QEMU con Pure64) es tipicamente >= 32 MB. Se debe elegir una region que no pise:

- El kernel (0x100000 hasta endOfKernel + stack)
- Los modulos de usuario (0x400000 y 0x500000)
- El framebuffer de video (direccion del VBE, tipicamente alta)

### Opcion recomendada

Definir el heap en una zona fija a partir de, por ejemplo, `0x600000` con un tamano de varios MB (ej. 8 MB). Esto se configura en `kernel.c` al inicializar:

```c
// En kernel.c, dentro de initializeKernelBinary() o main():
#define HEAP_START  0x600000
#define HEAP_SIZE   (8 * 1024 * 1024)  // 8 MB

mm_init((void *)HEAP_START, HEAP_SIZE);
```

### Mapa de memoria resultante

```
0x000000 - 0x0FFFFF   BIOS / VBE info
0x100000 - ~0x1XXXXX  Kernel (text + data + bss + stack)
0x400000 - 0x4FFFFF   Userland code module
0x500000 - 0x5FFFFF   Userland data module
0x600000 - 0xDFFFFF   <<< HEAP (8 MB) >>> <-- memory manager administra esto
0xE00000 - ...         Libre (o framebuffer mas arriba)
```

**Nota:** si necesitas mas memoria, podes aumentar `HEAP_SIZE`. Pure64 con QEMU por defecto da 256 MB de RAM, asi que hay espacio.

---

## 6. Implementacion A: Memory Manager con lista enlazada (First Fit)

### Archivo: `Kernel/c/memoryManagerFF.c`

La idea es mantener una lista enlazada de bloques. Cada bloque (libre o asignado) tiene un header:

```c
typedef struct MemBlock {
    uint64_t size;              // Tamano del bloque (sin contar el header)
    int is_free;                // 1 si esta libre, 0 si esta asignado
    struct MemBlock *next;      // Siguiente bloque en la lista
} MemBlock;
```

### mm_init

1. Recibe `start` y `size`.
2. Crea un unico bloque libre que ocupa toda la memoria disponible (descontando el tamano del header).
3. Guarda punteros a la cabeza de la lista y datos globales (total, used).

```
Antes de cualquier malloc:

[Header: size=8MB-sizeof(Header), free=1, next=NULL | ......todo libre......]
```

### mm_malloc(size)

1. Recorre la lista buscando el **primer bloque libre** con `block->size >= size` (First Fit).
2. Si lo encuentra:
   - Si el bloque es suficientemente mas grande (sobrante > sizeof(Header) + un minimo), lo **parte**: el pedazo de adelante queda asignado, el sobrante se convierte en un nuevo bloque libre.
   - Marca el bloque como `is_free = 0`.
   - Retorna `(void *)((uint8_t *)block + sizeof(MemBlock))` (puntero justo despues del header).
3. Si no encuentra bloque suficiente, retorna `NULL`.

```
Despues de malloc(100):

[H: size=100, free=0] [datos 100B] [H: size=resto, free=1] [...libre...]
```

### mm_free(ptr)

1. Calcula el header: `block = (MemBlock *)((uint8_t *)ptr - sizeof(MemBlock))`.
2. Marca `is_free = 1`.
3. **Coalescing**: recorre la lista y fusiona bloques libres contiguos.
   - Si `block->next` existe y esta libre, absorbe su tamano.
   - Tambien se puede buscar el bloque anterior para fusionar hacia atras.

### mm_status

Recorre la lista sumando tamanos de bloques libres y ocupados.

### Ejemplo conceptual paso a paso

```
Estado inicial:  [FREE: 8MB]

malloc(100):     [USED:100] [FREE: 8MB-100-H]

malloc(200):     [USED:100] [USED:200] [FREE: 8MB-300-2H]

free(ptr1):      [FREE:100] [USED:200] [FREE: 8MB-300-2H]
                  ^ no se puede fusionar con el de la derecha (USED en el medio)

free(ptr2):      [FREE:100] [FREE:200] [FREE: 8MB-300-2H]
                  ^ ahora se fusionan los 3 -> [FREE: 8MB]
```

---

## 7. Implementacion B: Buddy System

### Archivo: `Kernel/c/memoryManagerBuddy.c`

### Concepto

La memoria total debe ser una potencia de 2. Si se inicializa con 8 MB (2^23), el buddy system maneja bloques de tamanos 2^5 (32B, el minimo), 2^6, ..., 2^23.

Se puede implementar con un **arbol binario implicito** (como un heap array). Cada nodo del arbol representa un bloque de memoria. Los hijos representan las dos mitades (buddies).

### Estructura de datos

Un enfoque comun es usar un **array de bits/estados** que mapea el arbol binario:

```
Nivel 0: [         bloque de 2^23 (8MB)         ]     -> indice 1
Nivel 1: [    2^22 (4MB)    ][    2^22 (4MB)    ]     -> indices 2, 3
Nivel 2: [ 2^21 ][ 2^21 ][ 2^21 ][ 2^21 ]            -> indices 4, 5, 6, 7
...
Nivel k: bloques de 2^(23-k)                           -> indices 2^k ... 2^(k+1)-1
```

Para un arbol de N niveles se necesitan `2^(N+1)` nodos. Cada nodo almacena el tamano maximo disponible en su subarbol.

```c
// El arbol: tree[i] = tamano maximo libre en el subarbol i
// Tamano total del array: 2 * (total_size / MIN_BLOCK_SIZE)
static uint64_t *tree;
static uint64_t total_size;
static void *base_address;

#define MIN_BLOCK_SIZE 32  // Bloque minimo: 32 bytes
```

### mm_init

1. Redondear `size` a la potencia de 2 inferior mas cercana.
2. Inicializar el arbol: cada nodo empieza con el tamano completo de su nivel.
   - `tree[1] = total_size`
   - `tree[2] = tree[3] = total_size / 2`
   - etc.

### mm_malloc(size)

1. Redondear `size` al siguiente potencia de 2 (minimo `MIN_BLOCK_SIZE`).
2. Buscar en el arbol desde la raiz hacia abajo:
   - Si `tree[nodo]` < size pedido, no hay espacio -> `NULL`.
   - Si el tamano del nodo es exactamente `size` y `tree[nodo] == size`, asignar este nodo (poner `tree[nodo] = 0`).
   - Si no, bajar al hijo con mas espacio disponible.
3. Propagar hacia arriba: cada padre actualiza su valor al maximo de sus dos hijos.
4. Calcular la direccion de memoria a partir del indice del nodo.

### mm_free(ptr)

1. Calcular el indice del nodo a partir de la direccion.
2. Restaurar su tamano original.
3. Propagar hacia arriba: si ambos buddies estan completamente libres, fusionar (el padre recupera su tamano completo).

### Calculo de direccion <-> indice

```c
// indice del nodo -> offset en memoria
offset = (indice - nodos_en_su_nivel) * tamano_bloque_del_nivel

// puntero -> indice
offset = ptr - base_address
indice = offset / tamano_bloque + nodos_en_nivel
```

### Nota sobre fragmentacion interna

Si se piden 33 bytes, el buddy system asigna 64 bytes (siguiente potencia de 2). Se desperdician 31 bytes. Esto es esperable y aceptable para el TP.

---

## 8. Compilacion condicional

### Modificar el Makefile del Kernel

El objetivo es que `make` compile con First Fit y `make buddy` compile con Buddy System.

En `Kernel/Makefile`, agregar:

```makefile
# Variable para elegir memory manager (por defecto: first fit)
MM ?= FF

ifeq ($(MM), BUDDY)
    MM_SRC = c/memoryManagerBuddy.c
    MM_FLAG = -DMM_BUDDY
else
    MM_SRC = c/memoryManagerFF.c
    MM_FLAG = -DMM_FF
endif

SOURCES=$(filter-out c/memoryManagerFF.c c/memoryManagerBuddy.c, $(wildcard c/*.c c/drivers/*.c)) $(MM_SRC)
```

Y agregar `$(MM_FLAG)` a los GCCFLAGS.

En el `Makefile` raiz, agregar un target:

```makefile
buddy:
    $(MAKE) MM=BUDDY all
```

Asi:
- `make` o `make all` -> compila con First Fit.
- `make buddy` -> compila con Buddy System.

### Alternativa con #ifdef en el codigo

Si preferis un solo archivo `.c`:

```c
#ifdef MM_BUDDY
    // implementacion buddy
#else
    // implementacion first fit
#endif
```

Pero es mas limpio tener dos archivos separados y elegir cual compilar desde el Makefile.

---

## 9. Agregar las syscalls de memoria

### 9.1 Agregar syscalls en el kernel

En `Kernel/c/syscallDispatcher.c`, agregar tres funciones nuevas:

```c
#include "memoryManager.h"

// Syscall 16: malloc
uint64_t sys_malloc(uint64_t size) {
    return (uint64_t)mm_malloc(size);
}

// Syscall 17: free
void sys_free(uint64_t ptr) {
    mm_free((void *)ptr);
}

// Syscall 18: mem status
void sys_mem_status(uint64_t statusPtr) {
    mm_status((MemStatus *)statusPtr);
}
```

Actualizar la tabla `syscalls[]`:

```c
#define CANT_SYS 19  // actualizar en defs.h (antes era 16)

void *syscalls[CANT_SYS] = {
    // ... las 16 existentes (0-15) ...
    &sys_malloc,       // 16
    &sys_free,         // 17
    &sys_mem_status,   // 18
};
```

**Importante**: actualizar `CANT_SYS` en `Kernel/include/defs.h` y tambien el `cmp rax, 16` en `interrupts.asm` (linea 273) para que acepte los nuevos numeros de syscall.

### 9.2 Agregar wrappers en userland

En `Userland/asm/userlib.asm`:

```nasm
GLOBAL sys_malloc
GLOBAL sys_free
GLOBAL sys_mem_status

sys_malloc:
    mov rax, 16
    int 0x80
    ret

sys_free:
    mov rax, 17
    int 0x80
    ret

sys_mem_status:
    mov rax, 18
    int 0x80
    ret
```

En `Userland/c/include/userlib.h`, declarar:

```c
void *sys_malloc(uint64_t size);
void sys_free(void *ptr);
void sys_mem_status(MemStatus *status);
```

(Tambien necesitas que userland conozca la struct `MemStatus`; podes crear un header compartido o duplicar la definicion en un header de userland.)

### 9.3 Inicializar el MM en el kernel

En `Kernel/c/kernel.c`, dentro de `initializeKernelBinary()` (despues de `load_idt()`):

```c
#include "memoryManager.h"

#define HEAP_START  ((void *)0x600000)
#define HEAP_SIZE   (8 * 1024 * 1024)

void *initializeKernelBinary(void) {
    // ... lo que ya existe ...
    loadModules(&endOfKernelBinary, moduleAddresses);
    clearBSS(&bss, &endOfKernel - &bss);
    load_idt();
    mm_init(HEAP_START, HEAP_SIZE);     // <<< NUEVO
    return getStackBase();
}
```

---

## 10. Integrar el test_mm

El test `test_mm` lo provee la catedra. Es un programa de userland que:

1. Pide bloques de tamano aleatorio con `malloc`.
2. Verifica que no se solapen.
3. Libera bloques aleatoriamente.
4. Repite en un ciclo infinito.
5. Solo imprime si detecta errores.

### Pasos para integrarlo

1. Obtener el codigo fuente del test de la catedra (suele estar en un repo aparte o en el campus).
2. Ubicarlo en `Userland/c/` o `Userland/tests/`.
3. Asegurarse de que use las funciones `sys_malloc` y `sys_free` de la interfaz de userland.
4. Registrarlo como un comando en la shell para poder ejecutarlo.
5. Compilar y ejecutar:
   - `make` -> correr `test_mm` -> no deberia imprimir errores.
   - `make buddy` -> correr `test_mm` -> idem (al menos con una implementacion debe pasar).

### Nota importante

El test se debe poder correr como **proceso de usuario** (no built-in de la shell) tanto en foreground como en background. Esto requiere tener primero el sistema de procesos (Paso 2). Sin embargo, podes testear el memory manager de forma aislada antes (llamando a malloc/free directamente desde la shell como built-in y migrarlo a proceso despues).

---

## 11. Checklist final

- [ ] Header `memoryManager.h` creado con interfaz comun (`mm_init`, `mm_malloc`, `mm_free`, `mm_status`).
- [ ] Implementacion First Fit (`memoryManagerFF.c`) completa y funcionando.
- [ ] Implementacion Buddy System (`memoryManagerBuddy.c`) completa y funcionando.
- [ ] Makefile modificado: `make` compila con FF, `make buddy` compila con Buddy.
- [ ] `CANT_SYS` actualizado en `defs.h` (de 16 a 19+).
- [ ] Limite de syscalls actualizado en `interrupts.asm` (`cmp rax, 19` o mas).
- [ ] Syscalls `sys_malloc` (16), `sys_free` (17), `sys_mem_status` (18) agregadas en `syscallDispatcher.c` y en la tabla `syscalls[]`.
- [ ] Wrappers ASM agregados en `userlib.asm`.
- [ ] Declaraciones agregadas en `userlib.h` de userland.
- [ ] `mm_init()` llamado en `initializeKernelBinary()` de `kernel.c`.
- [ ] `test_mm` integrado y pasa sin errores con al menos un MM.
- [ ] Compila con `-Wall` sin warnings.
