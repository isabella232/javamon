/***************************************************************************
* 
* File              : user_main.c
*
* Author			: Kurt E. Clothier
* Date				: July 6, 2015
* Modified			: July 14, 2015
*
* Description       : Pubnub Demo - Javamon
*
* Compiler			: Xtensa Tools - GCC
* Hardware			: ESP8266-x
* SDK Version		: 0.9.3 - 1.1.0
*
* More Information	: http://www.projectsbykec.com/
*					: http://www.pubnub.com/
*					: http://www.esp8266.com/
*
****************************************************************************/

/***************************************************************************

 | Digital :  ( LCD )|-->|ATmega328|<-(TWI)->|ESP8266|<-(PubNub)->|  The  |
 |  Scale  :(buttons)|<--|  MCU    |         | WiFI  |            | World |

	- Software TWI Master - Requests data about scale every 5 seconds.
		If data has changed, the new value is published using PubNub.

	- Send information like this: {"columns":[["Coffee",X]]}    
		where x is the weight in grams 
		This formatting is used by the web interface to display the data.

	- Serial Debugging available: rate (9600) set in include/user_config.h

	- You must put add your WiFi SSID and PW to include/user_config.h 
***************************************************************************/

/**************************************************************************
	Included Header Files
***************************************************************************/
#include "esp8266.h"
#include "pubnub.h"
#include "twi.h"

/**************************************************************************
	Global Variables
***************************************************************************/
os_event_t    user_procTaskQueue[user_procTaskQueueLen];
static volatile unsigned char stat_flag = 0x00;

// TWI Communication
static volatile unsigned char	TWI_state = TWI_IDLE;
static volatile bool			TWI_sclIsLO = false;
static volatile unsigned char	TWI_msgBuf = 0x00;
static volatile unsigned char	TWI_msg[2] = {0x00, 0x00};
static volatile uint16_t		TWI_lastMsg = 0;
static volatile uint16_t		TWI_fullMsg = 0;
static volatile unsigned char	TWI_sclCnt = 0;
static volatile unsigned char	TWI_comCnt = 0;

static volatile unsigned char pubTimer = 0;
// ms Timer
static volatile os_timer_t msTimer;
static volatile int msCnt = 0;	// Counter for ms timer

// Networking
static volatile os_timer_t networkTimer;

// Pubnub Attributes
static const char pubkey[] = "demo";
static const char subkey[] = "demo";
static const char channel[] = "javamon";

/**************************************************************************
	Local Function Prototypes
***************************************************************************/
static void IFA user_procTask(os_event_t *events);
static void IFA initUART(void);
static void IFA initGPIO(void);

static void IFA PN_connectedCB(void);			// Callback for PubNub Connections
static void IFA PN_connErrorCB(sint8 error);	// Callback for PubNub Connection Error
static void IFA PN_subscribeCB(char *m);		// Callback for PubNub Subscriptions
static void IFA connectedCB(void);				// Callback for network connection
static void IFA network_checkIP(void);			// Check for IP address

static void IFA msISR(void *arg);			// 1 ms interrupt service routine
static void publishMsg (void);				// Publish the TWI msg using PubNub

/**************************************************************************
	ESP8266 MAIN FUNCITONS
***************************************************************************/

// Required for SDK v1.1.0 - Can be used to disable RF
void user_rf_pre_init(void)
{
}

// Loop function - Not being used
static void IFA user_procTask(os_event_t *events)
{
	os_delay_us(10);
    //system_os_post(user_procTaskPrio, 0, 0 );
}

// User Initialization
void IFA user_init()
{
    struct station_config stationConf;
	const char ssid[SSID_LENGTH] = SSID;
    const char pw[PW_LENGTH] = SSID_PW;

	initUART();
	initGPIO();

	pubnub_init(pubkey, subkey);

	// Create a 1ms timer
	os_timer_disarm(&msTimer);
    os_timer_setfn(&msTimer, (os_timer_func_t *)msISR, NULL);

	// Connect to network
	// Putting this here seems to work best (over external functions)
    wifi_set_opmode(STATION_MODE);
	stationConf.bssid_set = 0;	
    os_memcpy(&stationConf.ssid, ssid, SSID_LENGTH);
    os_memcpy(&stationConf.password, pw, PW_LENGTH);

    wifi_station_set_config(&stationConf);

	// Setup timer to check for IP address
	os_timer_disarm(&networkTimer);
	os_timer_setfn(&networkTimer, (os_timer_func_t *)network_checkIP, NULL);
	os_timer_arm(&networkTimer, IP_CHECK_DELAY, NO_REPEAT);

    //Start os task
    system_os_task(user_procTask, user_procTaskPrio, user_procTaskQueue, user_procTaskQueueLen);
    system_os_post(user_procTaskPrio, 0, 0 );

}

/**************************************************************************
	INTERRUPT SERVICE ROUTINES
***************************************************************************/

// 1 ms timer
static void IFA msISR(void *arg)
{

	//--------------------------------
	// TWI Transmission
	//--------------------------------
	switch(TWI_state) {

		//------------------------------
		// START - SDA Pulled LO while SCL is HI
		//------------------------------
		case TWI_START:
			if(SCL_IS_HI && SDA_IS_HI) {
				SDA_SET_LO();
				TWI_sclIsLO = false;
				TWI_state = TWI_SLA_WRITE;
				TWI_sclCnt = 8;
				//DEBUG_PRINT(("TWI Sending: 0x%08x\n", TWI_msgBuf));
			}
			else {
				RELEASE_SDA();
				RELEASE_SCL();
			}
			break;

		//------------------------------
		// Address Slave Device
		//------------------------------
		case TWI_SLA_READ:
		case TWI_SLA_WRITE:
			//- - - - - - - - - -
			// SCL Rising Edge
			// - Read Data, If applicable
			//- - - - - - - - - -
			if(TWI_sclIsLO) {
				SCL_SET_HI();
				TWI_sclIsLO = false;
				if(!TRANSMITTING) {
					TWI_msgBuf <<= 1;
					if(SDA_IS_HI) TWI_msgBuf |= 0x01;
					else TWI_msgBuf &= ~0x01;
				}
				--TWI_sclCnt;
				break;
			}

			//- - - - - - - - - -
			// SCL Falling Edge
			// - Transmit Data, If applicable
			//- - - - - - - - - -
			if(SCL_IS_HI) {
				SCL_SET_LO();
				TWI_sclIsLO = true;
				if(TWI_sclCnt) {
					if(TRANSMITTING) {
						if(TWI_msgBuf & 0x80) SDA_SET_HI();
						else SDA_SET_LO();
						TWI_msgBuf <<= 1;
					}
				}
				// Final Bit sent, Time for ACK/NACK
				else {
					if(TRANSMITTING)
						RELEASE_SDA();
					else
						SDA_SEND_ACK();
					TWI_state = TWI_ACK;
				}
			}
			break;

		//------------------------------
		// Send / Receive ACK
		//------------------------------
		case TWI_ACK:
			//- - - - - - - - - -
			// SCL Rising Edge
			// - Read ACK, if transmitting
			// - Record message if receiving
			//- - - - - - - - - -
			if (TWI_sclIsLO) {
				SCL_SET_HI();
				TWI_sclIsLO = false;
				if(TRANSMITTING) {
					if(SDA_IS_HI) TWI_msgBuf = TWI_RECV_NACK;
					else TWI_msgBuf = TWI_RECV_ACK;
				}
				else {
					//DEBUG_PRINT(("TWI Received: %d\n", TWI_msgBuf));
					if(stat_flag & FIRST_BYTE) {
						TWI_msg[0] = TWI_msgBuf;
					}
					else {
						TWI_msg[1] = TWI_msgBuf;
					}


				}
				break;
			}

			//- - - - - - - - - -
			// SCL Falling Edge
			// - End of Transmission Cycle
			//- - - - - - - - - -
			if(SCL_IS_HI) {
				SCL_SET_LO();
				TWI_sclIsLO = true;
				// Time to read response
				if (TRANSMITTING) {
					stat_flag &= ~TRANSMIT;
					TWI_state = TWI_SLA_READ;
					TWI_sclCnt = 8;
				}
				// Reading
				else {
					// One more byte to read...
					if(stat_flag & FIRST_BYTE) {
						RELEASE_SDA();
						stat_flag &= ~FIRST_BYTE;
						TWI_state = TWI_SLA_READ;
						TWI_sclCnt = 8;
					}
					// Done reading
					else {
						TWI_state = TWI_STOP;
						SDA_SET_LO();
					}
				}
			}
			break;

		//------------------------------
		// STOP - SDA Released while SCL is HI
		// Publish Message!
		//------------------------------
		case TWI_STOP:
			if(TWI_sclIsLO) {
				TWI_sclIsLO = false;
				RELEASE_SCL();
			}
			else if (SCL_IS_HI) {
				RELEASE_SDA();
				TWI_sclIsLO = false;
				TWI_state = TWI_IDLE;
				publishMsg();
			}
			break;

		//------------------------------
		// IDLE
		//------------------------------
		case TWI_IDLE:
			if (++msCnt == TIME_CHECK_SCALE) {
				stat_flag |= TRANSMIT;
				TWI_state = TWI_START;
				stat_flag |= FIRST_BYTE;

				if(stat_flag & REQUEST_RESET)
					TWI_msgBuf = (ATMEGA_SLA << 1) | TWI_WRITE; 
				else
					TWI_msgBuf = (ATMEGA_SLA << 1) | TWI_READ; 
				msCnt = 0;

				if (++pubTimer == TIME_PUBLISH) {
					stat_flag |= REQUEST_PUBLISH;
					pubTimer = 0;
				}
			}
			break;

		//------------------------------
		// Something Went Wrong...
		//------------------------------
		default:
			TWI_state = TWI_STOP;
			break;

	}

}

/**************************************************************************
	UTILITIES
***************************************************************************/

// Check for IP address before continuing with connection...
static void IFA network_checkIP(void)
{
	struct ip_info ipconfig;
	os_timer_disarm(&networkTimer);
	wifi_get_ip_info(STATION_IF, &ipconfig);
	if (wifi_station_get_connect_status() == STATION_GOT_IP && ipconfig.ip.addr != 0) {
		connectedCB();
	}
   	else {
		DEBUG_PRINT(("  No ip found...\r\n"));
		os_timer_setfn(&networkTimer, (os_timer_func_t *)network_checkIP, NULL);
		os_timer_arm(&networkTimer, IP_CHECK_DELAY, NO_REPEAT);
	}
}

// Callback - Runs when connected to network
static void IFA connectedCB(void)
{
	pubnub_connect(PN_connectedCB, PN_connErrorCB);
}

// Callback - Runs when someone publishes to subscribed channel(s)
static void IFA PN_subscribeCB(char *m)
{
	DEBUG_PRINT(("\nSubscribe Callback:\n%s\n", m));
}

// Callback - Called when a connection to PubNub is made 
static void IFA PN_connectedCB(void)
{
	DEBUG_PRINT(("\nPubnub connection callback...\n"));


	// Request initial publish
	stat_flag |= REQUEST_PUBLISH;

	// enable 1ms timer
    os_timer_arm(&msTimer, 1, REPEAT);
}

// Callback - Called when on PubNub connection error
static void IFA PN_connErrorCB(sint8 error)
{
	DEBUG_PRINT(("\nPubNub connection error!\n"));
	DEBUG_PRINT(("\n.. requesting reset..\n"));
	stat_flag |= REQUEST_RESET;
}

// Publish the value using PubNub
static void publishMsg (void)
{
	// Check values for I2C line error...
	if (TWI_msg[0] > 99 || TWI_msg[1] > 99) {
		TWI_fullMsg = 10000;
	}
	// Ignore very small values
	else if (TWI_msg[0] == 0 && TWI_msg[1] < 50) {
		TWI_fullMsg = 0;
	}
	// Record full value and round to nearest 10
	else {
		int mod = TWI_msg[1] % 10;
		TWI_msg[1] /= 10;
		if (mod > 4)
			++TWI_msg[1];
		TWI_msg[1] *= 10;
		TWI_fullMsg = (uint16_t)TWI_msg[1] + 100 * (uint16_t)TWI_msg[0];
	}
	// Only publish if this is a new value or it's been a while...
	if((TWI_fullMsg != TWI_lastMsg) || TIME_TO_PUBLISH) {
		char buf[40] = { 0, };	
		sprintf(buf, "{\"columns\":[[\"Coffee\",\"%d\"]]}", TWI_fullMsg);
		pubnub_publish(channel, buf);
		TWI_lastMsg = TWI_fullMsg;
		stat_flag &= ~REQUEST_PUBLISH;
	}
}

/**************************************************************************
	INITIALIZATION ROUTINES AND POWER MODES
***************************************************************************/

// Configure UART for serial debugging
static void IFA initUART(void)
{
	// BITRATE set in user_config.h
	uart_div_modify(0, UART_CLK_FREQ / BITRATE);
}

// Configure GPIO
static void IFA initGPIO(void)
{
	gpio_init();

	// SCL Pin
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);
	SET_AS_INPUT(2);

	// SDA Pin
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);
	SET_AS_INPUT(0);
}

