#ifndef __REALRTC_H_
#define __REALRTC_H_

#include "c_types.h"

// Real RTC
typedef struct {
	unsigned char valid;	// is RTC valid or not (It is only valid after synchronizing with the Server. It is invalid after wakeup/powerup!)

	unsigned char dst_added;	// if we added one hour (summer time madness)

	unsigned char dst;		// Daylight Savings Option (0=not enabled, 1=Yes Europe, 2=Yes USA)
	unsigned char second;
	unsigned char minute;
	unsigned char hour;
	unsigned char weekday;
	unsigned char day;
	unsigned char month;
	unsigned short year;
} tRealRTC;

// private functions
static unsigned char realrtc_isleapyear(unsigned short);
static unsigned char ctrl_real_rtc_is_dst(void);
static void ctrl_real_rtc_1s(void *);

// public functions
void realrtc_set_validity(unsigned char);
unsigned char realrtc_get_validity(void);
void realrtc_set(tRealRTC *);
void realrtc_get(tRealRTC *);
void realrtc_peek(tRealRTC **);
void realrtc_start(void(*)(tRealRTC *));

#endif