#include "include/syscall.h"
#include "include/test_util.h"
#include "include/userlib.h"
#include "include/shell.h"

/* Nombre del pipe nombrado usado para IPC entre escritor y lector */
#define NP_PIPE_NAME "test_np"

#define NP_MSG "HELLO_NAMED_PIPE" /* Mensaje a transmitir por el pipe. */

#define NP_MSG_LEN  17  /* Longitud del mensaje. */

/* Tamano del buffer de lectura en el proceso lector. */
#define NP_BUF_SIZE  64

/* Punto de entrada del proceso escritor. */
void np_writer_entry(int argc, char *argv[]){
    (void)argc; 
    (void)argv;
    
    /* Par de descriptores: fds[0]=lectura, fds[1]=escritura. */
    int fds[2];
    
    /* Abrir/crear pipe nombrado; ambos extremos quedan en fds. */
    int64_t rc = sys_pipe_open(NP_PIPE_NAME, fds);

    if(rc < 0){
        printf("[Writer] Error: pipe_open fallo\n");
        sys_exit(1);
    }

    /* Cerrar extremo de lectura (el escritor no lee). */
    sys_close((uint64_t)fds[0]);

    /* Redirigir stdout (fd=1) al extremo de escritura del pipe. */
    sys_dup2((uint64_t)fds[1], 1);

    /* Escribir el mensaje en un bucle para manejar escrituras parciales. */
    uint64_t written = 0;
    while(written < NP_MSG_LEN){
        /* Escribir bytes restantes desde la posicion actual. */
        uint64_t n = sys_write(1, NP_MSG + written, NP_MSG_LEN - written);
        
        /* Si fallo la escritura, reintentar */
        if(n == (uint64_t)-1){
            continue;
        }
        
        /* Si write devuelve 0, cortar (pipe cerrado o error). */
        if(n == 0){
            break;
        }
        
        /* Avanzar la posicion segun bytes escritos. */
        written += n;
    }
    
    /* Verificar que se escribieron todos los bytes. */
    if(written != NP_MSG_LEN){
        printf("[Writer] Error: escribio %u, esperaba %u\n", (unsigned)written, (unsigned)NP_MSG_LEN);
    }

    /* Cerrar el extremo de escritura y salir. */
    sys_close((uint64_t)fds[1]);
    sys_exit(0);
}

/* Punto de entrada del proceso lector
 * 
 * Abre un pipe nombrado y lee un mensaje. Valida que el mensaje recibido
 * coincida exactamente con el esperado (misma longitud y mismo contenido).
 */
void np_reader_entry(int argc, char *argv[]){
    (void)argc; 
    (void)argv;
    
    /* Par de descriptores: fds[0]=lectura, fds[1]=escritura */
    int fds[2];
    
    /* Abrir/crear pipe nombrado (debe coincidir con el del escritor) */
    int64_t rc = sys_pipe_open(NP_PIPE_NAME, fds);
    if(rc < 0){
        printf("[Reader] Error: pipe_open fallo\n");
        sys_exit(1);
    }

    /* Cerrar extremo de escritura (el lector no escribe) */
    sys_close((uint64_t)fds[1]);

    /* Redirigir stdin (fd=0) al extremo de lectura del pipe */
    sys_dup2((uint64_t)fds[0], 0);

    /* Buffer para almacenar los datos recibidos */
    char buf[NP_BUF_SIZE];
    
    /* Total de bytes leidos hasta ahora */
    uint64_t total = 0;
    
    /* Bytes leidos en la llamada actual a sys_read */
    uint64_t n;
    
    /* Leer en bucle hasta recibir todos los bytes esperados */
    while(total < NP_MSG_LEN){
        /* Leer bytes restantes al buffer; escribir desde buf[total] */
        n = sys_read(0, buf + total, NP_BUF_SIZE - total);
        
        /* Si read devuelve 0, pipe cerrado o EOF, dejar de leer */
        if(n == 0){
            break;
        }
        
        /* Si read fallo, reintentar */
        if(n == (uint64_t)-1){
            continue;
        }
        
        /* Incrementar total segun bytes leidos */
        total += n;
    }
    
    /* Terminar la cadena con '\\0' para printf seguro */
    buf[total] = '\0';

    /* Cerrar el extremo de lectura */
    sys_close((uint64_t)fds[0]);
    printf("[Reader] Recibio: %s\n", buf);

    /* Bandera para indicar si la validacion paso */
    int ok = 1;
    
    /* Chequeo 1: validar longitud recibida vs esperada */
    if(total != NP_MSG_LEN){
        ok = 0;

        printf("[Reader] Error: largo %u, esperaba %u\n", (unsigned)total, (unsigned)NP_MSG_LEN);
    }else{
        /* Chequeo 2: validar cada byte vs mensaje esperado */
        for(uint64_t i = 0; i < total; i++){
            if(buf[i] != NP_MSG[i]){
                ok = 0;
                printf("[Reader] Error: diferencia en pos %u\n", (unsigned)i);
                break;
            }
        }
    }

    /* Imprimir resultado final y salir con el codigo correspondiente */
    if(ok){
        printf("[Reader] Datos OK!\n");
    }

    sys_exit(ok ? 0 : 1);
}

/* Entrada del comando de la shell (primer plano/segundo plano). */
void test_named_pipe_cmd(void){
    printf("[Named pipe] Creando writer y reader...\n");

    /* Crear proceso escritor (fg=0: no es de primer plano). */
    int64_t wid = my_create_process_fg("np_writer", 0, 0, 0);
    
    /* Crear proceso lector (fg=0: no es de primer plano). */
    int64_t rid = my_create_process_fg("np_reader", 0, 0, 0);

    /* Verificar que ambos procesos se crearon bien. */
    if((wid < 0) || (rid < 0)){
        printf("[Named pipe] Error: no se pudieron crear procesos\n");
        return;
    }

    /* Subir la prioridad del lector de 3 a 4 para que lea rapido. */
    my_nice((uint64_t)rid, 4);

    /* Esperar al escritor y obtener su codigo de salida. */
    int64_t writer_ret = my_wait(wid);
    
    /* Esperar al lector y obtener su codigo de salida. */
    int64_t reader_ret = my_wait(rid);

    /* Reporte: pasa solo si ambos procesos salen con 0 (exito) */
    if(writer_ret == 0 && reader_ret == 0){
        printf("[Named pipe] PASSED\n");
    }else{
        printf("[Named pipe] FAILED (writer=%d reader=%d)\n", (int)writer_ret, (int)reader_ret);
    }
}

/*
 * test_named_pipe_entry - Entrada como proceso independiente
 * 
 * Misma logica que test_named_pipe_cmd, pero usada cuando la prueba se
 * spawnea como proceso separado (no built-in de la shell). Retorna un
 * codigo de salida en vez de imprimir un mensaje final.
 */
int64_t test_named_pipe_entry(int argc, char *argv[]){
    (void)argc; 
    (void)argv;
    printf("[Named pipe] Creando writer y reader...\n");

    /* Crear proceso escritor */
    int64_t wid = my_create_process_fg("np_writer", 0, 0, 0);
    
    /* Crear proceso lector */
    int64_t rid = my_create_process_fg("np_reader", 0, 0, 0);

    /* Verificar que ambos procesos se crearon bien. */
    if((wid < 0) || (rid < 0)){
        printf("[Named pipe] Error: no se pudieron crear procesos\n");
        return -1;
    }

    /* Subir la prioridad del lector para que lea rapido. */
    my_nice((uint64_t)rid, 4);

    /* Esperar al escritor. */
    int64_t writer_ret = my_wait(wid);
    
    /* Esperar al lector. */
    int64_t reader_ret = my_wait(rid);

    /* Retornar 0 (exito) si ambos procesos ok, 1 (falla) si no. */
    if(writer_ret == 0 && reader_ret == 0){
        printf("[Named pipe] PASSED\n");
        return 0;
    }
    printf("[Named pipe] FAILED (w=%d r=%d)\n", (int)writer_ret, (int)reader_ret);
    
    return 1;
}
