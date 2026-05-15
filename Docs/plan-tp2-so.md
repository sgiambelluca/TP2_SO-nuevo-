# Plan de implementacion - TP2 Sistemas Operativos

## Estado actual (heredado del TPE de Arquitectura)

Ya se cuenta con: kernel 64-bit, IDT/IRQs, driver de video (framebuffer VBE), driver de teclado, driver de sonido (PIT), timer, syscalls via `int 0x80`, shell basica y juego Tron. El sistema es **single-tasking** (un solo flujo de ejecucion, sin procesos ni memoria dinamica).

---

## Paso 0 - Preparacion del entorno

- [x] Limpiar binarios y `.o` del repositorio (el enunciado prohibe binarios en el repo).
- [x] Configurar el entorno de compilacion con la imagen Docker de la catedra: `agodio/itba-so-multiarch:3.1`.
- [x] Ajustar los Makefiles para que `make` / `make all` solo compilen (sin lanzar QEMU ni Docker). Separar reglas de ejecucion.
- [x] Agregar soporte para compilacion condicional del memory manager: `MM=FF ./compile.sh` vs `MM=BUDDY ./compile.sh`, usando flags `-DMM_FF` / `-DMM_BUDDY` en el Makefile.
- [x] Verificar que todo compile con `-Wall` sin warnings desde el principio.

---

## Paso 1 - Memory Management

**Dependencias:** ninguna (es la base para todo lo demas).

### 1.1 Definir la interfaz comun del memory manager
- [x] Crear un header compartido (ej. `memoryManager.h`) con la interfaz que ambas implementaciones deben respetar:
  - `void mm_init(void *start, uint64_t size)` - inicializar el memory manager con un bloque de memoria.
  - `void *mm_malloc(uint64_t size)` - reservar memoria.
  - `void mm_free(void *ptr)` - liberar memoria.
  - `void mm_status(MemStatus *status)` - consultar estado (total, ocupada, libre).

### 1.2 Implementar el memory manager elegido por el grupo
- [x] Elegir e implementar un algoritmo (ej. first-fit con lista enlazada de bloques libres, o bitmap).
- [x] Debe soportar `malloc` y `free`.
- [x] Testear de forma aislada antes de conectar con el resto.

### 1.3 Implementar Buddy System
- [x] Implementar el buddy system como alternativa.
- [x] Misma interfaz que el MM anterior.

### 1.4 Compilacion condicional
- [x] Usar `#ifdef MM_BUDDY` / `#else` (o similar) para elegir entre las dos implementaciones.
- [x] Verificar que `make` compila con una y `make buddy` con la otra.

### 1.5 Syscalls de memoria
- [x] Agregar syscalls nuevas:
  - `sys_malloc(size)` -> devuelve puntero.
  - `sys_free(ptr)` -> libera bloque.
  - `sys_mem_status()` -> devuelve info del estado de la memoria.
- [x] Registrar las syscalls en la tabla `syscalls[]` del kernel y crear los wrappers en `userlib.asm`.

### 1.6 Test
- [x] Integrar `test_mm` (provisto por la catedra) como programa de usuario.
- [x] Verificar que pase sin errores con al menos uno de los dos MM. (20 OK / 0 FAIL con First-Fit)

---

## Paso 2 - Procesos, Context Switching y Scheduler

**Dependencias:** Paso 1 (se necesita `malloc` para crear PCBs y stacks).

### 2.1 Definir el PCB (Process Control Block)
- [ ] Crear la estructura PCB con al menos:
  - PID, nombre, prioridad, estado (READY, RUNNING, BLOCKED, ZOMBIE).
  - Stack pointer (RSP guardado), base pointer.
  - Foreground/background flag.
  - File descriptors (stdin, stdout) - necesario luego para pipes.
  - Puntero al padre (para `waitpid`).
  - Argv/argc (pasaje de parametros).

### 2.2 Implementar la tabla de procesos
- [ ] Array o lista de PCBs con un limite maximo de procesos.
- [ ] Funcion para crear proceso: asignar PID, reservar stack, preparar stack frame inicial (simular un contexto como si ya hubiera sido interrumpido).
- [ ] Funcion para destruir/finalizar proceso: liberar recursos.

### 2.3 Implementar Context Switch
- [ ] Escribir la rutina de context switch en ASM:
  - Guardar todos los registros del proceso actual en su stack.
  - Cambiar RSP al stack del proximo proceso.
  - Restaurar registros del proximo proceso.
  - `iretq` para reanudar ejecucion.
- [ ] Invocar el context switch desde el handler del timer (IRQ0) para lograr multitasking preemptivo.

### 2.4 Implementar el Scheduler (Round Robin con prioridades)
- [ ] Implementar Round Robin donde la prioridad determina cuantos quantums consecutivos recibe un proceso antes de ser desalojado.
- [ ] Procesos con mayor prioridad reciben mas tiempo de CPU.
- [ ] El scheduler ignora procesos en estado BLOCKED o ZOMBIE.

### 2.5 Crear el proceso idle
- [ ] Proceso con la prioridad mas baja que ejecuta `hlt` en loop.
- [ ] Se ejecuta cuando no hay ningun otro proceso READY.

### 2.6 Adaptar el kernel al modelo multiproceso
- [ ] El kernel ya no salta directamente a userland; en su lugar, crea el proceso `init` (o `shell`) como primer proceso y arranca el scheduler.
- [ ] Adaptar el driver de teclado para que despierte procesos bloqueados esperando input (en vez de usar un polling simple).

### 2.7 Syscalls de procesos
- [ ] Agregar las siguientes syscalls:
  - `sys_create_process(name, function, argc, argv, fg)` -> PID.
  - `sys_exit(retval)` -> finalizar proceso actual.
  - `sys_getpid()` -> PID del proceso actual.
  - `sys_ps(info_buffer)` -> listar procesos.
  - `sys_kill(pid)` -> matar proceso.
  - `sys_nice(pid, new_priority)` -> cambiar prioridad.
  - `sys_block(pid)` -> bloquear proceso.
  - `sys_unblock(pid)` -> desbloquear proceso.
  - `sys_yield()` -> renunciar al CPU (forzar context switch).
  - `sys_waitpid(pid)` -> esperar a que un hijo termine.
- [ ] Wrappers correspondientes en `userlib.asm`.

### 2.8 Tests
- [ ] Integrar `test_proc` como programa de usuario y verificar que pase.
- [ ] Integrar `test_prio` como programa de usuario y verificar que se visualicen diferencias de ejecucion.

---

## Paso 3 - Sincronizacion (Semaforos)

**Dependencias:** Paso 2 (necesita poder bloquear/desbloquear procesos).

### 3.1 Implementar semaforos con nombre
- [ ] Estructura de semaforo: valor, cola de procesos bloqueados, nombre/identificador.
- [ ] Tabla global de semaforos.
- [ ] Usar una instruccion atomica (`xchg` o `lock cmpxchg`) para proteger el acceso al valor del semaforo y evitar race conditions.
- [ ] `sem_wait`: si el valor es 0, bloquear el proceso (sin busy waiting); si es > 0, decrementar.
- [ ] `sem_post`: incrementar valor; si hay procesos en la cola, desbloquear uno.
- [ ] Mecanismo de apertura por nombre para que procesos no relacionados compartan semaforos.

### 3.2 Syscalls de semaforos
- [ ] Agregar syscalls:
  - `sys_sem_open(name, initial_value)` -> crea o abre semaforo por nombre.
  - `sys_sem_close(sem_id)` -> cierra semaforo.
  - `sys_sem_wait(sem_id)` -> wait (P).
  - `sys_sem_post(sem_id)` -> post (V).
- [ ] Wrappers en `userlib.asm`.

### 3.3 Test
- [ ] Integrar `test_sync` y verificar:
  - Con semaforos: resultado final siempre 0.
  - Sin semaforos: resultado varia entre ejecuciones.

---

## Paso 4 - Inter Process Communication (Pipes)

**Dependencias:** Paso 2 y Paso 3 (necesita procesos y posiblemente semaforos para la sincronizacion interna del pipe).

### 4.1 Implementar pipes unidireccionales
- [ ] Estructura del pipe: buffer circular, punteros de lectura/escritura, semaforos o mecanismo de bloqueo para sincronizar productor/consumidor.
- [ ] Lectura bloqueante: si el pipe esta vacio, el proceso lector se bloquea hasta que haya datos.
- [ ] Escritura bloqueante: si el pipe esta lleno, el proceso escritor se bloquea.
- [ ] Soporte para EOF: cuando se cierra el extremo de escritura, los lectores reciben EOF.

### 4.2 Abstraccion de file descriptors
- [ ] Cada proceso tiene un array de file descriptors (al menos stdin=0, stdout=1).
- [ ] Un FD puede apuntar a la terminal (teclado/pantalla) o a un extremo de un pipe.
- [ ] `sys_read` y `sys_write` deben ser transparentes: el proceso no necesita saber si lee/escribe de un pipe o de la terminal.
- [ ] Adaptar `sys_read` y `sys_write` existentes para que consulten el FD del proceso actual.

### 4.3 Pipes con nombre
- [ ] Permitir que procesos no relacionados compartan un pipe acordando un identificador.

### 4.4 Syscalls de pipes
- [ ] Agregar syscalls:
  - `sys_pipe(fd_array)` -> crea un pipe, devuelve FDs de lectura y escritura.
  - `sys_pipe_open(name)` -> abre pipe por nombre.
  - `sys_dup2(old_fd, new_fd)` -> redirigir file descriptors (util para que la shell conecte pipes).
  - `sys_close(fd)` -> cerrar un file descriptor.
- [ ] Wrappers en `userlib.asm`.

---

## Paso 5 - Aplicaciones de User Space

**Dependencias:** Pasos 1-4 completados.

### 5.1 Reescribir la shell (`sh`)
- [ ] La shell actual es single-tasking. Reescribirla para que:
  - Parsee el comando y cree un proceso hijo para ejecutarlo.
  - Soporte `&` al final del comando para ejecutar en **background** (no ceder foreground).
  - Soporte `|` para conectar 2 procesos via pipe (ej. `cat | filter`).
  - Soporte `Ctrl+C` para matar el proceso en foreground.
  - Soporte `Ctrl+D` para enviar EOF al proceso en foreground.
  - En foreground: la shell espera (`waitpid`) a que el hijo termine antes de mostrar el prompt.

### 5.2 Comandos basicos
- [ ] `help` - Lista todos los comandos disponibles, incluyendo los tests de la catedra.
- [ ] `mem` - Imprime estado de la memoria (total, usada, libre) via `sys_mem_status`.
- [ ] `ps` - Imprime lista de procesos con nombre, PID, prioridad, RSP, RBP, foreground.
- [ ] `loop` - Imprime su PID con un saludo cada cierto tiempo. Espera activa (no bloquearse).
- [ ] `kill <pid>` - Mata un proceso por su PID.
- [ ] `nice <pid> <prioridad>` - Cambia la prioridad de un proceso.
- [ ] `block <pid>` - Alterna el estado de un proceso entre BLOCKED y READY.

### 5.3 Comandos de IPC
- [ ] `cat` - Lee de stdin y lo imprime a stdout tal cual. Debe funcionar solo y con pipes.
- [ ] `wc` - Cuenta lineas del input recibido por stdin.
- [ ] `filter` - Lee de stdin y reimprime filtrando las vocales.
- [ ] `mvar <escritores> <lectores>` - Problema de lectores/escritores con MVar sincronizada por semaforos. Crear los procesos hijos y terminar inmediatamente.

### 5.4 Tests como programas de usuario
- [ ] `test_mm <max_memory>` - debe ejecutarse como proceso, no como built-in.
- [ ] `test_proc <max_processes>` - idem.
- [ ] `test_prio <target_value>` - idem.
- [ ] `test_sync <pairs> <increments> <use_sem>` - idem.
- [ ] Todos deben poder correrse en foreground y en background.

---

## Paso 6 - Verificacion y limpieza final

### 6.1 Verificacion obligatoria
- [ ] Compilar con `-Wall` y que no haya warnings.
- [ ] Correr `test_mm` en foreground y background -> sin errores.
- [ ] Correr `test_proc` en foreground y background -> sin errores.
- [ ] Correr `test_sync` en foreground y background -> resultado 0 con semaforos.
- [ ] Correr `test_prio` -> se visualizan diferencias de tiempo segun prioridad.
- [ ] Verificar que el sistema este libre de deadlocks, race conditions y busy waiting (excepto donde se indica: `loop`, `test_sync` sin semaforos).

### 6.2 Limpieza del repositorio
- [ ] Eliminar todos los binarios (`.o`, `.bin`, `.img`, `.qcow2`, `.vmdk`) del repo.
- [ ] Verificar que `.gitignore` cubra todos los artefactos de compilacion.
- [ ] Verificar que el historial de git sea coherente desde el comienzo del TP.

### 6.3 README.md
- [ ] Crear/actualizar `README.md` en la raiz con:
  - Instrucciones de compilacion y ejecucion.
  - Nombre y descripcion de cada comando y test, con sus parametros.
  - Caracteres especiales (`&` para background, `|` para pipes).
  - Atajos de teclado (`Ctrl+C`, `Ctrl+D`).
  - Ejemplos de uso para cada requerimiento.
  - Requerimientos faltantes o parcialmente implementados.
  - Limitaciones.
  - Citas de fragmentos de codigo / uso de IA.

---

## Orden de implementacion recomendado

```
Paso 0 (Preparacion)
  |
Paso 1 (Memory Management)
  |
Paso 2 (Procesos + Context Switch + Scheduler)
  |
  +------+------+
  |             |
Paso 3        Paso 4
(Semaforos)   (Pipes) -- puede usar semaforos internamente
  |             |
  +------+------+
         |
Paso 5 (Aplicaciones de usuario + Shell)
         |
Paso 6 (Verificacion + README)
```

Los pasos 3 y 4 pueden desarrollarse en paralelo una vez que los procesos funcionen, pero los pipes pueden beneficiarse de usar semaforos internamente para la sincronizacion.
