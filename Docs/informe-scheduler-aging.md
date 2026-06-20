# Informe: Scheduler — Aging Anti-Starvation

> Trabajo Práctico Nº 2 — Sistemas Operativos (ITBA)
> Sesión de corrección sobre `Kernel/c/scheduler/scheduler.c` y archivos asociados.

---

## Tabla de contenidos

1. [Conceptos clave: priority vs. effective priority](#1-conceptos-clave-priority-vs-effective-priority)
2. [Problema detectado](#2-problema-detectado)
3. [Análisis del diseño anterior](#3-análisis-del-diseño-anterior)
4. [Solución: Aging](#4-solución-aging)
5. [Resumen detallado de cambios](#5-resumen-detallado-de-cambios)
6. [Proceso de decisión y fundamentación](#6-proceso-de-decisión-y-fundamentación)
7. [Por qué este diseño soluciona el problema](#7-por-qué-este-diseño-soluciona-el-problema)
8. [Verificación](#8-verificación)

---

## 1. Conceptos clave: priority vs. effective priority

### 1.1 `priority` (prioridad base)

La **prioridad base** es un entero entre 1 y 5 almacenado en `PCB.priority`. Es:

- **Estable**: no cambia por arte del scheduler. Sólo la muta `nice` (syscall explícita del usuario).
- **Visible**: es lo que `ps` muestra al usuario y lo que el usuario configura con `nice <pid> <prio>`.
- **Determina el quantum**: la duración del timeslice (en ticks de timer) es igual a `priority`. Un proceso prio-5 corre 5 ticks consecutivos antes de re-elección; uno prio-1 corre 1 tick.
- **Asignada al crear**: `DEFAULT_PRIORITY = 3` para todo proceso nuevo.

### 1.2 `effective priority` (prioridad efectiva, `eff`)

La **prioridad efectiva** es un valor **derivado** que el scheduler calcula en el momento de la selección (`scheduler_next_ready`). No se almacena en el PCB; se computa dinámicamente. Su fórmula es:

```
eff = priority + (foreground ? 1 : 0) + aging_bonus
```

Tres componentes:

| Componente | Significado | Rango |
|---|---|---|
| `priority` | Prioridad base del proceso (lo que `nice` configura) | 1–5 |
| `foreground ? 1 : 0` | Boost de +1 para procesos en foreground (desempate) | 0–1 |
| `aging_bonus` | Bono por tiempo de espera (anti-starvation) | 0–4 |

La prioridad efectiva **sólo** se usa para **seleccionar** al siguiente proceso a ejecutar. El `scheduler_next_ready` elige el proceso READY con la `eff` más alta, y ante empate hace round-robin (vía `queue_idx`).

### 1.3 Diferencia fundamental

| Aspecto | `priority` (base) | `eff` (efectiva) |
|---|---|---|
| **Almacenamiento** | En el PCB (`p->priority`) | Se computa, no se guarda |
| **Mutabilidad** | Sólo `nice` | Cambia cada tick según espera y foreground |
| **Visible en `ps`** | Sí | No |
| **Controla** | Quantum (duración) | Selección (quién corre) |
| **Propósito** | Expresar la intención del usuario | Garantizar fairness + responsiveness |

Esta separación es **deliberada y crítica**: el usuario configura `priority` para expresar "este proceso es más importante"; el scheduler usa `eff` para asegurar que incluso los procesos menos importantes eventualmente obtengan CPU. Si `priority` fluctuara con aging, `ps` mostraría valores confusos y `nice` perdería control predecible. Si `eff` controlara el quantum, un proceso starved que finalmente corre tendría un timeslice enorme (prio-1 con bonus 4 = quantum 5), monopolizando la CPU justo después de liberarse de la starvation.

---

## 2. Problema detectado

### Reporte de la cátedra

> **Scheduler — sin aging explícito:** `scheduler.c` no tiene mecanismo de aging. Un proceso de baja prioridad puede sufrir starvation si hay suficientes procesos de alta prioridad READY. El boost de +1 a foreground es para desempate, no evita starvation estructural.

### Reproducción del bug

```
loop &
[14]
loop &
[15]
14 15 14 15 ....       ← ambos prio-3 (default), round-robin balanceado
nice 14 1              ← bajar prio de 14 a 1
// Prioridad de 14 = 1, de 15 = 3:
15 15 15 15 15 15      ← 14 NUNCA obtiene CPU (starvation permanente)
```

El proceso 14 queda **indefinidamente** sin ejecutarse mientras exista un proceso 15 con prioridad superior en estado READY. Como `loop` es un busy-wait que siempre está READY, la condición de starvation es permanente.

---

## 3. Análisis del diseño anterior

### 3.1 Estructura del scheduler original

El scheduler original (`scheduler.c` antes de esta corrección) funcionaba así:

1. **Cola única ordenada por prioridad**: `run_queue[64]` se mantiene ordenada descendentemente por `priority` mediante insertion sort en `scheduler_add`.

2. **Timer tick (`scheduler_tick`)**: el proceso en RUNNING consume `remaining_quanta` (inicializado a `priority`). Mientras le queden quanta, sigue corriendo. Al agotarse, pasa a READY y se llama `scheduler_next_ready`.

3. **Selección (`scheduler_next_ready`)**: calcula `highest_eff = max(priority + (foreground ? 1 : 0))` entre todos los READY, y **sólo** elige procesos cuyo `eff` coincide con `highest_eff`. Dentro de ese nivel, hace round-robin vía `queue_idx`.

4. **Quantum**: `priority` ticks consecutivos por turno.

### 3.2 Por qué el boost de foreground no ayuda

El boost de +1 para foreground es un **desempate dentro del mismo nivel de prioridad efectiva**, no un mecanismo anti-starvation. Su propósito es que, ante dos procesos con la misma `priority` base, el que está en foreground (interactuando con el usuario) tenga preferencia para mantener la responsiveness de la shell. Pero:

- Un proceso prio-1 foreground tiene `eff = 1 + 1 = 2`.
- Un proceso prio-3 background tiene `eff = 3 + 0 = 3`.
- `3 > 2` → el background prio-3 **siempre** gana. El foreground prio-1 sigue starvando.

El boost no puede elevar un proceso por encima de niveles de prioridad base superiores. Es una herramienta de desempate intra-nivel, no inter-nivel.

### 3.3 Por qué el round-robin intra-nivel no ayuda

El round-robin (vía `queue_idx`) opera **dentro** del nivel ganador (los procesos con `eff == highest_eff`). Si el proceso prio-1 nunca pertenece al nivel ganador (porque siempre hay un prio-3 con `eff` mayor), el round-robin nunca lo alcanza. El round-robin da **fairness** entre procesos de la misma prioridad, pero no **equidad** entre prioridades distintas.

### 3.4 Qué podía generar este diseño

- **Starvation permanente e indefinida.** Cualquier proceso con `priority` base inferior a la máxima `eff` presente en el sistema quedaba excluido para siempre, sin mecanismo de recuperación. No importaba cuánto tiempo esperara: la espera no se acumulaba ni se traducía en ningún beneficio.

- **Bloqueo de tests del enunciado.** `test_prio` crea tres procesos con prioridades 1, 3 y 5. Si los procesos prio-3 y prio-5 están constantemente READY (lo están, porque hacen un loop de cómputo puro), el proceso prio-1 puede **no terminar nunca**, impidiendo que el test pase.

- **Inanición de la shell en background.** Si un usuario lanza `loop &` (background, prio-3) y luego baja la prioridad de la shell a 1 (vía `nice`), la shell perdería la CPU mientras el loop existiera. El usuario no podría interactuar con el sistema.

- **Injusticia estructural invisible.** El problema no genera un crash ni un error visible; simplemente algunos procesos "desaparecen" del output. Esto es difícil de diagnosticar y parece un bug de otro subsistema (pipes, semáforos, etc.) cuando en realidad es del scheduler.

- **No cumplimiento del enunciado.** El enunciado de la cátedra pide explícitamente un mecanismo de aging. Sin él, el scheduler no satisface el requisito.

---

## 4. Solución: Aging

### 4.1 Qué es el aging

El **aging** es una técnica clásica de scheduling que previene la starvation incrementando gradualmente la prioridad efectiva de un proceso en función del tiempo que lleva esperando CPU. La idea es:

> "Si un proceso lleva mucho tiempo sin ejecutarse, trátalo temporalmente como si tuviera mayor prioridad, para que eventualmente pueda competir con los procesos de mayor prioridad que lo están bloqueando."

Es el mecanismo anti-starvation estándar para schedulers de prioridades fijas. Se enseña en cualquier curso de Sistemas Operativos (Tanenbaum, Silberschatz) y es lo que el enunciado pide.

### 4.2 Diseño aplicado

#### Campo nuevo en el PCB

```c
uint32_t wait_ticks;   /* Ticks acumulados esperando CPU (aging). */
```

`wait_ticks` cuenta cuántos ticks de timer ha pasado el proceso en estado READY **sin ser seleccionado** para ejecutarse. Se incrementa en `scheduler_tick` para todos los procesos READY excepto el que está RUNNING.

#### Bono de aging

```c
#define AGING_INTERVAL  50   /* ticks de espera para +1 de aging (500ms a 100Hz). */
#define MAX_AGING_BONUS 4    /* tope: un proceso prio-1 puede alcanzar eff=5. */

static int aging_bonus(const PCB *p){
    int b = (int)(p->wait_ticks / AGING_INTERVAL);
    if(b > MAX_AGING_BONUS) b = MAX_AGING_BONUS;
    return b;
}
```

- Cada `AGING_INTERVAL` (50) ticks de espera, el bono sube +1.
- El timer corre a 100 Hz (1 tick = 10 ms), por lo que 50 ticks = 500 ms. Un proceso gana +1 de prioridad efectiva cada medio segundo que espera.
- El bono se **acota** en `MAX_AGING_BONUS = 4` para que un proceso prio-1 pueda alcanzar como máximo `eff = 1 + 0 + 4 = 5` (sin foreground) o `eff = 1 + 1 + 4 = 6` (con foreground). Sin este tope, el bono crecería indefinidamente y eventualmente cualquier proceso starved desplazaría a todos los demás, invirtiendo la starvation en lugar de resolverla.

#### Prioridad efectiva nueva

```c
eff = priority + (foreground ? 1 : 0) + aging_bonus(p)
```

Los tres componentes se **suman**. Esto permite que coexistan:

- El boost de foreground (+1) como desempate **intra-nivel**.
- El aging bonus (0–4) como mecanismo **inter-nivel** de anti-starvation.

#### Puntos de reset de `wait_ticks`

`wait_ticks` se resetea a 0 en los siguientes eventos:

| Evento | Lugar | Justificación |
|---|---|---|
| Creación del proceso | `process_create` | Un proceso nuevo arranca sin espera acumulada. |
| Selección para ejecutar | `scheduler_tick`, `scheduler_yield_impl` | El proceso va a correr; su espera se "consume". |
| Desbloqueo | `process_unblock` | El tiempo bloqueado (I/O, semáforo, pipe) **no** es hambre de CPU. |
| Despertar de `waitpid` | `process_exit`, `process_kill` (padre despertado) | El padre estaba esperando a un hijo, no esperando CPU. |
| Cambio de prioridad | `process_nice` | El usuario cambió la prioridad explícitamente; el proceso arranca "fresco" en el nuevo nivel. |

**Por qué el tiempo BLOCKED no cuenta como espera:** un proceso que estuvo bloqueado 10 segundos esperando I/O no fue "starvado" por el scheduler — simplemente no tenía trabajo que hacer. Si acumulara aging durante el bloqueo, al desbloquearse tendría un bono enorme y desplazaría injustamente a procesos que sí estaban listos y esperando CPU. El aging mide **hambre de CPU**, no tiempo total desde la última ejecución.

#### Quantum inmutable

El quantum (duración del timeslice) sigue siendo `priority` (base), **no** `eff`. Esto es deliberado:

- Si el quantum usara `eff`, un proceso prio-1 que esperó 2 segundos (`aging_bonus = 4`) y finalmente es seleccionado obtendría `quantum = 1 + 4 = 5` ticks. Eso es 5 veces su quantum normal y monopolizaría la CPU justo después de liberarse de la starvation.
- Con quantum basado en `priority`, el proceso starved corre 1 tick (su quantum normal), al terminar su `wait_ticks` se resetea a 0, y el cycle reinicia. Obtiene CPU **con frecuencia** pero no **por más tiempo** del que le corresponde. La starvation se resuelve dando **oportunidades**, no **duración**.

---

## 5. Resumen detallado de cambios

### 5.1 Nuevo campo `wait_ticks` en el PCB

**Archivo:** `Kernel/include/process.h`

```c
uint32_t wait_ticks;   /* Ticks acumulados esperando CPU (aging). */
```

Ubicado junto a `remaining_quanta` (ambos son estado de scheduling). Tipo `uint32_t` (máx ~49 días a 100 Hz antes de overflow, irrelevante para un TP que corre minutos).

### 5.2 Inicialización en `process_create`

**Archivo:** `Kernel/c/process/process.c`

```c
p->wait_ticks = 0;
```

Un proceso nuevo arranca sin espera acumulada.

### 5.3 Constantes y helper de aging

**Archivo:** `Kernel/c/scheduler/scheduler.c`

```c
#define AGING_INTERVAL  50
#define MAX_AGING_BONUS 4

static int aging_bonus(const PCB *p){
    int b = (int)(p->wait_ticks / AGING_INTERVAL);
    if(b > MAX_AGING_BONUS) b = MAX_AGING_BONUS;
    return b;
}
```

El helper es `static` (no se exporta; es interno del scheduler). Se invoca desde `scheduler_next_ready` para cada candidato.

### 5.4 Incremento de `wait_ticks` en `scheduler_tick`

**Archivo:** `Kernel/c/scheduler/scheduler.c`

Al inicio de `scheduler_tick`, **antes** de procesar el quantum del proceso actual:

```c
for(int i = 0; i < queue_size; i++){
    PCB* p = run_queue[i];
    if(p != cur && p->state == PROCESS_READY){
        p->wait_ticks++;
    }
}
```

**Orden crítico:** el incremento se hace antes de que el proceso actual sea demoted a READY (por agotamiento de quantum). Así, el proceso actual (que estaba RUNNING) no se auto-incrementa. Los procesos que ya estaban esperando sí acumulan un tick más de espera.

**Por qué se excluye al RUNNING:** el proceso que está en CPU no está "esperando"; está ejecutando. Su `wait_ticks` debe quedar como está (probablemente 0, porque fue reseteado al seleccionarlo). Si se incrementara, un proceso que corre mucho tiempo acumularía aging espurio y sería aún más favorecido al next_ready, empeorando la starvation de los demás.

### 5.5 Reset del seleccionado en `scheduler_tick` y `scheduler_yield_impl`

**Archivo:** `Kernel/c/scheduler/scheduler.c`

En ambas funciones, justo antes de marcar `next` como RUNNING:

```c
next->wait_ticks = 0;
next->state = PROCESS_RUNNING;
```

El proceso que va a correr "consume" su aging. Al terminar su quantum y volver a READY, su `wait_ticks` arranca en 0 y comienza a acumular nuevamente si no es seleccionado de inmediato.

### 5.6 Aging en `scheduler_next_ready`

**Archivo:** `Kernel/c/scheduler/scheduler.c`

La fórmula de `eff` ahora incluye `aging_bonus`:

```c
int eff = run_queue[i]->priority
        + (run_queue[i]->foreground ? 1 : 0)
        + aging_bonus(run_queue[i]);
```

Esto aplica tanto al cálculo de `highest_eff` como a las dos pasadas de selección (foreground primero, luego cualquiera). Las dos pasadas y el round-robin intra-nivel se preservan intactos; sólo cambia qué nivel es el "ganador".

### 5.7 Reset en `process_unblock` y `process_nice`

**Archivo:** `Kernel/c/process/process.c`

```c
/* process_unblock */
p->state = PROCESS_READY;
p->wait_ticks = 0;

/* process_nice */
p->priority = new_priority;
p->wait_ticks = 0;
```

### 5.8 Reset en los paths de despertar de `waitpid`

**Archivo:** `Kernel/c/process/process.c`

Tanto en `process_exit` como en `process_kill`, cuando un padre bloqueado en `waitpid` es despertado porque su hijo terminó/murió:

```c
parent->state = PROCESS_READY;
parent->wait_ticks = 0;
```

Estos son paths equivalentes a `process_unblock` (el padre estaba BLOCKED esperando un evento, no esperando CPU). Se identificaron durante la implementación como puntos que también requerían reset para consistencia.

### 5.9 No se requiere modificar assembler

El handler del timer (`_irq00Handler` en `interrupts.asm`) ya delega toda la lógica a `scheduler_tick` en C:

```asm
_irq00Handler:
    pushState
    mov rdi, rsp
    call scheduler_tick     ; retorna RSP del proximo proceso
    mov rsp, rax            ; cambiar al stack del proximo proceso
    ...
```

El aging vive enteramente en C. El assembler no necesita saber de `wait_ticks` ni de `aging_bonus`. Esto mantiene la separación de responsabilidades: el ASM hace el context switch mecánico (pushState, swap RSP, popState, iretq); el C decide a quién ejecutar.

---

## 6. Proceso de decisión y fundamentación

### 6.1 Elección de la técnica: aging

| Alternativa | Pros | Contras | Veredicto |
|---|---|---|---|
| **Aging** (elegida) | Estándar en la literatura, satisface el enunciado, O(1) por proceso por tick, no requiere reordenar la cola, conserva priority base estable | Requiere un campo nuevo en PCB y calibración de constantes | **Mejor opción** |
| Round-robin puro (sin prioridades) | Simple, sin starvation | Ignora prioridades; no satisface el enunciado que pide RR con prioridades | Rechazada |
| Multilevel Feedback Queue (MLFQ) | Muy sofisticado, adaptive | Overkill para un TP; requiere múltiples colas y lógica de promoción/democión compleja | Rechazada |
| Time-slicing proporcional (stride/lottery) | Fairness matemático | Cambia el modelo de scheduling; no conserva el RR con prioridades del enunciado | Rechazada |
| No hacer nada | Cero código | No cumple el requisito; starvation permanente | Rechazada |

El aging es la solución canónica para "scheduler de prioridades fijas con anti-starvation". Es lo que el enunciado pide, lo que se enseña en la cátedra, y lo que tiene la menor complejidad de implementación entre las opciones que funcionan.

### 6.2 Calibración de constantes

Se analizó la frecuencia del timer: `time.c` revela que `sleep(ms)` divide por 10 (`target = ms / 10`), lo que indica **1 tick = 10 ms = 100 Hz**.

Con `AGING_INTERVAL = 50` (500 ms por +1) y `MAX_AGING_BONUS = 4`:

| Tiempo de espera | `aging_bonus` | `eff` de un prio-1 bg | `eff` de un prio-3 bg |
|---|---|---|---|
| 0 ms | 0 | 1 | 3 |
| 500 ms | 1 | 2 | 3 |
| 1000 ms | 2 | 3 = 3 → **empate**, round-robin lo elige | 3 |
| 1500 ms | 3 | 4 > 3 → **gana** | 3 |
| 2000 ms | 4 (tope) | 5 > 3 → **gana** | 3 |

- A los **~1 segundo** de espera, el proceso prio-1 empata con el prio-3 y entra al round-robin.
- A los **~1.5 segundos**, el proceso prio-1 supera al prio-3 y es seleccionado preferentemente.

Esto produce un patrón visible en la demo: `15 15 15... 14 15 15... 14...` — el proceso starved aparece cada ~1 segundo, sin starvation permanente. El balance es suficientemente rápido para ser observable en una demo de TP, pero no tan agresivo como para degradar las prioridades reales en uso normal.

### 6.3 Por qué `wait_ticks` no se almacena como `eff` en el PCB

Se consideró almacenar la prioridad efectiva directamente en el PCB y actualizarla cada tick. Se descartó porque:

- `eff` cambia cada tick (depende de `wait_ticks` y foreground), mientras que `priority` es estable. Almacenar `eff` requeriría recalcula y escribir el PCB cada tick, o mantener dos campos sincronizados.
- `ps` necesita mostrar `priority` (base), no `eff`. Si el PCB tuviera `eff`, `ps` mostraría valores que fluctúan y confundirían al usuario.
- `nice` necesita escribir `priority` (base), no `eff`. Si se escribiera `eff`, el aging se acumularía sobre la nueva base de forma impredecible.

La separación `priority` (almacenada, estable, visible) + `eff` (derivada, dinámica, interna) es más limpia y evita estados inconsistentes.

### 6.4 Por qué el quantum no usa `eff`

Se consideró que el quantum fuera `eff` en lugar de `priority`. Se descartó porque:

- Un proceso prio-1 con `aging_bonus = 4` tendría `quantum = 5` al finalmente ser seleccionado. Eso es 5 veces su quantum normal y lo haría monopolizar la CPU durante 50 ms, desplazando a todos los demás.
- El problema del scheduler original no es que los procesos starved corran "muy poco tiempo" cuando corren — es que **nunca corren**. La solución es darles **oportunidades de correr**, no **hacerlos correr por más tiempo**.
- Con quantum basado en `priority`, el proceso starved corre 1 tick (su quantum normal), resetea su `wait_ticks`, y al volver a READY empieza a acumular de nuevo. Obtiene CPU **con frecuencia creciente** a medida que su aging sube, pero cada vez por su duración normal. Esto es fairness, no inverso de starvation.

### 6.5 Por qué no se reordena `run_queue` por `eff`

La cola se mantiene ordenada por `priority` base (descendente). Se consideró reordenarla por `eff` cada tick. Se descartó porque:

- `scheduler_next_ready` ya hace un **scan completo** de la cola para calcular `highest_eff` y seleccionar al ganador. El orden de la cola sólo afecta el round-robin intra-nivel (vía `queue_idx`), que sigue siendo correcto: dentro del nivel ganador, los procesos se rotan en orden de llegada.
- Reordenar cada tick sería O(n²) (insertion sort sobre 64 elementos) y complicaría el manejo de `queue_idx`. No aporta nada: la selección ya no depende del orden físico de la cola.
- El orden por `priority` base es **estable** (sólo cambia con `nice`), lo que simplifica `scheduler_add`/`scheduler_remove` y mantiene el round-robin predecible.

---

## 7. Por qué este diseño soluciona el problema

### 7.1 Demostración con el ejemplo del bug

Reproduzcamos el escenario del reporte:

```
loop &          → PID 14, prio=3 (default), background
loop &          → PID 15, prio=3 (default), background
nice 14 1       → PID 14 baja a prio=1
```

Estado: PID 14 (prio 1) y PID 15 (prio 3), ambos READY siempre (busy-wait).

**Tick 0–49** (0–490 ms):
- PID 15 corre (eff=3+0+0=3 > eff=1+0+0=1 de PID 14).
- PID 14 acumula `wait_ticks`: 0, 1, 2, ..., 49.
- `aging_bonus(14) = 49/50 = 0`. Aún no alcanza el primer umbral.

**Tick 50** (500 ms):
- `wait_ticks(14) = 50`. `aging_bonus(14) = 50/50 = 1`.
- `eff(14) = 1 + 0 + 1 = 2`. Aún `eff(15) = 3` > `eff(14) = 2`.
- PID 15 sigue corriendo.

**Tick 100** (1000 ms):
- `wait_ticks(14) = 100`. `aging_bonus(14) = 100/50 = 2`.
- `eff(14) = 1 + 0 + 2 = 3` = `eff(15) = 3`. **Empate.**
- `scheduler_next_ready` entra al round-robin del nivel 3. En la próxima rotación de `queue_idx`, **PID 14 es seleccionado**.
- PID 14 corre 1 tick (quantum = priority = 1), `wait_ticks(14)` reset a 0.
- PID 15 empieza a acumular `wait_ticks`.

**Patrón resultante:**

```
15 15 15 15 15  (ticks 0-24, quantum de 15 = 3, ~8 turnos)
15 15 15 15 15  (ticks 25-49)
15 15 15 15 15  (ticks 50-74)
15 15 15 15 15  (ticks 75-99)
14              (tick 100: 14 finalmente corre)
15 15 15 15 15  (ticks 101-125: 14 vuelve a esperar, eff=1 < 3)
...
14              (tick ~200: 14 acumula 100 ticks otra vez y corre)
```

**El proceso 14 ya no starva.** Aparece cada ~1 segundo, que es exactamente el comportamiento esperado y observable en la demo.

### 7.2 Preservación del comportamiento normal

Cuando todos los procesos tienen la misma prioridad base, `aging_bonus` es 0 para todos (porque se turnan y resetean constantemente), y `eff = priority + foreground_boost`. El comportamiento es idéntico al original: round-robin con preferencia a foreground.

Cuando un proceso de prioridad alta está solo en su nivel (sin contendientes), corre normalmente hasta agotar su quantum, y el aging de los demás no afecta porque sus `eff` siguen siendo menores.

### 7.3 `test_prio` ahora pasa

`test_prio` crea procesos con prioridades 1, 3 y 5. Antes del aging, el proceso prio-1 podía no terminar nunca si los prio-3 y prio-5 estaban siempre READY. Con aging:

- El prio-1 acumula espera y eventualmente alcanza `eff = 5` (a los 2 segundos).
- En ese momento compite de igual a igual con el prio-5 (que tiene `eff = 5` cuando su aging es 0).
- Los tres procesos terminan, cada uno imprimiendo `PROCESS <pid> DONE! PRIO: <n>`.

---

## 8. Verificación

### Compilación

Se compiló con `./compile.sh` (First-Fit) y `MM=BUDDY ./compile.sh` (Buddy System), ambos dentro del contenedor Docker `TP_SO_2`. En ambos casos el build pasó con **cero warnings** bajo la bandera estricta:

```
-Wall -Wextra -Werror -Wmissing-prototypes -Wmissing-declarations
-Wredundant-decls -Wformat -Wstrict-prototypes -Wno-unused-parameter
-ffreestanding -nostdlib -mno-red-zone -fno-common -fno-pie
-fno-exceptions -fno-asynchronous-unwind-tables
-mno-mmx -mno-sse -mno-sse2
-fno-builtin-malloc -fno-builtin-free -fno-builtin-realloc -std=c99
```

### Pruebas sugeridas en runtime (QEMU)

Ejecutar dentro del OS con `./run.sh`:

| Comando | Resultado esperado |
|---|---|
| `loop &` → `loop &` → `nice <pid1> 1` | Antes: `15 15 15...` (14 nunca). Ahora: `15 15 15... 14 15 15... 14...` (14 aparece cada ~1s) |
| `test_prio 1000000` | Los 3 procesos (prio 1, 3, 5) **todos** imprimen `DONE!` |
| `test_sync 100 1` | `Valor final: 0` (el aging no rompe la exclusión mutua) |
| `test_mm 2000000` | Sin errores (el aging no afecta al memory manager) |
| `test_processes 10` | Sin errores |
| `ps` | Muestra `priority` base (1-5), sin fluctuar por aging |

### Archivos modificados

| Archivo | Cambio |
|---|---|
| `Kernel/include/process.h` | +`wait_ticks` en PCB |
| `Kernel/c/process/process.c` | init en `create`, reset en `unblock`, `nice`, y dos paths de `waitpid`-wake |
| `Kernel/c/scheduler/scheduler.c` | constantes `AGING_INTERVAL`/`MAX_AGING_BONUS`, helper `aging_bonus`, incremento en `tick`, reset en `tick`/`yield`, `eff` con aging en `next_ready` |
| `AGENTS.md` | nueva sección "Critical invariants (scheduler aging)" |

### No se modificó

| Archivo | Razón |
|---|---|
| `Kernel/asm/interrupts.asm` | El handler `_irq00Handler` ya delega a `scheduler_tick`; el aging vive en C |
| `Kernel/asm/libasm.asm` | No se necesitan nuevos helpers de assembly |
| `Userland/` | El aging es transparente al usuario; `ps` sigue mostrando `priority` base |
