#ifndef TEST_SYNC_H
#define TEST_SYNC_H

#include <stdint.h>

void test_sync_cmd(void);

/* Entry point de proceso worker. No-static para el registro de syscall.c.
   Firma compatible con ProcessEntry (int argc, char **argv). */
void my_process_inc(int argc, char *argv[]);

#endif
