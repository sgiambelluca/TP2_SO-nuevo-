# Plan de implementación — TP2 SO

## Estado actual

Sistema multitasking preemptivo funcional. Lo completado incluye: kernel 64-bit, IDT/IRQs, drivers (video, teclado, sonido, timer), syscalls `int 0x80`, ambos memory managers (FF / Buddy), scheduler Round-Robin con prioridades, semáforos nombrados, pipes anónimos/nombrados + FDs, shell con parseo de `&` y `|` ya operativos, y tests (`test_mm`, `test_processes`, `test_prio`, `test_sync`, `test_named_pipe`) corriendo como procesos hijos.

Lo que resta son: comandos de userland faltantes, señales de teclado (`Ctrl+C` / `Ctrl+D`), y la migración total de built-ins a procesos reales para cumplir el enunciado.

---

## Pasos naturales pendientes (orden recomendado)

### 1. Comandos core simples
Implementar los comandos del enunciado que ya tienen syscalls listas. Cada uno es un proceso separado (`int64_t cmd(int argc, char *argv[])`); registrar en `Userland/c/syscall.c` → `registry[]`, en `Userland/c/userlib.c` → `is_child_command()`, y actualizar `help`.

- [ ] `mem` — muestra estado de la memoria (`sys_mem_status`).
- [ ] `kill <pid>` — envía `sys_kill` al PID.
- [ ] `nice <pid> <prio>` — ajusta prioridad (`sys_nice`).
- [ ] `block <pid>` — alterna BLOCKED/READY (`sys_block`).
- [ ] `loop` — imprime PID cada X tiempo usando busy waiting (caso permitido).
- [ ] `sh` — si aplica, un mini shell recursivo o reutilizar la shell actual como proceso.

> **Nota:** `help`, `ps`, `clear`, `printTime`, `printDate`, etc. aún son built-ins. Se migrarán en el paso 5.

### 2. Ctrl+C y Ctrl+D en el keyboard driver
- [ ] `Ctrl+C` (scan code + Ctrl flag): identificar el proceso en foreground (`fg=1`) y enviarle `sys_kill`.
- [ ] `Ctrl+D` (scan code + Ctrl flag): enviar EOF al stdin del proceso foreground (ej. cerrar/cerrar su pipe de entrada o marcar EOF en su `fd[0]`).
- [ ] Requiere que el driver de teclado tenga acceso al PCB del foreground, o que genere una syscall especial interna para manejar la señal.

### 3. Comandos de IPC básicos
Estos leen de `fd[0]` y escriben en `fd[1]`; la shell ya maneja `|` y `sys_pipe_setup`.

- [ ] `cat` — lee stdin, escribe stdout (hasta EOF).
- [ ] `wc` — cuenta líneas del stdin; termina en EOF.
- [ ] `filter` — lee stdin, filtra vocales (mayúsculas y minúsculas), escribe stdout.

### 4. Comando `mvar`
- [ ] `mvar <escritores> <lectores>` — problema lectores/escritores sobre variable compartida usando semáforos.
  - Escritores: espera activa aleatoria, espera variable vacía, escribe valor único ('A', 'B', ...).
  - Lectores: espera activa aleatoria, espera que haya valor, consume e imprime con color.
  - El proceso principal debe terminar inmediatamente tras crear los hijos.

### 5. Migrar todos los built-ins a procesos reales
El enunciado requiere que **todos** los comandos sean procesos; actualmente solo los tests y `np_writer`/`np_reader` lo son. Refactorizar la shell (`Userland/c/userlib.c` → `commands[]` y `processLine`) para que:

- [ ] Ningún comando se ejecute in-process.
- [ ] `help`, `clear`, `ps`, `printTime`, `printDate`, `registers`, `testDiv0`, `invOp`, `playBeep`, `bmFPS`, `bmCPU`, `bmMEM`, `bmKEY` (+ y -) se conviertan en entry points de procesos registrados.
- [ ] La shell quede reducida a: leer línea, parsear `&` / `|`, spawnear proceso(s), `waitpid` si es foreground.
- [ ] Actualizar `help` para reflejar qué comandos existen y sus parámetros.

### 6. Verificación final
- [ ] Compilar con `-Wall` sin warnings.
- [ ] `test_mm`, `test_processes`, `test_sync`, `test_prio` en foreground y background → sin errores.
- [ ] `test_sync` con semáforos debe dar resultado `0`.
- [ ] Probar pipes: `cat | wc`, `filter | cat`, etc.
- [ ] Probar `Ctrl+C` y `Ctrl+D` sobre procesos foreground.
- [ ] Confirmar que no hay busy waiting fuera de `loop` y `test_sync` sin semáforos.
- [ ] Eliminar binarios del repo; verificar `.gitignore`.

### 7. Documentación
- [ ] Actualizar `README.md` con: compilación/ejecución, lista de comandos con parámetros, uso de `&` y `|`, atajos de teclado, ejemplos y limitaciones.

---

## Leyenda

- `[ ]` Pendiente  
- `[x]` Completado  
- `[-]` No aplica / Postergado
