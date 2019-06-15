#include <stm32f1xx_ll_usart.h>
#include <stm32f1xx_ll_gpio.h>
#include <stm32f1xx_hal_rcc.h>

#include <Arduino_FreeRTOS.h>
#include <NMEAGPS.h>
#include "Streamers.h"

#include "GPSThread.h"
#include "GPSDataModel.h"
#include "USBDebugLogger.h"

#include "SDThread.h"
#include "USBDebugLogger.h"

// A GPS parser
static NMEAGPS gpsParser;

// Size of UART input buffer
const uint8_t gpsBufferSize = 128;

// Pin constants
static GPIO_TypeDef * const		TX_PIN_PORT		= GPIOA;
static const uint32_t			TX_PIN_NUM		= LL_GPIO_PIN_9;
static GPIO_TypeDef * const		RX_PIN_PORT		= GPIOA;
static const uint32_t			RX_PIN_NUM		= LL_GPIO_PIN_10;
static GPIO_TypeDef * const		ENABLE_PIN_PORT	= GPIOB;
static const uint32_t			ENABLE_PIN_NUM	= LL_GPIO_PIN_9;


// This class handles UART interface that receive chars from GPS and stores them to a buffer
class GPS_UART
{
	// Receive ring buffer
	uint8_t rxBuffer[gpsBufferSize];
	volatile uint8_t lastReadIndex = 0;
	volatile uint8_t lastReceivedIndex = 0;

	// GPS thread handle
	TaskHandle_t xGPSThread = nullptr;

public:
	void init()
	{
		// Reset pointers (just in case someone calls init() multiple times)
		lastReadIndex = 0;
		lastReceivedIndex = 0;

		// Initialize GPS Thread handle
		xGPSThread = xTaskGetCurrentTaskHandle();

		// Enable clocking of corresponding periperhal
		__HAL_RCC_GPIOA_CLK_ENABLE();
		__HAL_RCC_GPIOB_CLK_ENABLE();
		__HAL_RCC_USART1_CLK_ENABLE();

		// Init pins in alternate function mode
		LL_GPIO_SetPinMode(TX_PIN_PORT, TX_PIN_NUM, LL_GPIO_MODE_ALTERNATE);	// TX pin
		LL_GPIO_SetPinSpeed(TX_PIN_PORT, TX_PIN_NUM, LL_GPIO_SPEED_FREQ_HIGH);
		LL_GPIO_SetPinOutputType(TX_PIN_PORT, TX_PIN_NUM, LL_GPIO_OUTPUT_PUSHPULL);

		LL_GPIO_SetPinMode(RX_PIN_PORT, RX_PIN_NUM, LL_GPIO_MODE_INPUT);		// RX pin

		LL_GPIO_SetPinMode(ENABLE_PIN_PORT, ENABLE_PIN_NUM, LL_GPIO_MODE_OUTPUT);					// Enable pin
		LL_GPIO_SetPinOutputType(ENABLE_PIN_PORT, ENABLE_PIN_NUM, LL_GPIO_OUTPUT_PUSHPULL);
		LL_GPIO_SetPinSpeed(ENABLE_PIN_PORT, ENABLE_PIN_NUM, LL_GPIO_SPEED_FREQ_LOW);

		//@TODO: Always enable GPS UART for now. Consider shutting down GPS on idle based on accelerometer values
		LL_GPIO_ResetOutputPin(ENABLE_PIN_PORT, ENABLE_PIN_NUM);

		// Prepare for initialization
		LL_USART_Disable(USART1);

		// Init
		LL_USART_SetBaudRate(USART1, HAL_RCC_GetPCLK2Freq(), 9600);
		LL_USART_SetDataWidth(USART1, LL_USART_DATAWIDTH_8B);
		LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_1);
		LL_USART_SetParity(USART1, LL_USART_PARITY_NONE);
		LL_USART_SetTransferDirection(USART1, LL_USART_DIRECTION_TX_RX);
		LL_USART_SetHWFlowCtrl(USART1, LL_USART_HWCONTROL_NONE);

		// We will be using UART interrupt to get data
		HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
		HAL_NVIC_EnableIRQ(USART1_IRQn);

		// Enable UART interrupt on byte reception
		LL_USART_EnableIT_RXNE(USART1);

		// Finally enable the peripheral
		LL_USART_Enable(USART1);
	}

	// Check if byte is available
	bool available() const
	{
		return lastReadIndex != lastReceivedIndex;
	}

	// Read a symbol
	char readChar()
	{
		if (available())
			return (char)rxBuffer[lastReadIndex++ % gpsBufferSize];
		else
			return '\0';
	}

	// Wait until whole line is received
	bool waitForString()
	{
		return ulTaskNotifyTake(pdTRUE, 10);
	}

	// Process received byte
	inline void charReceivedCB(uint8_t c)
	{
		rxBuffer[lastReceivedIndex % gpsBufferSize] = c;
		lastReceivedIndex++;

		// If a EOL symbol received, notify GPS thread that line is avaialble to read
		if(c == '\n')
			vTaskNotifyGiveFromISR(xGPSThread, nullptr);
	}
};

static GPS_UART gpsUart; // An instance of UART handler


extern "C" void USART1_IRQHandler(void)
{
	uint8_t byte = LL_USART_ReceiveData8(USART1);
	gpsUart.charReceivedCB(byte);
}

__attribute__((noreturn))
void vGPSTask(void *)
{
	uint8_t maxLen = 0;

	// GPS initialization must be done withing GPS thread as thread handle is stored
	// and used later for synchronization purposes
	gpsUart.init();

	for (;;)
	{
		//Receive one line from GPS
		char * buf = requestRawGPSBuffer();
		uint8_t len = 0;

		// Wait until whole string is received
		if(!gpsUart.waitForString())
			continue;

		// Read received string and parse GPS stream char by char
		while(gpsUart.available())
		{
			char c = gpsUart.readChar();
			gpsParser.handle(c);
			buf[len] = c;

			// Reached end of line
			if(c == '\n')
			{
				buf[len] = '\0';
				break;
			}

			len++;
			// Buffer overrun protection
			if(len == maxRawGPSDataLen)
			{
				buf[len] = '\n';
				break;
			}
		}
		
		if(len > maxLen)
		{
			maxLen = len;
			usbDebugWrite("=== New max len detected: %d\n", maxLen);
		}

		usbDebugWrite("GPS message: %s\n", buf);

		//Send received raw data to SD thread
//		ackRawGPSData(len);

		// Update GPS model data
		if(gpsParser.available())
		{
			GPSDataModel::instance().processNewGPSFix(gpsParser.read());
			GPSDataModel::instance().processNewSatellitesData(gpsParser.satellites, gpsParser.sat_count);
		}

		vTaskDelay(10);
	}
}
