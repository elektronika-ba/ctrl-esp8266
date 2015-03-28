#ifndef __CTRL_APP_SERVO_H
#define __CTRL_APP_SERVO_H

#include "c_types.h"
#include "../../ctrl/include/ctrl_platform.h"
#include "../../ctrl/include/ctrl_stack.h"

// custom functions for this app
static void ICACHE_FLASH_ATTR ctrl_app_servo_pulse(void *);

// required functions used by ctrl_platform.c
static void ctrl_app_message_received(tCtrlMessage *);
void ctrl_app_init(tCtrlAppCallbacks *);

#endif
