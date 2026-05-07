#ifndef SDIO_BUS_H
#define SDIO_BUS_H

// Minimal SDIO bus transport for CYW43 bring-up.
// This targets the Arasan host with SD1 pinmux (GPIO 34..39 ALT3).

int sdio_bus_init(void);
int sdio_bus_is_ready(void);
unsigned short sdio_bus_get_rca(void);
void sdio_bus_reset_state(void);

int sdio_bus_cmd52_read(unsigned int fn, unsigned int addr, unsigned char* out_val);
int sdio_bus_cmd52_write(unsigned int fn, unsigned int addr, unsigned char val);

int sdio_bus_cmd53_read(unsigned int fn, unsigned int addr, unsigned char* out, unsigned int len);
int sdio_bus_cmd53_write(unsigned int fn, unsigned int addr, const unsigned char* data, unsigned int len);

int sdio_bus_enable_func(unsigned int fn);
int sdio_bus_wait_func_ready(unsigned int fn, unsigned int timeout_ms);

#endif
