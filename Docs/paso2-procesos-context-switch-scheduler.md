# Paso 2 - Procesos, Context Switch y Scheduler: Teoria e Implementacion

## Indice

1. [Contexto: que hay actualmente](#1-contexto-que-hay-actualmente)
2. [Que se pide](#2-que-se-pide)
3. [Teoria: multitasking y procesos](#3-teoria-multitasking-y-procesos)
4. [El PCB (Process Control Block)](#4-el-pcb-process-control-block)
5. [La tabla de procesos](#5-la-tabla-de-procesos)
6. [Context Switch en ASM](#6-context-switch-en-asm)
7. [El Scheduler: Round Robin con prioridades](#7-el-scheduler-round-robin-con-prioridades)
8. [El proceso idle](#8-el-proceso-idle)
9. [Adaptar el kernel al modelo multiproceso](#9-adaptar-el-kernel-al-modelo-multiproceso)
10. [Syscalls de procesos](#10-syscalls-de-procesos)
11. [Checklist final](#11-checklist-final)

---

## 1. Contexto: que hay actualmente

El kernel actual es **single-tasking**: tiene un unico flujo de ejecucion. Al arrancar, `kernel.c::main()` salta directamente a la direccion `0x400000` y la shell toma el control para siempre. No existe el concepto de proceso, ni de scheduler, ni de cambio de contexto.

```
Flujo actual:
  Pure64 -> kernel.c::initializeKernelBinary() -> jmp 0x400000 -> [shell corre forever]
```

Los IRQs (timer, teclado) interrumpen la shell momentaneamente, ejecutan su handler, y devuelven el control a exactamente donde se estaba.

El timer (IRQ0 a 18.2 Hz) actualmente solo incrementa un contador de ticks en `time.c`. No toma ninguna decision de scheduling.

Para este paso hay que transformar ese modelo en uno donde **multiples procesos coexisten**, el timer es el motor que los desaloja periodicamente, y un scheduler decide quien corre a continuacion.

---

## 2. Que se pide

1. **PCB** con al menos: PID, nombre, prioridad, estado (READY/RUNNING/BLOCKED/ZOMBIE), RSP guardado, foreground/background, file descriptors, puntero al padre, argc/argv.
2. **Tabla de procesos**: array de PCBs con un maximo definido.
3. **Context switch** en ASM: activado por el timer (IRQ0), intercambia el RSP entre procesos.
4. **Scheduler Round Robin con prioridades**: la prioridad determina cuantos quantums consecutivos recibe un proceso.
5. **Proceso idle**: siempre listo, ejecuta `hlt` en loop, corre cuando no hay nadie mas.
6. **Syscalls**: `create_process`, `exit`, `getpid`, `ps`, `kill`, `nice`, `block`, `unblock`, `yield`, `waitpid`.
7. **Pasar los tests** `test_proc` y `test_prio` de la catedra.

---

## 3. Teoria: multitasking y procesos

### Que es un proceso

Un proceso es la **instancia en ejecucion de un programa**. Tiene:
- Su propio flujo de instrucciones (RIP).
- Su propio stack.
- Su propio conjunto de registros.
- Estado: que tan listo esta para correr.

Varios procesos comparten el mismo espacio de memoria fisica (no hay memoria virtual en este TP), pero cada uno tiene su stack separado.

### Multitasking preemptivo

El kernel interrumpe al proceso actual periodicamente (via el timer) sin que este tenga que hacer nada. Eso es **preemptivo**, a diferencia del cooperativo donde el proceso tiene que ceder voluntariamente.

```
Proceso A corre  ---->|  timer  |----> Proceso B corre  ---->|  timer  |----> Proceso A corre
                      ^ IRQ0 dispara context switch          ^ IRQ0 dispara context switch
```

### El quantum

Un **quantum** es la unidad de tiempo de CPU que recibe un proceso antes de ser posiblemente desalojado. En este kernel, un quantum = un tick del timer (1/18.2 segundos).

Con Round Robin puro, todos reciben 1 quantum. Con prioridades, un proceso de prioridad P recibe P quantums antes de ceder.

### Que hace el context switch

El context switch es el mecanismo que **guarda el estado del proceso actual y restaura el de otro**. El "estado" de un proceso es la foto de todos sus registros en el momento en que fue interrumpido.

```
Estado proceso A (guardado en su stack):
  R15, R14, ..., RAX, RIP, CS, RFLAGS, RSP, SS

Estado proceso B (guardado en su stack):
  R15, R14, ..., RAX, RIP, CS, RFLAGS, RSP, SS
```

La clave: **guardar el stack pointer del proceso actual y cargar el del siguiente**. Todo lo demas (los registros) ya esta en el stack.

---

## 4. El PCB (Process Control Block)

### Archivo: `Kernel/include/process.h`

```c
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define MAX_PROCESSES   64
#define STACK_SIZE      (4 * 1024)   // 4 KB por proceso
#define MAX_NAME_LEN    32
#define MIN_PRIORITY    1
#define MAX_PRIORITY    5
#define DEFAULT_PRIORITY 3

typedef enum {
    READY,
    RUNNING,
    BLOCKED,
    ZOMBIE
} ProcessState;

typedef struct PCB {
    uint64_t       pid;
    char           name[MAX_NAME_LEN];
    uint8_t        priority;          // 1 (minima) a 5 (maxima)
    uint8_t        remaining_quanta;  // quantums que le quedan en el turno actual
    ProcessState   state;
    uint64_t      *stack_base;        // base del stack asignado (para liberar)
    uint64_t      *rsp;               // RSP guardado (valido cuando no esta RUNNING)
    uint8_t        foreground;        // 1 si es foreground
    int            fd[2];             // fd[0]=stdin, fd[1]=stdout
    struct PCB    *parent;            // proceso padre (para waitpid)
    int            argc;
    char         **argv;
    int            retval;            // valor de retorno (cuando ZOMBIE)
} PCB;

// Gestion de procesos
int      process_create(const char *name, void (*entry)(int, char**),
                        int argc, char **argv, uint8_t fg);
void     process_exit(int retval);
PCB     *process_get(uint64_t pid);
PCB     *process_current(void);
void     process_kill(uint64_t pid);
void     process_block(uint64_t pid);
void     process_unblock(uint64_t pid);
void     process_nice(uint64_t pid, uint8_t new_priority);
int      process_waitpid(uint64_t pid);
void     process_ps(void *buf);        // llena buffer con lista de procesos

#endif
```

### Campos importantes explicados

**`rsp`**: cuando el proceso no esta corriendo, aqui se guarda el RSP en el momento en que fue interrumpido. El context switch escribe y lee este campo. Es el unico campo que el codigo ASM toca directamente.

**`stack_base`**: puntero al bloque de memoria asignado con `mm_malloc` para el stack de este proceso. Se usa en `process_exit` para liberar la memoria.

**`remaining_quanta`**: cuantos ticks mas puede correr antes de ser desalojado. Se inicializa con `priority` y se decrementa en cada tick. Cuando llega a 0, se resetea y se cede el CPU.

**`fd[2]`**: por ahora stdin=0 (teclado) y stdout=1 (pantalla). En el Paso 4 (pipes), estos podran apuntar a extremos de pipes.

---

## 5. La tabla de procesos

### Archivo: `Kernel/c/process.c`

```c
#include "process.h"
#include "memoryManager.h"
#include "scheduler.h"

static PCB process_table[MAX_PROCESSES];
static uint64_t next_pid = 0;
static PCB *current = NULL;

PCB *process_current(void) {
    return current;
}

// Llamado por el scheduler para cambiar el proceso actual
void process_set_current(PCB *pcb) {
    if (current != NULL && current->state == RUNNING)
        current->state = READY;
    current = pcb;
    current->state = RUNNING;
}
```

### Crear un proceso: `process_create`

La funcion mas importante. Tiene que:

1. Encontrar un slot libre en `process_table`.
2. Asignar un stack con `mm_malloc(STACK_SIZE)`.
3. Armar el **stack frame inicial** (ver seccion 6.3).
4. Rellenar los campos del PCB.
5. Marcar el proceso como READY para que el scheduler lo considere.

```c
int process_create(const char *name, void (*entry)(int, char**),
                   int argc, char **argv, uint8_t fg) {
    // Buscar slot libre
    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i].state == ZOMBIE || process_table[i].pid == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;   // tabla llena

    PCB *p = &process_table[slot];

    // Asignar stack
    p->stack_base = (uint64_t *)mm_malloc(STACK_SIZE);
    if (p->stack_base == NULL) return -1;

    // Armar frame inicial (ver seccion 6.3)
    p->rsp = build_initial_stack(
        (void *)((uint8_t *)p->stack_base + STACK_SIZE),
        entry, argc, argv
    );

    // Llenar PCB
    p->pid              = ++next_pid;
    p->priority         = DEFAULT_PRIORITY;
    p->remaining_quanta = DEFAULT_PRIORITY;
    p->state            = READY;
    p->foreground       = fg;
    p->fd[0]            = 0;   // stdin: teclado
    p->fd[1]            = 1;   // stdout: pantalla
    p->parent           = current;
    p->argc             = argc;
    p->argv             = argv;
    p->retval           = 0;

    // Copiar nombre
    int i = 0;
    while (name[i] && i < MAX_NAME_LEN - 1) { p->name[i] = name[i]; i++; }
    p->name[i] = '\0';

    scheduler_add(p);
    return (int)p->pid;
}
```

### Destruir un proceso: `process_exit`

```c
void process_exit(int retval) {
    current->retval = retval;
    current->state  = ZOMBIE;

    // Despertar al padre si esta esperando en waitpid
    if (current->parent != NULL && current->parent->state == BLOCKED)
        process_unblock(current->parent->pid);

    // Liberar stack
    mm_free(current->stack_base);
    current->stack_base = NULL;

    // Ceder CPU (nunca retorna a este proceso)
    scheduler_yield();
}
```

---

## 6. Context Switch en ASM

### 6.1 Como funciona actualmente el IRQ0

El handler actual del timer (IRQ0) usa el macro `irqHandlerMaster`:

```nasm
%macro irqHandlerMaster 1
    pushState           ; guarda los 15 registros GP en el stack
    mov rdi, %1         ; IRQ number como arg
    call irqDispatcher  ; llama a C (timer_handler, etc.)
    mov al, 20h
    out 20h, al         ; EOI al PIC
    popState            ; restaura los 15 registros GP
    iretq               ; restaura RIP, CS, RFLAGS, RSP, SS (desde el stack)
%endmacro

_irq00Handler:
    irqHandlerMaster 0
```

### 6.2 Layout del stack tras una interrupcion

Cuando el hardware de x86-64 toma una interrupcion, la CPU empuja automaticamente (de mayor a menor direccion de memoria):

```
[alta dir]
  SS            <- segmento de pila (si hay cambio de privilegio)
  RSP           <- stack pointer del proceso interrumpido
  RFLAGS        <- flags del proceso
  CS            <- segmento de codigo
  RIP           <- proxima instruccion del proceso
[baja dir]  <- RSP apunta aca al entrar al handler
```

Luego `pushState` empuja los 15 registros GP. Al terminar `pushState`, el stack queda:

```
[RSP + 0*8 ] = R15   (ultimo en pusharse)
[RSP + 1*8 ] = R14
[RSP + 2*8 ] = R13
[RSP + 3*8 ] = R12
[RSP + 4*8 ] = R11
[RSP + 5*8 ] = R10
[RSP + 6*8 ] = R9
[RSP + 7*8 ] = R8
[RSP + 8*8 ] = RSI
[RSP + 9*8 ] = RDI
[RSP + 10*8] = RBP
[RSP + 11*8] = RDX
[RSP + 12*8] = RCX
[RSP + 13*8] = RBX
[RSP + 14*8] = RAX   (primero en pusharse)
--- frame de hardware (empujado por la CPU) ---
[RSP + 15*8] = RIP del proceso interrumpido
[RSP + 16*8] = CS
[RSP + 17*8] = RFLAGS
[RSP + 18*8] = RSP del proceso interrumpido
[RSP + 19*8] = SS
```

**Conclusion clave**: el RSP actual (dentro del handler, despues de pushState) es la "foto completa" del proceso interrumpido. Guardarlo en el PCB es suficiente para poder retomar ese proceso despues.

### 6.3 Armar el stack inicial de un proceso nuevo

Para que el scheduler pueda despachar un proceso nuevo la primera vez, su PCB debe tener un `rsp` que apunte a un stack armado exactamente como si hubiera sido interrumpido. La funcion C `build_initial_stack` hace eso:

```c
// En process.c
// Arma un frame inicial para que popState + iretq ejecuten entry(argc, argv)
static uint64_t *build_initial_stack(void *stack_top,
                                     void (*entry)(int, char **),
                                     int argc, char **argv) {
    uint64_t *sp = (uint64_t *)stack_top;

    // Frame de hardware (iretq consume estos 5 valores)
    *(--sp) = 0x0;                   // SS
    *(--sp) = (uint64_t)stack_top;   // RSP (stack del proceso)
    *(--sp) = 0x202;                 // RFLAGS: IF=1
    *(--sp) = 0x8;                   // CS (kernel code segment)
    *(--sp) = (uint64_t)entry;       // RIP: punto de entrada

    // Registros GP (mismo orden que pushState: rax primero, r15 ultimo)
    *(--sp) = 0;           // RAX
    *(--sp) = 0;           // RBX
    *(--sp) = 0;           // RCX
    *(--sp) = 0;           // RDX
    *(--sp) = 0;           // RBP
    *(--sp) = argc;        // RDI (1er arg System V AMD64)
    *(--sp) = (uint64_t)argv; // RSI (2do arg)
    *(--sp) = 0;           // R8
    *(--sp) = 0;           // R9
    *(--sp) = 0;           // R10
    *(--sp) = 0;           // R11
    *(--sp) = 0;           // R12
    *(--sp) = 0;           // R13
    *(--sp) = 0;           // R14
    *(--sp) = 0;           // R15 (ultimo en pushState -> primero en popState)

    return sp;  // <- este valor se guarda en pcb->rsp
}
```

Cuando el scheduler por primera vez despache este proceso, `popState` cargara los registros y `iretq` saltara a `entry` con RDI=argc y RSI=argv.

### 6.4 El nuevo handler de IRQ0 con context switch

Reemplazar el `irqHandlerMaster 0` del handler del timer por un handler dedicado que:
1. Guarda el RSP del proceso actual en su PCB.
2. Llama al scheduler (en C) para obtener el RSP del siguiente proceso.
3. Carga ese nuevo RSP.

#### En `interrupts.asm`:

Agregar la declaracion extern del scheduler y reescribir `_irq00Handler`:

```nasm
EXTERN scheduler_tick   ; nueva funcion C que decide quien corre

_irq00Handler:
    pushState

    ; Pasar RSP actual al scheduler. Retorna el RSP del proceso a ejecutar.
    mov rdi, rsp
    call scheduler_tick
    mov rsp, rax        ; cargar RSP del proximo proceso

    mov al, 20h
    out 20h, al         ; EOI al PIC master

    popState
    iretq
```

#### En `scheduler.c`:

```c
// scheduler_tick: llamado desde ASM en cada tick del timer.
// Recibe el RSP actual, guarda en el PCB y retorna el RSP del siguiente proceso.
uint64_t *scheduler_tick(uint64_t *current_rsp) {
    if (current_process != NULL) {
        current_process->rsp = current_rsp;  // guardar estado

        if (current_process->remaining_quanta > 0)
            current_process->remaining_quanta--;

        if (current_process->remaining_quanta == 0) {
            if (current_process->state == RUNNING)
                current_process->state = READY;
            current_process->remaining_quanta = current_process->priority;
        }
    }

    PCB *next = scheduler_next();      // elegir siguiente proceso READY
    next->state = RUNNING;
    current_process = next;
    return next->rsp;                  // el handler cargara este RSP
}
```

**Importante**: `scheduler_tick` debe ser llamada con interrupciones deshabilitadas (ya lo son: estamos dentro de un handler de interrupcion). No hay race condition.

---

## 7. El Scheduler: Round Robin con prioridades

### Archivo: `Kernel/c/scheduler.c`

### Algoritmo

Round Robin puro asigna 1 quantum a cada proceso en forma circular. Con prioridades, un proceso de prioridad P recibe P quantums seguidos antes de ceder (si sigue READY):

```
Prioridad 1 -> 1 quantum  (minima)
Prioridad 3 -> 3 quantums (default)
Prioridad 5 -> 5 quantums (maxima)
```

### Estructura de datos

Una lista circular de punteros a PCBs READY. El scheduler mantiene un puntero al ultimo proceso elegido para saber desde donde continuar el round robin.

```c
#include "process.h"
#include "scheduler.h"

static PCB *run_queue[MAX_PROCESSES];
static int  queue_size = 0;
static int  queue_idx  = 0;         // indice del ultimo despachado
static PCB *current_process = NULL;

void scheduler_add(PCB *p) {
    if (queue_size < MAX_PROCESSES)
        run_queue[queue_size++] = p;
}

void scheduler_remove(PCB *p) {
    for (int i = 0; i < queue_size; i++) {
        if (run_queue[i] == p) {
            run_queue[i] = run_queue[--queue_size];
            return;
        }
    }
}

// Elegir el siguiente proceso READY (round robin)
PCB *scheduler_next(void) {
    if (queue_size == 0)
        return idle_process;   // nunca deberia llegar aqui si idle existe

    for (int i = 0; i < queue_size; i++) {
        queue_idx = (queue_idx + 1) % queue_size;
        PCB *candidate = run_queue[queue_idx];
        if (candidate->state == READY || candidate->state == RUNNING)
            return candidate;
    }

    return idle_process;  // solo idle esta disponible
}

// Ceder voluntariamente el CPU (usado por sys_yield y sys_exit)
void scheduler_yield(void) {
    // Forzar un context switch inmediato via software
    // Esto se puede implementar con una interrupcion por software o
    // decrementando remaining_quanta a 0 y esperando el proximo tick.
    // La forma mas limpia: llamar scheduler_tick con el RSP actual.
    // En ASM: (ver sys_yield en syscallDispatcher)
    asm volatile("int $0x20");  // dispara IRQ0 por software (si el PIC lo permite)
    // Alternativa: simplemente poner remaining_quanta = 0 y esperar el proximo tick
}
```

### Integracion con irqDispatcher

En `irqDispatcher.c`, el timer ya no necesita hacer nada especial porque el context switch ocurre directamente en el handler ASM. Solo hay que asegurarse de que `timer_handler()` siga corriendo para actualizar el contador de ticks:

```c
void irqDispatcher(uint64_t irq) {
    switch (irq) {
        case 0:
            timer_handler();   // actualiza ticks, no hace context switch
            break;
        case 1:
            handlePressedKey();
            break;
    }
}
```

El context switch ocurre en ASM antes de llamar a `irqDispatcher`, asi que el dispatcher no necesita saber del scheduler.

---

## 8. El proceso idle

El proceso idle es el **proceso de respaldo**: siempre existe, siempre esta READY y nunca se bloquea. El scheduler lo elige cuando no hay ningun otro proceso READY.

```c
// En process.c o kernel.c
static void idle_entry(int argc, char **argv) {
    while (1) {
        _hlt();   // suspende la CPU hasta el proximo interrupt (ahorra energia)
    }
}

void init_idle(void) {
    // Crear con prioridad 1 (minima), foreground=0
    idle_pid = process_create("idle", idle_entry, 0, NULL, 0);
    idle_process = process_get(idle_pid);
    // El idle nunca deberia ser desalojado normalmente,
    // pero su remaining_quanta se resetea igual que cualquier proceso.
}
```

**Por que `hlt`**: la instruccion `hlt` detiene la CPU hasta el proximo interrupt (timer, teclado, etc.). Esto evita que el idle consuma el 100% del CPU haciendo busy-wait. La CPU se despierta en el proximo tick del timer y el scheduler toma el control.

---

## 9. Adaptar el kernel al modelo multiproceso

### 9.1 Modificar `kernel.c`

El punto de entrada del kernel ya no puede saltar directamente a `0x400000`. En su lugar:

```c
// En kernel.c
#include "process.h"
#include "scheduler.h"

void *initializeKernelBinary(void) {
    void *moduleAddresses[] = { sampleCodeModuleAddress, sampleDataModuleAddress };
    loadModules(&endOfKernelBinary, moduleAddresses);
    clearBSS(&bss, &endOfKernel - &bss);
    load_idt();
    mm_init((void *)HEAP_START, HEAP_SIZE);

    // Inicializar el subsistema de procesos
    init_idle();                        // crear proceso idle primero
    process_create("shell",             // crear la shell como primer proceso
                   (void(*)(int,char**))sampleCodeModuleAddress,
                   0, NULL, 1);         // foreground=1

    // El scheduler arranca con el proximo tick del timer.
    // Habilitar interrupciones y esperar.
    _sti();
    while (1) _hlt();   // este loop nunca deberia correr porque idle toma el control
    return getStackBase();
}
```

**Por que ya no hay `jmp 0x400000`**: la shell ahora es un proceso como cualquier otro. El primer context switch (primer tick del timer) despachara a la shell.

### 9.2 Adaptar el driver de teclado

Actualmente `sys_read` usa polling activo: llama a `readKeyBuff` en un loop hasta que haya un caracter. Eso gasta quantums del proceso que espera.

Con multiproceso, lo correcto es **bloquear el proceso** que espera input y **desbloquearlo** desde el IRQ1 (teclado) cuando llega un caracter:

```c
// En keyboardDriver.c
void handlePressedKey(void) {
    // ... guardar caracter en buffer circular ...

    // Si habia un proceso bloqueado esperando input, desbloquearlo
    if (waiting_for_input != NULL) {
        process_unblock(waiting_for_input->pid);
        waiting_for_input = NULL;
    }
}

// En syscallDispatcher.c
uint64_t sys_read(char *buff, uint64_t count) {
    while (keyboard_buffer_empty()) {
        // Registrar que este proceso espera input y bloquearlo
        waiting_for_input = process_current();
        process_block(process_current()->pid);
        scheduler_yield();  // ceder CPU hasta ser desbloqueado
    }
    return readKeyBuff(buff, count);
}
```

### 9.3 Mapa de memoria actualizado

Con procesos, los stacks se asignan dinamicamente desde el heap:

```
0x000000 - 0x0FFFFF   BIOS / VBE info
0x100000 - ~0x1XXXXX  Kernel (text + data + bss + kernel stack)
0x400000 - 0x4FFFFF   Userland code module (shell + programas)
0x500000 - 0x5FFFFF   Userland data module
0x600000 - 0xDFFFFF   Heap (8 MB)
                         - PCB stacks asignados dinamicamente con mm_malloc
                         - Estructuras de datos del kernel (PCBs, etc.)
```

---

## 10. Syscalls de procesos

### 10.1 Nuevas syscalls (indices 19-28)

Agregar en `Kernel/include/defs.h`:

```c
#define CANT_SYS 29   // era 19, ahora 29
```

Actualizar en `interrupts.asm` la validacion:

```nasm
cmp rax, 29    ; era 19
jge .syscall_end
```

Agregar en `syscallDispatcher.c`:

```c
// Syscall 19: crear proceso
int64_t sys_create_process(const char *name, void *entry,
                           int argc, char **argv, uint8_t fg) {
    return process_create(name, (void(*)(int,char**))entry, argc, argv, fg);
}

// Syscall 20: terminar proceso actual
void sys_exit(int retval) {
    process_exit(retval);
}

// Syscall 21: obtener PID del proceso actual
uint64_t sys_getpid(void) {
    return process_current()->pid;
}

// Syscall 22: listar procesos (copia info en buffer)
void sys_ps(void *buf) {
    process_ps(buf);
}

// Syscall 23: matar proceso por PID
void sys_kill(uint64_t pid) {
    process_kill(pid);
}

// Syscall 24: cambiar prioridad
void sys_nice(uint64_t pid, uint8_t new_priority) {
    process_nice(pid, new_priority);
}

// Syscall 25: bloquear proceso
void sys_block(uint64_t pid) {
    process_block(pid);
}

// Syscall 26: desbloquear proceso
void sys_unblock(uint64_t pid) {
    process_unblock(pid);
}

// Syscall 27: ceder CPU voluntariamente
void sys_yield(void) {
    scheduler_yield();
}

// Syscall 28: esperar a que un hijo termine
int sys_waitpid(uint64_t pid) {
    return process_waitpid(pid);
}
```

Actualizar la tabla `syscalls[]`:

```c
void *syscalls[CANT_SYS] = {
    // ... indices 0-18 existentes ...
    &sys_create_process,  // 19
    &sys_exit,            // 20
    &sys_getpid,          // 21
    &sys_ps,              // 22
    &sys_kill,            // 23
    &sys_nice,            // 24
    &sys_block,           // 25
    &sys_unblock,         // 26
    &sys_yield,           // 27
    &sys_waitpid,         // 28
};
```

### 10.2 Wrappers en userland

En `Userland/asm/userlib.asm`, agregar:

```nasm
GLOBAL sys_create_process
GLOBAL sys_exit
GLOBAL sys_getpid
GLOBAL sys_ps
GLOBAL sys_kill
GLOBAL sys_nice
GLOBAL sys_block
GLOBAL sys_unblock
GLOBAL sys_yield
GLOBAL sys_waitpid

sys_create_process:
    mov rax, 19
    int 0x80
    ret

sys_exit:
    mov rax, 20
    int 0x80
    ret

sys_getpid:
    mov rax, 21
    int 0x80
    ret

sys_ps:
    mov rax, 22
    int 0x80
    ret

sys_kill:
    mov rax, 23
    int 0x80
    ret

sys_nice:
    mov rax, 24
    int 0x80
    ret

sys_block:
    mov rax, 25
    int 0x80
    ret

sys_unblock:
    mov rax, 26
    int 0x80
    ret

sys_yield:
    mov rax, 27
    int 0x80
    ret

sys_waitpid:
    mov rax, 28
    int 0x80
    ret
```

En `Userland/c/include/userlib.h`, agregar:

```c
int64_t  sys_create_process(const char *name, void *entry,
                            int argc, char **argv, uint8_t fg);
void     sys_exit(int retval);
uint64_t sys_getpid(void);
void     sys_ps(void *buf);
void     sys_kill(uint64_t pid);
void     sys_nice(uint64_t pid, uint8_t new_priority);
void     sys_block(uint64_t pid);
void     sys_unblock(uint64_t pid);
void     sys_yield(void);
int      sys_waitpid(uint64_t pid);
```

### 10.3 Estructura para sys_ps

Para que `sys_ps` sea util en userland, definir una estructura compartida:

```c
// En userlib.h o un header compartido
#define PS_MAX_PROCESSES 64

typedef struct {
    uint64_t pid;
    char     name[32];
    uint8_t  priority;
    uint8_t  state;      // 0=READY, 1=RUNNING, 2=BLOCKED, 3=ZOMBIE
    uint8_t  foreground;
    uint64_t rsp;
    uint64_t rbp;        // si se guarda en el PCB
} ProcessInfo;
```

---

## 11. Checklist final

- [ ] `Kernel/include/process.h` creado con struct `PCB` y prototipos.
- [ ] `Kernel/c/process.c` con `process_create`, `process_exit`, `process_kill`, `process_block`, `process_unblock`, `process_nice`, `process_waitpid`, `process_ps`.
- [ ] `build_initial_stack` implementado correctamente (frame de hardware + 15 GP regs).
- [ ] `Kernel/c/scheduler.c` con `scheduler_tick`, `scheduler_next`, `scheduler_add`, `scheduler_remove`, `scheduler_yield`.
- [ ] `Kernel/include/scheduler.h` con los prototipos del scheduler.
- [ ] `_irq00Handler` en `interrupts.asm` reescrito para llamar a `scheduler_tick` y hacer el RSP swap.
- [ ] Proceso idle creado (`idle_entry` con `hlt` en loop).
- [ ] `kernel.c` modificado: ya no hace `jmp 0x400000`; crea idle + shell y habilita interrupciones.
- [ ] `irqDispatcher.c` actualizado si corresponde.
- [ ] `sys_read` actualizado para bloquear el proceso en vez de hacer polling activo.
- [ ] `CANT_SYS` actualizado a 29 en `defs.h`.
- [ ] Limite de syscalls actualizado en `interrupts.asm` (`cmp rax, 29`).
- [ ] Syscalls 19-28 implementadas en `syscallDispatcher.c` y en la tabla `syscalls[]`.
- [ ] Prototipos de syscalls 19-28 en `syscallDispatcher.h`.
- [ ] Wrappers ASM para syscalls 19-28 en `userlib.asm`.
- [ ] Declaraciones en `userlib.h` de userland.
- [ ] Compila con `-Wall` sin warnings.
- [ ] `test_proc` integrado y pasa.
- [ ] `test_prio` integrado y se visualizan diferencias de tiempo segun prioridad.
