#include "../include/test_proc.h"
#include "../include/userlib.h"
#include "../include/shell.h"

#define TP_MAX 32
#define TP_ROUNDS 3

typedef enum { TP_RUNNING, TP_BLOCKED, TP_KILLED } TpState;

typedef struct {
    int64_t pid;
    TpState state;
} TpEntry;

/* LCG simple para no depender de un generador externo. */
static uint64_t rng_state = 0xCAFEBABE12345678ULL;

static uint64_t my_rand(uint64_t mod){
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (rng_state >> 33) % (mod == 0 ? 1 : mod);
}

/* atoi minimo: lee digitos decimales hasta el primer no-digito. */
static int parse_uint(const char *s){
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

/* Proceso dummy: loopea para siempre quemando CPU. Solo muere por kill externo. */
static void endless_loop(int argc, char **argv){
    (void)argc; (void)argv;
    while(1){
        /* sin sys_yield: queremos que el scheduler lo desaloje por quantum */
    }
}

void test_proc(void){
    const char *args = cmd_args();
    int max = parse_uint(args);
    if(max <= 0 || max > TP_MAX){
        shellPrintString("uso: test_proc <max_processes 1-");
        char tmp[8];
        num_to_str(TP_MAX, tmp, 10);
        shellPrintString(tmp);
        shellPrintString(">\n");
        return;
    }

    shellPrintString("test_proc: arrancando ");
    char buf[16];
    num_to_str((uint64_t)TP_ROUNDS, buf, 10);
    shellPrintString(buf);
    shellPrintString(" rondas con max=");
    num_to_str((uint64_t)max, buf, 10);
    shellPrintString(buf);
    shellPrintString("\n");

    TpEntry table[TP_MAX];
    char *empty_argv[1] = { 0 };
    int errors = 0;

    for(int round = 0; round < TP_ROUNDS; round++){
        /* Crear max procesos */
        int created = 0;
        for(int i = 0; i < max; i++){
            int64_t pid = sys_create_process("endless", (void *)endless_loop,
                                             0, empty_argv, 0);
            if(pid < 0){
                shellPrintString("test_proc: ERROR creando proceso\n");
                errors++;
                /* Detenerse en esta ronda; matar lo que haya. */
                for(int j = 0; j < created; j++){
                    sys_kill((uint64_t)table[j].pid);
                }
                goto done;
            }
            table[i].pid = pid;
            table[i].state = TP_RUNNING;
            created++;
        }

        /* Ciclo: matar/bloquear/desbloquear hasta que todos esten muertos. */
        int alive = max;
        int safety = 1000;  /* limite para no colgarse si algo sale mal */
        while(alive > 0 && safety-- > 0){
            for(int i = 0; i < max; i++){
                uint64_t action = my_rand(2);
                if(action == 0){
                    /* kill */
                    if(table[i].state == TP_RUNNING || table[i].state == TP_BLOCKED){
                        sys_kill((uint64_t)table[i].pid);
                        table[i].state = TP_KILLED;
                        alive--;
                    }
                } else {
                    /* block */
                    if(table[i].state == TP_RUNNING){
                        sys_block((uint64_t)table[i].pid);
                        table[i].state = TP_BLOCKED;
                    }
                }
            }
            /* Desbloquear los que quedaron bloqueados. */
            for(int i = 0; i < max; i++){
                if(table[i].state == TP_BLOCKED){
                    sys_unblock((uint64_t)table[i].pid);
                    table[i].state = TP_RUNNING;
                }
            }
            /* Ceder CPU para que los dummies corran un rato. */
            sys_yield();
        }

        if(alive > 0){
            shellPrintString("test_proc: ERROR no terminaron todos los procesos\n");
            errors++;
            for(int i = 0; i < max; i++){
                if(table[i].state != TP_KILLED){
                    sys_kill((uint64_t)table[i].pid);
                }
            }
        }
    }

done:
    if(errors == 0){
        shellPrintString("test_proc: OK\n");
    } else {
        shellPrintString("test_proc: FAIL (");
        num_to_str((uint64_t)errors, buf, 10);
        shellPrintString(buf);
        shellPrintString(" errores)\n");
    }
}
