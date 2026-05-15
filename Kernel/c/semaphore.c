#include "semaphore.h"
#include "process.h"
#include "scheduler.h"
#include <stddef.h>

#define MAX_SEMS     32
#define SEM_NAME_LEN 32

typedef struct {
    char     name[SEM_NAME_LEN];
    int64_t  value;
    uint64_t wait_queue[MAX_PROCESSES]; /* PIDs bloqueados esperando este sem */
    int      wait_head;
    int      wait_tail;
    int      wait_count;
    int      open_count; /* 0 = entrada libre en la tabla */
} Semaphore;

static Semaphore sem_table[MAX_SEMS];

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int sem_str_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a == (unsigned char)*b;
}

static void sem_str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (i < max - 1 && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static Semaphore *find_sem(const char *name) {
    int i;
    for (i = 0; i < MAX_SEMS; i++)
        if (sem_table[i].open_count > 0 && sem_str_eq(sem_table[i].name, name))
            return &sem_table[i];
    return NULL;
}

static void queue_push(Semaphore *s, uint64_t pid) {
    if (s->wait_count >= MAX_PROCESSES) return;
    s->wait_queue[s->wait_tail] = pid;
    s->wait_tail = (s->wait_tail + 1) % MAX_PROCESSES;
    s->wait_count++;
}

static uint64_t queue_pop(Semaphore *s) {
    uint64_t pid = s->wait_queue[s->wait_head];
    s->wait_head = (s->wait_head + 1) % MAX_PROCESSES;
    s->wait_count--;
    return pid;
}

/* ── API pública ─────────────────────────────────────────────────────────── */

void sem_init(void) {
    int i, j;
    for (i = 0; i < MAX_SEMS; i++) {
        sem_table[i].open_count = 0;
        sem_table[i].value      = 0;
        sem_table[i].wait_head  = 0;
        sem_table[i].wait_tail  = 0;
        sem_table[i].wait_count = 0;
        sem_table[i].name[0]    = '\0';
        for (j = 0; j < MAX_PROCESSES; j++)
            sem_table[i].wait_queue[j] = 0;
    }
}

/* Abre (o crea) un semáforo por nombre. Retorna 1 en éxito, 0 si tabla llena. */
int64_t sem_open(const char *name, uint64_t initial_value) {
    if (!name) return 0;

    /* ¿Ya existe? */
    Semaphore *s = find_sem(name);
    if (s) {
        s->open_count++;
        return 1;
    }

    /* Buscar slot libre */
    int i;
    for (i = 0; i < MAX_SEMS; i++) {
        if (sem_table[i].open_count == 0) {
            s = &sem_table[i];
            break;
        }
    }
    if (!s) return 0;

    sem_str_copy(s->name, name, SEM_NAME_LEN);
    s->value      = (int64_t)initial_value;
    s->open_count = 1;
    s->wait_head  = 0;
    s->wait_tail  = 0;
    s->wait_count = 0;
    return 1;
}

/* Decrementa el semáforo; bloquea el proceso actual si value < 0. */
int64_t sem_wait(const char *name) {
    if (!name) return -1;

    Semaphore *s = find_sem(name);
    if (!s) return -1;

    s->value--;

    if (s->value < 0) {
        PCB *cur = process_current();
        if (cur == NULL) return -1;
        queue_push(s, cur->pid);
        process_block(cur->pid); /* también setea force_switch si es el actual */
    }

    return 0;
}

/* Incrementa el semáforo; despierta un proceso en espera si value <= 0. */
int64_t sem_post(const char *name) {
    if (!name) return -1;

    Semaphore *s = find_sem(name);
    if (!s) return -1;

    s->value++;

    if (s->value <= 0 && s->wait_count > 0) {
        uint64_t pid = queue_pop(s);
        process_unblock(pid);
    }

    return 0;
}

/* Decrementa el contador de usuarios; libera la entrada si llega a 0. */
int64_t sem_close(const char *name) {
    if (!name) return -1;

    Semaphore *s = find_sem(name);
    if (!s) return -1;

    s->open_count--;
    if (s->open_count <= 0) {
        s->open_count = 0;
        s->name[0]    = '\0';
        s->wait_head  = 0;
        s->wait_tail  = 0;
        s->wait_count = 0;
    }

    return 0;
}
