#include "scheduler.h"
#include "process.h"
#include "time.h"

// Leida desde interrupts.asm para decidir si hacer context switch voluntario
volatile uint64_t force_switch = 0;

/* Lista de procesos listos para ejecutarse */
static PCB* run_queue[MAX_PROCESSES];      

static int queue_size = 0;
static int queue_idx  = 0;

/* Inicializa la cola de ejecución. */
void scheduler_init(void){
    queue_size = 0;         /* Tamaño de la cola. */
    queue_idx = 0;         /* Índice del proceso actual en la cola (round-robin). */
}

/* Agrega un proceso a la cola de ejecución. */
void scheduler_add(PCB* p){
    if(queue_size < MAX_PROCESSES){
        run_queue[queue_size++] = p;
    }
}

/* Elimina un proceso de la cola de ejecución. */
void scheduler_remove(PCB* p){
    for(int i = 0; i < queue_size; i++){

        if(p == run_queue[i]){

            /* Reemplazar con el último proceso. */
            run_queue[i] = run_queue[--queue_size]; 

            if((queue_idx >= queue_size) && (queue_size > 0)){
                /* Caso en el que estaba procesando el último proceso. */
                queue_idx = queue_size - 1;
            }

            return;
        }
    }
}

/* Elegir el siguiente proceso READY usando round-robin. Solo elige procesos
   en estado READY (excluye explicitamente al RUNNING actual; debe ser marcado
   como READY antes de llamar). Si no hay nadie listo, devuelve NULL. */
PCB* scheduler_next_ready(void){
    if(queue_size == 0){
        return NULL;
    }

    for(int i = 0; i < queue_size; i++){
        /* Avanzo de forma circular. */
        queue_idx = (queue_idx + 1) % queue_size;
        PCB* c = run_queue[queue_idx];

        if(c->state == PROCESS_READY){
            return c;
        }
    }

    return NULL;
}

// ─── Llamada desde _irq00Handler (timer) ─────────────────────────────────────
// Round-Robin con prioridades: el proceso actual sigue corriendo mientras le
// queden quanta (priority quanta consecutivas por turno). Cuando se le agotan,
// pasa a READY y se elige el siguiente de la cola con round-robin.
uint64_t* scheduler_tick(uint64_t *current_rsp){

    /* actualizar contador de ticks. */
    timer_handler();

    PCB* cur = process_current();

    if(cur != NULL){
        cur->rsp = current_rsp;

        if(cur->state == PROCESS_RUNNING){
            if(cur->remaining_quanta > 0){
                cur->remaining_quanta--;
            }

            if(cur->remaining_quanta > 0){
                /* Todavia tiene quanta: NO desalojar — sigue corriendo. */
                return current_rsp;
            }

            /* Quanta agotada: pasa a READY y se reinicia el contador. */
            cur->state = PROCESS_READY;
            cur->remaining_quanta = cur->priority;
        }
    }

    /* Elegir el siguiente proceso READY. */
    PCB* next = scheduler_next_ready();

    if(next == NULL){
        /* Nadie listo: si el actual aun tiene un stack (no esta ZOMBIE/FREE),
           seguimos con el; sino quedamos con el RSP actual y la CPU hace hlt
           hasta el proximo IRQ. */
        if(cur != NULL && (cur->state == PROCESS_READY || cur->state == PROCESS_RUNNING)){
            cur->state = PROCESS_RUNNING;
            return current_rsp;
        }
        return current_rsp;
    }

    next->state = PROCESS_RUNNING;
    process_set_current(next);

    return next->rsp;
}

/* Implementación de yield (pausa). Llamada desde _irq128Handler cuando
   force_switch = 1 (sys_yield, sys_exit, sys_block sobre el proceso actual,
   sys_read sin datos disponibles, etc.). */
uint64_t *scheduler_yield_impl(uint64_t *current_rsp){
    PCB* cur = process_current();

    if(cur != NULL){
        cur->rsp = current_rsp;

        if(cur->state == PROCESS_RUNNING){
            /* Lo pauso (si no fue ya BLOCKED/ZOMBIE por la syscall). */
            cur->state = PROCESS_READY;
        }

        /* Reiniciar quanta para la proxima vez que sea elegido. */
        cur->remaining_quanta = cur->priority;
    }

    PCB* next = scheduler_next_ready();
    if(next == NULL){
        /* Nadie listo distinto del actual. Si cur sigue listo, retomarlo. */
        if(cur != NULL && cur->state == PROCESS_READY){
            cur->state = PROCESS_RUNNING;
            return current_rsp;
        }
        /* Caso patologico: nadie listo y cur esta BLOCKED/ZOMBIE.
           Devolvemos el rsp actual; la CPU hara hlt hasta el proximo IRQ que
           desbloquee a alguien (idle deberia evitar este caso siempre). */
        return current_rsp;
    }

    next->state = PROCESS_RUNNING;
    process_set_current(next);

    return next->rsp;
}

// ─── Arranque: lanza el primer proceso sin retornar ───────────────────────────
void scheduler_start(void){
    PCB* first = scheduler_next_ready();
    if(first == NULL){
        return;
    }

    first->state = PROCESS_RUNNING;
    process_set_current(first);

    /* Definida en interrupts.asm; no retorna */
    scheduler_start_asm(first->rsp);   
}
