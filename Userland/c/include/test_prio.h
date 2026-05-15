#ifndef TEST_PRIO_H
#define TEST_PRIO_H

#include "userlib.h"

/* Test de prioridades: 3 procesos incrementando un contador hasta target.
   Primero con la misma prioridad, luego con prioridades distintas para
   visualizar la diferencia en tiempo de ejecucion.
   Lee el argumento (target) via cmd_args(). */
void test_prio(void);

/* Entry point de proceso worker. No-static para el registro de syscall.c. */
void zero_to_max(int argc, char *argv[]);

#endif
