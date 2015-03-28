#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "mem.h"
#include "espconn.h"
#include "os_type.h"
#include "gpio.h"

#include "../ctrl/include/ctrl_platform.h"
#include "../ctrl/include/ctrl_stack.h"
#include "../misc/include/realrtc.h"

#include "include/ctrl_app_with_real_rtc.h"

static void ICACHE_FLASH_ATTR ctrl_app_message_received(tCtrlMessage *msg)
{
	os_printf("APP MSG:");
	unsigned short i;
	for(i=0; i<msg->length-1-4; i++)
	{
		char tmp2[10];
		os_sprintf(tmp2, " 0x%X", msg->data[i]);
		os_printf_plus(tmp2);
	}
	os_printf(".\r\n");

	tRealRTC *rtc;
	realrtc_get(&rtc);
	char tmp3[120];
	os_sprintf(tmp3, "@RTC: %4d-%2d-%2d %2d:%2d:%2d (%d)\r\n", rtc->year, rtc->month, rtc->day, rtc->hour, rtc->minute, rtc->second, rtc->weekday);
	os_printf_plus(tmp3);
}

// entry point to user app
void ICACHE_FLASH_ATTR ctrl_app_init(tCtrlAppCallbacks *ctrlAppCallbacks)
{
	ctrlAppCallbacks->message_received = ctrl_app_message_received;

	os_printf("ctrl_app_init()\r\n");
}
