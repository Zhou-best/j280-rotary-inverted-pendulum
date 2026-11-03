#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define FPGA_MODE_STOP   0
#define FPGA_MODE_RIGHT  3

#define FPGA_MODE_LEFT   4

void fpga_link_init(void);
void fpga_link_send_mode(uint8_t mode);
void fpga_link_send_stop(void);
void fpga_link_send_next(void);
void fpga_link_send_heartbeat(void);


void fpga_link_handle_gesture(const char *arg);


void fpga_link_selftest_start(void);

#ifdef __cplusplus
}
#endif
