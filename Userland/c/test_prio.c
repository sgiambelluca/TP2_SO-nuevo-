#include "../include/test_prio.h"
#include "../include/userlib.h"
#include "../include/shell.h"

#define TPRIO_NPROCS 3

/* Estado compartido: cada worker incrementa su propio slot.
   volatile para evitar que el compilador asuma que no cambia. */
static volatile int64_t counters[TPRIO_NPROCS];
static volatile int64_t target_value;
static volatile uint64_t start_ticks[TPRIO_NPROCS];
static volatile uint64_t end_ticks[TPRIO_NPROCS];

/* argv para cada worker contiene un unico string "0", "1" o "2". */
static char idx_str[TPRIO_NPROCS][2] = { {'0', 0}, {'1', 0}, {'2', 0} };
static char *prio_argv[TPRIO_NPROCS][2] = {
    { idx_str[0], 0 },
    { idx_str[1], 0 },
    { idx_str[2], 0 },
};

static const char *worker_names[TPRIO_NPROCS] = { "prio_0", "prio_1", "prio_2" };

/* atoi minimo */
static int parse_uint_prio(const char *s){
    if(s == 0) return -1;
    int v = 0;
    int i = 0;
    while(s[i] == ' ') i++;
    if(s[i] < '0' || s[i] > '9') return -1;
    while(s[i] >= '0' && s[i] <= '9'){
        v = v*10 + (s[i] - '0');
        i++;
    }
    return v;
}

/* Worker: lee idx desde argv[0][0], incrementa counters[idx] hasta target. */
static void prio_worker(int argc, char **argv){
    (void)argc;
    int idx = 0;
    if(argv != 0 && argv[0] != 0){
        idx = argv[0][0] - '0';
    }
    if(idx < 0 || idx >= TPRIO_NPROCS){
        sys_exit(-1);
    }

    start_ticks[idx] = sys_ticks();
    while(counters[idx] < target_value){
        counters[idx]++;
    }
    end_ticks[idx] = sys_ticks();
    sys_exit(0);
}

static void print_phase_results(const char *label){
    char buf[24];
    shellPrintString(label);
    shellPrintString("\n");
    for(int i = 0; i < TPRIO_NPROCS; i++){
        shellPrintString("  ");
        shellPrintString((char *)worker_names[i]);
        shellPrintString(": counter=");
        num_to_str((uint64_t)counters[i], buf, 10);
        shellPrintString(buf);
        shellPrintString("  ticks=");
        uint64_t elapsed = end_ticks[i] - start_ticks[i];
        num_to_str(elapsed, buf, 10);
        shellPrintString(buf);
        shellPrintString("\n");
    }
}

static int spawn_workers(int64_t *pids, const uint8_t *priorities){
    /* priorities == NULL -> todos default. Sino, aplica nice tras crear. */
    for(int i = 0; i < TPRIO_NPROCS; i++){
        counters[i] = 0;
        start_ticks[i] = 0;
        end_ticks[i] = 0;
    }

    for(int i = 0; i < TPRIO_NPROCS; i++){
        int64_t pid = sys_create_process(worker_names[i], (void *)prio_worker,
                                         1, prio_argv[i], 0);
        if(pid < 0){
            shellPrintString("test_prio: ERROR creando proceso\n");
            /* Matar los previos para limpiar */
            for(int j = 0; j < i; j++) sys_kill((uint64_t)pids[j]);
            return -1;
        }
        pids[i] = pid;
    }

    if(priorities != 0){
        for(int i = 0; i < TPRIO_NPROCS; i++){
            sys_nice((uint64_t)pids[i], (uint64_t)priorities[i]);
        }
    }
    return 0;
}

static void wait_workers(const int64_t *pids){
    for(int i = 0; i < TPRIO_NPROCS; i++){
        sys_waitpid((uint64_t)pids[i]);
    }
}

void test_prio(void){
    const char *args = cmd_args();
    int target = parse_uint_prio(args);
    if(target <= 0){
        shellPrintString("uso: test_prio <target_value>\n");
        return;
    }
    target_value = target;

    int64_t pids[TPRIO_NPROCS];

    /* --- Fase 1: misma prioridad (default) --- */
    shellPrintString("=== Fase 1: misma prioridad ===\n");
    if(spawn_workers(pids, 0) < 0) return;
    wait_workers(pids);
    print_phase_results("Resultados fase 1:");

    /* --- Fase 2: prioridades distintas (1, 3, 5) --- */
    shellPrintString("\n=== Fase 2: prioridades 1, 3, 5 ===\n");
    static const uint8_t priorities[TPRIO_NPROCS] = { 1, 3, 5 };
    if(spawn_workers(pids, priorities) < 0) return;
    wait_workers(pids);
    print_phase_results("Resultados fase 2:");

    shellPrintString("\ntest_prio: hecho. En fase 2, prio_0 (prioridad 1) debe haber tardado mas que prio_2 (prioridad 5).\n");
}
