# Informe: MVar — Migración de Syscall Kernel a Aplicación de Userland

> Trabajo Práctico Nº 2 — Sistemas Operativos (ITBA)
> Sesión de corrección sobre el diseño de syscalls dedicadas a MVar.

---

## Tabla de contenidos

1. [El problema: syscalls dedicadas a MVar](#1-el-problema-syscalls-dedicadas-a-mvar)
2. [Por qué es un mal diseño](#2-por-qué-es-un-mal-diseño)
3. [La solución: MVar como aplicación de userland](#3-la-solución-mvar-como-aplicación-de-userland)
4. [Resumen detallado de cambios](#4-resumen-detallado-de-cambios)
5. [Proceso de decisión y fundamentación](#5-proceso-de-decisión-y-fundamentación)
6. [Por qué este diseño soluciona el problema](#6-por-qué-este-diseño-soluciona-el-problema)
7. [Cumplimiento de la consigna de MVar](#7-cumplimiento-de-la-consigna-de-mvar)
8. [Verificación](#8-verificación)

---

## 1. El problema: syscalls dedicadas a MVar

### El diseño anterior

El kernel tenía 4 syscalls dedicadas exclusivamente a MVar:

```c
int64_t sys_mvar_create(const char *name)  // syscall 40
int64_t sys_mvar_put(const char *name, char value)   // syscall 41
int64_t sys_mvar_take(const char *name)   // syscall 42
int64_t sys_mvar_destroy(const char *name)  // syscall 43
```

Estas syscalls eran thin wrappers que delegaban directamente a la implementación del kernel (`mvar_create`, `mvar_put`, etc. en `Kernel/c/syscall/mvar.c`). La tabla de MVars, las colas de espera, el spinlock y toda la lógica vivían en el kernel.

### La corrección de la cátedra

> **Mal diseño de syscall:** Se tienen syscalls dedicadas a Mvar mientras debería ser una aplicación de espacio de usuario que haga uso de las otras syscalls solicitadas.

El corrector rechazó el diseño de syscalls dedicadas. MVar debe ser una **aplicación de espacio de usuario** construida sobre las syscalls generales (semáforos), no una primitiva del kernel con su propia interfaz de syscall.

---

## 2. Por qué es un mal diseño

### 2.1 Violación del principio de minimalismo del kernel

El kernel debe exponer un conjunto **mínimo** de primitivas generales que permitan construir mecanismos de sincronización más complejos en userland. Los semáforos (`sem_open`/`sem_wait`/`sem_post`/`sem_close`) son esa primitiva general. MVar es un patrón de sincronización de **nivel superior** (un buffer de tamaño 1) que se puede construir perfectamente sobre semáforos. Tener syscalls dedicadas a MVar es **redundante**: agrega 4 entradas a la tabla de syscalls, 4 wrappers en assembler, 4 funciones en el kernel, y toda una maquinaria de colas + spinlock + cleanup que duplica lo que los semáforos ya proveen.

### 2.2 Acoplamiento innecesario kernel ↔ userland

Cada syscall dedicada crea un **contrato** entre el kernel y userland. Si se cambia la semántica de MVar (por ejemplo, el valor de retorno, el comportamiento ante destrucción, el manejo de errores), hay que modificar **ambos** lados: el kernel y el wrapper de userland. Con el diseño de userland sobre semáforos, MVar es una biblioteca de userland que puede evolucionar sin tocar el kernel.

### 2.3 Contaminación de la tabla de syscalls

La tabla de syscalls es un recurso **finito y compartido**. `CANT_SYS` debe sincronizarse entre `defs.h` (C), `syscallDispatcher.c` (tabla), e `interrupts.asm` (bounds check hardcoded). Cada syscall nueva aumenta el riesgo de desincronización. Las 4 syscalls de MVar consumían 4 entradas (índices 40-43) que ahora se liberan.

### 2.4 Duplicación de mecanismos de sincronización

El kernel ya tenía semáforos con:
- Spinlock con `atomic_xchg` para atomicidad.
- Colas de espera con bloqueo/desbloqueo.
- Cleanup al morir un proceso (`sem_cleanup_for_process`).
- Transferencia de recurso via `held_sems`.

La implementación de MVar en el kernel **duplicaba** toda esta infraestructura: su propio spinlock, sus propias colas (`wq`/`rq`), su propio cleanup (`mvar_cleanup_for_process`), su propio handoff con `rsp[14]`. Es código que mantiene el kernel pero que podría vivir en userland usando los semáforos como primitiva base.

### 2.5 Inconsistencia con el enunciado

El enunciado pide semáforos como syscall y MVar como **aplicación** que los usa. El diseño de syscalls dedicadas desobedecía explícitamente esta indicación.

---

## 3. La solución: MVar como aplicación de userland

### 3.1 El patrón productor-consumidor con dos semáforos

Una MVar es un **buffer de tamaño 1**. El patrón clásico para implementarlo con semáforos es productor-consumidor:

```
sem_empty (inicial 1): cuenta slots vacíos. Escritores esperan aquí.
sem_full  (inicial 0): cuenta valores disponibles. Lectores esperan aquí.
```

| Operación | Implementación |
|---|---|
| `put(value)` | `sem_wait(empty)` → `mv->value = value` → `sem_post(full)` |
| `take()` | `sem_wait(full)` → `val = mv->value` → `sem_post(empty)` → return `val` |

**Por qué garantiza exclusión mutua:** el buffer tiene 1 slot. `sem_empty` empieza en 1 (un slot vacío). Cuando un escritor hace `put`, decrementa `empty` a 0 — ningún otro escritor puede entrar hasta que un lector haga `take` e incremente `empty` de vuelta. Idem para lectores: `sem_full` empieza en 0, así que el primer lector se bloquea hasta que un escritor haga `post(full)`. Sólo un proceso accede a `mv->value` a la vez.

### 3.2 Shared state sin paging

El kernel no tiene paging: todos los procesos comparten el mismo address space. Una **global** en `mvar.c` (userland) es visible para todos los procesos (writers, readers, shell). La tabla de MVars es:

```c
static user_mvar_t umvar_table[MAX_USER_MVARS];
```

Siendo `static`, no es accesible desde otros archivos `.c`, pero **sí** es compartida entre procesos (todos ejecutan el mismo binario `0000-sampleCodeModule.bin` en `0x400000`). No hay copia-on-write ni nada por el estilo: es la misma memoria física.

### 3.3 Nombres de semáforos

Cada MVar genera dos semáforos con nombres derivados del nombre del MVar:
- `<mvarname>e` — empty slots (inicial 1)
- `<mvarname>f` — full slots (inicial 0)

Con `mvar_name = "mvar_42"` (8 chars), los nombres son `"mvar_42e"` (9 chars) y `"mvar_42f"` (9 chars) — bien dentro del límite de 32 chars de `SEM_NAME_LEN`. El nombre del MVar incluye el PID del creador, así que no hay colisión entre MVars de distintos procesos.

### 3.4 Cleanup al morir

No se necesita `mvar_cleanup_for_process`. Si un proceso muere mientras está bloqueado en `sem_wait` sobre `<name>e` o `<name>f`, el `sem_cleanup_for_process` del kernel (corregido en la primera sesión) lo remueve de la cola del semáforo y compensa el `value`. Los semáforos quedan en un estado consistente y otros procesos pueden seguir usándolos.

---

## 4. Resumen detallado de cambios

### 4.1 Eliminación del MVar del kernel (8 archivos)

| Archivo | Cambio |
|---|---|
| `Kernel/c/syscall/mvar.c` | **Eliminado** del repo |
| `Kernel/include/mvar.h` | **Eliminado** del repo |
| `Kernel/c/kernel/kernel.c` | Eliminado `#include "mvar.h"` y `mvar_init()` |
| `Kernel/c/process/process.c` | Eliminado `#include "mvar.h"` y `mvar_cleanup_for_process(pid)` |
| `Kernel/c/syscall/syscallDispatcher.c` | Eliminado `#include "mvar.h"`, las 4 funciones `sys_mvar_*`, y las 4 entradas de `syscalls[]` (índices 40-43) |
| `Kernel/include/syscallDispatcher.h` | Eliminadas las 4 declaraciones `sys_mvar_*` |
| `Kernel/include/defs.h` | `CANT_SYS` cambiado de 44 a 40 |
| `Kernel/asm/interrupts.asm` | `cmp rax, 44` cambiado a `cmp rax, 40` |

### 4.2 Implementación de MVar en userland (3 archivos)

| Archivo | Cambio |
|---|---|
| `Userland/c/commands/mvar.c` | **Reescrito completo**: tabla global `umvar_table`, funciones `user_mvar_create`/`put`/`take`/`destroy` usando `sem_open`/`sem_wait`/`sem_post`/`sem_close`. `mvar_writer` y `mvar_reader` llaman a las funciones userland. `mvar()` unchanged en su estructura (crea MVar + spawnea hijos + return 0) |
| `Userland/asm/userlib.asm` | Eliminadas las 4 rutinas `sys_mvar_create`/`put`/`take`/`destroy` (GLOBAL + implementación) |
| `Userland/c/include/syscall.h` | Eliminadas las 4 declaraciones `sys_mvar_*` |
| `Userland/c/include/test_util.h` | Agregadas las declaraciones de `user_mvar_create`/`put`/`take`/`destroy` (para `-Wmissing-prototypes`) |

### 4.3 Documentación

| Archivo | Cambio |
|---|---|
| `AGENTS.md` | Actualizado: `CANT_SYS=40`, `cmp rax, 40`, nueva sección "MVar is NOT a syscall", invariants de MVar reescritos para reflejar el diseño userland |

---

## 5. Proceso de decisión y fundamentación

### 5.1 Por qué semáforos y no otra primitiva

El enunciado pide semáforos como syscall. Los semáforos son la primitiva de sincronización **más general**: pueden implementar mutexes, barriers, rendezvous, y por supuesto buffers de tamaño N (de los cuales MVar es el caso N=1). Usar semáforos como base de MVar es la decisión más natural y la que el enunciado espera.

### 5.2 Por qué la tabla es una global compartida

En un sistema con paging, cada proceso tendría su propio address space y una global de userland **no** sería compartida. Habría que usar memoria compartida (shared memory) o un mecanismo similar. Pero este kernel **no tiene paging**: todos los procesos ven la misma memoria física. Una global `static` en `mvar.c` es compartida entre todos los procesos que ejecutan el mismo binario. Es el mecanismo más simple y correcto para este kernel.

### 5.3 Por qué no hay race condition en `umvar_table`

`mvar_create` es llamado por el proceso padre **antes** de spawnear los writers/readers. En ese momento, no hay otros procesos accediendo a la tabla. Después del spawn, los hijos sólo hacen `find_umvar` (read-only) y acceden a `mv->value` (protegido por semáforos). No hay window donde dos procesos escriban la tabla concurrentemente.

### 5.4 Por qué no se necesita cleanup de MVar al morir

Si un writer muere mientras está bloqueado en `sem_wait("<name>e")`:
1. `process_kill` llama `sem_cleanup_for_process(pid)`.
2. `sem_cleanup_for_process` itera todos los semáforos abiertos.
3. Encuentra `<name>e`, ve que el PID estaba en la cola de espera, lo remueve, y compensa `value++` (deshace el decremento que hizo `sem_wait` antes de bloquearlo).
4. El semáforo `<name>e` vuelve a su valor correcto. Otros writers pueden progresar.

Idem si un reader muere bloqueado en `sem_wait("<name>f")`. El kernel ya maneja esto correctamente gracias a la corrección de semáforos (bitmap `held_sems` + iteración completa). No hay necesidad de un `mvar_cleanup_for_process` en userland.

### 5.5 Qué se pierde vs. la implementación en kernel

| Feature | Kernel MVar (anterior) | Userland MVar (nuevo) |
|---|---|---|
| Desbloqueo por prioridad + cooldown | `wq_pop_highest`/`rq_pop_highest` | FIFO del semáforo (fair) |
| Handoff con `rsp[14]` | Sí (sin retry, valor directo en RAX) | No — pero `sem_wait` bloquea limpio (no hay retry ni `-2`) |
| Anti-starvation de escritores | cooldown del MVar | Scheduler aging + FIFO del semáforo |
| Atomicidad | spinlock propio en kernel | spinlock del semáforo (en kernel) |
| Syscalls dedicadas | 4 (índices 40-43) | 0 (usa sem_open/wait/post/close) |

El desbloqueo FIFO del semáforo es **fair** (no hay starvation: todos los procesos esperan en cola y se despiertan en orden de llegada). Combinado con el aging del scheduler, todos los procesos eventualmente progresan. Es el diseño correcto para una primitiva de userland: simple, correcto, y reutiliza la infraestructura existente.

---

## 6. Por qué este diseño soluciona el problema

### 6.1 Elimina las syscalls dedicadas

Las 4 syscalls `sys_mvar_*` desaparecen por completo. `CANT_SYS` baja de 44 a 40. La tabla de syscalls, los wrappers en assembler, y las funciones del kernel se eliminan. El kernel queda más pequeño y más limpio.

### 6.2 MVar es una aplicación de userland

MVar ahora vive en `Userland/c/commands/mvar.c` como una biblioteca de funciones (`user_mvar_create`/`put`/`take`/`destroy`) construida sobre `sem_open`/`sem_wait`/`sem_post`/`sem_close`. El kernel no sabe que MVar existe; sólo provee semáforos. Esto es exactamente lo que el corrector pidió.

### 6.3 Reutiliza la infraestructura de semáforos

El spinlock, las colas de espera, el bloqueo/desbloqueo, y el cleanup al morir un proceso ya están implementados y corregidos en los semáforos del kernel. MVar los reutiliza sin duplicar nada.

### 6.4 El kernel queda más mantenible

- Menos código en el kernel (se eliminó `mvar.c` completo, ~480 líneas).
- Menos syscalls que sincronizar (`CANT_SYS`, `cmp rax`, tabla).
- Menos invariants que preservar (no hay `mvar_cleanup_for_process`, no hay handoff con `rsp[14]` para MVar, no hay `mvar_q_entry`).
- Separación de responsabilidades: el kernel provee primitivas, userland construye mecanismos.

---

## 7. Cumplimiento de la consigna de MVar

La consigna dice:

> mvar: Implementa el problema de múltiples lectores y escritores sobre una variable global, similar a una MVar de Haskell. [...] De esta forma, se simula el comportamiento de una MVar, garantizando que solo un proceso accede a la variable a la vez y que los accesos están correctamente sincronizados. El proceso principal debe terminar inmediatamente después de crear los lectores y escritores.

| Requisito | Cumple | Cómo |
|---|---|---|
| "espera a que la variable esté vacía y escribe" | ✓ | `sem_wait(empty)` bloquea si FULL; al despertar, escribe |
| "espera a que la variable tenga un valor y lo consume" | ✓ | `sem_wait(full)` bloquea si EMPTY; al despertar, lee |
| "solo un proceso accede a la variable a la vez" | ✓ | Buffer de tamaño 1 + semáforos: exclusión mutua implícita |
| "acceses correctamente sincronizados" | ✓ | Protocolo sem_wait/sem_post del kernel (con spinlock) |
| "espera activa aleatoria" | ✓ | `random_busy_wait()` antes de put/take |
| "valor único ('A', 'B', 'C'...)" | ✓ | Cada writer escribe `'A' + idx` |
| "consume e imprime con identificador (color)" | ✓ | Reader imprime con `sys_write_color` |
| "proceso principal termina inmediatamente" | ✓ | `mvar()` crea MVar + spawnea hijos + `return 0` |
| **"aplicación de espacio de usuario"** | ✓ | **Ahora sí**: userland sobre semáforos, sin syscalls dedicadas |

---

## 8. Verificación

### Compilación

Se compiló con `./compile.sh` (First-Fit) y `MM=BUDDY ./compile.sh` (Buddy System), ambos dentro del contenedor Docker `TP_SO_2`. En ambos casos el build pasó con **cero warnings** bajo `-Wall -Wextra -Werror` + banderas estrictas completas.

### Pruebas sugeridas en runtime (QEMU)

| Comando | Resultado esperado |
|---|---|
| `mvar 2 2` | Letras `A`, `B` se imprimen en colores rotativos; no se pierden ni duplican valores |
| `mvar 5 3` | 5 escritores (A-E), 3 lectores; flujo continuo de letras en colores |
| `mvar 1 1` | Un escritor, un lector; alternancia estricta A A A... |
| `kill <pid-writer>` mientras corre `mvar` | El MVar no se deadlocka; `sem_cleanup_for_process` compensa el semáforo; otros writers/readers siguen |
| `test_sync 100 1` | `Valor final: 0` (el cambio no afecta semáforos) |
| `test_prio 1000000` | Los 3 procesos terminan (el cambio no afecta el scheduler) |
| `ps` | Lista de procesos consistente |

### Archivos modificados

| Archivo | Cambio |
|---|---|
| `Kernel/c/syscall/mvar.c` | **Eliminado** |
| `Kernel/include/mvar.h` | **Eliminado** |
| `Kernel/c/kernel/kernel.c` | -include, -mvar_init() |
| `Kernel/c/process/process.c` | -include, -mvar_cleanup_for_process() |
| `Kernel/c/syscall/syscallDispatcher.c` | -include, -sys_mvar_* (4 funciones), -4 entradas de tabla |
| `Kernel/include/syscallDispatcher.h` | -sys_mvar_* (4 declaraciones) |
| `Kernel/include/defs.h` | CANT_SYS 44 → 40 |
| `Kernel/asm/interrupts.asm` | cmp rax 44 → 40 |
| `Userland/c/commands/mvar.c` | Reescrito: user_mvar_create/put/take/destroy sobre semáforos |
| `Userland/asm/userlib.asm` | -sys_mvar_* (4 rutinas) |
| `Userland/c/include/syscall.h` | -sys_mvar_* (4 declaraciones) |
| `Userland/c/include/test_util.h` | +user_mvar_* (4 declaraciones) |
| `AGENTS.md` | CANT_SYS=40, MVar es userland, invariants actualizados |

### Reducción de código

- **Kernel**: ~480 líneas eliminadas (`mvar.c` + `mvar.h` + referencias).
- **Syscalls**: 4 menos (índices 40-43 liberados).
- **Userland**: `mvar.c` reescrito a ~280 líneas, pero usando infraestructura existente (semáforos) en lugar de duplicarla.
