#include <stdint.h>
#include "include/shell.h"
#include "include/userlib.h"
#include "include/test_util.h"

#define CAT_BUF_SIZE 128

int64_t cat(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    char buf[CAT_BUF_SIZE];

    while (1) {
        uint64_t n = sys_read(STDIN, buf, CAT_BUF_SIZE);
        if (n == 0) {
            shellPrintString("[CAT] EOF\n");
            break;
        }
        if (n == (uint64_t)-1) {
            shellPrintString("[CAT] BLOCKED\n");
            continue;
        }
        shellPrintString("[CAT] got ");
        char tmp[16];
        num_to_str(n, tmp, 10);
        shellPrintString(tmp);
        shellPrintString(" bytes\n");
        sys_write(STDOUT, buf, n);
    }

    return 0;
}
