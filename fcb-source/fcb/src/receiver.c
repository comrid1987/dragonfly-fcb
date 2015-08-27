/******************************************************************************
 * @file    receiver.c
 * @author  Dragonfly
 * @version v. 1.0.0
 * @date    2015-05-28
 * @brief   File contains functionality for signal reading from the Dragonfly
 *          RC receiver. The receiver model is Spektrum AR610, which uses DSMX
 *          frequency modulation technology. It outputs 6 channels: throttle,
 *          aileron, elevator, rudder, gear and aux1.
 *
 *          _RECEIVER CHANNEL ENCODING_
 *          Since a timer IC on STM32 only has up to 4 channels, two timers are
 *          needed to collect the receiver pulses. The pulses sent from the
 *          receiver are typically ~1-2 ms width with a period of ~22 ms.
 *
 *          It is the pulse width that encodes the received transmitter stick
 *          action and is therefore the most relevant quantity when reading the
 *          signals from the receiver. As example, the aileron channel outputs
 *          ~1 ms when the transmitter control stick is held in one direction and
 *          ~2 ms when it is held in the opposite direction.
 *
 *          _PERFORMING A CALIBRATION_
 *          To perform a calibration of the receiver channels, the function
 *          StartReceiverCalibration() must be called. The receiver channels
 *          will then be sampled for up to RECEIVER_MAX_CALIBRATION_DURATION
 *          during which the user must push all the sticks in to their top and
 *          bottom ranges cyclically as well as toggling the aux1 and gear switches
 *          a few times.
 *
 *          Naturally, the receiver must be actively reading from the transmitter
 *          so checking that these are binded and by calling IsReceiverActive()
 *          before starting the calibration, this can be assured.
 *
 *          During calibration, the StopReceiverCalibration() must be
 *          called to finalize the calibration procedure, where the new values
 *          are taken into use (if they are valid) and written to persistent
 *          flash memory for future use. If it is not called within
 *          RECEIVER_MAX_CALIBRATION_DURATION time, the calibration will time out.
 ******************************************************************************/

/* Includes ------------------------------------------------------------------*/
#include "receiver.h"
#include "usbd_cdc_if.h"
#include "flash.h"
#include "common.h"
#include "fcb_error.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "stm32f3xx_hal.h"

/* Private typedef -----------------------------------------------------------*/
typedef enum {
	RECEIVER_CALIBRATION_WAITING = 0, RECEIVER_CALIBRATION_IN_PROGRESS = 1,
} ReceiverCalibrationState;

typedef struct {
	Pulse_State ThrottleInputState;
	Pulse_State AileronInputState;
	Pulse_State ElevatorInputState;
	Pulse_State RudderInputState;
	Pulse_State GearInputState;
	Pulse_State Aux1InputState;
} Receiver_Pulse_States_TypeDef;

typedef struct {
	uint32_t PeriodCount;
	uint16_t RisingCount;
	uint16_t FallingCounter;
	uint16_t PreviousRisingCount;
	uint16_t PreviousRisingCountTimerPeriodCount;
	uint16_t PulseTimerCount;
	ReceiverErrorStatus IsActive;
} Receiver_IC_Values_TypeDef;

typedef struct {
	uint16_t maxSamplesBuffer[RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE];
	uint16_t minSamplesBuffer[RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE];
	uint16_t channelCalibrationPulseSamples;
	uint16_t tmpMaxIndex;
	uint16_t tmpMaxBufferMinValue;
	uint16_t tmpMinIndex;
	uint16_t tmpMinBufferMaxValue;
	bool maxBufferUpdated;
	bool minBufferUpdated;
} Receiver_ChannelCalibrationSampling_TypeDef;

/* Private define ------------------------------------------------------------*/
#define RECEIVER_PRINT_SAMPLING_THREAD_PRIO				3

#define RECEIVER_SAMPLING_MAX_STRING_SIZE				256
#define RECEIVER_SAMPLE_VALUE_STRING_SIZE				8

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

/* Timer time base handlers for each timer - These are exported to stm32f3xx_it.c */
TIM_HandleTypeDef PrimaryReceiverTimHandle;
TIM_HandleTypeDef AuxReceiverTimHandle;

/* Timer IC init declarations for each channel */
static TIM_IC_InitTypeDef ThrottleChannelICConfig;
static TIM_IC_InitTypeDef AileronChannelICConfig;
static TIM_IC_InitTypeDef ElevatorChannelICConfig;
static TIM_IC_InitTypeDef RudderChannelICConfig;
static TIM_IC_InitTypeDef GearChannelICConfig;
static TIM_IC_InitTypeDef Aux1ChannelICConfig;

/* Struct contaning HIGH/LOW state for each input channel pulse */
static Receiver_Pulse_States_TypeDef ReceiverPulseStates;

/* Structs for each channel's timer count values */
static volatile Receiver_IC_Values_TypeDef ThrottleICValues;
static volatile Receiver_IC_Values_TypeDef AileronICValues;
static volatile Receiver_IC_Values_TypeDef ElevatorICValues;
static volatile Receiver_IC_Values_TypeDef RudderICValues;
static volatile Receiver_IC_Values_TypeDef GearICValues;
static volatile Receiver_IC_Values_TypeDef Aux1ICValues;

/* Struct for all receiver channel's calibration values */
static volatile Receiver_CalibrationValues_TypeDef CalibrationValues;
static volatile ReceiverCalibrationState receiverCalibrationState;
static volatile uint32_t calibrationStartTime;

/* Structs for sampling the receiver channels when calibrating */
static volatile Receiver_ChannelCalibrationSampling_TypeDef ThrottleCalibrationSampling;
static volatile Receiver_ChannelCalibrationSampling_TypeDef AileronCalibrationSampling;
static volatile Receiver_ChannelCalibrationSampling_TypeDef ElevatorCalibrationSampling;
static volatile Receiver_ChannelCalibrationSampling_TypeDef RudderCalibrationSampling;
static volatile Receiver_ChannelCalibrationSampling_TypeDef GearCalibrationSampling;
static volatile Receiver_ChannelCalibrationSampling_TypeDef Aux1CalibrationSampling;

/* Timer reset counters for each timer */
static volatile uint16_t PrimaryReceiverTimerPeriodCount;
static volatile uint16_t AuxReceiverTimerPeriodCount;

/* Task handle for printing of receiver values task */
xTaskHandle ReceiverPrintSamplingTaskHandle = NULL;
static volatile uint16_t receiverPrintSampleTime;
static volatile uint16_t receiverPrintSampleDuration;

/* Private function prototypes -----------------------------------------------*/
static ReceiverErrorStatus InitReceiverCalibrationValues(void);
static ReceiverErrorStatus LoadReceiverCalibrationValuesFromFlash(
		volatile Receiver_CalibrationValues_TypeDef* calibrationValues);
static void SetDefaultReceiverCalibrationValues(volatile Receiver_CalibrationValues_TypeDef* calibrationValues);
static ReceiverErrorStatus PrimaryReceiverInputConfig(void);
static ReceiverErrorStatus AuxReceiverInput_Config(void);

static ReceiverErrorStatus UpdateReceiverChannel(TIM_HandleTypeDef* TimHandle, TIM_IC_InitTypeDef* TimIC,
		Pulse_State* channelInputState, volatile Receiver_IC_Values_TypeDef* ChannelICValues,
		const uint32_t receiverChannel, volatile const uint16_t ReceiverTimerPeriodCount,
		volatile Receiver_ChannelCalibrationSampling_TypeDef* ChannelCalibrationSampling);
static ReceiverErrorStatus UpdateChannelCalibrationSamples(
		volatile Receiver_ChannelCalibrationSampling_TypeDef* channelCalibrationSampling,
		const uint16_t channelPulseTimerCount);
static ReceiverErrorStatus UpdateReceiverThrottleChannel(void);
static ReceiverErrorStatus UpdateReceiverAileronChannel(void);
static ReceiverErrorStatus UpdateReceiverElevatorChannel(void);
static ReceiverErrorStatus UpdateReceiverRudderChannel(void);
static ReceiverErrorStatus UpdateReceiverGearChannel(void);
static ReceiverErrorStatus UpdateReceiverAux1Channel(void);

static int16_t GetSignedReceiverChannel(volatile const Receiver_IC_Values_TypeDef* ChannelICValues,
		volatile const Receiver_IC_ChannelCalibrationValues_TypeDef* ChannelCalibrationValues);
//static uint16_t GetUnsignedReceiverChannel(volatile const Receiver_IC_Values_TypeDef* ChannelICValues,
//		volatile const Receiver_IC_ChannelCalibrationValues_TypeDef* ChannelCalibrationValues);
static uint16_t GetReceiverChannelPulseMicros(volatile const Receiver_IC_Values_TypeDef* ChannelICValues);
static uint16_t GetReceiverChannelPeriodMicros(volatile const Receiver_IC_Values_TypeDef* ChannelICValues);

static ReceiverErrorStatus IsReceiverChannelActive(volatile Receiver_IC_Values_TypeDef* ChannelICValues,
		const uint16_t ReceiverTimerPeriodCount);
static ReceiverErrorStatus IsReceiverPulseValid(const uint16_t pulseTimerCount, const uint16_t currentPeriodCount,
		const uint16_t previousPeriodCount);
static ReceiverErrorStatus IsReceiverPeriodValid(const uint32_t periodTimerCount);
static ReceiverErrorStatus IsCalibrationValuesValid(
		volatile const Receiver_CalibrationValues_TypeDef* calibrationValues);
static ReceiverErrorStatus IsCalibrationMaxPulseValueValid(const uint16_t maxPulseValue);
static ReceiverErrorStatus IsCalibrationMinPulseValueValid(const uint16_t minPulseValue);

static void EnforceNewCalibrationValues(volatile Receiver_CalibrationValues_TypeDef* newCalibrationValues);
static void ResetCalibrationSampling(volatile Receiver_ChannelCalibrationSampling_TypeDef* channelCalibrationSampling);
static void ReceiverToggleICPolarity(TIM_HandleTypeDef* htim, TIM_IC_InitTypeDef* sConfig, uint32_t Channel);

static void ReceiverPrintSamplingTask(void const *argument);

/* Exported functions --------------------------------------------------------*/

/*
 * @brief  Initializes timers in input capture mode to read the receiver input signals
 * @param  None
 * @retval None
 */
ReceiverErrorStatus ReceiverInputConfig(void) {
	InitReceiverCalibrationValues();

	if (!PrimaryReceiverInputConfig())
		return RECEIVER_ERROR;

	if (!AuxReceiverInput_Config())
		return RECEIVER_ERROR;

	return RECEIVER_OK;
}

/*
 * @brief  Creates a task to sample print receiver values over USB
 * @param  sampleTime : Sets how often a sample should be printed
 * @param  sampleDuration : Sets for how long sampling should be performed
 * @retval RECEIVER_OK if thread started, else RECEIVER_ERROR
 */
ReceiverErrorStatus StartReceiverSamplingTask(const uint16_t sampleTime, const uint32_t sampleDuration) {
	receiverPrintSampleTime = sampleTime;
	receiverPrintSampleDuration = sampleDuration;

	/* Receiver value print sampling handler thread creation
	 * Task function pointer: ReceiverPrintSamplingTask
	 * Task name: RC_PRINT_SAMPL
	 * Stack depth: configMINIMAL_STACK_SIZE
	 * Parameter: NULL
	 * Priority: RECEIVER_PRINT_SAMPLING_THREAD_PRIO ([0, inf] possible)
	 * Handle: ReceiverPrintSamplingTask
	 * */
	if (pdPASS != xTaskCreate((pdTASK_CODE )ReceiverPrintSamplingTask, (signed portCHAR*)"RC_PRINT_SAMPL",
					configMINIMAL_STACK_SIZE, NULL, RECEIVER_PRINT_SAMPLING_THREAD_PRIO,
					&ReceiverPrintSamplingTaskHandle)) {
		ErrorHandler();
		return RECEIVER_ERROR;
	}

	return RECEIVER_OK;
}

/*
 * @brief  Stops receiver print sampling by deleting the task
 * @param  None
 * @retval RECEIVER_OK if task deleted
 */
ReceiverErrorStatus StopReceiverSamplingTask(void) {
	vTaskDelete(ReceiverPrintSamplingTaskHandle);
	return RECEIVER_OK;
}

/*
 * @brief  Returns a normalized receiver throttle pulse value as an unsigned integer.
 * @param  None
 * @retval throttle value [-32768, 32767]
 */
int16_t GetThrottleReceiverChannel(void) {
	return GetSignedReceiverChannel(&ThrottleICValues, &CalibrationValues.ThrottleChannel);
}

/*
 * @brief  Returns a normalized receiver aileron pulse value as a signed integer.
 * @param  None
 * @retval aileron value [-32768, 32767]
 */
int16_t GetAileronReceiverChannel(void) {
	return GetSignedReceiverChannel(&AileronICValues, &CalibrationValues.AileronChannel);
}

/*
 * @brief  Returns a normalized receiver elevator pulse value as a signed integer.
 * @param  None
 * @retval elevator value [-32768, 32767]
 */
int16_t GetElevatorReceiverChannel(void) {
	return GetSignedReceiverChannel(&ElevatorICValues, &CalibrationValues.ElevatorChannel);
}

/*
 * @brief  Returns a normalized receiver rudder pulse value as a signed integer.
 * @param  None
 * @retval rudder value [-32768, 32767]
 */
int16_t GetRudderReceiverChannel(void) {
	return GetSignedReceiverChannel(&RudderICValues, &CalibrationValues.RudderChannel);
}

/*
 * @brief  Returns a normalized receiver gear pulse value as a signed integer.
 * @param  None
 * @retval gear value [-32768, 32767]
 */
int16_t GetGearReceiverChannel(void) {
	return GetSignedReceiverChannel(&GearICValues, &CalibrationValues.GearChannel);
}

/*
 * @brief  Returns a normalized receiver aux1 pulse value as a signed integer.
 * @param  None
 * @retval aux1 value [-32768, 32767]
 */
int16_t GetAux1ReceiverChannel(void) {
	return GetSignedReceiverChannel(&Aux1ICValues, &CalibrationValues.Aux1Channel);
}

/*
 * @brief  Returns the last throttle pulse value in microseconds
 * @param  None
 * @retval throttle pulse value in microseconds
 */
uint16_t GetThrottleReceiverChannelPulseMicros(void) {
	return GetReceiverChannelPulseMicros(&ThrottleICValues);
}

/*
 * @brief  Returns the last aileron pulse value in microseconds
 * @param  None
 * @retval aileron pulse value in microseconds
 */
uint16_t GetAileronReceiverChannelPulseMicros(void) {
	return GetReceiverChannelPulseMicros(&AileronICValues);
}

/*
 * @brief  Returns the last elevator pulse value in microseconds
 * @param  None
 * @retval elevator pulse value in microseconds
 */
uint16_t GetElevatorReceiverChannelPulseMicros(void) {
	return GetReceiverChannelPulseMicros(&ElevatorICValues);
}

/*
 * @brief  Returns the last rudder pulse value in microseconds
 * @param  None
 * @retval rudder pulse value in microseconds
 */
uint16_t GetRudderReceiverChannelPulseMicros(void) {
	return GetReceiverChannelPulseMicros(&RudderICValues);
}

/*
 * @brief  Returns the last gear pulse value in microseconds
 * @param  None
 * @retval gear pulse value in microseconds
 */
uint16_t GetGearReceiverChannelPulseMicros(void) {
	return GetReceiverChannelPulseMicros(&GearICValues);
}

/*
 * @brief  Returns the last aux1 pulse value in microseconds
 * @param  None
 * @retval aux1 pulse value in microseconds
 */
uint16_t GetAux1ReceiverChannelPulseMicros(void) {
	return GetReceiverChannelPulseMicros(&Aux1ICValues);
}

/*
 * @brief  Returns the last throttle period value in microseconds
 * @param  None
 * @retval throttle period value in microseconds
 */
uint16_t GetThrottleReceiverChannelPeriodMicros(void) {
	return GetReceiverChannelPeriodMicros(&ThrottleICValues);
}

/*
 * @brief  Returns the last aileron period value in microseconds
 * @param  None
 * @retval aileron period value in microseconds
 */
uint16_t GetAileronReceiverChannelPeriodMicros(void) {
	return GetReceiverChannelPeriodMicros(&AileronICValues);
}

/*
 * @brief  Returns the last elevator period value in microseconds
 * @param  None
 * @retval elevator period value in microseconds
 */
uint16_t GetElevatorReceiverChannelPeriodMicros(void) {
	return GetReceiverChannelPeriodMicros(&ElevatorICValues);
}

/*
 * @brief  Returns the last rudder period value in microseconds
 * @param  None
 * @retval rudder period value in microseconds
 */
uint16_t GetRudderReceiverChannelPeriodMicros(void) {
	return GetReceiverChannelPeriodMicros(&RudderICValues);
}

/*
 * @brief  Returns the last gear period value in microseconds
 * @param  None
 * @retval gear period value in microseconds
 */
uint16_t GetGearReceiverChannelPeriodMicros(void) {
	return GetReceiverChannelPeriodMicros(&GearICValues);
}

/*
 * @brief  Returns the last aux1 period value in microseconds
 * @param  None
 * @retval aux1 period value in microseconds
 */
uint16_t GetAux1ReceiverChannelPeriodMicros(void) {
	return GetReceiverChannelPeriodMicros(&Aux1ICValues);
}

/*
 * @brief  Returns the last throttle pulse value in timer ticks
 * @param  None
 * @retval throttle pulse value in timer ticks
 */
uint16_t GetThrottleReceiverChannelPulseTicks(void) {
	return ThrottleICValues.PulseTimerCount;
}

/*
 * @brief  Returns the last aileron pulse value in timer ticks
 * @param  None
 * @retval aileron pulse value in timer ticks
 */
uint16_t GetAileronReceiverChannelPulseTicks(void) {
	return AileronICValues.PulseTimerCount;
}

/*
 * @brief  Returns the last elevator pulse value in timer ticks
 * @param  None
 * @retval elevator pulse value in timer ticks
 */
uint16_t GetElevatorReceiverChannelPulseTicks(void) {
	return ElevatorICValues.PulseTimerCount;
}

/*
 * @brief  Returns the last rudder pulse value in timer ticks
 * @param  None
 * @retval rudder pulse value in timer ticks
 */
uint16_t GetRudderReceiverChannelPulseTicks(void) {
	return RudderICValues.PulseTimerCount;
}

/*
 * @brief  Returns the last gear pulse value in timer ticks
 * @param  None
 * @retval gear pulse value in timer ticks
 */
uint16_t GetGearReceiverChannelPulseTicks(void) {
	return GearICValues.PulseTimerCount;
}

/*
 * @brief  Returns the last aux1 pulse value in timer ticks
 * @param  None
 * @retval aux1 pulse value in timer ticks
 */
uint16_t GetAux1ReceiverChannelPulseTicks(void) {
	return Aux1ICValues.PulseTimerCount;
}

/*
 * @brief  Returns the last throttle period value in timer ticks
 * @param  None
 * @retval throttle period value in timer ticks
 */
uint32_t GetThrottleReceiverChannelPeriodTicks(void) {
	return ThrottleICValues.PeriodCount;
}

/*
 * @brief  Returns the last aileron period value in timer ticks
 * @param  None
 * @retval aileron period value in timer ticks
 */
uint32_t GetAileronReceiverChannelPeriodTicks(void) {
	return AileronICValues.PeriodCount;
}

/*
 * @brief  Returns the last elevator period value in timer ticks
 * @param  None
 * @retval elevator period value in timer ticks
 */
uint32_t GetElevatorReceiverChannelPeriodTicks(void) {
	return ElevatorICValues.PeriodCount;
}

/*
 * @brief  Returns the last rudder period value in timer ticks
 * @param  None
 * @retval rudder period value in timer ticks
 */
uint32_t GetRudderReceiverChannelPeriodTicks(void) {
	return RudderICValues.PeriodCount;
}

/*
 * @brief  Returns the last gear period value in timer ticks
 * @param  None
 * @retval gear period value in timer ticks
 */
uint32_t GetGearReceiverChannelPeriodTicks(void) {
	return GearICValues.PeriodCount;
}

/*
 * @brief  Returns the last aux1 period value in timer ticks
 * @param  None
 * @retval aux1 period value in timer ticks
 */
uint32_t GetAux1ReceiverChannelPeriodTicks(void) {
	return Aux1ICValues.PeriodCount;
}

void PrintReceiverValues(void)
{
	static char sampleString[RECEIVER_SAMPLING_MAX_STRING_SIZE];
	char sampleValueTmpString[RECEIVER_SAMPLE_VALUE_STRING_SIZE]; // Only needs enough space to store an uint16_t/int16_t in string format

	strncpy(sampleString, "Receiver channel values (Norm / Ticks):\r\nStatus: ", RECEIVER_SAMPLING_MAX_STRING_SIZE);
	if (IsReceiverActive())
		strncat(sampleString, "ACTIVE\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	else
		strncat((char*) sampleString, "INACTIVE\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	strncat((char*) sampleString, "Throttle: ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%d", GetThrottleReceiverChannel());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, " / ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%u", GetThrottleReceiverChannelPulseTicks());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, "\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	strncat((char*) sampleString, "Aileron: ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 16, "%d", GetAileronReceiverChannel());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, " / ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%u", GetAileronReceiverChannelPulseTicks());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, "\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	strncat((char*) sampleString, "Elevator: ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 16, "%d", GetElevatorReceiverChannel());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, " / ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%u", GetElevatorReceiverChannelPulseTicks());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, "\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	strncat((char*) sampleString, "Rudder: ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 16, "%d", GetRudderReceiverChannel());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, " / ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%u", GetRudderReceiverChannelPulseTicks());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, "\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	strncat((char*) sampleString, "Gear: ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 16, "%d", GetGearReceiverChannel());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, " / ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%u", GetGearReceiverChannelPulseTicks());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, "\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	strncat((char*) sampleString, "Aux1: ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 16, "%d", GetAux1ReceiverChannel());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, " / ", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	snprintf((char*) sampleValueTmpString, 8, "%u", GetAux1ReceiverChannelPulseTicks());
	strncat((char*) sampleString, sampleValueTmpString, RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);
	strncat((char*) sampleString, "\r\n\r\n", RECEIVER_SAMPLING_MAX_STRING_SIZE - strlen(sampleString) - 1);

	USBComSendString(sampleString, portMAX_DELAY, portMAX_DELAY);
}

/*
 * @brief  Starts the receiver calibration procedure to identify stick/switch max and min values. During
 *         calibration, make sure the receiver and transmitter has connected and that the receiver is
 *         actively reading from the transmitter. Be sure to push the four sticks (throttle, aileron,
 *         elevator and rudder) to their top and bottom positions and toggle the gear and aux1 switches
 *         a few times.
 * @param  None.
 * @retval RECEIVER_OK if calibration could be started, RECEIVER_ERROR if calibration already in progress
 */
ReceiverErrorStatus StartReceiverCalibration(void) {
	/* Check so that calibration is not already being performed */
	if (receiverCalibrationState != RECEIVER_CALIBRATION_IN_PROGRESS) {
		/* Reset the receiver channel's calibration sampling structs */
		ResetCalibrationSampling(&ThrottleCalibrationSampling);
		ResetCalibrationSampling(&ElevatorCalibrationSampling);
		ResetCalibrationSampling(&AileronCalibrationSampling);
		ResetCalibrationSampling(&RudderCalibrationSampling);
		ResetCalibrationSampling(&GearCalibrationSampling);
		ResetCalibrationSampling(&Aux1CalibrationSampling);

		/* Set the calibration start time */
		calibrationStartTime = HAL_GetTick();

		receiverCalibrationState = RECEIVER_CALIBRATION_IN_PROGRESS;

		/* Start printing calibration samples */
		StartReceiverSamplingTask(RECEIVER_CALIBRATION_PRINT_SAMPLE_PERIOD, RECEIVER_MAX_CALIBRATION_DURATION/configTICK_RATE_HZ);

		return RECEIVER_OK;
	}

	/* Calibration busy */
	return RECEIVER_ERROR;
}

/*
 * @brief  If receiver calibration has been performed, calling this function updates the channels' calibration
 *         states and writes calibration values to flash where they are stored for future use. It is necessary
 *         to call this function after calibration in order for the new calibration values to be set and written
 *         to flash.
 * @param  None.
 * @retval RECEIVER_OK if calibration finalized correctly, else RECEIVER_ERROR
 */
ReceiverErrorStatus StopReceiverCalibration(void) {
	ReceiverErrorStatus returnStatus = RECEIVER_OK;

	/* Check so that receiver calibration is currently being performed */
	if (receiverCalibrationState == RECEIVER_CALIBRATION_IN_PROGRESS) {
		/* Check so that each channel has collected enough pulse samples during calibration */
		if (ThrottleCalibrationSampling.channelCalibrationPulseSamples < RECEIVER_CALIBRATION_MIN_PULSE_COUNT)
			returnStatus = RECEIVER_ERROR;
		if (AileronCalibrationSampling.channelCalibrationPulseSamples < RECEIVER_CALIBRATION_MIN_PULSE_COUNT)
			returnStatus = RECEIVER_ERROR;
		if (ElevatorCalibrationSampling.channelCalibrationPulseSamples < RECEIVER_CALIBRATION_MIN_PULSE_COUNT)
			returnStatus = RECEIVER_ERROR;
		if (RudderCalibrationSampling.channelCalibrationPulseSamples < RECEIVER_CALIBRATION_MIN_PULSE_COUNT)
			returnStatus = RECEIVER_ERROR;
		if (GearCalibrationSampling.channelCalibrationPulseSamples < RECEIVER_CALIBRATION_MIN_PULSE_COUNT)
			returnStatus = RECEIVER_ERROR;
		if (Aux1CalibrationSampling.channelCalibrationPulseSamples < RECEIVER_CALIBRATION_MIN_PULSE_COUNT)
			returnStatus = RECEIVER_ERROR;

		/* Calculate mean of max and mean sample buffers and store in temporary calibration values struct */
		Receiver_CalibrationValues_TypeDef tmpCalibrationValues;
		tmpCalibrationValues.ThrottleChannel.ChannelMaxCount = UInt16Mean(
				(uint16_t*) &ThrottleCalibrationSampling.maxSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.ThrottleChannel.ChannelMinCount = UInt16Mean(
				(uint16_t*) &ThrottleCalibrationSampling.minSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.AileronChannel.ChannelMaxCount = UInt16Mean(
				(uint16_t*) &AileronCalibrationSampling.maxSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.AileronChannel.ChannelMinCount = UInt16Mean(
				(uint16_t*) &AileronCalibrationSampling.minSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.ElevatorChannel.ChannelMaxCount = UInt16Mean(
				(uint16_t*) &ElevatorCalibrationSampling.maxSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.ElevatorChannel.ChannelMinCount = UInt16Mean(
				(uint16_t*) &ElevatorCalibrationSampling.minSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.RudderChannel.ChannelMaxCount = UInt16Mean(
				(uint16_t*) &RudderCalibrationSampling.maxSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.RudderChannel.ChannelMinCount = UInt16Mean(
				(uint16_t*) &RudderCalibrationSampling.minSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.GearChannel.ChannelMaxCount = UInt16Mean(
				(uint16_t*) &GearCalibrationSampling.maxSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.GearChannel.ChannelMinCount = UInt16Mean(
				(uint16_t*) &GearCalibrationSampling.minSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.Aux1Channel.ChannelMaxCount = UInt16Mean(
				(uint16_t*) &Aux1CalibrationSampling.maxSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);
		tmpCalibrationValues.Aux1Channel.ChannelMinCount = UInt16Mean(
				(uint16_t*) &Aux1CalibrationSampling.minSamplesBuffer[0], RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE);

		/* Check validity of calibration values */
		if (!IsCalibrationValuesValid(&tmpCalibrationValues))
			returnStatus = RECEIVER_ERROR;

		if (returnStatus != RECEIVER_ERROR) {
			/* Write calibration values to flash for persistent storage */
			if(WriteCalibrationValuesToFlash(&tmpCalibrationValues))
				USBComSendString("Receiver calibration values saved.\n\n", portMAX_DELAY, portMAX_DELAY);
			else
				USBComSendString("Receiver calibration values save failed.\n\n", portMAX_DELAY, portMAX_DELAY);

			/* Copy values to used calibration values and start using them */
			EnforceNewCalibrationValues(&tmpCalibrationValues);
		}

		/* Reset calibration states to waiting so a new calibration may be initiated */
		receiverCalibrationState = RECEIVER_CALIBRATION_WAITING;

		/* Stop printing calibration samples */
		StopReceiverSamplingTask();

		return returnStatus;
	}

	/* If receiver calibration has not been started yet */
	return RECEIVER_ERROR;
}

/*
 * @brief  Checks if the RC transmission between transmitter and receiver is active.
 * @param  None.
 * @retval RECEIVER_OK if transmission is active, else RECEIVER_ERROR.
 */
ReceiverErrorStatus IsReceiverActive(void) {
	ReceiverErrorStatus aileronChannelActive;
	ReceiverErrorStatus elevatorChannelActive;
	ReceiverErrorStatus rudderChannelActive;

	/* When transmission stops, the throttle channel on the Spektrum AR610 receiver keeps sending pulses on its
	 * channel based on its last received throttle command. But the other channels go silents, so they can be
	 * used to check if the transmission is down. */
	aileronChannelActive = IsReceiverChannelActive(&AileronICValues, PrimaryReceiverTimerPeriodCount);
	elevatorChannelActive = IsReceiverChannelActive(&ElevatorICValues, PrimaryReceiverTimerPeriodCount);
	rudderChannelActive = IsReceiverChannelActive(&RudderICValues, PrimaryReceiverTimerPeriodCount);

	return (aileronChannelActive && elevatorChannelActive && rudderChannelActive);
}

/**
 * @brief  Input Capture callback in non blocking mode
 * @param  htim : TIM IC handle
 * @retval None
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
	if (htim->Instance == PRIMARY_RECEIVER_TIM) {
		if (htim->Channel == PRIMARY_RECEIVER_THROTTLE_ACTIVE_CHANNEL)
			UpdateReceiverThrottleChannel();
		else if (htim->Channel == PRIMARY_RECEIVER_AILERON_ACTIVE_CHANNEL)
			UpdateReceiverAileronChannel();
		else if (htim->Channel == PRIMARY_RECEIVER_ELEVATOR_ACTIVE_CHANNEL)
			UpdateReceiverElevatorChannel();
		else if (htim->Channel == PRIMARY_RECEIVER_RUDDER_ACTIVE_CHANNEL)
			UpdateReceiverRudderChannel();
	} else if (htim->Instance == AUX_RECEIVER_TIM) {
		if (htim->Channel == AUX_RECEIVER_GEAR_ACTIVE_CHANNEL)
			UpdateReceiverGearChannel();
		else if (htim->Channel == AUX_RECEIVER_AUX1_ACTIVE_CHANNEL)
			UpdateReceiverAux1Channel();
	}
}

/**
 * @brief  Period elapsed callback in non blocking mode
 * @param  htim : TIM handle
 * @retval None
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
	if (htim->Instance == PRIMARY_RECEIVER_TIM) {
		PrimaryReceiverTimerPeriodCount++;
	} else if (htim->Instance == AUX_RECEIVER_TIM) {
		AuxReceiverTimerPeriodCount++;
	}
}

/**
 * @brief  Gets current throttle calibration max value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetThrottleReceiverCalibrationMaxValue(void) {
	return CalibrationValues.ThrottleChannel.ChannelMaxCount;
}

/**
 * @brief  Gets current throttle calibration min value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetThrottleReceiverCalibrationMinValue(void) {
	return CalibrationValues.ThrottleChannel.ChannelMinCount;
}

/**
 * @brief  Gets current aileron calibration max value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetAileronReceiverCalibrationMaxValue(void) {
	return CalibrationValues.AileronChannel.ChannelMaxCount;
}

/**
 * @brief  Gets current aileron calibration min value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetAileronReceiverCalibrationMinValue(void) {
	return CalibrationValues.AileronChannel.ChannelMinCount;
}

/**
 * @brief  Gets current elevator calibration max value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetElevatorReceiverCalibrationMaxValue(void) {
	return CalibrationValues.ElevatorChannel.ChannelMaxCount;
}

/**
 * @brief  Gets current elevator calibration min value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetElevatorReceiverCalibrationMinValue(void) {
	return CalibrationValues.ElevatorChannel.ChannelMinCount;
}

/**
 * @brief  Gets current rudder calibration max value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetRudderReceiverCalibrationMaxValue(void) {
	return CalibrationValues.RudderChannel.ChannelMaxCount;
}

/**
 * @brief  Gets current rudder calibration min value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetRudderReceiverCalibrationMinValue(void) {
	return CalibrationValues.RudderChannel.ChannelMinCount;
}

/**
 * @brief  Gets current gear calibration max value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetGearReceiverCalibrationMaxValue(void) {
	return CalibrationValues.GearChannel.ChannelMaxCount;
}

/**
 * @brief  Gets current gear calibration min value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetGearReceiverCalibrationMinValue(void) {
	return CalibrationValues.GearChannel.ChannelMinCount;
}

/**
 * @brief  Gets current aux1 calibration max value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetAux1ReceiverCalibrationMaxValue(void) {
	return CalibrationValues.Aux1Channel.ChannelMaxCount;
}

/**
 * @brief  Gets current aux1 calibration min value
 * @param  None
 * @retval Calibrated value
 */
uint16_t GetAux1ReceiverCalibrationMinValue(void) {
	return CalibrationValues.Aux1Channel.ChannelMinCount;
}

/* Private functions ---------------------------------------------------------*/

/*
 * @brief  Initializes receiver input calibration values (max and min timer IC counts)
 * @param  None
 * @retval RECEIVER_OK if calibration values loaded, RECEIVER_ERROR if default values used
 */
static ReceiverErrorStatus InitReceiverCalibrationValues(void) {
	if (!LoadReceiverCalibrationValuesFromFlash(&CalibrationValues)) {
		SetDefaultReceiverCalibrationValues(&CalibrationValues);
		// TODO This hangs the system as it is sent before USB configured?  USBComSendString("No valid receiver calibration values could be loaded. Setting to default.\n\n", portMAX_DELAY,
		//		portMAX_DELAY);
		return RECEIVER_ERROR;
	}

	// TODO This hangs the system as it is sent before USB configured? USBComSendString("Receiver calibration values loaded succesfully.\n\n", portMAX_DELAY, portMAX_DELAY);
	return RECEIVER_OK;
}

/*
 * @brief  Sets receiver calibration values to default
 * @param  calibrationValues : pointer to a calibration values struct
 * @retval None
 */
static void SetDefaultReceiverCalibrationValues(volatile Receiver_CalibrationValues_TypeDef* calibrationValues) {
	calibrationValues->ThrottleChannel.ChannelMaxCount = RECEIVER_PULSE_DEFAULT_MAX_COUNT;
	calibrationValues->ThrottleChannel.ChannelMinCount = RECEIVER_PULSE_DEFAULT_MIN_COUNT;

	calibrationValues->AileronChannel.ChannelMaxCount = RECEIVER_PULSE_DEFAULT_MAX_COUNT;
	calibrationValues->AileronChannel.ChannelMinCount = RECEIVER_PULSE_DEFAULT_MIN_COUNT;

	calibrationValues->ElevatorChannel.ChannelMaxCount = RECEIVER_PULSE_DEFAULT_MAX_COUNT;
	calibrationValues->ElevatorChannel.ChannelMinCount = RECEIVER_PULSE_DEFAULT_MIN_COUNT;

	calibrationValues->RudderChannel.ChannelMaxCount = RECEIVER_PULSE_DEFAULT_MAX_COUNT;
	calibrationValues->RudderChannel.ChannelMinCount = RECEIVER_PULSE_DEFAULT_MIN_COUNT;

	calibrationValues->GearChannel.ChannelMaxCount = RECEIVER_PULSE_DEFAULT_MAX_COUNT;
	calibrationValues->GearChannel.ChannelMinCount = RECEIVER_PULSE_DEFAULT_MIN_COUNT;

	calibrationValues->Aux1Channel.ChannelMaxCount = RECEIVER_PULSE_DEFAULT_MAX_COUNT;
	calibrationValues->Aux1Channel.ChannelMinCount = RECEIVER_PULSE_DEFAULT_MIN_COUNT;
}

/*
 * @brief  Returns a normalized receiver channel value as a signed integer.
 * @param  ChannelICValues : Reference to channel's IC values struct
 * @param  ChannelCalibrationValues : Reference to channel's calibration values struct
 * @retval channel value [-32768, 32767]
 */
static int16_t GetSignedReceiverChannel(volatile const Receiver_IC_Values_TypeDef* ChannelICValues,
		volatile const Receiver_IC_ChannelCalibrationValues_TypeDef* ChannelCalibrationValues) {
	if (ChannelICValues->PulseTimerCount < ChannelCalibrationValues->ChannelMinCount)
		return INT16_MIN;
	else if (ChannelICValues->PulseTimerCount > ChannelCalibrationValues->ChannelMaxCount)
		return INT16_MAX;
	else if (ChannelCalibrationValues->ChannelMaxCount > ChannelCalibrationValues->ChannelMinCount)
		return INT16_MIN
				+ (((uint32_t) (ChannelICValues->PulseTimerCount - ChannelCalibrationValues->ChannelMinCount)
						* UINT16_MAX)
						/ (ChannelCalibrationValues->ChannelMaxCount - ChannelCalibrationValues->ChannelMinCount));
	else
		return 0;
}

/*
 * @brief  Returns a normalized receiver channel value as an unsigned integer.
 * @param  ChannelICValues : Reference to channel's IC values struct
 * @param  ChannelCalibrationValues : Reference to channel's calibration values struct
 * @retval channel value [0, 65535]
 */
//static uint16_t GetUnsignedReceiverChannel(volatile const Receiver_IC_Values_TypeDef* ChannelICValues,
//		volatile const Receiver_IC_ChannelCalibrationValues_TypeDef* ChannelCalibrationValues) {
//	if (ChannelICValues->PulseTimerCount < ChannelCalibrationValues->ChannelMinCount)
//		return 0;
//	else if (ChannelICValues->PulseTimerCount > ChannelCalibrationValues->ChannelMaxCount)
//		return UINT16_MAX;
//	else if (ChannelCalibrationValues->ChannelMaxCount > ChannelCalibrationValues->ChannelMinCount)
//		return ((uint32_t) (ChannelICValues->PulseTimerCount - ChannelCalibrationValues->ChannelMinCount) * UINT16_MAX)
//				/ (ChannelCalibrationValues->ChannelMaxCount - ChannelCalibrationValues->ChannelMinCount);
//	else
//		return 0;
//}

/*
 * @brief  Returns the receiver channel pulse count in microseconds
 * @param  ChannelICValues : Reference to a channel's IC values struct
 * @retval Channel pulse timer count in microseconds
 */
static uint16_t GetReceiverChannelPulseMicros(volatile const Receiver_IC_Values_TypeDef* ChannelICValues) {
	return (uint16_t) (((uint32_t) ChannelICValues->PulseTimerCount * 1000000) / RECEIVER_TIM_COUNTER_CLOCK);
}

/*
 * @brief  Returns the receiver channel period count in microseconds
 * @param  ChannelICValues : Reference to a channel's IC values struct
 * @retval Channel period timer count in microseconds
 */
static uint16_t GetReceiverChannelPeriodMicros(volatile const Receiver_IC_Values_TypeDef* ChannelICValues) {
	/* The period count will never be more than RECEIVER_MAX_VALID_PERIOD_COUNT, take care so that 32-bit overflow does not occur below */
	return (uint16_t) (((uint32_t) ChannelICValues->PeriodCount * 100000) / RECEIVER_TIM_COUNTER_CLOCK * 10);
}

/*
 * @brief  Gets calibration values stored in flash after previously performed receiver calibration
 * @param  calibrationValues : pointer to calibration values struct
 * @retval true if valid calibration values has been loaded, else false.
 */
static ReceiverErrorStatus LoadReceiverCalibrationValuesFromFlash(
		volatile Receiver_CalibrationValues_TypeDef* calibrationValues) {
	/* Load previously stored values into the calibrationValues struct */
	if (!ReadCalibrationValuesFromFlash(calibrationValues))
		return RECEIVER_ERROR;

	/* Check so that loaded values are within valid ranges */
	if (!IsCalibrationValuesValid(calibrationValues))
		return RECEIVER_ERROR;

	return RECEIVER_OK;
}

/*
 * @brief  Initializes reading from the receiver primary input channels, i.e. throttle aileron,
 *     elevator and rudder channels. The signals are encoded as pulses of ~1-2 ms.
 * @param  None.
 * @retval RECEIVER_OK if configured without errors, else RECEIVER_ERROR
 */
static ReceiverErrorStatus PrimaryReceiverInputConfig(void) {
	ReceiverErrorStatus errorStatus = RECEIVER_OK;

	/*##-1- Configure the Primary Receiver TIM peripheral ######################*/
	/* Set TIM instance */
	PrimaryReceiverTimHandle.Instance = PRIMARY_RECEIVER_TIM;

	/* Initialize TIM peripheral to maximum period with suitable counter clocking (receiver input period is ~22 ms) */
	PrimaryReceiverTimHandle.Init.Period = RECEIVER_COUNTER_PERIOD;
	PrimaryReceiverTimHandle.Init.Prescaler = SystemCoreClock / RECEIVER_TIM_COUNTER_CLOCK - 1;
	PrimaryReceiverTimHandle.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	PrimaryReceiverTimHandle.Init.CounterMode = TIM_COUNTERMODE_UP;
	if (HAL_TIM_Base_Init(&PrimaryReceiverTimHandle) != HAL_OK) {
		/* Initialization Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}
	PrimaryReceiverTimHandle.State = HAL_TIM_STATE_RESET;
	if (HAL_TIM_IC_Init(&PrimaryReceiverTimHandle) != HAL_OK) {
		/* Initialization Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	/*##-2- Configure the Input Capture channels ###############################*/
	/* Common configuration */
	ThrottleChannelICConfig.ICPrescaler = TIM_ICPSC_DIV1;
	ThrottleChannelICConfig.ICFilter = 0;
	ThrottleChannelICConfig.ICPolarity = TIM_ICPOLARITY_RISING;
	ThrottleChannelICConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
	/* Configure the Input Capture of throttle channel */
	if (HAL_TIM_IC_ConfigChannel(&PrimaryReceiverTimHandle, &ThrottleChannelICConfig, PRIMARY_RECEIVER_THROTTLE_CHANNEL)
			!= HAL_OK) {
		/* Configuration Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	AileronChannelICConfig.ICPrescaler = TIM_ICPSC_DIV1;
	AileronChannelICConfig.ICFilter = 0;
	AileronChannelICConfig.ICPolarity = TIM_ICPOLARITY_RISING;
	AileronChannelICConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
	/* Configure the Input Capture of aileron channel */
	if (HAL_TIM_IC_ConfigChannel(&PrimaryReceiverTimHandle, &AileronChannelICConfig, PRIMARY_RECEIVER_AILERON_CHANNEL)
			!= HAL_OK) {
		/* Configuration Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	ElevatorChannelICConfig.ICPrescaler = TIM_ICPSC_DIV1;
	ElevatorChannelICConfig.ICFilter = 0;
	ElevatorChannelICConfig.ICPolarity = TIM_ICPOLARITY_RISING;
	ElevatorChannelICConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
	/* Configure the Input Capture of elevator channel */
	if (HAL_TIM_IC_ConfigChannel(&PrimaryReceiverTimHandle, &ElevatorChannelICConfig, PRIMARY_RECEIVER_ELEVATOR_CHANNEL)
			!= HAL_OK) {
		/* Configuration Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	RudderChannelICConfig.ICPrescaler = TIM_ICPSC_DIV1;
	RudderChannelICConfig.ICFilter = 0;
	RudderChannelICConfig.ICPolarity = TIM_ICPOLARITY_RISING;
	RudderChannelICConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
	/* Configure the Input Capture of rudder channel */
	if (HAL_TIM_IC_ConfigChannel(&PrimaryReceiverTimHandle, &RudderChannelICConfig, PRIMARY_RECEIVER_RUDDER_CHANNEL)
			!= HAL_OK) {
		/* Configuration Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	/*##-3- Start the Input Capture in interrupt mode ##########################*/
	if (HAL_TIM_IC_Start_IT(&PrimaryReceiverTimHandle, PRIMARY_RECEIVER_THROTTLE_CHANNEL) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	if (HAL_TIM_IC_Start_IT(&PrimaryReceiverTimHandle, PRIMARY_RECEIVER_AILERON_CHANNEL) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	if (HAL_TIM_IC_Start_IT(&PrimaryReceiverTimHandle, PRIMARY_RECEIVER_ELEVATOR_CHANNEL) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	if (HAL_TIM_IC_Start_IT(&PrimaryReceiverTimHandle, PRIMARY_RECEIVER_RUDDER_CHANNEL) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	/*##-4- Start the Time Base update interrupt mode ##########################*/
	if (HAL_TIM_Base_Start_IT(&PrimaryReceiverTimHandle) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}
	return errorStatus;
}

/*
 * @brief    Initializes reading from the receiver aux input channels, i.e.
 *       Gear and Aux1. The signals are encoded as pulses of ~1-2 ms.
 * @param    None.
 * @retval   RECEIVER_OK if configured without errors, else RECEIVER_ERROR
 */
static ReceiverErrorStatus AuxReceiverInput_Config(void) {
	ReceiverErrorStatus errorStatus = RECEIVER_OK;

	/*##-1- Configure the Aux Receiver TIM peripheral ##########################*/
	/* Set TIM instance */
	AuxReceiverTimHandle.Instance = AUX_RECEIVER_TIM;

	/* Initialize TIM peripheral to maximum period with suitable counter clocking (receiver input period is ~22 ms) */
	AuxReceiverTimHandle.Init.Period = RECEIVER_COUNTER_PERIOD;
	AuxReceiverTimHandle.Init.Prescaler = SystemCoreClock / RECEIVER_TIM_COUNTER_CLOCK - 1;
	AuxReceiverTimHandle.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	AuxReceiverTimHandle.Init.CounterMode = TIM_COUNTERMODE_UP;
	if (HAL_TIM_Base_Init(&AuxReceiverTimHandle) != HAL_OK) {
		/* Initialization Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}
	AuxReceiverTimHandle.State = HAL_TIM_STATE_RESET;
	if (HAL_TIM_IC_Init(&AuxReceiverTimHandle) != HAL_OK) {
		/* Initialization Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	/*##-2- Configure the Input Capture channels ###############################*/
	/* Common configuration */
	GearChannelICConfig.ICPrescaler = TIM_ICPSC_DIV1;
	GearChannelICConfig.ICFilter = 0;
	GearChannelICConfig.ICPolarity = TIM_ICPOLARITY_RISING;
	GearChannelICConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
	/* Configure the Input Capture of gear channel */
	if (HAL_TIM_IC_ConfigChannel(&AuxReceiverTimHandle, &GearChannelICConfig, AUX_RECEIVER_GEAR_CHANNEL) != HAL_OK) {
		/* Configuration Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	Aux1ChannelICConfig.ICPrescaler = TIM_ICPSC_DIV1;
	Aux1ChannelICConfig.ICFilter = 0;
	Aux1ChannelICConfig.ICPolarity = TIM_ICPOLARITY_RISING;
	Aux1ChannelICConfig.ICSelection = TIM_ICSELECTION_DIRECTTI;
	/* Configure the Input Capture of aux1 channel */
	if (HAL_TIM_IC_ConfigChannel(&AuxReceiverTimHandle, &Aux1ChannelICConfig, AUX_RECEIVER_AUX1_CHANNEL) != HAL_OK) {
		/* Configuration Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	/*##-3- Start the Input Capture in interrupt mode ##########################*/
	if (HAL_TIM_IC_Start_IT(&AuxReceiverTimHandle, AUX_RECEIVER_GEAR_CHANNEL) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	if (HAL_TIM_IC_Start_IT(&AuxReceiverTimHandle, AUX_RECEIVER_AUX1_CHANNEL) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	/*##-4- Start the Time Base update interrupt mode ##########################*/
	if (HAL_TIM_Base_Start_IT(&AuxReceiverTimHandle) != HAL_OK) {
		/* Starting Error */
		errorStatus = RECEIVER_ERROR;
		ErrorHandler();
	}

	return errorStatus;
}

/*
 * @brief  Updates a receiver channel IC counts. The channel is specified by the function parameters.
 * @param  TimHandle : Reference to the TIM_HandleTypeDef struct used to read the channel's IC count
 * @param  TimIC : Reference to the TIM_IC_InitTypeDef struct used to configure the IC to count on rising/falling pulse flank
 * @param  channelInputState : The current channel pulse input state (PULSE_LOW or PULSE_HIGH)
 * @param  ChannelICValues : Reference to the channels IC value struct
 * @param  receiverChannel : TIM Channel
 * @param  ReceiverTimerPeriodCount : Reference to the timer period count variable (primary or aux)
 * @param  ChannelCalibrationSampling : Reference to channel's calibration sampling struct
 * @retval RECEIVER_OK if valid pulse or period detected, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverChannel(TIM_HandleTypeDef* TimHandle, TIM_IC_InitTypeDef* TimIC,
		Pulse_State* channelInputState, volatile Receiver_IC_Values_TypeDef* ChannelICValues,
		const uint32_t receiverChannel, volatile const uint16_t ReceiverTimerPeriodCount,
		volatile Receiver_ChannelCalibrationSampling_TypeDef* ChannelCalibrationSampling) {
	ReceiverErrorStatus errorStatus = RECEIVER_OK;

	/* Detected rising pulse edge */
	if ((*channelInputState) == PULSE_LOW) {
		uint32_t tempPeriodTimerCount;

		/* Get the Input Capture value */
		uint32_t icValue = HAL_TIM_ReadCapturedValue(TimHandle, receiverChannel);
		(*channelInputState) = PULSE_HIGH; // Set input state to high

		TimIC->ICPolarity = TIM_ICPOLARITY_FALLING; // Set IC polarity property to falling

		/* Set the rising timer IC counts */
		ChannelICValues->PreviousRisingCount = ChannelICValues->RisingCount;
		ChannelICValues->RisingCount = icValue;

		/* Calculate the period between pulses by computing the difference between current and previous rising edge timer counts.
		 * Since the counter typically resets more often than a new pulse is triggered for the receiver, one must also use the
		 * number of timer period resets since the last pulse. */
		if (ReceiverTimerPeriodCount > ChannelICValues->PreviousRisingCountTimerPeriodCount)
			tempPeriodTimerCount = ChannelICValues->RisingCount + UINT16_MAX - ChannelICValues->PreviousRisingCount
					+ UINT16_MAX
							* (ReceiverTimerPeriodCount - ChannelICValues->PreviousRisingCountTimerPeriodCount - 1);
		else
			tempPeriodTimerCount = ChannelICValues->RisingCount - ChannelICValues->PreviousRisingCount; // Short period value, likely incorrect

		if (IsReceiverPeriodValid(tempPeriodTimerCount))
			ChannelICValues->PeriodCount = tempPeriodTimerCount;
		else
			errorStatus = RECEIVER_ERROR;

		ChannelICValues->PreviousRisingCountTimerPeriodCount = ReceiverTimerPeriodCount;
	}
	/* Detected falling pulse edge */
	else if ((*channelInputState) == PULSE_HIGH) {
		uint16_t tempPulseTimerCount;

		/* Get the Input Capture value */
		uint32_t icValue = HAL_TIM_ReadCapturedValue(TimHandle, receiverChannel);
		(*channelInputState) = PULSE_LOW; // Set input state to low

		TimIC->ICPolarity = TIM_ICPOLARITY_RISING; // Set IC polarity property to rising

		/* Set the falling timer IC count */
		ChannelICValues->FallingCounter = icValue;

		/* Calculate the pulse of the 16-bit counter by computing the difference between falling and rising edges timer counts */
		tempPulseTimerCount = ChannelICValues->FallingCounter - ChannelICValues->RisingCount;

		/* Sanity check of pulse count before updating it */
		if (IsReceiverPulseValid(tempPulseTimerCount, ReceiverTimerPeriodCount,
				ChannelICValues->PreviousRisingCountTimerPeriodCount)) {
			ChannelICValues->PulseTimerCount = tempPulseTimerCount;
			ChannelICValues->IsActive = RECEIVER_OK; // Set channel to active

			/* Check if calibration is being performed */
			if (receiverCalibrationState == RECEIVER_CALIBRATION_IN_PROGRESS) {
				/* Check if max calibration time has been reached (time out) */
				if (HAL_GetTick() > RECEIVER_MAX_CALIBRATION_DURATION + calibrationStartTime)
					receiverCalibrationState = RECEIVER_CALIBRATION_WAITING;
				else
					UpdateChannelCalibrationSamples(ChannelCalibrationSampling, tempPulseTimerCount);
			}
		} else
			errorStatus = RECEIVER_ERROR;
	}

	/* Toggle the IC Polarity */
	ReceiverToggleICPolarity(TimHandle, TimIC, receiverChannel);

	return errorStatus;
}

/*
 * @brief  Updates the receiver channel's calibration samples
 * @param  channelCalibrationSampling : calibration sampling struct
 * @param  channelPulseTimerCount : The pulse count value
 * @retval RECEIVER_OK if updated correctly
 */
static ReceiverErrorStatus UpdateChannelCalibrationSamples(
		volatile Receiver_ChannelCalibrationSampling_TypeDef* channelCalibrationSampling,
		const uint16_t channelPulseTimerCount) {
	uint16_t i;

	/* # Find the min value in the buffer containing the max values ########### */
	if (channelCalibrationSampling->maxBufferUpdated) {
		for (i = 0; i < RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE; i++) {
			if (channelCalibrationSampling->tmpMaxBufferMinValue > channelCalibrationSampling->maxSamplesBuffer[i]) {
				channelCalibrationSampling->tmpMaxBufferMinValue = channelCalibrationSampling->maxSamplesBuffer[i];
				channelCalibrationSampling->tmpMaxIndex = i;
				channelCalibrationSampling->maxBufferUpdated = false;
			}
		}
	}

	/* If the pulse value is larger than the min value of the max value buffer, replace it */
	if (channelPulseTimerCount > channelCalibrationSampling->tmpMaxBufferMinValue) {
		channelCalibrationSampling->tmpMaxBufferMinValue = 0xFFFF;
		channelCalibrationSampling->maxSamplesBuffer[channelCalibrationSampling->tmpMaxIndex] = channelPulseTimerCount;
		channelCalibrationSampling->maxBufferUpdated = true;
	}

	/* # Find the max value in the buffer containing the min values ########### */
	if (channelCalibrationSampling->minBufferUpdated) {
		for (i = 0; i < RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE; i++) {
			if (channelCalibrationSampling->tmpMinBufferMaxValue < channelCalibrationSampling->minSamplesBuffer[i]) {
				channelCalibrationSampling->tmpMinBufferMaxValue = channelCalibrationSampling->minSamplesBuffer[i];
				channelCalibrationSampling->tmpMinIndex = i;
				channelCalibrationSampling->minBufferUpdated = false;
			}
		}
	}

	/* If the pulse value is larger than the min value of the max value buffer, replace it */
	if (channelPulseTimerCount < channelCalibrationSampling->tmpMinBufferMaxValue) {
		channelCalibrationSampling->tmpMinBufferMaxValue = 0;
		channelCalibrationSampling->minSamplesBuffer[channelCalibrationSampling->tmpMinIndex] = channelPulseTimerCount;
		channelCalibrationSampling->minBufferUpdated = true;
	}

	/* Increase amount of channel calibration samples */
	channelCalibrationSampling->channelCalibrationPulseSamples++;

	return RECEIVER_OK;
}

/*
 * @brief  Handles the receiver input pulse measurements from the throttle channel and updates pulse and frequency values.
 * @param  None.
 * @retval RECEIVER_OK if updated correctly, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverThrottleChannel(void) {
	return UpdateReceiverChannel(&PrimaryReceiverTimHandle, &ThrottleChannelICConfig,
			&ReceiverPulseStates.ThrottleInputState, &ThrottleICValues, PRIMARY_RECEIVER_THROTTLE_CHANNEL,
			PrimaryReceiverTimerPeriodCount, &ThrottleCalibrationSampling);
}

/*
 * @brief  Handles the receiver input pulse measurements from the aileron channel and updates pulse and frequency values.
 * @param  None.
 * @retval RECEIVER_OK if updated correctly, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverAileronChannel(void) {
	return UpdateReceiverChannel(&PrimaryReceiverTimHandle, &AileronChannelICConfig,
			&ReceiverPulseStates.AileronInputState, &AileronICValues, PRIMARY_RECEIVER_AILERON_CHANNEL,
			PrimaryReceiverTimerPeriodCount, &AileronCalibrationSampling);
}

/*
 * @brief  Handles the receiver input pulse measurements from the elevator channel and updates pulse and frequency values.
 * @param  None.
 * @retval RECEIVER_OK if updated correctly, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverElevatorChannel(void) {
	return UpdateReceiverChannel(&PrimaryReceiverTimHandle, &ElevatorChannelICConfig,
			&ReceiverPulseStates.ElevatorInputState, &ElevatorICValues, PRIMARY_RECEIVER_ELEVATOR_CHANNEL,
			PrimaryReceiverTimerPeriodCount, &ElevatorCalibrationSampling);
}

/*
 * @brief  Handles the receiver input pulse measurements from the rudder channel and updates pulse and frequency values.
 * @param  None.
 * @retval RECEIVER_OK if updated correctly, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverRudderChannel(void) {
	return UpdateReceiverChannel(&PrimaryReceiverTimHandle, &RudderChannelICConfig,
			&ReceiverPulseStates.RudderInputState, &RudderICValues, PRIMARY_RECEIVER_RUDDER_CHANNEL,
			PrimaryReceiverTimerPeriodCount, &RudderCalibrationSampling);
}

/*
 * @brief  Handles the receiver input pulse measurements from the gear channel and updates pulse and frequency values.
 * @param  None.
 * @retval RECEIVER_OK if updated correctly, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverGearChannel(void) {
	return UpdateReceiverChannel(&AuxReceiverTimHandle, &GearChannelICConfig, &ReceiverPulseStates.GearInputState,
			&GearICValues, AUX_RECEIVER_GEAR_CHANNEL, AuxReceiverTimerPeriodCount, &GearCalibrationSampling);
}

/*
 * @brief  Handles the receiver input pulse measurements from the aux1 channel and updates pulse and frequency values.
 * @param  None.
 * @retval RECEIVER_OK if updated correctly, else RECEIVER_ERROR
 */
static ReceiverErrorStatus UpdateReceiverAux1Channel(void) {
	return UpdateReceiverChannel(&AuxReceiverTimHandle, &Aux1ChannelICConfig, &ReceiverPulseStates.Aux1InputState,
			&Aux1ICValues, AUX_RECEIVER_AUX1_CHANNEL, AuxReceiverTimerPeriodCount, &Aux1CalibrationSampling);
}

/*
 * @brief  Checks if the RC transmission between transmitter and receiver is active for a specified channel.
 * @param  ChannelICValues : Reference to a channel's IC values struct
 * @param  ReceiverTimerPeriodCount : The period count value for the receiver counter
 * @retval RECEIVER_OK if transmission is active, else RECEIVER_ERROR.
 */
static ReceiverErrorStatus IsReceiverChannelActive(volatile Receiver_IC_Values_TypeDef* ChannelICValues,
		const uint16_t ReceiverTimerPeriodCount) {
	uint32_t periodsSinceLastChannelPulse;

	/* Check how many timer resets have been performed since the last rising pulse edge */
	periodsSinceLastChannelPulse = ReceiverTimerPeriodCount - ChannelICValues->PreviousRisingCountTimerPeriodCount;

	/* Set channel as inactive if too many periods have passed since last channel pulse update */
	if (periodsSinceLastChannelPulse > IS_RECEIVER_CHANNEL_INACTIVE_PERIODS_COUNT)
		ChannelICValues->IsActive = RECEIVER_ERROR;

	return ChannelICValues->IsActive;
}

/*
 * @brief  Checks if a receiver channel pulse count is within a valid range
 * @param  pulseTimerCount : pulse timer count value
 * @param  currentPeriodCount : The current period count
 * @param  previousPeriodCount : The previous period count
 * @retval RECEIVER_OK if receiver pulse count is valid, else RECEIVER_ERROR.
 */
static ReceiverErrorStatus IsReceiverPulseValid(const uint16_t pulseTimerCount, const uint16_t currentPeriodCount,
		const uint16_t previousPeriodCount) {
	/* Pulse count considered valid if it is within defined bounds and doesn't strech over more than 2 timer periods */
	return (pulseTimerCount <= RECEIVER_MAX_VALID_IC_PULSE_COUNT && pulseTimerCount >= RECEIVER_MIN_VALID_IC_PULSE_COUNT
			&& currentPeriodCount - previousPeriodCount <= 1);
}

/*
 * @brief  Checks if a receiver channel period count is within a valid range
 * @param  periodTimerCount : period timer count value
 * @retval RECEIVER_OK if receiver period count is valid, else RECEIVER_ERROR.
 */
static ReceiverErrorStatus IsReceiverPeriodValid(const uint32_t periodTimerCount) {
	/* Period count considered valid if it is within defined bounds */
	return (periodTimerCount <= RECEIVER_MAX_VALID_PERIOD_COUNT && periodTimerCount >= RECEIVER_MIN_VALID_PERIOD_COUNT);
}

/*
 * @brief  Checks if receiver calibration values are valid
 * @param  calibrationValues : pointer to calibration values struct
 * @retval RECEIVER_OK if receiver calibration values are valid, else RECEIVER_ERROR.
 */
static ReceiverErrorStatus IsCalibrationValuesValid(
		volatile const Receiver_CalibrationValues_TypeDef* calibrationValues) {
	/* Check throttle channel calibration */
	if (!IsCalibrationMaxPulseValueValid(calibrationValues->ThrottleChannel.ChannelMaxCount))
		return RECEIVER_ERROR;
	if (!IsCalibrationMinPulseValueValid(calibrationValues->ThrottleChannel.ChannelMinCount))
		return RECEIVER_ERROR;

	/* Check aileron channel calibration */
	if (!IsCalibrationMaxPulseValueValid(calibrationValues->AileronChannel.ChannelMaxCount))
		return RECEIVER_ERROR;
	if (!IsCalibrationMinPulseValueValid(calibrationValues->AileronChannel.ChannelMinCount))
		return RECEIVER_ERROR;

	/* Check elevator channel calibration */
	if (!IsCalibrationMaxPulseValueValid(calibrationValues->ElevatorChannel.ChannelMaxCount))
		return RECEIVER_ERROR;
	if (!IsCalibrationMinPulseValueValid(calibrationValues->ElevatorChannel.ChannelMinCount))
		return RECEIVER_ERROR;

	/* Check rudder channel calibration */
	if (!IsCalibrationMaxPulseValueValid(calibrationValues->RudderChannel.ChannelMaxCount))
		return RECEIVER_ERROR;
	if (!IsCalibrationMinPulseValueValid(calibrationValues->RudderChannel.ChannelMinCount))
		return RECEIVER_ERROR;

	/* Check gear channel calibration */
	if (!IsCalibrationMaxPulseValueValid(calibrationValues->GearChannel.ChannelMaxCount))
		return RECEIVER_ERROR;
	if (!IsCalibrationMinPulseValueValid(calibrationValues->GearChannel.ChannelMinCount))
		return RECEIVER_ERROR;

	/* Check aux1 channel calibration */
	if (!IsCalibrationMaxPulseValueValid(calibrationValues->Aux1Channel.ChannelMaxCount))
		return RECEIVER_ERROR;
	if (!IsCalibrationMinPulseValueValid(calibrationValues->Aux1Channel.ChannelMinCount))
		return RECEIVER_ERROR;

	return RECEIVER_OK;
}

/*
 * @brief  Checks if receiver pulse value is a valid max calibration value
 * @param  maxPulseValue : pulse value
 * @retval RECEIVER_OK if receiver pulse value valid for max calibration, else RECEIVER_ERROR
 */
static ReceiverErrorStatus IsCalibrationMaxPulseValueValid(const uint16_t maxPulseValue) {
	if (maxPulseValue <= RECEIVER_MAX_CALIBRATION_MAX_PULSE_COUNT
			&& maxPulseValue >= RECEIVER_MAX_CALIBRATION_MIN_PULSE_COUNT)
		return RECEIVER_OK;

	return RECEIVER_ERROR;
}

/*
 * @brief  Checks if receiver pulse value is a valid min calibration value
 * @param  minPulseValue : pulse value
 * @retval RECEIVER_OK if receiver pulse value valid for min calibration, else RECEIVER_ERROR
 */
static ReceiverErrorStatus IsCalibrationMinPulseValueValid(const uint16_t minPulseValue) {
	if (minPulseValue <= RECEIVER_MIN_CALIBRATION_MAX_PULSE_COUNT
			&& minPulseValue >= RECEIVER_MIN_CALIBRATION_MIN_PULSE_COUNT)
		return RECEIVER_OK;

	return RECEIVER_ERROR;
}

/*
 * @brief  Updates the currently used calibration values
 * @param  newCalibrationValues : reference to new calibration values
 * @retval None
 */
static void EnforceNewCalibrationValues(volatile Receiver_CalibrationValues_TypeDef* newCalibrationValues) {
	CalibrationValues.ThrottleChannel.ChannelMaxCount = newCalibrationValues->ThrottleChannel.ChannelMaxCount;
	CalibrationValues.ThrottleChannel.ChannelMinCount = newCalibrationValues->ThrottleChannel.ChannelMinCount;
	CalibrationValues.AileronChannel.ChannelMaxCount = newCalibrationValues->AileronChannel.ChannelMaxCount;
	CalibrationValues.AileronChannel.ChannelMinCount = newCalibrationValues->AileronChannel.ChannelMinCount;
	CalibrationValues.ElevatorChannel.ChannelMaxCount = newCalibrationValues->ElevatorChannel.ChannelMaxCount;
	CalibrationValues.ElevatorChannel.ChannelMinCount = newCalibrationValues->ElevatorChannel.ChannelMinCount;
	CalibrationValues.RudderChannel.ChannelMaxCount = newCalibrationValues->RudderChannel.ChannelMaxCount;
	CalibrationValues.RudderChannel.ChannelMinCount = newCalibrationValues->RudderChannel.ChannelMinCount;
	CalibrationValues.GearChannel.ChannelMaxCount = newCalibrationValues->GearChannel.ChannelMaxCount;
	CalibrationValues.GearChannel.ChannelMinCount = newCalibrationValues->GearChannel.ChannelMinCount;
	CalibrationValues.Aux1Channel.ChannelMaxCount = newCalibrationValues->Aux1Channel.ChannelMaxCount;
	CalibrationValues.Aux1Channel.ChannelMinCount = newCalibrationValues->Aux1Channel.ChannelMinCount;
}

/*
 * @brief  Resets a Receiver_ChannelCalibrationSampling_TypeDef struct
 * @param  channelCalibrationSampling : Receiver_ChannelCalibrationSampling_TypeDef struct to be reset
 * @retval None
 */
static void ResetCalibrationSampling(volatile Receiver_ChannelCalibrationSampling_TypeDef* channelCalibrationSampling) {
	channelCalibrationSampling->channelCalibrationPulseSamples = 0;

	channelCalibrationSampling->maxBufferUpdated = false;
	for(uint16_t i = 0; i < RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE; i++)
		channelCalibrationSampling->maxSamplesBuffer[i] = RECEIVER_CALIBRATION_BUFFER_INIT_VALUE;
	channelCalibrationSampling->tmpMaxBufferMinValue = RECEIVER_CALIBRATION_BUFFER_INIT_VALUE;
	channelCalibrationSampling->tmpMaxIndex = 0;

	channelCalibrationSampling->minBufferUpdated = false;
	for(uint16_t i = 0; i < RECEIVER_CALIBRATION_SAMPLES_BUFFER_SIZE; i++)
			channelCalibrationSampling->minSamplesBuffer[i] = RECEIVER_CALIBRATION_BUFFER_INIT_VALUE;
	channelCalibrationSampling->tmpMinBufferMaxValue = RECEIVER_CALIBRATION_BUFFER_INIT_VALUE;
	channelCalibrationSampling->tmpMinIndex = 0;
}

/**
 * @brief  Toggles the IC polarity
 * @param  htim : timer handle reference
 * @param  sConfig : timer IC init struct reference
 * @param  Channel : timer channel
 * @retval None
 */
static void ReceiverToggleICPolarity(TIM_HandleTypeDef* htim, TIM_IC_InitTypeDef* sConfig, uint32_t Channel) {
	uint32_t tmpccmrx = 0;
	uint32_t tmpccer = 0;

	htim->State = HAL_TIM_STATE_BUSY;

	if (Channel == TIM_CHANNEL_1)
	{
		/* Disable the Channel 1: Reset the CC1E Bit */
		htim->Instance->CCER &= ~TIM_CCER_CC1E;
		tmpccmrx = htim->Instance->CCMR1;
		tmpccer = htim->Instance->CCER;

		/* Select the Polarity and set the CC1E Bit */
		tmpccer &= ~(TIM_CCER_CC1P | TIM_CCER_CC1NP);
		tmpccer |= (sConfig->ICPolarity & (TIM_CCER_CC1P | TIM_CCER_CC1NP));

		/* Write to TIMx CCMR1 and CCER registers */
		htim->Instance->CCMR1 = tmpccmrx;
		htim->Instance->CCER = tmpccer;
	}
	else if (Channel == TIM_CHANNEL_2)
	{
		/* Disable the Channel 2: Reset the CC2E Bit */
		htim->Instance->CCER &= ~TIM_CCER_CC2E;
		tmpccmrx = htim->Instance->CCMR1;
		tmpccer = htim->Instance->CCER;

		/* Select the Polarity and set the CC2E Bit */
		tmpccer &= ~(TIM_CCER_CC2P | TIM_CCER_CC2NP);
		tmpccer |= ((sConfig->ICPolarity << 4) & (TIM_CCER_CC2P | TIM_CCER_CC2NP));

		/* Write to TIMx CCMR1 and CCER registers */
		htim->Instance->CCMR1 = tmpccmrx ;
		htim->Instance->CCER = tmpccer;
	}
	else if (Channel == TIM_CHANNEL_3)
	{
		/* Disable the Channel 3: Reset the CC3E Bit */
		htim->Instance->CCER &= ~TIM_CCER_CC3E;
		tmpccmrx = htim->Instance->CCMR2;
		tmpccer = htim->Instance->CCER;

		/* Select the Polarity and set the CC3E Bit */
		tmpccer &= ~(TIM_CCER_CC3P | TIM_CCER_CC3NP);
		tmpccer |= ((sConfig->ICPolarity << 8) & (TIM_CCER_CC3P | TIM_CCER_CC3NP));

		/* Write to TIMx CCMR2 and CCER registers */
		htim->Instance->CCMR2 = tmpccmrx;
		htim->Instance->CCER = tmpccer;
	}
	else
	{
		/* Disable the Channel 4: Reset the CC4E Bit */
		htim->Instance->CCER &= ~TIM_CCER_CC4E;
		tmpccmrx = htim->Instance->CCMR2;
		tmpccer = htim->Instance->CCER;

		/* Select the Polarity and set the CC4E Bit */
		tmpccer &= ~(TIM_CCER_CC4P | TIM_CCER_CC4NP);
		tmpccer |= ((sConfig->ICPolarity << 12) & (TIM_CCER_CC4P | TIM_CCER_CC4NP));

		/* Write to TIMx CCMR2 and CCER registers */
		htim->Instance->CCMR2 = tmpccmrx;
		htim->Instance->CCER = tmpccer;
	}

	htim->State = HAL_TIM_STATE_READY;

	/* Enable the Input Capture channel */
	TIM_CCxChannelCmd(htim->Instance, Channel, TIM_CCx_ENABLE);
}

/**
 * @brief  Task code handles receiver print sampling
 * @param  argument : Unused parameter
 * @retval None
 */
static void ReceiverPrintSamplingTask(void const *argument) {
	(void) argument;

	portTickType xLastWakeTime;
	portTickType xSampleStartTime;

	/* Initialise the xLastWakeTime variable with the current time */
	xLastWakeTime = xTaskGetTickCount();
	xSampleStartTime = xLastWakeTime;

	for (;;) {
		vTaskDelayUntil(&xLastWakeTime, receiverPrintSampleTime);

		PrintReceiverValues();

		/* If sampling duration exceeded, delete task to stop sampling */
		if (xTaskGetTickCount() >= xSampleStartTime + receiverPrintSampleDuration * configTICK_RATE_HZ)
			StopReceiverSamplingTask();
	}
}

/**
 * @}
 */

/**
 * @}
 */
/*****END OF FILE****/
