#ifndef KEYBOARDDRIVER_H
#define KEYBOARDDRIVER_H

#include <defs.h>
#include <stdint.h>

void writeBuff(unsigned char c);
void clearBuff(void);
void handlePressedKey(void);
uint64_t copyRegistersBuffer(char * buff);
uint8_t getFromBuffer(void);
uint64_t readKeyBuff(char * buff, uint64_t count);
uint32_t intToHexa(uint64_t value, char * dest);
extern uint64_t regsArray[];
uint8_t kbd_scancode_read(void);

// Registra el proceso que esta bloqueado esperando input de teclado.
// handlePressedKey lo desbloquea cuando llega un caracter.
struct PCB;
void kbd_set_waiting(struct PCB *p);

#endif