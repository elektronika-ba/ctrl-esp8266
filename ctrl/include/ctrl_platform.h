#ifndef __CTRL_PLATFORM_H
#define __CTRL_PLATFORM_H

#include "c_types.h"
#include "os_type.h"
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "espconn.h"

#include "ctrl_stack.h"

// When defined, will spit out logging messages on UART.
#define CTRL_LOGGING

// When defined (also include "ctrl_database.h"!), the platform stores
// outgoing messages in database before sending them to CTRL Server.
// This means that you don't need to think about messages being
// delivered to Server because they WILL BE.
// When not defined, you need to handle acknowledging current
// transmission and re-transmitting it if something happens.
#define USE_DATABASE_APPROACH
#ifdef USE_DATABASE_APPROACH
	#define TMR_ITEMS_SENDER_MS			150		// sending of all outgoing items when using the database approach
#endif

// How many unprocessed CTRL messages can we hold until we tell Server to backoff?
#define TASK_QUEUE_LEN		5

#define SETUP_OK_KEY					0xAA4529BA	// MAGIC VALUE. When settings exist in flash this is the valid-flag.

#define	LED_FLASH_FREQUENCY				3000	// delay between status flashes in ms
#define LED_FLASH_DURATION_MS			60 		// length of Status LED flash in ms
#define LED_FLASH_OK					1		// number of flashes for this status
#define	LED_FLASH_CTRLERROR				2		// number of flashes for this status
#define	LED_FLASH_NOWIFI				3		// number of flashes for this status

typedef struct {
	unsigned char count; // how many times to blink?
	unsigned char blinks;
	unsigned char ledstate;
	os_timer_t tmr;
} tStatusLed;

typedef enum {
	CTRL_WIFI_CONNECTING,
	CTRL_WIFI_CONNECTING_ERROR,
	CTRL_TCP_DISCONNECTED,
	CTRL_TCP_CONNECTING,
	CTRL_TCP_CONNECTING_ERROR,
	CTRL_TCP_CONNECTED,
	CTRL_AUTHENTICATED,
	CTRL_AUTHENTICATION_ERROR
} tCtrlConnState;

// WARNING: this structure's memory amount must be dividable by 4 in order to save to FLASH memory!!!
typedef struct {
	unsigned long stationSetupOk; // this holds the SETUP_OK_KEY value if settings are OK in flash memory
	char baseid[16];
	char aes128Key[16];
	char serverIp[4];
	unsigned int serverPort;

	char pad[2]; // added for the structure to be dividable by 4!
} tCtrlSetup;

typedef struct {
	void(*message_received)(tCtrlMessage *);
} tCtrlAppCallbacks;

// private
static void ctrl_platform_reconnect(struct espconn *);
static void ctrl_platform_discon(struct espconn *);
static void ctrl_platform_check_ip(void *);
static void ctrl_platform_recon_cb(void *, sint8);
static void ctrl_platform_sent_cb(void *);
static void ctrl_platform_recv_cb(void *, char *, unsigned short);
static void ctrl_platform_connect_cb(void *);
static void ctrl_platform_discon_cb(void *);
static void ctrl_status_led_blinker(void *);
static void ctrl_platform_task_processor(os_event_t *);
static void ctrl_platform_enter_configuration_mode(void);
// CTRL stack callbacks
static void ctrl_message_recv_cb(tCtrlMessage *);
static void ctrl_message_ack_cb(tCtrlMessage *);
static void ctrl_auth_response_cb(void);
static char ctrl_send_data_cb(char *, unsigned short);

// public
unsigned char ctrl_platform_send(char *, unsigned short, unsigned char);
void ctrl_platform_init(void);

#endif
