#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdint.h>

uint32_t GetUint(void);
uint32_t GetUniform(uint32_t max);
uint8_t  memcheck(void *start, uint8_t value, uint32_t size);
int64_t  satoi(char *str);
void     bussy_wait(uint64_t n);
void     endless_loop(int argc, char *argv[]);
void     endless_loop_print(int argc, char *argv[]);

/* Provisto por _loader.c */
void    *memset(void *s, int32_t c, uint64_t n);

int      printf(const char *fmt, ...);

#endif
