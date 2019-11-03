#include <TimeLib.h>
#include <AccelStepper.h>
#include <Arduino.h>
#include <FuGPS.h>
#include <HardwareSerial.h>
#include <MultiStepper.h>

#include "config.h"
#include "conversion.h"
//#include "location.h"

//Load the timer library, depending on the selected BOARD_TYPE
#ifdef BOARD_ARDUINO_MEGA
#include <TimerOne.h>
#elif defined BOARD_ARDUINO_DUE
#include <DueTimer.h>
#endif

// Load the display unit if it's enabled
#ifdef SERIAL_DISPLAY_ENABLED
#include "display_unit.h"
#endif

// Include the mount, depending on the selected MOUNT_TYPE
#ifdef MOUNT_TYPE_DOBSON
#include "./Dobson.h"
#elif defined MOUNT_TYPE_EQUATORIAL
#include "./Equatorial.h" // NOT IMPLEMENTED!
#elif defined MOUNT_TYPE_DIRECT
#include "./DirectDrive.h"
#endif


// Global variables
AccelStepper azimuth(AccelStepper::DRIVER, AZ_STEP_PIN, AZ_DIR_PIN);    // Azimuth stepper
AccelStepper altitude(AccelStepper::DRIVER, ALT_STEP_PIN, ALT_DIR_PIN); // Altitude stepper
FuGPS gps(Serial1); // GPS module

#ifdef MOUNT_TYPE_DOBSON
Dobson scope(azimuth, altitude, gps);
#elif defined MOUNT_TYPE_EQUATORIAL
Equatorial scope(azimuth, altitude, gps); // NOT IMPLEMENTED!
#elif defined MOUNT_TYPE_DIRECT
DirectDrive scope(azimuth, altitude, gps);
#endif

// Are the stepper drivers currently enabled?
bool motorsEnabled = false;

// This increases every loop iteration and resets at 10.000
unsigned int loopIteration = 0;
bool first_loop_run = true;
long last_motor_update = 0;

#ifdef DEBUG_HOME_IMMEDIATELY
const bool homeImmediately = true;
#else
const bool homeImmediately = false;
#endif


/**
 * Motor Interrupt handler
 * This is attached to timer interrupt 1. It gets called every STEPPER_INTERRUPT_FREQ / 1.000.000 seconds and moves our steppers
 */
void moveSteppers() {
	azimuth.run();
	altitude.run();
}


/**
 * Initialize stepper drivers. Sets pin inversion config, max speed and acceleration per stepper
 */
void setupSteppers() {
	// Set stepper pins
	pinMode(AZ_ENABLE_PIN, OUTPUT);  // Azimuth pin
	pinMode(ALT_ENABLE_PIN, OUTPUT); // Altitude pin

	azimuth.setPinsInverted(true, false, false);
	azimuth.setMaxSpeed(AZ_MAX_SPEED);
	azimuth.setAcceleration(AZ_MAX_ACCEL);

	altitude.setPinsInverted(true, false, false);
	altitude.setMaxSpeed(ALT_MAX_SPEED);
	altitude.setAcceleration(ALT_MAX_ACCEL);

	// Setup the stepper interrupt. Depends on the selected board
#ifdef BOARD_ARDUINO_MEGA
	//Timer1.initialize(STEPPER_INTERRUPT_FREQ);
	//Timer1.attachInterrupt(&moveSteppers);
#elif defined BOARD_ARDUINO_DUE
	Timer.getAvailable()
		.attachInterrupt(&moveSteppers)
		.start(STEPPER_INTERRUPT_FREQ);
#endif

	DEBUG_PRINT("Steppers enabled:  ");
#ifdef AZ_ENABLE
	DEBUG_PRINT("Az=ON");
#else
	DEBUG_PRINT("Az=OFF");
#endif

#ifdef ALT_ENABLE
	DEBUG_PRINTLN("  Alt=ON");
#else
	DEBUG_PRINTLN("  Alt=OFF");
#endif
} // setupSteppers


/*
 * This function turns stepper motor drivers on, if these conditions are met:
 * STEPPERS_ON_PIN reads HIGH
 * operating mode is not Mode::INITIALIZING
 * TODO Maybe the conditions need to change (esp. the opmode one)
 */
void setSteppersOnOffState() {
	const bool isInitialized = scope.getMode() != Mode::INITIALIZING;

#ifdef STEPPERS_ON_PIN
	// If the STEPPERS_ON switch is installed, check its state
	const bool steppersSwitchOn = digitalRead(STEPPERS_ON_PIN) == HIGH;
#else
	// If no STEPPERS_ON switch is installed, define its state as enabled
#define steppersSwitchOn true
#endif

	if (steppersSwitchOn && isInitialized) {
		// Motors on
		motorsEnabled = true;
#ifdef AZ_ENABLE
		digitalWrite(AZ_ENABLE_PIN, LOW);
#endif
#ifdef ALT_ENABLE
		digitalWrite(ALT_ENABLE_PIN, LOW);
#endif
	}
	else {
		// Motors OFF
		motorsEnabled = false;
#ifdef AZ_ENABLE
		digitalWrite(AZ_ENABLE_PIN, HIGH);
#endif
#ifdef ALT_ENABLE
		digitalWrite(ALT_ENABLE_PIN, HIGH);
#endif
	}
}

/**
 * Run various tasks required to initialize the following:
 * Serial connection
 * Serial communication
 * Stepper motors
 * If enabled:
 *     Display module
 *     GPS module
 *     Buzzer
 *     Steppers On/Off Button
 *     Home Now Button
 *     Target Select Button
 *
 * Finally, telescope.initialize() is called. This sets an initial target (depending on the MOUNT_TYPE) and the initial scope operating mode
 */
void setup() {
#if defined DEBUG && defined DEBUG_SERIAL
	Serial.begin(SERIAL_BAUDRATE);
#endif

	DEBUG_PRINTLN("Initializing");

	// If the serial display is enabled, begin communicating with it
#ifdef SERIAL_DISPLAY_ENABLED
	initDisplayCommunication(scope);
#endif
	// This initializes the GPS module
	// TODO Wrap in ifdef
	initGPS(gps);

	// Initialize the telescope
	scope.initialize();

	// This sets up communication with Stellarium / Serial console
	initCommunication(scope);

	// Initialize stepper motor drivers
	setupSteppers();

	// Button pins
#ifdef BUZZER_PIN
	pinMode(BUZZER_PIN, OUTPUT);
	digitalWrite(BUZZER_PIN, HIGH);
	//delay(1000);
	digitalWrite(BUZZER_PIN, LOW);
#endif

#ifdef STEPPERS_ON_PIN
	pinMode(STEPPERS_ON_PIN, INPUT);
#endif

#ifdef HOME_NOW_PIN
	pinMode(HOME_NOW_PIN, INPUT);
#endif

#ifdef TARGET_SELECT_PIN
	// Input mode for the select pin is INPUT with PULLUP enabled, so that we can use a longer cable
	pinMode(TARGET_SELECT_PIN, INPUT_PULLUP);
#endif

	//delay(5000);



	// If the serial display is enabled, begin communicating with it
#ifdef SERIAL_DISPLAY_ENABLED
	initDisplayCommunication(scope);
#endif
}


/*
 * Main program loop. It does roughly the following (in order)
 * 1) Check whether the Home Button is pressed. If so, set the scope back to homing mode
 * 2) Get updates from the GPS module and tell the telescope instance about it
 * 3) Handle communication over serial
 * 4) Check whether homing mode should end (TODO This should move to the telescope class)
 * 5) Turn the stepper drivers on/off
 * 6) Every 10.000 loop iterations:
 *    - Run the necessary calculations to update the telescope target position
 *    - Update the target values for the steppers
 * 7) Debug communications, if enabled
*/
void loop() {
#ifdef HOME_NOW_PIN
	// If the HOME button is installed and pressed/switched on start homing mode
	const bool currentlyHoming = digitalRead(HOME_NOW_PIN) == HIGH;

	if (currentlyHoming) {
		// The telescope stands still and waits for the next target selection (which it then assumes it is pointing at)
		// Setting homed to false also sets the telescope to operating mode Mode::HOMING
		scope.setHomed(false);
	}
#else
	// If no HOME button exists, it never gets pressed, so we define it as false
#define currentlyHoming false
#endif

// Get the current position from our GPS module. If no GPS is installed
// or no fix is available values from EEPROM are used
	TelescopePosition pos = handleGPS(gps);

	scope.updateGpsPosition(pos);

	// Homing needs to be performed in HOMING mode
	bool requiresHoming = scope.getMode() == Mode::HOMING;

	// Handle serial serial communication. Returns true if homing was just performed
	bool justHomed = handleSerialCommunication(scope, gps, requiresHoming);

#ifdef SERIAL_DISPLAY_ENABLED
	handleDisplayCommunication(scope, gps, requiresHoming);
#endif

	// If DEBUG_HOME_IMMEDIATELY is defined, homing is performed on first loop iteration.
	// Otherwise a serial command or HOME_NOW Button are required
	if (justHomed || currentlyHoming
		|| (homeImmediately && loopIteration == 0)) {
		justHomed = true;

		// Sets the telescope to operating mode Mode::TRACKING
		scope.setHomed(true);
	}

	// Turn the stepper motors on or off, depending on state of STEPPERS_ON_PIN
	setSteppersOnOffState();

	// Every 10.000 loop iterations: Handle motor movements.
	// TODO This should be dynamic, based on how long calculations/serial comms took and/or if a new command is available
	if (millis() - last_motor_update >= UPDATE_MOTOR_POS_MS || first_loop_run) {
		first_loop_run = false;
		last_motor_update = millis();

#if defined(DEBUG) && defined(DEBUG_SERIAL_STEPPER_MOVEMENT) && defined(DEBUG_TIMING)
		// Start timing the calculation
		long micros_start = micros();
#endif

		// This function converts the coordinates
		scope.calculateMotorTargets();

		// This actually makes the motors move to their desired target positions
		scope.move();

#if defined(DEBUG) && defined(DEBUG_SERIAL_STEPPER_MOVEMENT) && defined(DEBUG_TIMING)
		// Debug: If a move took place, output how long it took from beginning to end of the calculation
		if (scope._didMove) {
			long calc_time = scope._lastCalcMicros - micros_start;
			long dbg_time = micros() - scope._lastCalcMicros;
			DEBUG_PRINT("; Calc: ");
			DEBUG_PRINT(calc_time / 1000.);
			DEBUG_PRINT("ms; DbgComms: ");
			DEBUG_PRINT(dbg_time / 1000.);
			DEBUG_PRINTLN("ms");
		}
#endif
	}

#ifdef BOARD_ARDUINO_MEGA
	moveSteppers();
#endif

	loopIteration++;
}