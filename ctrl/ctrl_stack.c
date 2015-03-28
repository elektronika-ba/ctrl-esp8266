#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "mem.h"
#include "espconn.h" // only for ESPCONN_OK enum
#include "../driver/include/uart.h"
#include "../driver/include/aes_cbc.h"
#include "include/ctrl_platform.h"

#include "include/ctrl_stack.h"

os_timer_t tmrDataExpecter;

static unsigned long TXserver;
static char *baseid;
static char *rxBuff = NULL;
static unsigned short rxBuffLen;
static unsigned char authMode;
static unsigned char authPhase;
static unsigned char authSync;
static tCtrlCallbacks *ctrlCallbacks;
static unsigned char backoff;

// About safeToUnBackoff: when Server receives our BACKOFF ACK, it will send a special ACK to that request so that we
// can safely mark that it is safe to unbackoff. Imagine that Server sent us lets say 100 messages, and they are traveling
// through the wire. Base receives first one, processes it and decides that Server needs to backoff. Ok, it sets backoff=1
// which will be sent to Server in ACK for message number 2. Server receives that ACK and says OK (remember those 98
// remaining messages are still traveling and arriving to Base). Now we (Base) need to reject all those 98 messages and ACK
// to each one with BACKOFF (it MUST!). Now, what happens if we (Base) decides that it now wants to receive further messages
// and that happens on for example 58th message which is on the wire. We would start receiving messages from 58 to the end
// but we would find ourself in OUT OF SYNC with server, since we are now missing those messages from 2 to 57! That's why we
// need to get ServerBackedOff message from Server because it would arrive after the 100th message (after the last one we need
// to let pass by) and only then we can unset the backoff. How do we know that it will arrive here after 100th message? - simple,
// TCP/IP protocol guaranties that! We will actually get 98 of those BACKOFF ACKs but it shouldn't be a problem for us. Server will
// keep trying to re-send all those messages after backoff expires. Naturally, backoff delay will be huge after 100 backed-off
// messages but there is nothing we can do about it.
static unsigned char safeToUnBackoff = 1;

static char *aes128Key; // secret key
static char random16bytes[16]; // IV for encryption

// find first message and return its length. 0 = not found, since CTRL message always has a length (it has at least header byte)!
static unsigned short ICACHE_FLASH_ATTR ctrl_find_message(char *data, unsigned short len)
{
	unsigned short length;

	if(len < 2) return 0;

	os_memcpy(&length, data, 2); // little endian

	// entire message available in buffer?
	if(length+2 <= len) // 4-feb-2015 added +2 here
	{
		return length;
	}

	return 0;
}

// internal function to process extracted message from the received socket stream
static void ICACHE_FLASH_ATTR ctrl_stack_process_message(tCtrlMessage *msg)
{
	// if we are currently in the authorization mode, process received data differently
	if(authMode)
	{
		// receiving challenge?
		// we must answer to it here
		if(authPhase == 1)
		{
			authPhase++;

			char challResponse[32];
			unsigned char i = 0;
			for(i=0; i<4; i++)
			{
				unsigned long r = system_get_time() + rand(); // TODO: make better random generation here. system_get_time() will probably have only one value in this loop... Idea: ESP8266 has SSL/TLS so it must have a good random number generator. It would be nice to have access to it!
				os_memcpy(challResponse+(i*4), &r, 4);
			}
			os_memcpy(challResponse+16, msg->data, 16);

			tCtrlMessage msg;
			msg.length = 1 + 4 + 32;
			msg.header = 0x00; // value not relevant during authentication procedure
			msg.TXsender = 0; // value not relevant during authentication procedure
			msg.data = challResponse; // contains: random 16 bytes + original challenge value

			// We have nothing pending to send? (TXbase is 1 in that case)
			if(authSync == 1)
			{
				msg.header |= CH_SYNC;
			}

			ctrl_stack_send_msg(&msg);
		}
		// receiving reply to our response on server's challenge :)
		// this means we are authenticated! here we get 4 bytes of data
		// containing TXserver value we need to reload. it is saved on Server
		// so that we don't wear out our Flash or EEPROM memory. cool feature, right?
		// also, here we need to check if Header has SYNC so that we reset TXserver to 0
		else if(authPhase == 2)
		{
			authMode = 0;

			if((msg->header) & CH_SYNC)
			{
				TXserver = 0;
			}
			else
			{
				os_memcpy(&TXserver, msg->data, 4);
			}

			if(ctrlCallbacks->auth_response != NULL)
			{
				ctrlCallbacks->auth_response();
			}
		}
	}
	else
	{
		// we received an ACK?
		if((msg->header) & CH_ACK)
		{
			// if Server has set CH_BACKOFF, it means that it is confirming our BackOff command, and that it is now safe to unBackOff
			if((msg->header) & CH_BACKOFF)
			{
				safeToUnBackoff = 1;
			}
			else
			{
				// push the received ack to callback
				if(ctrlCallbacks->message_acked != NULL)
				{
					ctrlCallbacks->message_acked(msg);
				}
			}
		}
		// fresh message, acknowledge and push it to the app
		else
		{
			// Reply with an ACK on received message. Mind the global backoff variable!
			tCtrlMessage ack;
			ack.header = CH_ACK;
			if(backoff)
			{
				ack.header |= CH_BACKOFF;
			}
			ack.TXsender = msg->TXsender;
			ack.length = 1+4; // fixed ACK length, without payload (data)
			char TXserver2Save[4]; // will need this bellow

			// is this NOT a notification message?
			if(!(msg->header & CH_NOTIFICATION))
			{
				if (msg->TXsender <= TXserver)
                {
                    ack.header &= ~CH_PROCESSED;
                    //os_printf("ERROR: Re-transmitted message, ignoring!\r\n");
                }
                else if (msg->TXsender > (TXserver + 1))
                {
					// SYNC PROBLEM! Server sent higher than we expected! This means we missed some previous Message!
                    // This part should be handled on Server's side.
                    // Server will flush all data after receiving X successive out-of-sync messages from us,
                    // and he will break the connection. Re-sync should then naturally occur
                    // in auth procedure as there would be nothing pending in queue to send to us.

                    ack.header |= CH_OUT_OF_SYNC;
                    ack.header &= ~CH_PROCESSED;
                    //os_printf("ERROR: Out-of-sync message!\r\n");
                }
                else
                {
                	ack.header |= CH_PROCESSED;
                	TXserver++; // next package we will receive must be +1 of current value, so lets ++

					// Important note: We shouldn't save TXserver if we are telling Server to backoff,
					// but since Server will not store our TXserver value if we are backing off, we can
					// safelly always send it to Server knowing it will only be saved if we are not
					// backing him off :)

					// Server offers us a feature to store a copy of this TXserver on his side so that
					// we don't wear out our Flash or EEPROM memory, lets do that! Thanks Server :)
					// Why are we saving this? We might get powered down and we must not loose this value!
					// If we do, Server will send it to us when we (re-)connect! Cool.
					ack.length += 4; // extend the length of this ACK because we are appending data as well
					ack.header |= CH_SAVE_TXSERVER;

					os_memcpy(&TXserver2Save, &TXserver, 4);
					ack.data = TXserver2Save;
                }

                // send reply
                ctrl_stack_send_msg(&ack);

                //os_printf("ACKed to a msg!\r\n");
			}
			else
			{
				ack.header |= CH_PROCESSED; // need this for code bellow to execute
				//os_printf("Didn't ACK because this is a notification-type msg!\r\n");
			}

			// received a message which is new and as expected?
			if(ack.header & CH_PROCESSED)
			{
				//os_printf("Got fresh message!\r\n");

				// 7-12-2014 pushing system messages to ctrl_platform.c!
				// push the received message to callback
				if(ctrlCallbacks->message_received != NULL)
				{
					ctrlCallbacks->message_received(msg);
				}
			}
		}
	}
}

// data expecter timeout, in case it triggers things aren't going well
static void ICACHE_FLASH_ATTR data_expecter_timeout(void *arg)
{
	#ifdef CTRL_LOGGING
		os_printf("data_expecter_timeout() - FLUSH RX BUFF\r\n");
	#endif

	if(rxBuff != NULL)
	{
		os_free(rxBuff);
		rxBuff = NULL;
	}
}

// all socket data which is received is flushed into this function
void ICACHE_FLASH_ATTR ctrl_stack_recv(char *data, unsigned short len)
{
	// Optimised for RAM memory usage. Reallocating memory only if messages
	// come in multiple TCP segments (multiple calls of this function for one CTRL message)

	unsigned char shouldFree = 0;

	if(rxBuff == NULL)
	{
		// fresh data arrived
		rxBuff = data;
		rxBuffLen = len;
	}
	else
	{
		// concatenate
		shouldFree = 1;
		char *newRxBuff = (char *)os_malloc(rxBuffLen+len);
		if(newRxBuff == NULL)
		{
			#ifdef CTRL_LOGGING
				os_printf("Out of memory (1) in ctrl_stack_recv()\r\n");
			#endif
		}
		os_memcpy(newRxBuff, rxBuff, rxBuffLen);
		os_memcpy(newRxBuff+rxBuffLen, data, len);
		os_free(rxBuff);

		rxBuff = newRxBuff;
		rxBuffLen = rxBuffLen+len;
	}

	// process data we have in rxBuff buffer
	unsigned short processedLen = 0;
	while(rxBuffLen > 0)
	{
		unsigned short allLength = ctrl_find_message(rxBuff+processedLen, rxBuffLen);

		if(allLength > 0)
		{
			// entire message found in buffer, lets process it
			os_timer_disarm(&tmrDataExpecter);

			// messages must come in 16 byte blocks (minus the first two bytes for [ALL_LENGTH]). ctrl_find_message() doesn't include ALL_LENGTH field in result
			if(!(allLength % 16))
			{
				// Packet structure:
				// [ALL_LENGTH] { [RANDOM_IV] [MESSAGE_LENGTH] [HEADER] [TX_SENDER] [DATA] [padding when needed] } [CMAC]
				// 		2             16              2           1          4        n              m               16

				char *msgPtr = rxBuff+processedLen+2; // skip the ALL_LENGTH field

				// Verify CMAC
				char calculatedCmac[16];
				cmac_generate(aes128Key, msgPtr, allLength-16, calculatedCmac);
				if(os_strncmp(calculatedCmac, msgPtr+allLength-16, 16) == 0)
				{
					// Decrypt
					aes128_cbc_decrypt(msgPtr, allLength-16, aes128Key);

					msgPtr += 16; // skip IV

					// Lets parse it into tCtrlMessage type
					tCtrlMessage msg;

					// Take MSG_LENGTH
					os_memcpy((char *)&msg.length, msgPtr, 2); // little endian
					msgPtr += 2;

					// Take HEADER
					os_memcpy(&msg.header, msgPtr, 1);
					msgPtr += 1;

					// Take TXsender
					os_memcpy((char *)&msg.TXsender, msgPtr, 4); // little endian
					msgPtr += 4;

					// Take data. Don't care about discard padding and CMAC because
					// whoever reads this "msg" will consider msg.length to calculate
					// the actual length of msg.data!
					msg.data = msgPtr;

					// Process
					ctrl_stack_process_message(&msg);
				}
			}

			processedLen += allLength+2;
			rxBuffLen -= allLength+2;
		}
		else
		{
			// has remaining data in buffer (beginning of another message but not entire message)?
			if(rxBuffLen > 0)
			{
				os_timer_disarm(&tmrDataExpecter);
				os_timer_arm(&tmrDataExpecter, TMR_DATA_EXPECTER_MS, 0); // 0 = do not repeat automatically

				char *newRxBuff = (char *)os_malloc(rxBuffLen);
				if(newRxBuff == NULL)
				{
					#ifdef CTRL_LOGGING
						os_printf("Out of memory (2) in ctrl_stack_recv()\r\n");
					#endif
				}
				os_memcpy(newRxBuff, rxBuff+processedLen, rxBuffLen);
				if(shouldFree)
				{
					os_free(rxBuff);
				}

				rxBuff = newRxBuff;
			}

			break;
		}
	}

	if(rxBuffLen == 0)
	{
		os_timer_disarm(&tmrDataExpecter); // TODO: see if required here as well?

		if(shouldFree)
		{
			os_free(rxBuff);
		}
		rxBuff = NULL;
	}
}

// creates a message from data and sends it to Server
unsigned char ICACHE_FLASH_ATTR ctrl_stack_send(char *data, unsigned short len, unsigned long TXbase, unsigned char notification)
{
	tCtrlMessage msg;
	msg.header = 0;

	if(notification)
	{
		msg.header |= CH_NOTIFICATION;
	}

	msg.TXsender = TXbase;

	msg.data = data;

	msg.length = 1+4+len;

	return ctrl_stack_send_msg(&msg);
}

// calls a pre-set callback that sends data to socket
// returns: 1 on error, 0 on success
static unsigned char ICACHE_FLASH_ATTR ctrl_stack_send_msg(tCtrlMessage *msg)
{
	if(ctrlCallbacks->send_data == NULL)
	{
		return 1;
	}

	char *activeAes128Key;

	// Special situation: When we are currently in authMode and in authPhase==1 we need
	// to use zero-aes128-key (key with all zeroes) to encrypt packet we are about to send.
	char zeroAes128Key[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
	if(authMode && authPhase == 1)
	{
		activeAes128Key = zeroAes128Key;
	}
	else
	{
		activeAes128Key = aes128Key;
	}

	// Packet structure:
	// [ALL_LENGTH] { [RANDOM_IV] [MESSAGE_LENGTH] [HEADER] [TX_SENDER] [DATA] [padding when needed] } [CMAC]
	// 		2             16              2           1          4        n              m               16

	// We need to allocate memory to fit everything into single byte-stream! Lets hope we will have enough memory to fit the provided "msg".
	unsigned char paddThisMuch = 16 - ((16 + 2 + msg->length) % 16);
	unsigned int allocateThisMuch = 2 + 16 + 2 + msg->length + paddThisMuch + 16; // dude!

	// package too long, can't fit so we will not even try!
	if(allocateThisMuch > 0xFFFF)
	{
		return 1;
	}

	char *toSend = (char *)os_zalloc(allocateThisMuch); // use os_zalloc for easier debugging
	// failed to allocate? sorry...
	if(toSend == NULL)
	{
		#ifdef CTRL_LOGGING
			os_printf("Out of memory (1) in ctrl_stack_send_msg()\r\n");
		#endif
		return 1;
	}

	// Copy ALL_LENGTH, since we already know it
	unsigned short all_length = allocateThisMuch-2;
	os_memcpy(toSend, &all_length, 2);

	char *toSendTempPtr = toSend + 2; // for simpler copying bellow
	// Copy IV (random16bytes) to its position
	os_memcpy(toSendTempPtr, random16bytes, 16);
	toSendTempPtr += 16;

	// Copy msg.length
	os_memcpy(toSendTempPtr, &msg->length, 2);
	toSendTempPtr += 2;

	// Copy msg.header
	os_memcpy(toSendTempPtr, &msg->header, 1);
	toSendTempPtr += 1;

	// Copy msg.TXsender
	os_memcpy(toSendTempPtr, &msg->TXsender, 4);
	toSendTempPtr += 4;

	// Copy msg.data (if any)
	if((unsigned int)msg->length - 5 > 0)
	{
		os_memcpy(toSendTempPtr, msg->data, msg->length-5);
		toSendTempPtr += msg->length-5;
	}

	// Now add padding if required (we already calculated how much at the beginning)
	if(paddThisMuch > 0)
	{
		os_memcpy(toSendTempPtr, random16bytes, paddThisMuch); // lets use random16bytes as source for padding
		toSendTempPtr += paddThisMuch;
	}

	/*#ifdef CTRL_LOGGING
		os_printf("plaintext: ");

		unsigned short i;
		for(i=2; i<(toSendTempPtr-toSend); i++)
		{
			char tmp2[10];
			os_sprintf(tmp2, " 0x%X", toSend[i]);
			os_printf_plus(tmp2);
		}
		os_printf(".\r\n");
	#endif*/

	// Now encrypt the plaintext (but skip first 2 bytes of [ALL_LENGTH])
	aes128_cbc_encrypt(toSend+2, (toSendTempPtr-toSend-2), activeAes128Key);

	/*#ifdef CTRL_LOGGING
		os_printf("encrypted: ");

		for(i=2; i<(toSendTempPtr-toSend); i++)
		{
			char tmp2[10];
			os_sprintf(tmp2, " 0x%X", toSend[i]);
			os_printf_plus(tmp2);
		}
		os_printf(".\r\n");
	#endif*/

	// Now calculate CMAC over entire ciphertext (but skip first 2 bytes of [ALL_LENGTH]) and place it at the last 16 bytes of toSend!
	cmac_generate(activeAes128Key, toSend+2, (toSendTempPtr-toSend-2), toSendTempPtr);

	/*#ifdef CTRL_LOGGING
		os_printf("CMAC: ");

		for(i=0; i<16; i++)
		{
			char tmp2[10];
			os_sprintf(tmp2, " 0x%X", toSendTempPtr[i]);
			os_printf_plus(tmp2);
		}
		os_printf(".\r\n");
	#endif*/

	// prepare IV for next encryption (lets use last 16 bytes, actually that's the CMAC of current encryption... this is supposed to be "safe to do" in AES-CBC mode)
	os_memcpy(random16bytes, toSendTempPtr, 16);

	// That should be it, now send it to Server!
	if(ctrlCallbacks->send_data(toSend, allocateThisMuch) != ESPCONN_OK)
	{
		os_free(toSend);
		return 1;
	}

	os_free(toSend);

	return 0;
}

// this sets or clears the backoff!
void ICACHE_FLASH_ATTR ctrl_stack_backoff(unsigned char backoff_)
{
	if(!safeToUnBackoff && !backoff_) return; // prevend unbackingoff if it is not safe
	if(backoff_) safeToUnBackoff = 0; // say it is not safe to unbackoff if we just backedoff server

	backoff = backoff_;
}

// this sends a request for current timestamp from Server. It should arrive asynchroniously ASAP
void ICACHE_FLASH_ATTR ctrl_stack_get_rtc(void)
{
	tCtrlMessage msg;
	msg.header = CH_SYSTEM_MESSAGE | CH_NOTIFICATION; // lets set NOTIFICATION type also, because we don't need ACKs on this system command, not a VERY big problem if it doesn't get through really
	msg.TXsender = 0; // since we set NOTIFICATION type, this is not relevant

	char d = SYSTEM_MESSAGE_GET_RTC;
	msg.data = (char *)&d;

	msg.length = 1+4+1;

	ctrl_stack_send_msg(&msg);
}

// this enables or disables the keep-alive on server's side
void ICACHE_FLASH_ATTR ctrl_stack_keepalive(unsigned char keepalive)
{
	tCtrlMessage msg;
	msg.header = CH_SYSTEM_MESSAGE | CH_NOTIFICATION; // lets set NOTIFICATION type also, because we don't need ACKs on this system command, not a VERY big problem if it doesn't get through really
	msg.TXsender = 0; // since we set NOTIFICATION type, this is not relevant

	char d = SYSTEM_MESSAGE_KEEPALIVE_OFF; // disable
	if(keepalive)
	{
		d = SYSTEM_MESSAGE_KEEPALIVE_ON; // enable
	}
	msg.data = (char *)&d;

	msg.length = 1+4+1;

	ctrl_stack_send_msg(&msg);
}

// authorize connection and synchronize TXsender fields in both directions
void ICACHE_FLASH_ATTR ctrl_stack_authorize(char *baseid_, char *aes128Key_, unsigned char sync)
{
	baseid = baseid_;
	aes128Key = aes128Key_;

	authMode = 1; // used in our local ctrl_stack_process_message() to know how to parse incoming data from server
	authPhase = 1;
	authSync = sync;

	tCtrlMessage msg;
	msg.length = 1 + 4 + 16;
	msg.header = 0x00; // value not relevant during authentication procedure
	msg.TXsender = 0; // value not relevant during authentication procedure
	msg.data = baseid; //contains: baseid

	// In case we already have something partial in rxBuff, we must free it since the remaining partial data will never arrive.
	// We will never have this != NULL in case there was a full message available there, because it would be parsed at the time
	// it arrived into this buffer!
	if(rxBuff != NULL)
	{
		os_timer_disarm(&tmrDataExpecter);
		os_free(rxBuff);
		rxBuff = NULL;
	}

	// prepare IV for very first encryption of the "msg"
	unsigned char i;
	for(i=0; i<4; i++)
	{
		unsigned long r = rand(); // here this randomness is not *that* important since the authKey is known to everyone (zeroes)
		os_memcpy(random16bytes+(i*4), &r, 4);
	}

	ctrl_stack_send_msg(&msg);
}

// init the CTRL stack
void ICACHE_FLASH_ATTR ctrl_stack_init(tCtrlCallbacks *cc)
{
	ctrlCallbacks = cc;

	os_timer_disarm(&tmrDataExpecter);
	os_timer_setfn(&tmrDataExpecter, (os_timer_func_t *)data_expecter_timeout, NULL);
}
