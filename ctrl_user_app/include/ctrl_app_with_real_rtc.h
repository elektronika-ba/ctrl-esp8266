#ifndef __CTRL_APP_WITH_REAL_RTC_H
#define __CTRL_APP_WITH_REAL_RTC_H

#include "c_types.h"
#include "../../ctrl/include/ctrl_platform.h"
#include "../../ctrl/include/ctrl_stack.h"

// custom functions for this app

// required functions used by ctrl_platform.c
static void ctrl_app_message_received(tCtrlMessage *);
void ctrl_app_init(tCtrlAppCallbacks *);

#endif