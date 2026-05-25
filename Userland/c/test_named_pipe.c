#include "include/syscall.h"
#include "include/test_util.h"
#include "include/userlib.h"
#include "include/shell.h"

#define NP_PIPE_NAME "test_np"
#define NP_MSG      "HELLO_NAMED_PIPE"
#define NP_MSG_LEN  17
#define NP_BUF_SIZE  64

void np_writer_entry(int argc, char *argv[]){
    (void)argc; (void)argv;
    int fds[2];
    int64_t rc = sys_pipe_open(NP_PIPE_NAME, fds);
    if(rc < 0){
        printf("[Writer] Error: pipe_open fallo\n");
        sys_exit(1);
    }

    sys_close((uint64_t)fds[0]);

    sys_dup2((uint64_t)fds[1], 1);

    uint64_t written = 0;
    while(written < NP_MSG_LEN){
        uint64_t n = sys_write(1, NP_MSG + written, NP_MSG_LEN - written);
        if(n == (uint64_t)-1)
            continue;
        if(n == 0)
            break;
        written += n;
    }
    if(written != NP_MSG_LEN){
        printf("[Writer] Error: escribio %u, esperaba %u\n",
               (unsigned)written, (unsigned)NP_MSG_LEN);
    }

    sys_close((uint64_t)fds[1]);
    sys_exit(0);
}

void np_reader_entry(int argc, char *argv[]){
    (void)argc; (void)argv;
    int fds[2];
    int64_t rc = sys_pipe_open(NP_PIPE_NAME, fds);
    if(rc < 0){
        printf("[Reader] Error: pipe_open fallo\n");
        sys_exit(1);
    }

    sys_close((uint64_t)fds[1]);

    sys_dup2((uint64_t)fds[0], 0);

    char buf[NP_BUF_SIZE];
    uint64_t total = 0;
    uint64_t n;
    while(total < NP_MSG_LEN){
        n = sys_read(0, buf + total, NP_BUF_SIZE - total);
        if(n == 0)
            break;
        if(n == (uint64_t)-1)
            continue;
        total += n;
    }
    buf[total] = '\0';

    sys_close((uint64_t)fds[0]);
    printf("[Reader] Recibio: %s\n", buf);

    int ok = 1;
    if(total != NP_MSG_LEN){
        ok = 0;
        printf("[Reader] Error: largo %u, esperaba %u\n",
               (unsigned)total, (unsigned)NP_MSG_LEN);
    } else {
        for(uint64_t i = 0; i < total; i++){
            if(buf[i] != NP_MSG[i]){
                ok = 0;
                printf("[Reader] Error: diferencia en pos %u\n", (unsigned)i);
                break;
            }
        }
    }

    if(ok){
        printf("[Reader] Datos OK!\n");
    }
    sys_exit(ok ? 0 : 1);
}

void test_named_pipe_cmd(void){
    printf("[Named pipe] Creando writer y reader...\n");

    int64_t wid = my_create_process_fg("np_writer", 0, 0, 0);
    int64_t rid = my_create_process_fg("np_reader", 0, 0, 0);

    if(wid < 0 || rid < 0){
        printf("[Named pipe] Error: no se pudieron crear procesos\n");
        return;
    }

    my_nice((uint64_t)rid, 4);

    int64_t writer_ret = my_wait(wid);
    int64_t reader_ret = my_wait(rid);

    if(writer_ret == 0 && reader_ret == 0){
        printf("[Named pipe] PASSED\n");
    } else {
        printf("[Named pipe] FAILED (writer=%d reader=%d)\n",
               (int)writer_ret, (int)reader_ret);
    }
}

int64_t test_named_pipe_entry(int argc, char *argv[]){
    (void)argc; (void)argv;
    printf("[Named pipe] Creando writer y reader...\n");

    int64_t wid = my_create_process_fg("np_writer", 0, 0, 0);
    int64_t rid = my_create_process_fg("np_reader", 0, 0, 0);

    if(wid < 0 || rid < 0){
        printf("[Named pipe] Error: no se pudieron crear procesos\n");
        return -1;
    }

    my_nice((uint64_t)rid, 4);

    int64_t writer_ret = my_wait(wid);
    int64_t reader_ret = my_wait(rid);

    if(writer_ret == 0 && reader_ret == 0){
        printf("[Named pipe] PASSED\n");
        return 0;
    }
    printf("[Named pipe] FAILED (w=%d r=%d)\n",
           (int)writer_ret, (int)reader_ret);
    return 1;
}