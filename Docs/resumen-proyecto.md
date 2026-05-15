# Resumen del Proyecto: MASS OS

Es un **sistema operativo bare-metal para x86-64** desarrollado como TP2 de Sistemas Operativos (ITBA). Corre directamente sobre el hardware (o QEMU) sin depender de ningun OS subyacente. Incluye un kernel con drivers, una shell interactiva y un juego Tron.

---

## Arquitectura General

El proyecto sigue una arquitectura de **dos capas** (Kernel + Userland) con comunicacion mediante **syscalls via `int 0x80`**:

```
+------------------------------------------+
|              Userland (0x400000)          |
|  Shell + Benchmarks + Tron               |
|  userlib.asm (wrappers int 0x80)         |
+------------------------------------------+
|              Kernel                       |
|  IDT/IRQs + Syscall Dispatcher           |
|  Drivers: Video, Teclado, Sonido, Timer  |
+------------------------------------------+
|         Bootloader (Pure64 + BMFS)        |
+------------------------------------------+
```

---

## Modulos y su funcion

### Bootloader (`Bootloader/`)
- **Pure64**: bootloader de terceros que lleva el CPU a modo largo (64-bit) y carga el kernel.
- **BMFS**: sistema de archivos simple (BareMetal File System) para empaquetar modulos en la imagen de disco.

### Kernel (`Kernel/`)

| Archivo | Funcion |
|---|---|
| `kernel.c` | Punto de entrada. Carga modulos de usuario en memoria y salta a `0x400000` (userland). |
| `idtLoader.c` | Configura la IDT con 5 entradas: timer (0x20), teclado (0x21), div-by-zero (0x00), invalid opcode (0x06), y syscalls (0x80). Enmascara el PIC. |
| `interrupts.asm` | Handlers ASM de IRQs y excepciones. Implementa push/pop de contexto, snapshot de registros (al presionar CTRL), y el dispatcher de syscalls que indexa la tabla `syscalls[]`. |
| `irqDispatcher.c` | Enruta IRQ0 al timer y IRQ1 al handler de teclado. |
| `syscallDispatcher.c` | Tabla de **16 syscalls** (0-15): read, write, clear, ticks, beep, put pixel, fill rect, hora/fecha, registros, control de fuente, speaker, dimensiones de pantalla. |
| `videoDriver.c` | Driver de video con framebuffer VBE. Soporta modo texto (con fuente bitmap escalable, scroll, cursor) y modo grafico (putPixel, fillRect). |
| `keyboardDriver.c` | Driver de teclado con buffer circular. Traduce scancodes a caracteres (minusculas/mayusculas), maneja Shift, CapsLock, y captura snapshot de registros con CTRL. |
| `sound.c` | Control del parlante de PC (PIT canal 2). Funciones `startSpeaker(freq)`, `turnOff()`, y `beep(freq, ticks)` bloqueante. |
| `time.c` | Cuenta ticks del PIT, implementa `sleep()` por busy-wait con HLT, y lee hora/fecha del RTC en formato BCD. |
| `exceptions.c` | Manejo de excepciones #DE (div/0) y #UD (opcode invalido). Muestra dump de registros y espera ENTER para continuar. |
| `lib.c` / `moduleLoader.c` | Utilidades de memoria (memset, memcpy) y carga de modulos binarios empaquetados. |

### Userland (`Userland/`)

| Archivo | Funcion |
|---|---|
| `userlib.asm` | **Wrappers de syscalls**: cada funcion pone el numero de syscall en RAX y ejecuta `int 0x80`. Son la interfaz entre el codigo C de usuario y el kernel. |
| `shell.c` | Shell interactiva con cursor parpadeante. Lee lineas del teclado, busca el comando en una tabla y lo ejecuta. Soporta backspace y cambio de tamano de fuente con `+`/`-`. |
| `userlib.c` | Implementaciones de comandos: `help`, `clear`, `printTime`, `printDate`, `registers`, `testDiv0`, `invOp`, `playBeep` (toca "Para Elisa"), y 4 benchmarks (FPS, CPU, MEM, KEY). Tambien incluye `processLine()`, `getchar()`, `putchar()`, conversion numerica, y logica de redibujado de pantalla al cambiar fuente. |
| `tron/` | Juego Tron (solo quedan los `.o` compilados, los fuentes fueron eliminados). Incluia: logica de juego, IA, menu, input, renderizado texto/video, y sonido. |

### Toolchain (`Toolchain/`)
- **ModulePacker**: herramienta de host que empaqueta los binarios de userland (code + data modules) en un formato que el kernel sabe cargar.

### Image (`Image/`)
- Makefiles para generar la imagen de disco final (`.img`, `.qcow2`, `.vmdk`) combinando bootloader + kernel empaquetado.

### Scripts raiz
- `compile.sh` / `create.sh`: scripts de build con Docker.
- `run.sh`: lanza QEMU con la imagen generada.

---

## Flujo de ejecucion

1. **Pure64** arranca, pasa a modo 64-bit, carga el kernel.
2. `initializeKernelBinary()` copia los modulos de usuario a `0x400000` y `0x500000`, limpia BSS, y carga la IDT.
3. `main()` del kernel salta a `0x400000` -> ejecuta la **shell de usuario**.
4. La shell lee comandos, que invocan funciones locales o hacen syscalls al kernel via `int 0x80`.
5. Las IRQs del timer (18.2 Hz) y teclado funcionan de forma asincronica via la IDT.
