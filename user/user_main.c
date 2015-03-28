#include "ets_sys.h"
#include "osapi.h"

#include "include/user_main.h"

#include "../ctrl/include/ctrl_platform.h"
#include "../driver/include/uart.h"

void user_init(void)
{
	#ifdef CTRL_LOGGING
		uart_init(BIT_RATE_115200/*, BIT_RATE_115200*/);
		os_delay_us(1000);
		os_printf("CTRL platform starting...\r\n");
	#endif

	ctrl_platform_init();

	#ifdef CTRL_LOGGING
		os_printf("CTRL platform started!\r\n");
	#endif
}
