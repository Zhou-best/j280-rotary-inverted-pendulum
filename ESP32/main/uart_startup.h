#ifndef UART_STARTUP_H
#define UART_STARTUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


extern int g_k230_action;
extern int g_k230_total;
extern int g_k230_standard;
extern int g_k230_nonstandard;

void uart_startup(void);


void uart_send_to_k230(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
