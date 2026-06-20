# Informe: Corrección de Semáforos — Atomicidad y Cleanup Multi-Semáforo

> Trabajo Práctico Nº 2 — Sistemas Operativos (ITBA)
> Sesión de corrección sobre `Kernel/c/semaphore/semaphore.c` y archivos asociados.

---

## Tabla de contenidos

1. [Conceptos clave](#1-conceptos-clave)
   - [1.1 Atomicidad con XCHG](#11-atomicidad-con-xchg)
   - [1.2 El bitmap `held_sems` en el PCB](#12-el-bitmap-held_sems-en-el-pcb)
2. [Problemas detectados](#2-problemas-detectados)
3. [Resumen detallado de cambios](#3-resumen-detallado-de-cambos)
4. [Por qué este diseño es la mejor decisión](#4-por-qué-este-diseño-es-la-mejor-decisión)
5. [Verificación](#5-verificación)

---

## 1. Conceptos clave

### 1.1 Atomicidad con XCHG

#### ¿Qué es la atomicidad?

En concurrencia, una operación es **atómica** si se ejecuta de forma indivisible: ningún otro hilo/proceso puede observar un estado intermedio de la operación, ni interrumpirla parcialmente. Si dos procesos ejecutan `value--` concurrentemente sobre la misma variable, sin atomicidad puede ocurrir una **condición de carrera** (race condition):

```
Proceso A: lee value (1)
Proceso B: lee value (1)     ← ambos leyeron el mismo valor
Proceso A: escribe value (0)
Proceso B: escribe value (0) ← se perdió un decremento
```

Resultado: `value` quedó en 0 en lugar de -1. Se **perdió** una operación.

#### ¿Qué es XCHG?

`XCHG` es una instrucción del set x86 que **intercambia atómicamente** el contenido de un registro con el de una ubicación de memoria. Su propiedad fundamental es:

> Cuando `XCHG` opera con un operando de memoria, la CPU garantiza que la operación es **indivisible**. En x86 moderno esto se implementa con un **LOCK implícito** sobre el bus/caché (coherencia de cachés via protocolo MESI), por lo que la instrucción no necesita un prefijo `LOCK` explícito.

La forma canónica de construir un **spinlock** (cerrojo giratorio) con `XCHG` es el patrón **test-and-set**:

```asm
atomic_xchg:
    mov rax, rsi          ; rax = newval (el valor que quiero poner)
    xchg rax, [rdi]       ; ATÓMICO: rax <- valor viejo, [rdi] <- newval
    ret                   ; retorno el valor viejo en rax
```

```c
void sem_lock(volatile uint64_t *l){
    while(atomic_xchg(l, LOCKED) == LOCKED){
        /* spin: alguien más tiene el lock */
    }
}
```

- Si `*l` era `0` (UNLOCKED), `XCHG` lo deja en `1` (LOCKED) y retorna `0` → salimos del while → **adquirimos el lock**.
- Si `*l` era `1` (LOCKED), `XCHG` lo deja en `1` y retorna `1` → seguimos girando.

#### ¿Para qué sirve en este kernel?

El kernel corre en **un solo núcleo** (QEMU single-core) y las syscalls se atienden desde una **interrupt gate**, lo que implica `IF=0` (interrupciones deshabilitadas) durante toda la ejecución del handler. Esto significa que, en la práctica, dos procesos **no pueden** ejecutar `sem_wait`/`sem_post` simultáneamente en este hardware. Entonces, ¿por qué hace falta atomicidad?

1. **Requisito de la cátedra:** el enunciado exige explícitamente "utilizar alguna instrucción que garantice atomicidad". La implementación anterior confiaba en `IF=0` implícitamente, lo cual es **funcionalmente** correcto pero **no satisface la letra** del requisito.

2. **Robustez frente a futuros cambios:** si el kernel migrara a multi-core, o si se habilitaran interrupciones durante una syscall (preemción del kernel), o si una interrupción de timer pudiera interrumpir `sem_wait` y reentrar en otra syscall de semáforo, la ausencia de atomicidad se volvería un bug silencioso y muy difícil de depurar. El spinlock es **defensivo**: no cuesta nada en single-core (nunira itera) y protege contra regresiones.

3. **Documentar la sección crítica:** el `sem_lock`/`sem_unlock` hace evidente qué regiones del código son sensibles a concurrencia, lo que mejora la mantenibilidad y legibilidad.

#### Limitación honesta

`XCHG` protege contra concurrencia de hilos/núcleos sobre la **misma** variable, pero no convierte una secuencia larga de código en atómica. Por eso el patrón correcto no es "hacer `value--` atómico" sino **rodear toda la sección crítica** (lectura de `value`, decisión de bloquear/desbloquear, manipulación de la cola de espera) con un spinlock:

```c
sem_lock(&s->lock);
    s->value--;              /* ┐                        */
    if(s->value < 0){        /* │ Sección crítica:       */
        queue_push(s, pid);  /* │ value + cola deben     */
    }                        /* │ verse consistentes     */
sem_unlock(&s->lock);        /* ┘                        */
process_block(pid);          /* fuera del lock           */
```

Si sólo se hiciera atómico el `value--` (con `lock cmpxchg`), otro proceso podría ver el `value` decrementado pero la cola aún sin el PID, o viceversa. El spinlock garantiza la **consistencia de la sección crítica completa**.

---

### 1.2 El bitmap `held_sems` en el PCB

#### ¿Qué es?

`held_sems` es un campo `uint32_t` (32 bits) agregado al PCB que registra **qué semáforos tiene actualmente un proceso**. Funciona como un **bitmap de tenencia**:

```
bit i = 1  →  el proceso posee el recurso del semáforo sem_table[i]
bit i = 0  →  el proceso no lo posee
```

Como `MAX_SEMS = 32`, un `uint32_t` alcanza exactamente para representar todos los semáforos posibles (1 bit por semáforo).

#### ¿Para qué sirve?

Sirve para tres cosas:

1. **Cleanup correcto al morir un proceso.** Cuando un proceso muere (por `kill` o `exit`), el kernel necesita liberar los recursos que tenía tomados para no dejar semáforos "colgados" y para despertar a otros procesos que estaban esperando. Con `held_sems`, el cleanup itera **todos** los semáforos abiertos y, para cada uno, consulta el bit correspondiente: si está en 1, libera el recurso (`value++` + despertar un waiter).

2. **Soportar múltiples semáforos por proceso.** Un proceso puede abrir y adquirir varios semáforos simultáneamente (por ejemplo, para proteger distintas secciones críticas). El campo anterior (`sem_name[32]`) sólo podía recordar **un** nombre de semáforo. `held_sems` puede recordar hasta 32 semáforos a la vez.

3. **Transferencia de recurso en `sem_post`.** Cuando `sem_post` despierta a un proceso bloqueado, el recurso que se libera no queda "libre" en el semáforo: se **transfiere** directamente al proceso despertado. Esto se modela poniendo el bit correspondiente en el `held_sems` del despertado, de forma que si ese proceso muere después (sin hacer su propio `sem_post`), el cleanup sepa que tiene que liberar ese recurso.

#### Semántica del bit

| Evento | Acción sobre `held_sems` |
|---|---|
| `sem_wait` adquiere recurso (path no-bloqueante, `value >= 0`) | `bit |= (1 << idx)` (lo setea) |
| `sem_wait` se bloquea (`value < 0`) | **no toca** el bit (no tiene el recurso, está esperando) |
| `sem_post` (el que llama libera) | `bit &= ~(1 << idx)` (lo limpia) |
| `sem_post` despierta a un waiter | `waiter->bit |= (1 << idx)` (transfiere al despertado) |
| `sem_close` | `bit &= ~(1 << idx)` (limpia por si acaso) |
| `sem_cleanup_for_process` | lee cada bit; si está en 1, libera; al final `held_sems = 0` |

---

## 2. Problemas detectados

### Problema A: Modificación de `value` sin instrucción atómica

**Ubicación:** `semaphore.c:152-191` (`sem_wait`) y `semaphore.c:194-216` (`sem_post`).

**Síntoma:** `s->value--` y `s->value++` se ejecutaban como instrucciones C comunes, sin ningún spinlock ni instrucción atómica.

**Por qué "funcionaba" igual:** las syscalls se atienden desde una interrupt gate (`_irq128Handler`), que entra con `IF=0`. Al estar las interrupciones deshabilitadas, ningún otro código del kernel puede interrumpir la secuencia `value--` + `queue_push` + `process_block`. En QEMU single-core no hay otro núcleo que pueda ejecutar concurrentemente. Es decir, la atomicidad provenía de una **propiedad implícita del entorno**, no de una garantía explícita del código.

**Qué problemas podía generar:**

- **No cumple el requisito de la cátedra.** El enunciado pide explícitamente el uso de una instrucción que garantice atomicidad. Confiar en `IF=0` es una justificación implícita que el corrector rechazó.
- **Fragilidad arquitectónica.** Si en el futuro:
  - se habilitara preemción dentro del kernel,
  - se agregara una interrupción de timer que reentre en el scheduler durante una syscall,
  - se migrara a SMP/multi-core,
  la sección crítica dejaría de ser segura sin ningún cambio visible en el código. Sería un bug silencioso.
- **Falta de documentación de la sección crítica.** Sin un lock explícito, no es evidente qué partes de `sem_wait`/`sem_post` son sensibles a concurrencia y cuáles no. Esto dificulta el mantenimiento y la revisión por pares.

### Problema B: Cleanup de proceso limitado a un solo semáforo

**Ubicación:** `semaphore.c:221-266` (`sem_cleanup_for_process`).

**Síntoma:** La función leía `p->sem_name` (un solo campo de 32 bytes en el PCB) y limpiaba únicamente el semáforo cuyo nombre coincidía. Si un proceso tenía **múltiples** semáforos abiertos, los restantes no se liberaban al morir el proceso.

**Por qué es grave:**

- **Fuga de recursos.** Un proceso que abre 2 semáforos, adquiere uno con `sem_wait`, y luego es matado, deja el segundo semáforo sin cleanup. El `value` no se compensa, y otros procesos que esperaban en ese semáforo pueden quedar **bloqueados para siempre** (deadlock).
- **Post espurio (el bug más sutil).** El `else` de `sem_cleanup_for_process` asumía: "si el proceso no está bloqueado en el semáforo pero tiene su nombre en `sem_name`, entonces **tomó el recurso**" y ejecutaba `value++`. Pero `sem_name` **nunca se limpiaba** en `sem_post`: el campo quedaba con el nombre del último semáforo que el proceso tocó, **incluso después de haber hecho `sem_post` correctamente**. Consecuencia: si el proceso moría después de un ciclo `sem_wait` → `sem_post` → `sem_post` (post extra), el cleanup ejecutaba `value++` **sin razón**, dejando el semáforo en un estado incorrecto. Este es un **post espurio**: un increment fantasma que puede desbloquear a un proceso que no debería haberse desbloqueado, rompiendo la exclusión mutua.

**Ejemplo concreto del post espurio:**

```
1. sem_open("s", 1)        → value = 1
2. sem_wait("s")           → value = 0, sem_name = "s", sem_blocked = 0
3. sem_post("s")           → value = 1, sem_name = "s" (NO se limpia), sem_blocked = 0
4. proceso muere (kill)
5. sem_cleanup_for_process:
     sem_name != ""        → entra
     sem_blocked == 0      → entra al else "tenía el recurso"
     value++               → value = 2  ← ¡POST ESPURIO! El proceso ya había liberado
```

Ahora `value = 2` permite que **dos** procesos adquieran el semáforo simultáneamente, rompiendo la exclusión mutua que el semáforo debería garantizar.

### Problema C (detectado adicionalmente): `process_exit` no hacía cleanup

**Ubicación:** `Kernel/c/process/process.c:280` (`process_exit`).

**Síntoma:** Sólo `process_kill` llamaba a `sem_cleanup_for_process`. `process_exit` (salida normal de un proceso que termina su `entry`) no limpiaba los semáforos.

**Por qué es grave:** Un proceso que sale normalmente sin haber hecho `sem_post`/`sem_close` (por ejemplo, porque retornó de su función por un camino de error) fuga el recurso. El semáforo queda "tomado" para siempre y cualquier otro proceso que haga `sem_wait` sobre él se bloquea indefinidamente. Esto es un **deadlock por fuga**.

### Problema D (detectado adicionalmente): MVar con el mismo problema de atomicidad

**Ubicación:** `Kernel/c/syscall/mvar.c`.

**Síntoma:** `mvar_put`/`mvar_take` modifican `state` y `value` sin instrucciones atómicas, igual que los semáforos originales.

**Decisión:** Queda **fuera del alcance** de esta corrección por decisión explícita (ver sección 4). Se documenta como pendiente en `AGENTS.md`.

---

## 3. Resumen detallado de cambios

### 3.1 Nuevo helper atómico `atomic_xchg`

**Archivos:** `Kernel/asm/libasm.asm`, `Kernel/include/lib.h`.

Se agregó una rutina en assembly que realiza el intercambio atómico:

```asm
atomic_xchg:
    mov rax, rsi          ; rax = newval
    xchg rax, [rdi]       ; atómico: rax <- old, [rdi] <- newval
    ret
```

Y su prototipo en C:

```c
uint64_t atomic_xchg(volatile uint64_t *ptr, uint64_t newval);
```

**Justificación:** `xchg` con operando de memoria tiene LOCK implícito en x86, por lo que es atómico sin prefijo adicional. Es la instrucción textbook para implementar spinlocks test-and-set. Se eligió una rutina reutilizable (no inline) para poder usarla tanto en semáforos como, eventualmente, en MVar u otros mecanismos de sincronización. El `Makefile` ya compila todos los `.asm` por wildcard, así que no requirió cambios de build.

### 3.2 Spinlock en la estructura `Semaphore`

**Archivo:** `Kernel/c/semaphore/semaphore.c`.

Se agregó el campo `volatile uint64_t lock` al struct `Semaphore` y los helpers `sem_lock`/`sem_unlock`:

```c
static void sem_lock(volatile uint64_t *l){
    while(atomic_xchg(l, LOCKED) == LOCKED){ /* spin */ }
}
static void sem_unlock(volatile uint64_t *l){
    atomic_xchg(l, UNLOCKED);
}
```

El lock se inicializa en `UNLOCKED (0)` tanto en `sem_init` como en `sem_open` (slot nuevo).

**Justificación del `volatile`:** evita que el compilador optimice lecturas del lock (lo mantendría en registro y nunca releería memoria). Aunque `xchg` ya impone barreras de memoria, `volatile` es la forma correcta de expresar "esta variable puede cambiar fuera del flujo de control local".

### 3.3 Reescritura de `sem_wait`

**Cambio:** toda la sección crítica (decremento de `value` + decisión de bloquear + encolado) ahora se hace bajo `sem_lock`. El `process_block` se llama **después** de `sem_unlock`.

**Lógica clave:**
- Si `value >= 0` tras el decremento: el proceso adquirió el recurso → setea su bit en `held_sems`.
- Si `value < 0`: el proceso se encola y se bloquea → **no** setea el bit (no tiene el recurso, está esperando).

**Por qué el unlock va antes del block:** `process_block` sólo setea `state = BLOCKED` y `force_switch = 1`; el cambio de contexto real ocurre al regresar de la interrupción (`iretq`). Si se liberara el lock después de `process_block`, el proceso descheduleado nunca ejecutaría el `sem_unlock` → el lock quedaría tomado para siempre → **deadlock** en la siguiente operación sobre ese semáforo. Este es el invariant más crítico de la implementación.

### 3.4 Reescritura de `sem_post`

**Cambio:** la sección crítica (limpieza del bit del caller + incremento de `value` + desencolado) se hace bajo `sem_lock`. El `process_unblock` se llama después de `sem_unlock`.

**Lógica clave — transferencia de recurso:**
- El caller limpia su propio bit en `held_sems` (libera su tenencia).
- Si hay un waiter, se lo despierta y se le **setea** el bit en `held_sems` (transfiere el recurso).

**Por qué transferir el bit:** el semáforo en este caso no vuelve a `value = 1` (libre), sino que el `value++` se "consume" inmediatamente desbloqueando al waiter. El recurso pasa directamente del caller al despertado. Si el despertado muere después sin hacer su propio `sem_post`, el cleanup debe saber que tiene ese recurso para liberarlo. Sin el bit, el cleanup no tendría forma de saberlo y se fugarían recursos.

### 3.5 Reescritura de `sem_cleanup_for_process`

**Cambio:** la función ahora itera **todos** los semáforos abiertos (no solo uno) y, para cada uno, aplica una lógica de dos ramas independientes:

```
Para cada semáforo s abierto:
  1. Si el PID estaba en la cola de espera:
       - lo saca de la cola
       - value++  (compensa el value-- que hizo sem_wait antes de bloquearlo)
  2. Si el PID tenía el bit held:
       - limpia el bit
       - value++  (libera el recurso que tenía tomado)
       - si hay waiters, despierte uno y transfiérale el bit
  3. Si ninguna de las dos: NO toca value
```

**Por qué esto elimina el post espurio:** en el diseño anterior, el `else` asumía que "no estar en cola + tener `sem_name` = tenía el recurso". Pero `sem_name` no se limpiaba tras un `sem_post` exitoso, así que la premisa era falsa. Ahora, la tenencia se determina por el bit `held_sems`, que **sí** se limpia en cada `sem_post`. Si el proceso ya liberó el recurso, el bit está en 0, y el cleanup no ejecuta `value++`. **Sólo** se incrementa `value` si hay evidencia positiva de que el proceso tenía algo que liberar.

**Por qué las dos ramas son independientes y se pueden cumplir ambas:** un proceso puede estar esperando en la cola de un semáforo A (bit de A en 0, pero encolado) y tener tomado el semáforo B (bit de B en 1). Al morir, hay que compensar el decremento de A (lo encoló pero nunca lo adquirió) **y** liberar el recurso de B. Ambos `value++` son correctos y necesarios. Si el proceso tenía el mismo semáforo en cola y a la vez el bit held (caso patológico, no debería ocurrir en uso normal), ambos compensan y el net es `value += 2`, lo cual también es correcto porque el `sem_wait` hizo un `value--` y nunca llegó a adquirirlo de vuelta.

### 3.6 `sem_close` limpia el bit held

**Cambio:** al cerrar un semáforo, el caller limpia su bit en `held_sems`.

**Justificación:** aunque cerrar un semáforo no es lo mismo que liberarlo (si el proceso tenía el recurso y lo cierra sin `sem_post`, lo fuga — bug del usuario, no del kernel), limpiar el bit evita referencias stale al índice del slot. Si el slot se reutiliza después con otro nombre y el bit quedó en 1, un cleanup futuro podría liberar un recurso que pertenece a un semáforo distinto. Limpiar el bit al cerrar es defensivo y gratuito.

### 3.7 Cambios en el PCB

**Archivo:** `Kernel/include/process.h`.

Se eliminaron los campos:
```c
char sem_name[32];      /* nombre del último semáforo tocado */
uint8_t sem_blocked;    /* 1 = bloqueado esperando, 0 = tenía el recurso */
```

Y se reemplazaron por:
```c
uint32_t held_sems;     /* bitmap: bit i = proceso tiene recurso de sem_table[i] */
```

**Justificación del cambio de tipo:** `sem_name` era un string de hasta 32 bytes que sólo podía recordar **un** semáforo, y `sem_blocked` era un booleano ambiguo que mezclaba dos estados (bloqueado vs. tenía-recurso) en un solo campo. `held_sems` es un bitmap compacto (4 bytes) que puede representar **32** semáforos simultáneos y expresa la tenencia de recurso de forma **positiva y unívoca** (bit en 1 = lo tiene; bit en 0 = no lo tiene). No hay ambigüedad.

### 3.8 Cambios en `process.c`

**Archivo:** `Kernel/c/process/process.c`.

- `process_create`: inicializa `held_sems = 0` (antes inicializaba `sem_name[0] = '\0'` y `sem_blocked = 0`).
- `process_exit`: **agregado** el llamado `sem_cleanup_for_process(current_process->pid)` antes de marcar ZOMBIE y liberar recursos. Esto cierra la ventana de fuga cuando un proceso termina normalmente sin `sem_post`/`sem_close`.
- `process_unblock`: se eliminó la línea `p->sem_blocked = 0` porque el campo ya no existe. `process_unblock` es una función genérica de cambio de estado; no debería saber de semáforos. La limpieza de tenencia se maneja ahora exclusivamente en `sem_post` y `sem_cleanup_for_process`.

### 3.9 Actualización de `AGENTS.md`

Se actualizó la sección "Critical constraints" para reflejar que la atomicidad ya se implementa con `xchg` (antes era un requisito pendiente), y se agregó una nueva sección "Critical invariants (semaphores)" que documenta los invariantes que no deben romperse en futuras modificaciones:

- el orden unlock → block/unblock (deadlock si se invierte),
- la semántica del bitmap `held_sems` (cuándo se setea, cuándo se limpia),
- la iteración completa en `sem_cleanup_for_process`,
- la llamada al cleanup desde ambos paths (`kill` y `exit`).

---

## 4. Por qué este diseño es la mejor decisión

### 4.1 Spinlock con XCHG vs. alternativas

| Alternativa | Pros | Contras | Veredicto |
|---|---|---|---|
| **Spinlock con XCHG** (elegida) | Patrón textbook, claramente "usa XCHG", protege **toda** la sección crítica (value + cola), future-proof para SMP, reutilizable en MVar | Overhead mínimo (un xchg por lock/unlock); en single-core nunca itera | **Mejor opción** |
| `lock cmpxchg` (CAS) sobre `value` solo | Menos código | Sólo hace atómico el `value--`; la cola sigue sin proteger. No protege la sección crítica completa. Satisface la letra del requisito pero no el espíritu | Insuficiente |
| Deshabilitar interrupciones (`cli`/`sti`) | Simple en single-core | Ya está implícito por la interrupt gate; no satisface el requisito de "instrucción atómica"; no portable a SMP | Rechazada |
| No hacer nada (status quo) | Cero código | No cumple el requisito; frágil | Rechazada |

El spinlock con XCHG es la opción que mejor satisface simultáneamente: (a) el requisito explícito de la cátedra, (b) la protección real de la sección crítica completa, (c) la robustez frente a cambios futuros del kernel, y (d) la claridad documental.

### 4.2 Bitmap `held_sems` vs. alternativas

| Alternativa | Pros | Contras | Veredicto |
|---|---|---|---|
| **Bitmap `held_sems`** (elegida) | Soporta múltiples semáforos, representación positiva y unívoca, compacto (4 bytes), permite transferencia de recurso en `sem_post` | Limita a 1 unidad de tenencia por semáforo por proceso (suficiente para uso mutex/test_sync) | **Mejor opción** |
| Iterar todos + compensar solo encolados (estilo MVar) | Más simple | Fuga el recurso si matás un proceso que estaba en sección crítica (no libera el `value` que tenía tomado) | Insuficiente |
| `sem_name[32]` (status quo) | — | Sólo un semáforo, post espurio por no limpiarse, ambigüedad bloqueado vs. tenencia | Rechazada |
| Array dinámico de semáforos por proceso | Sin límite de cantidad | Requiere `mm_malloc_kernel` por proceso, complejidad de gestión, fugas si se olvida liberar | Overkill para TP |

El bitmap es la opción que resuelve **los dos bugs reportados** (multi-semáforo + post espurio) con la menor complejidad y el menor costo de memoria. Asume 1 unidad de tenencia por semáforo por proceso, lo cual es suficiente para el uso del TP (mutex binario en `test_sync`, sincronización lector/escritor en MVar). Si se necesitara counting semáforos con tenencia múltiple, habría que pasar a un array de contadores, pero eso está fuera del scope.

### 4.3 Llamada al cleanup desde `process_exit`

Agregar `sem_cleanup_for_process` a `process_exit` (además de `process_kill` donde ya estaba) cierra la ventana de fuga cuando un proceso termina **normalmente** sin haber liberado sus semáforos. Esto es consistente con el principio de "todo path de salida de un proceso debe limpiar sus recursos". No tiene downsides: el cleanup es idempotente (si `held_sems == 0` y el PID no está en ninguna cola, no hace nada) y su costo es O(MAX_SEMS) = O(32), despreciable.

### 4.4 Por qué MVar quedó fuera

MVar tiene el mismo problema de atomicidad (modifica `state`/`value` sin `xchg`). Sin embargo, se decidió no incluirlo en esta corrección porque:

1. Las correcciones reportadas por la cátedra **sólo mencionaban semáforos**.
2. MVar tiene una semántica distinta (lectores/escritores, colas separadas, anti-starvation con cooldown) que requiere un análisis más cuidadoso del granularity del lock.
3. El helper `atomic_xchg` ya está disponible y es reutilizable, por lo que la corrección de MVar en el futuro será directa.

Se documentó explícitamente como pendiente en `AGENTS.md`.

---

## 5. Verificación

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
| `test_sync 100 1` | `Valor final: 0` (con semáforos, no hay race condition) |
| `test_sync 100 0` | Valor variable (sin semáforos, hay race condition — esperado) |
| `test_mm 2000000` | Sin errores de alloc/free |
| `test_processes 10` | Sin errores de creación/kill |
| `test_prio 100` | Visualización de diferencias de prioridad |
| `loop &` → `kill <pid>` (repetir) | No fuga recursos; no deadlock |
| `ps` | Lista de procesos consistente |

### Archivos modificados

| Archivo | Cambio |
|---|---|
| `Kernel/asm/libasm.asm` | +`atomic_xchg` |
| `Kernel/include/lib.h` | +prototipo `atomic_xchg` |
| `Kernel/include/process.h` | `sem_name`/`sem_blocked` → `held_sems` |
| `Kernel/c/process/process.c` | `process_create`, `process_exit` (+cleanup), `process_unblock` (-`sem_blocked`) |
| `Kernel/c/semaphore/semaphore.c` | Spinlock, `sem_wait`/`sem_post`/`sem_cleanup_for_process`/`sem_close` reescritos |
| `AGENTS.md` | Constraints e invariants actualizados |
