#include "ets_sys.h"
#include "os_type.h"
#include "osapi.h"
#include "mem.h"
#include "gpio.h"
#include "user_interface.h"

#include "include/realrtc.h"

static tRealRTC realRTC;
static os_timer_t tmrRealRTC;

void(*secondTickCallback)(tRealRTC *);

// calculate if given year is leap year
static unsigned char realrtc_isleapyear(unsigned short y)
{
	return ( ( !(y % 4) && (y % 100) ) || !(y % 400) );
}

static void ICACHE_FLASH_ATTR ctrl_real_rtc_1s(void *arg)
{
	realRTC.second++; // another second of our life has gone by

	// a minute!
	if(realRTC.second >= 60)
	{
		realRTC.second = 0;
		realRTC.minute++;
	}

	// an hour...
	if(realRTC.minute >= 60)
	{
		realRTC.minute = 0;
		realRTC.hour++;
	}

	// a day....
	if(realRTC.hour >= 24)
	{
		realRTC.hour = 0;
		realRTC.day++;

		realRTC.weekday++;
		if(realRTC.weekday > 7)
		{
			realRTC.weekday = 1; // It's Monday again...
		}
	}

	// a full month with leap year checking!
	if(
		(realRTC.day > 31)
		|| (
			(realRTC.day == 31)
			&& (
				(realRTC.month == 4)
				|| (realRTC.month == 6)
				|| (realRTC.month == 9)
				|| (realRTC.month == 11)
				)
		)
		|| (
			(realRTC.day == 30)
			&& (realRTC.month == 2)
		)
		|| (
			(realRTC.day == 29)
			&& (realRTC.month == 2)
			&& !realrtc_isleapyear(realRTC.year)
		)
	)
	{
		realRTC.day = 1;
		realRTC.month++;
	}

	// HAPPY NEW YEAR!
	if(realRTC.month >= 13)
	{
		realRTC.year++;
		realRTC.month = 1;
	}

	// Need to call 1second callback function?
	if(secondTickCallback)
	{
		secondTickCallback(&realRTC);
	}
}

void realrtc_set_validity(unsigned char valid)
{
	realRTC.valid = valid;
}

unsigned char realrtc_get_validity()
{
	return realRTC.valid;
}

void realrtc_set(tRealRTC *rtc)
{
	os_memcpy(&realRTC, rtc, sizeof(tRealRTC));
	realRTC.valid = 1;
}

void realrtc_get(tRealRTC **rtc)
{
	*rtc = &realRTC;
}

void realrtc_start(void(*secondTickCallback_)(tRealRTC *))
{
	secondTickCallback = secondTickCallback_;

	realRTC.valid = 0;

	os_timer_disarm(&tmrRealRTC);
	os_timer_setfn(&tmrRealRTC, (os_timer_func_t *)ctrl_real_rtc_1s, NULL);
	os_timer_arm(&tmrRealRTC, 1000, 1); // start the real RTC. 1 = repeat automatically
}
