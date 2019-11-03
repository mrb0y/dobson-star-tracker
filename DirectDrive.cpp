#include <AccelStepper.h>
#include <FuGPS.h>
#include <Time.h>

#include "./config.h"
#include "./location.h"

#include "./DirectDrive.h"


DirectDrive::DirectDrive(AccelStepper& azimuthStepper, AccelStepper& altitudeStepper, FuGPS& gps) :
	_azimuthStepper(azimuthStepper), _altitudeStepper(altitudeStepper), _gps(gps) {
}

void DirectDrive::initialize() {
	// This mount type does not actually track, but it is still
	// required by some other parts of the firmware to have tracking mode enabled
	setMode(Mode::TRACKING);

	// The initial setting is 0 degrees for both axes
	setTarget({ 0, 0 });
}

// Calculate new motor targets. This does not yet execute the move
void DirectDrive::calculateMotorTargets() {
	// The only conversion that's needed is degrees to motor steps
	_steppersTarget.azimuth = (long)(_target.rightAscension * AZ_STEPS_PER_DEG);
	_steppersTarget.altitude = (long)(_target.declination * ALT_STEPS_PER_DEG);
	
	#ifdef DEBUG_TIMING
		_lastCalcMicros = micros();
	#endif
}


/*
 * This method gets executed every 10.000 loop iterations right after Dobson::calculateMotorTargets() was called.
 * It checks whether the telescope is homed. If it is NOT homed, it sets the target as its current motor positions and sets _isHomed to true.
 * The next time the method gets called, _isHomed is true , and the stepper motors are actually moved to their new required position.
 */
void DirectDrive::move() {
	if ((millis() < 5000)) {
		DEBUG_PRINTLN("Ignore move for first 5 seconds");
		return;
	}

	// Move the steppers to their target positions
	_azimuthStepper.moveTo(_steppersTarget.azimuth);
	_altitudeStepper.moveTo(_steppersTarget.altitude);

	_currentPosition = {
		_target.rightAscension,
		_target.declination
	};

	// TODO Update this correctly
	_didMove = true;

	#if defined(DEBUG_SERIAL_STEPPER_MOVEMENT_VERBOSE) || defined(DEBUG_SERIAL_STEPPER_MOVEMENT)
		if (_didMove) {
			DEBUG_PRINTLN("-------------------------------------------------------------------------------------------------------------------");
			DEBUG_PRINT("Desired:     ");

			DEBUG_PRINT("Ra/Dec ");
			DEBUG_PRINT(_target.rightAscension);
			DEBUG_PRINT("� / ");
			DEBUG_PRINT(_target.declination);
			DEBUG_PRINT("�");

			DEBUG_PRINT("   Actual Steps ");
			DEBUG_PRINT(_azimuthStepper.currentPosition());
			DEBUG_PRINT("s / ");
			DEBUG_PRINT(_altitudeStepper.currentPosition());
			DEBUG_PRINT("s");

			#ifndef DEBUG_TIMING
				DEBUG_PRINTLN("");
			#endif
		}
	#endif
}