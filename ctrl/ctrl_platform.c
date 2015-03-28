#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "mem.h"
#include "espconn.h"
#include "os_type.h"
#include "gpio.h"

#include "../driver/include/flash_param.h"
#include "../misc/include/wifi.h"
#include "include/ctrl_stack.h"
#include "include/ctrl_config_server.h"
#include "../misc/include/realrtc.h"

#include "include/ctrl_platform.h"

#ifdef USE_DATABASE_APPROACH
	#include "include/ctrl_database.h"
	/*#ifndef CTRL_DATABASE_CAPACITY
		#error You probably forgot to include ctrl_database.h in ctrl_platform.c file?
	#endif*/
	os_timer_t tmrDatabaseItemSender;
#else
	static unsigned long TXbase;
#endif

os_event_t *taskQueue;

os_timer_t tmrConfigChecker;
struct espconn ctrlConn;
esp_tcp ctrlTcp;
os_timer_t tmrLinker;
static unsigned char tcpReconCount;
static tCtrlConnState connState = CTRL_WIFI_CONNECTING;

static tStatusLed statusLed;

tCtrlSetup ctrlSetup;
tCtrlCallbacks ctrlCallbacks;
static unsigned char outOfSyncCounter;
static unsigned char ctrlSynchronized;

tCtrlAppCallbacks ctrlAppCallbacks;

static void ICACHE_FLASH_ATTR ctrl_platform_check_ip(void *arg)
{
    os_timer_disarm(&tmrLinker);

    struct ip_info ipconfig;
    wifi_get_ip_info(STATION_IF, &ipconfig);
    if (wifi_station_get_connect_status() == STATION_GOT_IP && ipconfig.ip.addr != 0)
    {
        connState = CTRL_TCP_CONNECTING;
        #ifdef CTRL_LOGGING
        	os_printf("TCP CONNECTING...\r\n");
        #endif

        statusLed.count = LED_FLASH_CTRLERROR;

        ctrlConn.proto.tcp = &ctrlTcp;
        ctrlConn.type = ESPCONN_TCP;
        ctrlConn.state = ESPCONN_NONE;
        os_memcpy(ctrlConn.proto.tcp->remote_ip, ctrlSetup.serverIp, 4);
        ctrlConn.proto.tcp->local_port = espconn_port();
        ctrlConn.proto.tcp->remote_port = ctrlSetup.serverPort;

		espconn_regist_connectcb(&ctrlConn, ctrl_platform_connect_cb);
		espconn_regist_reconcb(&ctrlConn, ctrl_platform_recon_cb);
		espconn_regist_disconcb(&ctrlConn, ctrl_platform_discon_cb);

		espconn_connect(&ctrlConn);
    }
    else
    {
		statusLed.count = LED_FLASH_NOWIFI;

        if(wifi_station_get_connect_status() == STATION_WRONG_PASSWORD ||
           		wifi_station_get_connect_status() == STATION_NO_AP_FOUND ||
           		wifi_station_get_connect_status() == STATION_CONNECT_FAIL)
		{
            connState = CTRL_WIFI_CONNECTING_ERROR;
            #ifdef CTRL_LOGGING
            	os_printf("WIFI CONNECTING ERROR\r\n");
            #endif

            os_timer_setfn(&tmrLinker, (os_timer_func_t *)ctrl_platform_check_ip, NULL);
            os_timer_arm(&tmrLinker, 1000, 0); // try now slower
        }
        else {
            connState = CTRL_WIFI_CONNECTING;
            #ifdef CTRL_LOGGING
            	os_printf("WIFI CONNECTING...\r\n");
            #endif

            os_timer_setfn(&tmrLinker, (os_timer_func_t *)ctrl_platform_check_ip, NULL);
            os_timer_arm(&tmrLinker, 100, 0);
        }
    }
}

static void ICACHE_FLASH_ATTR ctrl_platform_recon_cb(void *arg, sint8 err)
{
    struct espconn *pespconn = (struct espconn *)arg;

	#ifdef CTRL_LOGGING
    	os_printf("ctrl_platform_recon_cb\r\n");
    #endif

	connState = CTRL_TCP_DISCONNECTED;
	statusLed.count = LED_FLASH_CTRLERROR;

    if (++tcpReconCount >= 5)
    {
        connState = CTRL_TCP_CONNECTING_ERROR;
        tcpReconCount = 0;

        #ifdef CTRL_LOGGING
        	os_printf("ctrl_platform_recon_cb, 5 failed TCP attempts!\r\n");
        	os_printf("Will reconnect in 10s...\r\n");
        #endif

		os_timer_disarm(&tmrLinker);
		os_timer_setfn(&tmrLinker, (os_timer_func_t *)ctrl_platform_reconnect, pespconn);
		os_timer_arm(&tmrLinker, 10000, 0);
    }
    else
    {
		#ifdef CTRL_LOGGING
			os_printf("Will reconnect in 1s...\r\n");
		#endif

		os_timer_disarm(&tmrLinker);
		os_timer_setfn(&tmrLinker, (os_timer_func_t *)ctrl_platform_reconnect, pespconn);
		os_timer_arm(&tmrLinker, 1000, 0);
	}
}

static void ICACHE_FLASH_ATTR ctrl_platform_sent_cb(void *arg)
{
	struct espconn *pespconn = arg;

	/*#ifdef CTRL_LOGGING
    	os_printf("ctrl_platform_sent_cb\r\n");
    #endif*/
}

static void ICACHE_FLASH_ATTR ctrl_platform_recv_cb(void *arg, char *pdata, unsigned short len)
{
	struct espconn *pespconn = arg;

	/*#ifdef CTRL_LOGGING
		os_printf("ctrl_platform_recv_cb\r\n");
	#endif*/

	// forward data to CTRL stack
	ctrl_stack_recv(pdata, len);
}

static void ICACHE_FLASH_ATTR ctrl_platform_connect_cb(void *arg)
{
    struct espconn *pespconn = arg;

	#ifdef CTRL_LOGGING
		os_printf("ctrl_platform_connect_cb\r\n");
	#endif

    tcpReconCount = 0;

    espconn_regist_recvcb(pespconn, ctrl_platform_recv_cb);
    espconn_regist_sentcb(pespconn, ctrl_platform_sent_cb);

	connState = CTRL_TCP_CONNECTED;

	unsigned char sync = 0;
	#ifdef USE_DATABASE_APPROACH
		// 1. Flush acked transmissions until we get to the first unacked (or until the end)
		// 2. Mark all unacked database items as unsent (not a problem if we unsend even the acked ones since that is a problem and a probably situation that will never happen)
		// 3. Count pending items to see if we need to tell server to Sync
		ctrl_database_flush_acked();
		ctrl_database_unsend_all();
		if(ctrl_database_count_unacked_items() == 0)
		{
			sync = 1;
		}
	#else
		// no database = always sync
		sync = 1;
		TXbase = 1;
	#endif

	ctrl_stack_authorize(ctrlSetup.baseid, ctrlSetup.aes128Key, sync);
}

static void ICACHE_FLASH_ATTR ctrl_platform_reconnect(struct espconn *pespconn)
{
	#ifdef CTRL_LOGGING
    	os_printf("ctrl_platform_reconnect\r\n");
    #endif

    ctrl_platform_check_ip(NULL);
}

static void ICACHE_FLASH_ATTR ctrl_platform_discon_cb(void *arg)
{
    struct espconn *pespconn = arg;

	#ifdef CTRL_LOGGING
    	os_printf("ctrl_platform_discon_cb\r\n");
    #endif

	connState = CTRL_TCP_DISCONNECTED;

    if (pespconn == NULL)
    {
		#ifdef CTRL_LOGGING
	    	os_printf("ctrl_platform_discon_cb - conn is NULL!\r\n");
	    #endif
        return;
    }

	#ifdef CTRL_LOGGING
		os_printf("Will reconnect in 1s...\r\n");
	#endif

    os_timer_disarm(&tmrLinker);
    os_timer_setfn(&tmrLinker, (os_timer_func_t *)ctrl_platform_reconnect, pespconn);
    os_timer_arm(&tmrLinker, 1000, 0);
}

static void ICACHE_FLASH_ATTR ctrl_platform_discon(struct espconn *pespconn)
{
	#ifdef CTRL_LOGGING
    	os_printf("ctrl_platform_discon\r\n");
    #endif

	connState = CTRL_TCP_DISCONNECTED;

    espconn_disconnect(pespconn);

    // hopefully now the ctrl_platform_discon_cb will trigger and re-connect us!?
}

#ifdef USE_DATABASE_APPROACH
	static void ICACHE_FLASH_ATTR ctrl_database_item_sender(void *arg)
	{
		os_timer_disarm(&tmrDatabaseItemSender);

		#ifdef CTRL_LOGGING
			os_printf("ctrl_database_item_sender\r\n");
		#endif

		if(connState != CTRL_AUTHENTICATED || !ctrlSynchronized)
		{
			#ifdef CTRL_LOGGING
				os_printf("ctrl_database_item_sender - not authed or synced\r\n");
			#endif
			return;
		}

		// get next item to send from DB, send it and mark as SENT even if it doesn't actually get sent to socket!
		tDatabaseRow *row = (tDatabaseRow *)ctrl_database_get_next_txbase2server();
		if(row != NULL)
		{
			ctrl_stack_send(row->data, row->len, row->TXbase, 0);

			// set us up to execute again
			os_timer_arm(&tmrDatabaseItemSender, TMR_ITEMS_SENDER_MS, 0); // 0 = don't repeat automatically
			#ifdef CTRL_LOGGING
				os_printf("ctrl_database_item_sender - ON again\r\n");
			#endif
		}
		else
		{
			#ifdef CTRL_LOGGING
				os_printf("ctrl_database_item_sender - nothing to send\r\n");
			#endif
		}
	}
#endif

// blinking status led according to the status of system
static void ICACHE_FLASH_ATTR ctrl_status_led_blinker(void *arg)
{
	os_timer_disarm(&(statusLed.tmr));

	if(!statusLed.ledstate)
	{
		GPIO_OUTPUT_SET(12, 1); // LED ON
		statusLed.ledstate = 1;
		os_timer_arm(&(statusLed.tmr), LED_FLASH_DURATION_MS, 0);
	}
	else
	{
		GPIO_OUTPUT_SET(12, 0); // LED OFF
		statusLed.ledstate = 0;
		if(!statusLed.blinks)
		{
			statusLed.blinks = statusLed.count; // very first initialization
		}
		statusLed.blinks--;
		if(statusLed.blinks)
		{
			os_timer_arm(&(statusLed.tmr), LED_FLASH_DURATION_MS*2, 0);
		}
		else
		{
			os_timer_arm(&(statusLed.tmr), LED_FLASH_FREQUENCY, 0);
			statusLed.blinks = statusLed.count; // reload
		}
	}
}

static void ICACHE_FLASH_ATTR ctrl_message_recv_cb(tCtrlMessage *msg)
{
	// do something with msg now
	#ifdef CTRL_LOGGING
		char tmp[100];
		os_sprintf(tmp, "GOT MSG. Length: %u, Header: 0x%X, TXsender: %u\r\n", msg->length, msg->header, msg->TXsender);
		os_printf(tmp);
	#endif

	// after we finish, and if we find out that we don't have enough
	// storage to process next message that will arive, we can simply
	// make a call to ctrl_stack_backoff(1); and it will acknowledge to
	// any following messages with BACKOFF which will tell server
	// to re-send that message and postpone sending any following messages
	// untill we call ctrl_stack_backoff(0);

	// Don't push system messages to user app, they are for private
	// communication between Base (us) and Server. There is no problem
	// in sending these system messages to user app though, I just don't
	// think it is neccessary right now. We will see in future.
	if(msg->header & CH_SYSTEM_MESSAGE)
	{
		// we received a system message here. we need to check what this is
		// because it might be new timestamp, or some server-stored-variable
		// we previously requested!

		// Recently requested variable is arriving from Server?
		if(msg->data[0] == SYSTEM_MESSAGE_GET_VAR)
		{
			// we have a Variable!
			char variableId[4];
			char variableValue[4];
			os_memcpy(variableId, msg->data+1, 4);
			os_memcpy(variableValue, msg->data+5, 4);

			// do something with this Variable+Value that arrived
		}
		// Recently requested timestamp is arriving from Server?
		else if(msg->data[0] == SYSTEM_MESSAGE_GET_RTC)
		{
			// we have Timestamp!

			#ifdef CTRL_LOGGING
				char debug[100];
				os_sprintf(debug, "GOT TIMESTAMP: %d%d%d%d-%d%d-%d%d %d%d:%d%d:%d%d (%d)\r\n", msg->data[1], msg->data[2], msg->data[3], msg->data[4], msg->data[5], msg->data[6], msg->data[7], msg->data[8], msg->data[9], msg->data[10], msg->data[11], msg->data[12], msg->data[13], msg->data[14], msg->data[15]);
				os_printf_plus(debug);
			#endif

			// Parse timestamp that arrived (format: YYYY MM DD HH MM SS DAY-OF-WEEK(1-7))
			tRealRTC rtc;

			char tmp[5];
			os_sprintf(tmp, "%d%d%d%d", msg->data[1], msg->data[2], msg->data[3], msg->data[4]);
			rtc.year = atoi(tmp);
			os_sprintf(tmp, "%d%d", msg->data[5], msg->data[6]);
			rtc.month = atoi(tmp);
			os_sprintf(tmp, "%d%d", msg->data[7], msg->data[8]);
			rtc.day = atoi(tmp);
			os_sprintf(tmp, "%d%d", msg->data[9], msg->data[10]);
			rtc.hour = atoi(tmp);
			os_sprintf(tmp, "%d%d", msg->data[11], msg->data[12]);
			rtc.minute = atoi(tmp);
			os_sprintf(tmp, "%d%d", msg->data[13], msg->data[14]);
			rtc.second = atoi(tmp);
			os_sprintf(tmp, "%d", msg->data[15]);
			rtc.weekday = atoi(tmp);

			realrtc_set(&rtc);
		}
	}
	else
	{
		// push message to ctrl-user-application
		if(ctrlAppCallbacks.message_received != NULL)
		{
			// copy message to new memory location. task must free it after it finishes!
			tCtrlMessage *newMsg = (tCtrlMessage *)os_malloc(sizeof(tCtrlMessage));
			if(newMsg == NULL)
			{
				#ifdef CTRL_LOGGING
					os_printf("Out of memory (1)! Ignoring received message.\r\n");
				#endif
			}
			os_memcpy(newMsg, msg, sizeof(tCtrlMessage));

			// copy data as well
			char *newData = (char *)os_malloc(msg->length-1-4);
			if(newData == NULL)
			{
				#ifdef CTRL_LOGGING
					os_printf("Out of memory (2)! Ignoring received message.\r\n");
				#endif
			}
			os_memcpy(newData, msg->data, msg->length-1-4);
			newMsg->data = newData;

			// call task that will process it
			if(!system_os_post(USER_TASK_PRIO_0, 0, (os_param_t)newMsg))
			{
				ctrl_stack_backoff(1);
				#ifdef CTRL_LOGGING
					os_printf("All tasks busy, backed off Server!\r\n");
				#endif
			}
		}
	}
}

static void ICACHE_FLASH_ATTR ctrl_message_ack_cb(tCtrlMessage *msg)
{
	// do something with ack now
	#ifdef CTRL_LOGGING
		char tmp[100];
		os_sprintf(tmp, "GOT ACK on TXsender: %u\r\n", msg->TXsender);
		os_printf(tmp);
	#endif

	// hendliraj out_of_sync koji nam server moze poslati
	if((msg->header) & CH_OUT_OF_SYNC)
	{
		#ifdef CTRL_LOGGING
			os_printf("Server is complaining that we are OUT OF SYNC!\r\n");
		#endif

		if(++outOfSyncCounter >= 3)
		{
			outOfSyncCounter = 0;

			#ifdef USE_DATABASE_APPROACH
				#ifdef CTRL_LOGGING
					os_printf("Out of sync (3): Flushing outgoing queue.\r\n");
				#endif

				os_timer_disarm(&tmrDatabaseItemSender);

				// Flush the outgoing queue, that's all we can do about it really.
				ctrl_database_delete_all();
			#endif

			#ifdef CTRL_LOGGING
				os_printf("Out of sync (3): Disconnecting!\r\n");
			#endif

			ctrl_platform_discon(&ctrlConn);
		}
		else
		{
			#ifdef CTRL_LOGGING
				char tmp[50];
				os_sprintf(tmp, "Out of sync report %u/3.\r\n", outOfSyncCounter);
				os_printf_plus(tmp);
			#endif

			#ifdef USE_DATABASE_APPROACH
				#ifdef CTRL_LOGGING
					os_printf("Re-sending outgoing queue...\r\n");
				#endif

				// Re-send all unacked outgoing messages, maybe that will resolve the sync problem
				os_timer_disarm(&tmrDatabaseItemSender);

				ctrl_database_unsend_all();

				os_timer_arm(&tmrDatabaseItemSender, TMR_ITEMS_SENDER_MS, 0); // 0 = don't repeat automatically
			#endif
		}
	}
	else
	{
		outOfSyncCounter = 0;

		#ifdef USE_DATABASE_APPROACH
			ctrl_database_ack_row(msg->TXsender);
		#endif
	}
}

static void ICACHE_FLASH_ATTR ctrl_auth_response_cb()
{
	#ifdef CTRL_LOGGING
		os_printf("CTRL Authenticated!\r\n");
	#endif

	connState = CTRL_AUTHENTICATED;
	ctrl_stack_keepalive(1); // lets enable keepalive for our connection because that's what all cool kids do these days

	// request current timestamp from Server
	ctrl_stack_get_rtc();

	ctrlSynchronized = 1;

	statusLed.count = LED_FLASH_OK;

	#ifdef USE_DATABASE_APPROACH
		if(ctrl_database_count_unacked_items() > 0)
		{
			// start sending pending data right now
			os_timer_disarm(&tmrDatabaseItemSender);
			os_timer_arm(&tmrDatabaseItemSender, TMR_ITEMS_SENDER_MS, 0); // 0 = don't repeat automatically
		}
	#endif
}

static char ICACHE_FLASH_ATTR ctrl_send_data_cb(char *data, unsigned short len)
{
	/*#ifdef CTRL_LOGGING
		os_printf("ctrl_send_data_cb\r\n");
	#endif*/

	if(connState != CTRL_TCP_CONNECTED && connState != CTRL_AUTHENTICATED)
	{
		#ifdef CTRL_LOGGING
			os_printf("ctrl_send_data_cb - not conn or not authed\r\n");
		#endif
		return ESPCONN_CONN;
	}

	/*#ifdef CTRL_LOGGING
		os_printf("TCP SENDING:");
		unsigned short i;
		for(i=0; i<len; i++)
		{
			char tmp2[10];
			os_sprintf(tmp2, " 0x%X", data[i]);
			os_printf(tmp2);
		}
		os_printf(".\r\n");
	#endif*/

	return espconn_sent(&ctrlConn, data, len);
}

// all user CTRL messages is sent to Server through this function
// returns: 1 on error, 0 on success
unsigned char ICACHE_FLASH_ATTR ctrl_platform_send(char *data, unsigned short len, unsigned char notification)
{
	#ifdef CTRL_LOGGING
		os_printf("ctrl_platform_send\r\n");
	#endif

	#ifdef USE_DATABASE_APPROACH
		if(notification)
		{
			if(connState != CTRL_AUTHENTICATED || !ctrlSynchronized)
			{
				#ifdef CTRL_LOGGING
					os_printf("ctrl_platform_send not authed or not synced\r\n");
				#endif
				return 1;
			}

			return ctrl_stack_send(data, len, 0, 1); // Send notifications immediatelly, no queue and no delivery order here
		}
		else
		{
			if(!ctrlSynchronized)
			{
				#ifdef CTRL_LOGGING
					os_printf("ctrl_platform_send not synced\r\n");
				#endif
				return 1;
			}

			unsigned char ret = ctrl_database_add_row(data, len);
			// No point in starting timer if we couldn't add data to DB. If we are not authenticated
			// or synched the timer itself has that check so no problem starting it now
			if(ret == 0)
			{
				os_timer_disarm(&tmrDatabaseItemSender);
				os_timer_arm(&tmrDatabaseItemSender, TMR_ITEMS_SENDER_MS, 0); // 0 = don't repeat automatically
			}
			return ret;
		}
	#else
		if(connState != CTRL_AUTHENTICATED)
		{
			#ifdef CTRL_LOGGING
				os_printf("ctrl_platform_send not authed\r\n");
			#endif
			return 1;
		}

		unsigned char ret = ctrl_stack_send(data, len, TXbase, notification);
		TXbase++;

		return ret;
	#endif
}

// this forces device into configuration mode
static void ICACHE_FLASH_ATTR ctrl_platform_enter_configuration_mode(void)
{
	wifi_set_opmode(STATIONAP_MODE);

	#ifdef CTRL_LOGGING
		os_printf("Restarting in STATIONAP mode...\r\n");
	#endif

	system_restart();
}

static void ICACHE_FLASH_ATTR ctrl_platform_config_checker(void *arg)
{
	// taken from eshttpd of Sprite_tm, file: io.c
	static int resetCnt = 0;
	if (!GPIO_INPUT_GET(0)) {
		resetCnt++;
	}
	else {
		if (resetCnt >= 6) { //3 sec pressed
			ctrl_platform_enter_configuration_mode();
		}
		resetCnt = 0;
	}
}

static void ICACHE_FLASH_ATTR ctrl_platform_task_processor(os_event_t *e) {
	tCtrlMessage *msg = (tCtrlMessage *)e->par;
	ctrlAppCallbacks.message_received(msg);
	os_free(msg);
}

// entry point to the ctrl platform
void ICACHE_FLASH_ATTR ctrl_platform_init(void)
{
	load_flash_param(ESP_PARAM_SAVE_1, (uint32 *)&ctrlSetup, sizeof(tCtrlSetup));

	// Start the real RTC
	realrtc_start(NULL);

	// Init the BTN and LED used by the platform
	gpio_init();
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDI_U, FUNC_GPIO12); // status led
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0); // config button
	GPIO_OUTPUT_SET(12, 0);

	// taken from eshttpd of Sprite_tm, file: io.c
	os_timer_disarm(&tmrConfigChecker);
	os_timer_setfn(&tmrConfigChecker, ctrl_platform_config_checker, NULL);
	os_timer_arm(&tmrConfigChecker, 500, 1);

	// Must enter into Configuration mode?
	if(ctrlSetup.stationSetupOk != SETUP_OK_KEY || wifi_get_opmode() != STATION_MODE)
	{
		// Init WiFi in STATIONAP mode
		setup_wifi_ap_mode();

		// The device is now booted into "configuration mode" so it acts as Access Point
		// and starts a web server to accept HTTP connection from browser.
		// After the user configures the device, the browser will change the configuration
		// of the ESP device and reboot into normal-working mode. When and if user wants
		// to make additional changes to the device, it must be booted into "configuration
		// mode" by pressing a button on the device which will restart it and get in here.

		#ifdef CTRL_LOGGING
			os_printf("Starting configuration web server...\r\n");
		#endif
		ctrl_config_server_init();
	}
	else
	{
		#ifdef CTRL_LOGGING
			os_printf("Starting in normal mode...\r\n");
		#endif

		// The device is now booted into "normal mode" where it acts as a STATION or STATIONAP.
		// It connects to the CTRL IoT platform via TCP socket connection. All incoming data is
		// then forwarded into the CTRL Stack and the stack will then call a callback function
		// for every received CTRL packet. That callback function is the actuall business-end
		// of the user-application.
		// The AP's parameters are set in ctrl_config_server.c by calling setup_wifi_st_mode()
		// which is located in wifi.c.

		#ifdef CTRL_LOGGING
			struct station_config stationConf;
			wifi_station_get_config(&stationConf);
			os_printf("OPMODE: %u, SSID: %s\r\n", wifi_get_opmode(), stationConf.ssid);
		#endif

		// Init the task that will process messages received from Server. They are processed in ctrl_user_app
		taskQueue = (os_event_t *)os_malloc(sizeof(os_event_t)*TASK_QUEUE_LEN);
		system_os_task(ctrl_platform_task_processor, USER_TASK_PRIO_0, taskQueue, TASK_QUEUE_LEN);

		// Init the database (a RAM version of DB - just a linked list)
		ctrl_database_init();

		// Init the CTRL stack with callback functions it requires
		ctrlCallbacks.message_received = &ctrl_message_recv_cb; // when CTRL stack receives a fresh message it will call this function
		ctrlCallbacks.send_data = &ctrl_send_data_cb; // when CTRL stack wants to send some data to socket it will use this function
		ctrlCallbacks.auth_response = &ctrl_auth_response_cb; // when CTRL stack gets an authentication response from Server it will call this function
		ctrlCallbacks.message_acked = &ctrl_message_ack_cb; // when CTRL stack receives an acknowledgement for a message it previously sent it will call this function
		ctrl_stack_init(&ctrlCallbacks);

		// Init the user-app callbacks
		ctrl_app_init(&ctrlAppCallbacks);

		#ifdef CTRL_LOGGING
			os_printf("System initialization done!\r\n");
		#endif

		// Wait for WIFI connection and start TCP connection
		os_timer_disarm(&tmrLinker);
		os_timer_setfn(&tmrLinker, (os_timer_func_t *)ctrl_platform_check_ip, NULL);
		os_timer_arm(&tmrLinker, 100, 0);

		// set a timer for Status LED blinking
		statusLed.count = LED_FLASH_NOWIFI;
		os_timer_disarm(&(statusLed.tmr));
		os_timer_setfn(&(statusLed.tmr), (os_timer_func_t *)ctrl_status_led_blinker, NULL);
		os_timer_arm(&(statusLed.tmr), LED_FLASH_FREQUENCY, 0);

		#ifdef USE_DATABASE_APPROACH
			// set a timer that will send items from the database (if database approach is used)
			os_timer_disarm(&tmrDatabaseItemSender);
			os_timer_setfn(&tmrDatabaseItemSender, (os_timer_func_t *)ctrl_database_item_sender, NULL);
		#endif
	}
}
