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
static unsigned char ICACHE_FLASH_ATTR realrtc_isleapyear(unsigned short y)
{
	return ( ( !(y % 4) && (y % 100) ) || !(y % 400) );
}

static unsigned char ICACHE_FLASH_ATTR ctrl_real_rtc_is_dst(void)
{
	if(realRTC.dst == 0) return false; // no DST used

	// NOTE: the good thing is that we can compare dates as string lexicographically with os_strncmp()
	char currStamp[15]; // eg 20150216110000
	os_sprintf(currStamp, "%04d%02d%02d%02d%02d%02d", realRTC.year, realRTC.month, realRTC.day, realRTC.hour, realRTC.minute, realRTC.second);

	char lowStamp[15]; // eg 20150216110000
	char highStamp[15]; // eg 20151116101500

	// formulas taken from: http://www.webexhibits.org/daylightsaving/b.html
	switch(realRTC.dst)
	{
		// EU
		case 1:
			{
				unsigned char marchDay = (31-(( (unsigned short)(realRTC.year * 5) / 4) + 4) % 7);
				unsigned char octoberDay = (31-(( (unsigned short)(realRTC.year * 5) / 4) + 1) % 7);
				os_sprintf(lowStamp, "%04d%02d%02d%02d%02d%02d", realRTC.year, 3, marchDay, 1, 0, 0);
				os_sprintf(highStamp, "%04d%02d%02d%02d%02d%02d", realRTC.year, 10, octoberDay, 1, 0, 0);
			}
			break;

		// USA
		case 2:
			{
				unsigned char marchDay = 14 - ((1 + (unsigned short)(realRTC.year * 5) / 4) % 7);
				unsigned char novemberDay = 7 - ((1 + (unsigned short)(realRTC.year * 5) / 4) % 7);
				os_sprintf(lowStamp, "%04d%02d%02d%02d%02d%02d", realRTC.year, 3, marchDay, 2, 0, 0);
				os_sprintf(highStamp, "%04d%02d%02d%02d%02d%02d", realRTC.year, 11, novemberDay, 2, 0, 0);
			}
			break;

		// others not implemented
		default:
			return false;
	}

	// compare low stamp to current stamp
	char lowCmp = os_strncmp(lowStamp, currStamp, 15);
	if(lowCmp <= 0)
	{
		// compare high stamp to current stamp
		char highCmp = os_strncmp(highStamp, currStamp, 15);
		if(highCmp >= 0)
		{
			return true;
		}
	}

	return false;
}

static void ICACHE_FLASH_ATTR ctrl_real_rtc_1s(void *arg)
{
	realRTC.second++; // another second of our life has just past by

	// a minute...
	if(realRTC.second >= 60)
	{
		realRTC.second = 0;
		realRTC.minute++;

		// an hour...
		if(realRTC.minute >= 60)
		{
			realRTC.minute = 0;
			realRTC.hour++;

			// DST was added when we got it from Server, and we are not in range anymore?
			if(realRTC.dst_added && !ctrl_real_rtc_is_dst())
			{
				// we can simply do -1 here because this transition never happens at 00:*:*, but in either 01:00:00 or 02:00:00
				realRTC.hour--;
				realRTC.dst_added = 0;
			}
			// DST was not added when we got it from Server, and we just got in range to add it?
			else if(!realRTC.dst_added && ctrl_real_rtc_is_dst())
			{
				// we can simply do +1 here because this transition never happens at 23:*:*, but in either 01:00:00 or 02:00:00
				realRTC.hour++;
				realRTC.dst_added = 1;
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
		}

	}

	// Need to call 1second callback function?
	if(secondTickCallback)
	{
		secondTickCallback(&realRTC);
	}

	// TODO: add these as callbacks too
	// each minute tasks
	if(realRTC.second == 0)
	{
		// enter 1 minute code here...
	}

	// each hour tasks
	if(realRTC.minute == 0)
	{
		// enter 1 hour code here...
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
	os_timer_disarm(&tmrRealRTC); // pause real RTC timer

	os_memcpy(&realRTC, rtc, sizeof(tRealRTC));
	realRTC.dst_added = 0; // init

	// if DST is used and we are in DST range, add an hour to the clock
	if(realRTC.dst)
	{
		// we can simply do +1 here because this transition never happens at 23:*:*, but in either 01:00:00 or 02:00:00
		realRTC.hour++;
		realRTC.dst_added = 1;
	}

	realRTC.valid = 1; // say it's OK now

	os_timer_arm(&tmrRealRTC, 1000, 1); // resume the real RTC. 1 = repeat automatically
}

void realrtc_peek(tRealRTC **rtc)
{
	*rtc = &realRTC;
}

void realrtc_get(tRealRTC *rtc)
{
	os_timer_disarm(&tmrRealRTC); // pause real RTC timer

	os_memcpy(rtc, &realRTC, sizeof(tRealRTC));

	os_timer_arm(&tmrRealRTC, 1000, 1); // resume the real RTC. 1 = repeat automatically
}

void ICACHE_FLASH_ATTR realrtc_start(void(*secondTickCallback_)(tRealRTC *))
{
	secondTickCallback = secondTickCallback_;

	realRTC.valid = 0;

	os_timer_disarm(&tmrRealRTC);
	os_timer_setfn(&tmrRealRTC, (os_timer_func_t *)ctrl_real_rtc_1s, NULL);
	os_timer_arm(&tmrRealRTC, 1000, 1); // start the real RTC. 1 = repeat automatically
}
