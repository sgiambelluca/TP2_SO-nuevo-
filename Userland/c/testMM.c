#include "include/testMM.h"
#include "include/syscall.h"
#include "include/test_util.h"
#include "include/userlib.h"

#define MAX_BLOCKS 128

typedef struct MM_rq {
    void    *address;
    uint32_t size;
} mm_rq;

static void test_mm_loop(uint64_t max_memory) {
    mm_rq    mm_rqs[MAX_BLOCKS];
    uint8_t  rq;
    uint32_t total;
    uint32_t i;

    while (1) {
        rq    = 0;
        total = 0;

        /* Pedir tantos bloques como sea posible */
        while (rq < MAX_BLOCKS && total < max_memory) {
            mm_rqs[rq].size    = GetUniform((uint32_t)(max_memory - total - 1)) + 1;
            mm_rqs[rq].address = malloc(mm_rqs[rq].size);

            if (mm_rqs[rq].address) {
                total += mm_rqs[rq].size;
                rq++;
            } else {
                break;
            }
        }

        /* Inicializar cada bloque con su índice */
        for (i = 0; i < rq; i++)
            if (mm_rqs[i].address)
                memset(mm_rqs[i].address, (int32_t)i, mm_rqs[i].size);

        /* Verificar integridad */
        for (i = 0; i < rq; i++) {
            if (mm_rqs[i].address) {
                if (!memcheck(mm_rqs[i].address, (uint8_t)i, mm_rqs[i].size)) {
                    printf("test_mm ERROR\n");
                    return;
                }
            }
        }

        /* Liberar */
        for (i = 0; i < rq; i++)
            if (mm_rqs[i].address)
                free(mm_rqs[i].address);
    }
}

/* Wrapper de shell: testMM <max_memory> */
void testMM(void) {
    const char *args = cmd_args();
    int64_t max_memory = satoi((char *)args);
    if (max_memory <= 0) {
        shellPrintString("uso: testMM <max_memory>\n");
        return;
    }
    test_mm_loop((uint64_t)max_memory);
}
