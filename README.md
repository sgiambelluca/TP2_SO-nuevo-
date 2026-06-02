# MASS OS — TP2 Sistemas Operativos (ITBA)

Kernel bare-metal x86-64 que corre directamente sobre QEMU o hardware real, sin sistema operativo subyacente. Implementa memoria, procesos, scheduling Round-Robin con prioridades, semaforos nombrados, pipes (anonimos y nombrados) y syscalls desde cero.

## Requisitos

- Docker (unico requisito en el host)
- QEMU (`qemu-system-x86_64`) para ejecutar la imagen
- Docker Desktop o el daemon Docker en ejecucion antes de correr `./create.sh`

---

## Primer uso: crear el contenedor

```bash
./create.sh
```

Descarga la imagen `agodio/itba-so-multiarch:3.1` y crea el contenedor `TP_SO_2` con el directorio actual montado en `/root`. Solo es necesario hacerlo una vez.

---

## Compilar

```bash
./compile.sh           # compila con First-Fit (default)
./compile.sh vbox      # idem, genera imagen VirtualBox (.vmdk)
```

### Seleccion del memory manager

El kernel soporta dos implementaciones de memoria, seleccionables en tiempo de compilacion mediante la variable de entorno `MM`:

| Comando | Memory Manager |
|---------|---------------|
| `./compile.sh` | First-Fit (default) |
| `MM=FF ./compile.sh` | First-Fit (explicito) |
| `MM=BUDDY ./compile.sh` | Buddy System |

```bash
# Buddy System + imagen QEMU
MM=BUDDY ./compile.sh

# Buddy System + imagen VirtualBox
MM=BUDDY ./compile.sh vbox
```

> **First-Fit**: lista enlazada de bloques libres; busca el primer bloque que ajuste.
> **Buddy System**: bloques de potencia de 2; divide y fusiona en pares (`buddies`).

La seleccion es **exclusiva en compilacion**: una imagen usa un unico MM. Para comparar ambos hay que compilar y correr por separado.

---

## Ejecutar

```bash
./run.sh           # QEMU (busca .qcow2, fallback a .img)
./run.sh vbox      # muestra pasos para VirtualBox
./run.sh usb       # muestra comando dd para grabar en USB
```

**Permiso denegado en la imagen**: el contenedor corre como root, por lo que el `.qcow2` queda con permisos de root. Si `./run.sh` falla:

```bash
sudo chown $USER Image/x64BareBonesImage.qcow2
```

---

## Compilar y correr en un paso

```bash
./compile.sh && ./run.sh              # First-Fit
MM=BUDDY ./compile.sh && ./run.sh     # Buddy System
```

---

## Shell de usuario

Al bootear se inicia una shell interactiva.

### Comandos del sistema

| Comando | Parametros | Descripcion |
|---------|-----------|-------------|
| `help` | — | Lista de comandos disponibles. |
| `clear` | — | Limpia la pantalla. |
| `printTime` | — | Imprime hora actual (UTC-3). |
| `printDate` | — | Imprime fecha actual. |
| `registers` | — | Dump de registros de la ultima excepcion (requiere `F1` para capturar). |
| `testDiv0` | — | Dispara excepcion #DE (division por cero). |
| `invOp` | — | Dispara excepcion #UD (instruccion invalida). |
| `playBeep` | — | Reproduce una melodia por el PC speaker. |
| `mem` | — | Muestra estado de la memoria (total, libre, usada, usada por kernel, cantidad de allocaciones). |
| `kill <pid>` | `pid`: ID del proceso | Termina el proceso indicado. |
| `nice <pid> <prio>` | `pid`: ID, `prio`: 1–5 | Cambia la prioridad del proceso. |
| `block <pid>` | `pid`: ID del proceso | Alterna estado entre BLOCKED y READY. |
| `loop` | — | Imprime su PID periodicamente (busy-wait permitido). |
| `sh` | — | Abre una nueva shell interactiva. |
| `cat` | — | Lee stdin y escribe stdout hasta EOF. |
| `wc` | — | Cuenta lineas, palabras y bytes de stdin. |
| `filter` | — | Filtra vocales (mayusculas y minusculas) de stdin. |
| `mvar <esc> <lec>` | `esc`: escritores, `lec`: lectores | Problema lectores/escritores sobre MVar nativa del kernel. |
| `ps` | — | Lista procesos activos con PID, prioridad, estado y foreground. |

### Tests provistos

| Test | Parametros | Descripcion |
|------|-----------|-------------|
| `test_mm <max>` | `max`: bytes maximos | Ciclo infinito de alloc/free; imprime solo si hay error. Debe funcionar con al menos un MM. |
| `test_processes <max>` | `max`: cantidad de procesos | Crea, bloquea, desbloquea y mata procesos dummy ciclicamente. |
| `test_prio <target>` | `target`: valor a alcanzar | Crea 3 procesos con distintas prioridades; visualiza diferencias de ejecucion. |
| `test_sync <n> <sem>` | `n`: iteraciones, `sem`: 0/1 | Test de condiciones de carrera. Resultado esperado `0` si `sem=1`. |
| `test_named_pipe` | — | Test de pipes con nombre (IPC entre escritor y lector). |

---

## Caracteres especiales de la shell

- `&` al final de un comando: ejecuta en **background**. El prompt vuelve inmediatamente.  
  Ejemplo: `loop &`
- `|` entre dos comandos: conecta stdout del primero con stdin del segundo mediante un pipe unidireccional.  
  Ejemplo: `cat | wc`, `filter | cat`

---

## Atajos de teclado

| Atajo | Accion |
|-------|--------|
| `Ctrl+C` | Mata el proceso en **foreground** mas reciente (el hijo, no la shell). |
| `Ctrl+D` | Envia EOF (`0x04`) al stdin del proceso foreground. |
| `F1` | Captura snapshot de registros (para luego visualizar con `registers`). |
| `+` / `-` | Aumenta / disminuye tamano de fuente durante la edicion de linea. |

---

## Ejemplos por fuera de los tests

### Memory Manager
```bash
mem                    # ver estado actual
```

### Procesos y Scheduling
```bash
loop &                 # proceso en background
ps                     # ver procesos activos
kill <pid>             # matar el loop
nice <pid> 5           # subir prioridad
block <pid>            # bloquear/desbloquear
sh                     # shell anidada; exit o Ctrl+D vuelve
```

### Sincronizacion
```bash
test_sync 100 1        # con semaforos; resultado esperado: 0
test_sync 100 0        # sin semaforos; resultado varia (race condition)
```

### IPC (pipes)
```bash
cat | wc               # contar lineas de lo que se escribe por teclado
filter | cat           # filtrar vocales de stdin
```

---

## Estado de implementacion

- **Comandos core como procesos reales**: `mem`, `kill`, `nice`, `block`, `loop`, `sh`, `cat`, `wc`, `filter`, `mvar`.
- **Tests como procesos reales**: `test_mm`, `test_processes`, `test_prio`, `test_sync`, `test_named_pipe`.
- **Built-ins aun en proceso** (pendientes de migrar a procesos hijos): `clear`, `ps`, `printTime`, `printDate`, `registers`, `testDiv0`, `invOp`, `playBeep`. El enunciado requiere que **todos** los comandos sean procesos.

---

## Limitaciones

- No se implemento `fork`/`execve` (modelo POSIX); los procesos se crean mediante `sys_create_process` con una tabla de funciones registradas (`registry[]`).
- No hay paginacion / MMU; todo el userland corre en un unico flat binary en `0x400000`.
- El scheduler usa Round-Robin con prioridades fijas (1–5); no hay aging ni starvation avoidance explicito.
- `testDiv0` e `invOp` como procesos hijos requieren que el handler de excepciones mate el proceso actual sin congelar la shell (pendiente de verificacion exhaustiva).
- `test_named_pipe` ya esta registrado como proceso hijo, pero el test original fue disenado para correr in-process; su comportamiento como proceso independiente debe verificarse.

---

## Uso de IA / Fragmentos de codigo asistidos

Algunas partes del codigo fueron desarrolladas o modificadas con asistencia de herramientas de IA generativa (OpenCode / GitHub Copilot). Los cambios fueron revisados manualmente, probados en QEMU y adaptados al contexto bare-metal del TP.

### Cambios asistidos principales (con commits)

**Correccion de bugs y alineacion con referencia de la catedra:**
- `Userland/c/tests/test_util.c`: distribucion uniforme real en `GetUniform()` (elimino modulo bias); fix de conteo de caracteres en `printf()`. *(commit `3d57ccb`)*
- `Userland/c/tests/test_sync.c`: firma de `my_process_inc` cambiada a `int64_t` con retorno de error. *(commit `13aa562`)*
- `Userland/c/shell/syscall.c` + `Userland/c/shell/userlib.c`: registro de `test_named_pipe` como proceso hijo. *(commit `f5a0db9`)*
- `Userland/c/shell/help.c`: corrigio texto de `+`/`-` de "built-in" a "atajo de teclado". *(commit `c052717`)*

**Estandarizacion C (`()` -> `(void)`) en ~24 funciones:**
- `Kernel/c/interrupts/idtLoader.c`, `Kernel/c/drivers/keyboardDriver.c`, `Kernel/c/drivers/videoDriver.c`, `Kernel/c/time.c`
- `Userland/c/shell/shell.c`, `Userland/c/shell/userlib.c`
*(commit `3d57ccb`)*

**Modularizacion del Kernel:**
- `Kernel/Makefile`: reorganizacion en carpetas (`scheduler/`, `process/`, `semaphore/`, `pipe/`, `interrupts/`, `syscall/`, `lib/`, `sound/`, `kernel/`).
*(commit `13aa562`)*

**Modularizacion de Userland:**
- `Userland/Makefile`: reorganizacion en carpetas (`tests/`, `shell/`, `commands/`).
- Movimiento de 18 archivos `.c` a sus carpetas correspondientes con actualizacion de includes.
*(commit `40624eb`)*

**Documentacion:**
- `README.md`: correcciones (`testMM` -> `test_mm`, descripciones de tests).
- `AGENTS.md`: actualizaciones de rutas y convenciones.
*(commits `c052717`, `40624eb`)*

### Verificacion

Todos los cambios asistidos compilaron con `-Wall -Wextra -Werror` sin warnings y fueron ejecutados en QEMU.

---

## Limpiar artefactos

```bash
make clean
```

> Nota: `make` debe ejecutarse **dentro** del contenedor Docker (`TP_SO_2`). En el host usar `./compile.sh`.
