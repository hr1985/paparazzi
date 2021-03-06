/*
 * $Id$
 *
 * Copyright (C) 2008-2009 Antoine Drouin <poinix@gmail.com>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "firmwares/rotorcraft/autopilot.h"

#include "subsystems/radio_control.h"
#include "firmwares/rotorcraft/commands.h"
#include "firmwares/rotorcraft/navigation.h"
#include "firmwares/rotorcraft/guidance.h"
#include "firmwares/rotorcraft/stabilization.h"
#include "firmwares/rotorcraft/camera_mount.h"
#include "led.h"
#include "subsystems/electrical.h"

uint8_t  autopilot_mode;
uint8_t  autopilot_mode_auto2;
bool_t   autopilot_motors_on;

bool_t   autopilot_rc_unkilled_startup; //toytronics: keep track of Tx on motor unkill @ vehicle power up
bool_t   autopilot_first_boot; //toytronics: determine first power up for ahrs time delay
bool_t   autopilot_mode1_kill; //toytronics: keep track of whether motor shutoff occurred in mode 1 
bool_t   autopilot_safety_violation; //safety condition violated during startup attempt.
bool_t   autopilot_safety_violation_mode; //safety condition violated during startup attempt, not in mode 1.
bool_t   autopilot_safety_violation_throttle; //safety condition violated during startup attempt, not zero throttle.
bool_t   autopilot_safety_violation_roll; //safety condition violated during startup attempt, roll stick not centered.
bool_t   autopilot_safety_violation_pitch; //safety condition violated during startup attempt, pitch stick not centered.
bool_t   autopilot_safety_violation_yaw; //safety condition violated during startup attempt, yaw stick not centered.

bool_t   autopilot_in_flight;
uint32_t autopilot_motors_on_counter;
uint32_t autopilot_in_flight_counter;
uint32_t autopilot_mode1_kill_counter;
uint8_t  autopilot_check_motor_status;
bool_t   kill_throttle;
bool_t   autopilot_rc;

bool_t   autopilot_power_switch;

bool_t   autopilot_detect_ground;
bool_t   autopilot_detect_ground_once;

uint16_t autopilot_flight_time;

#define AUTOPILOT_MOTOR_ON_TIME     40
#define AUTOPILOT_IN_FLIGHT_TIME    40
#define AUTOPILOT_THROTTLE_TRESHOLD (MAX_PPRZ * 1 / 20)
#define AUTOPILOT_YAW_TRESHOLD      (MAX_PPRZ * 19 / 20)
#define AUTOPILOT_STICK_CENTER_TRESHOLD      (MAX_PPRZ * 1 / 20)
// Motors ON check state machine
#define STATUS_MOTORS_OFF           0
#define STATUS_M_OFF_STICK_PUSHED   1
#define STATUS_START_MOTORS         2
#define STATUS_MOTORS_ON            3
#define STATUS_M_ON_STICK_PUSHED    4
#define STATUS_STOP_MOTORS          5

#ifndef AUTOPILOT_STARTUP_DELAY
#define AUTOPILOT_STARTUP_DELAY 512
#endif

void autopilot_init(void) {
  autopilot_mode = AP_MODE_KILL;
  autopilot_motors_on = FALSE;
  autopilot_rc_unkilled_startup = FALSE;
  autopilot_first_boot = TRUE;
  autopilot_mode1_kill = TRUE;
  autopilot_safety_violation = FALSE;
  autopilot_safety_violation_mode = FALSE;
  autopilot_safety_violation_throttle = FALSE;
  autopilot_safety_violation_roll = FALSE;
  autopilot_safety_violation_pitch = FALSE;
  autopilot_safety_violation_yaw = FALSE;
  autopilot_in_flight = FALSE;
  kill_throttle = ! autopilot_motors_on;
  autopilot_motors_on_counter = 0;
  autopilot_in_flight_counter = 0;
  autopilot_mode1_kill_counter = 0;
  autopilot_check_motor_status = STATUS_MOTORS_OFF;
  autopilot_mode_auto2 = MODE_AUTO2;
  autopilot_detect_ground = FALSE;
  autopilot_detect_ground_once = FALSE;
  autopilot_flight_time = 0;
  autopilot_rc = TRUE;
  autopilot_power_switch = FALSE;
  #ifdef POWER_SWITCH_LED
    LED_ON(POWER_SWITCH_LED); // POWER OFF
  #endif
  #ifdef USE_CAMERA_MOUNT
    camera_mount_init();
  #endif
}


void autopilot_periodic(void) {

  RunOnceEvery(NAV_PRESCALER, nav_periodic_task());
#ifdef FAILSAFE_GROUND_DETECT
  if (autopilot_mode == AP_MODE_FAILSAFE && autopilot_detect_ground) {
    autopilot_set_mode(AP_MODE_KILL);
    autopilot_detect_ground = FALSE;
  }
#endif
  if ( !autopilot_motors_on ||
#ifndef FAILSAFE_GROUND_DETECT
       autopilot_mode == AP_MODE_FAILSAFE ||
#endif
       autopilot_mode == AP_MODE_KILL ) {
    SetCommands(commands_failsafe,
		autopilot_in_flight, autopilot_motors_on);
  }
  else {
    guidance_v_run( autopilot_in_flight );
    guidance_h_run( autopilot_in_flight );
    SetCommands(stabilization_cmd,
        autopilot_in_flight, autopilot_motors_on);
  }
#ifdef USE_CAMERA_MOUNT
  camera_mount_run();
#endif
}


void autopilot_set_mode(uint8_t new_autopilot_mode) {

  if (new_autopilot_mode != autopilot_mode) {
    /* horizontal mode */
    switch (new_autopilot_mode) {
    case AP_MODE_FAILSAFE:
#ifndef KILL_AS_FAILSAFE
      stab_att_sp_euler.phi = 0;
      stab_att_sp_euler.theta = 0;
      guidance_h_mode_changed(GUIDANCE_H_MODE_ATTITUDE);
      break;
#endif
    case AP_MODE_KILL:
      autopilot_motors_on = FALSE;
      guidance_h_mode_changed(GUIDANCE_H_MODE_KILL);
      break;
    case AP_MODE_RC_DIRECT:
      guidance_h_mode_changed(GUIDANCE_H_MODE_RC_DIRECT);
      break;
    case AP_MODE_RATE_DIRECT:
    case AP_MODE_RATE_Z_HOLD:
      guidance_h_mode_changed(GUIDANCE_H_MODE_RATE);
      break;
    case AP_MODE_ATTITUDE_DIRECT:
    case AP_MODE_ATTITUDE_CLIMB:
    case AP_MODE_ATTITUDE_Z_HOLD:
      guidance_h_mode_changed(GUIDANCE_H_MODE_ATTITUDE);
      break;
    case AP_MODE_HOVER_DIRECT:
    case AP_MODE_HOVER_CLIMB:
    case AP_MODE_HOVER_Z_HOLD:
      guidance_h_mode_changed(GUIDANCE_H_MODE_HOVER);
      break;
    case AP_MODE_NAV:
      guidance_h_mode_changed(GUIDANCE_H_MODE_NAV);
      break;
    case AP_MODE_TOYTRONICS_HOVER:
      guidance_h_mode_changed(GUIDANCE_H_MODE_TOYTRONICS_HOVER);
      break;
    case AP_MODE_TOYTRONICS_HOVER_FORWARD:
      guidance_h_mode_changed(GUIDANCE_H_MODE_TOYTRONICS_HOVER_FORWARD);
      break;
    case AP_MODE_TOYTRONICS_FORWARD:
      guidance_h_mode_changed(GUIDANCE_H_MODE_TOYTRONICS_FORWARD);
      break;
    case AP_MODE_TOYTRONICS_AEROBATIC:
      guidance_h_mode_changed(GUIDANCE_H_MODE_TOYTRONICS_AEROBATIC);
      break;
    default:
      break;
    }
    /* vertical mode */
    switch (new_autopilot_mode) {
    case AP_MODE_FAILSAFE:
#ifndef KILL_AS_FAILSAFE
      guidance_v_zd_sp = SPEED_BFP_OF_REAL(0.0);
      guidance_v_mode_changed(GUIDANCE_V_MODE_CLIMB);
      break;
#endif
    case AP_MODE_KILL:
      guidance_v_mode_changed(GUIDANCE_V_MODE_KILL);
      break;
    case AP_MODE_RC_DIRECT:
    case AP_MODE_RATE_DIRECT:
    case AP_MODE_ATTITUDE_DIRECT:
    case AP_MODE_HOVER_DIRECT:
    case AP_MODE_TOYTRONICS_HOVER:
    case AP_MODE_TOYTRONICS_HOVER_FORWARD:
    case AP_MODE_TOYTRONICS_FORWARD:
    case AP_MODE_TOYTRONICS_AEROBATIC:
      guidance_v_mode_changed(GUIDANCE_V_MODE_RC_DIRECT);
      break;
    case AP_MODE_RATE_RC_CLIMB:
    case AP_MODE_ATTITUDE_RC_CLIMB:
      guidance_v_mode_changed(GUIDANCE_V_MODE_RC_CLIMB);
      break;
    case AP_MODE_ATTITUDE_CLIMB:
    case AP_MODE_HOVER_CLIMB:
      guidance_v_mode_changed(GUIDANCE_V_MODE_CLIMB);
      break;
    case AP_MODE_RATE_Z_HOLD:
    case AP_MODE_ATTITUDE_Z_HOLD:
    case AP_MODE_HOVER_Z_HOLD:
      guidance_v_mode_changed(GUIDANCE_V_MODE_HOVER);
      break;
    case AP_MODE_NAV:
      guidance_v_mode_changed(GUIDANCE_V_MODE_NAV);
      break;
    default:
      break;
    }
    autopilot_mode = new_autopilot_mode;
  }

}

#define THROTTLE_STICK_DOWN()						\
  (radio_control.values[RADIO_THROTTLE] < AUTOPILOT_THROTTLE_TRESHOLD)
#define YAW_STICK_PUSHED()						\
  (radio_control.values[RADIO_YAW] > AUTOPILOT_YAW_TRESHOLD || \
   radio_control.values[RADIO_YAW] < -AUTOPILOT_YAW_TRESHOLD)
#define YAW_STICK_CENTERED()						\
  (radio_control.values[RADIO_YAW] < AUTOPILOT_STICK_CENTER_TRESHOLD && \
   radio_control.values[RADIO_YAW] > -AUTOPILOT_STICK_CENTER_TRESHOLD)
#define PITCH_STICK_CENTERED()						\
  (radio_control.values[RADIO_PITCH] < AUTOPILOT_STICK_CENTER_TRESHOLD && \
   radio_control.values[RADIO_PITCH] > -AUTOPILOT_STICK_CENTER_TRESHOLD)
#define ROLL_STICK_CENTERED()						\
  (radio_control.values[RADIO_ROLL] < AUTOPILOT_STICK_CENTER_TRESHOLD && \
   radio_control.values[RADIO_ROLL] > -AUTOPILOT_STICK_CENTER_TRESHOLD)


static inline void autopilot_check_in_flight( void) {
  if (autopilot_in_flight) {
    if (autopilot_in_flight_counter > 0) {
      if (THROTTLE_STICK_DOWN()) {
        autopilot_in_flight_counter--;
        if (autopilot_in_flight_counter == 0) {
          autopilot_in_flight = FALSE;
        }
      }
      else {	/* !THROTTLE_STICK_DOWN */
        autopilot_in_flight_counter = AUTOPILOT_IN_FLIGHT_TIME;
      }
    }
  }
  else { /* not in flight */
    if (autopilot_in_flight_counter < AUTOPILOT_IN_FLIGHT_TIME &&
        autopilot_motors_on) {
      if (!THROTTLE_STICK_DOWN()) {
        autopilot_in_flight_counter++;
        if (autopilot_in_flight_counter == AUTOPILOT_IN_FLIGHT_TIME)
          autopilot_in_flight = TRUE;
      }
      else { /*  THROTTLE_STICK_DOWN */
        autopilot_in_flight_counter = 0;
      }
    }
  }
}

#ifdef AUTOPILOT_KILL_WITHOUT_AHRS
#include "subsystems/ahrs.h"
static inline int ahrs_is_aligned(void) {
  return (ahrs.status == AHRS_RUNNING);
}
#else
static inline int ahrs_is_aligned(void) {
  return TRUE;
}
#endif

#ifdef AUTOPILOT_INSTANT_START_WITH_SAFETIES
static inline void autopilot_check_motors_on( void ) {
	if (radio_control.values[RADIO_KILL_SWITCH]>0 && (!ahrs_is_aligned() || autopilot_first_boot || autopilot_safety_violation))	
		autopilot_rc_unkilled_startup = TRUE;
	if (autopilot_rc_unkilled_startup == TRUE)
		if (radio_control.values[RADIO_KILL_SWITCH]<0 && ahrs_is_aligned() && !autopilot_first_boot && radio_control.values[RADIO_MODE] < -4000)
			autopilot_rc_unkilled_startup = FALSE;
	if (autopilot_first_boot && ahrs_is_aligned()){
		RunOnceAfter(AUTOPILOT_STARTUP_DELAY,{autopilot_first_boot = FALSE;});
		}
	if (!autopilot_motors_on && !autopilot_first_boot && autopilot_mode1_kill){
		autopilot_motors_on=radio_control.values[RADIO_KILL_SWITCH]>0 && radio_control.values[RADIO_MODE] < -4000 && THROTTLE_STICK_DOWN() && YAW_STICK_CENTERED() && PITCH_STICK_CENTERED() && ROLL_STICK_CENTERED() && ahrs_is_aligned() && !autopilot_rc_unkilled_startup;
		  if (!autopilot_motors_on && radio_control.values[RADIO_KILL_SWITCH]>0){
		    autopilot_safety_violation = TRUE;
		    }
		  else{
		    autopilot_safety_violation = FALSE;
		    }
		}
	else{ 
		autopilot_motors_on=radio_control.values[RADIO_KILL_SWITCH]>0 && ahrs_is_aligned() && !autopilot_rc_unkilled_startup;
		if(autopilot_motors_on)
		  autopilot_mode1_kill = radio_control.values[RADIO_MODE]<-4000;
		}
	}
#elif defined AUTOPILOT_THROTTLE_INSTANT_START_WITH_SAFETIES
static inline void autopilot_check_motors_on( void ) {
	if (!THROTTLE_STICK_DOWN() && (!ahrs_is_aligned() || autopilot_first_boot || autopilot_safety_violation))	
		autopilot_rc_unkilled_startup = TRUE;
	if (autopilot_rc_unkilled_startup)
		if (THROTTLE_STICK_DOWN() && ahrs_is_aligned() && !autopilot_first_boot && radio_control.values[RADIO_MODE] < -4000)
			autopilot_rc_unkilled_startup = FALSE;
	if (autopilot_first_boot && ahrs_is_aligned()){
		RunOnceAfter(AUTOPILOT_STARTUP_DELAY,{autopilot_first_boot = FALSE;});
		}
	if (!autopilot_motors_on && !autopilot_first_boot && autopilot_mode1_kill && autopilot_mode1_kill_counter>512){
		autopilot_motors_on=!THROTTLE_STICK_DOWN() && radio_control.values[RADIO_MODE] < -4000 && YAW_STICK_CENTERED() && PITCH_STICK_CENTERED() && ROLL_STICK_CENTERED() && ahrs_is_aligned() && !autopilot_rc_unkilled_startup;
		  if (!autopilot_motors_on && ahrs_is_aligned() && (radio_control.values[RADIO_MODE] > -4000 || !YAW_STICK_CENTERED() || !PITCH_STICK_CENTERED() || !ROLL_STICK_CENTERED() || autopilot_rc_unkilled_startup)){
		    autopilot_safety_violation = TRUE;
		    if (radio_control.values[RADIO_MODE] > -4000) {autopilot_safety_violation_mode = TRUE;}
		    else { autopilot_safety_violation_mode = FALSE;}
		    if (autopilot_rc_unkilled_startup) autopilot_safety_violation_throttle = TRUE;
		    else { autopilot_safety_violation_throttle = FALSE;}
		    if (!ROLL_STICK_CENTERED()) autopilot_safety_violation_roll = TRUE;
		    else { autopilot_safety_violation_roll = FALSE;}
		    if (!PITCH_STICK_CENTERED()) autopilot_safety_violation_pitch = TRUE;
		    else { autopilot_safety_violation_pitch = FALSE;}
		    if (!YAW_STICK_CENTERED()) autopilot_safety_violation_yaw = TRUE;
		    else { autopilot_safety_violation_yaw = FALSE;}
		    }
		  else{
		    autopilot_safety_violation = FALSE;
		    autopilot_safety_violation_mode = FALSE;
		    autopilot_safety_violation_throttle = FALSE;
		    autopilot_safety_violation_roll = FALSE;
		    autopilot_safety_violation_pitch = FALSE;
		    autopilot_safety_violation_yaw = FALSE;
		    }
		}
	else{ 
		autopilot_motors_on=!THROTTLE_STICK_DOWN() && ahrs_is_aligned() && !autopilot_rc_unkilled_startup;
		if(autopilot_motors_on)
		  autopilot_mode1_kill = radio_control.values[RADIO_MODE]<-4000;
		if (autopilot_mode1_kill && !autopilot_motors_on)
		  autopilot_mode1_kill_counter++;
		else
		  autopilot_mode1_kill_counter=0;
		}
	}
#elif defined AUTOPILOT_INSTANT_START
static inline void autopilot_check_motors_on( void ) {
	autopilot_motors_on=radio_control.values[RADIO_KILL_SWITCH]>0 && ahrs_is_aligned();
	}
#else
/** Set motors ON or OFF and change the status of the check_motors state machine
 */
void autopilot_set_motors_on(bool_t motors_on) {
  autopilot_motors_on = motors_on;
  kill_throttle = ! autopilot_motors_on;
  if (autopilot_motors_on) autopilot_check_motor_status = STATUS_MOTORS_ON;
  else autopilot_check_motor_status = STATUS_MOTORS_OFF;
}

/**
 * State machine to check if motors should be turned ON or OFF
 * The motors start/stop when pushing the yaw stick without throttle during a given time
 * An intermediate state prevents oscillating between ON and OFF while keeping the stick pushed
 * The stick must return to a neutral position before starting/stoping again
 */
static inline void autopilot_check_motors_on( void ) {
  switch(autopilot_check_motor_status) {
    case STATUS_MOTORS_OFF:
      autopilot_motors_on = FALSE;
      autopilot_motors_on_counter = 0;
      if (THROTTLE_STICK_DOWN() && YAW_STICK_PUSHED()) // stick pushed
        autopilot_check_motor_status = STATUS_M_OFF_STICK_PUSHED;
      break;
    case STATUS_M_OFF_STICK_PUSHED:
      autopilot_motors_on = FALSE;
      autopilot_motors_on_counter++;
      if (autopilot_motors_on_counter >= AUTOPILOT_MOTOR_ON_TIME)
        autopilot_check_motor_status = STATUS_START_MOTORS;
      else if (!(THROTTLE_STICK_DOWN() && YAW_STICK_PUSHED())) // stick released too soon
        autopilot_check_motor_status = STATUS_MOTORS_OFF;
      break;
    case STATUS_START_MOTORS:
      autopilot_motors_on = TRUE;
      autopilot_motors_on_counter = AUTOPILOT_MOTOR_ON_TIME;
      if (!(THROTTLE_STICK_DOWN() && YAW_STICK_PUSHED())) // wait until stick released
        autopilot_check_motor_status = STATUS_MOTORS_ON;
      break;
    case STATUS_MOTORS_ON:
      autopilot_motors_on = TRUE;
      autopilot_motors_on_counter = AUTOPILOT_MOTOR_ON_TIME;
      if (THROTTLE_STICK_DOWN() && YAW_STICK_PUSHED()) // stick pushed
        autopilot_check_motor_status = STATUS_M_ON_STICK_PUSHED;
      break;
    case STATUS_M_ON_STICK_PUSHED:
      autopilot_motors_on = TRUE;
      autopilot_motors_on_counter--;
      if (autopilot_motors_on_counter == 0)
        autopilot_check_motor_status = STATUS_STOP_MOTORS;
      else if (!(THROTTLE_STICK_DOWN() && YAW_STICK_PUSHED())) // stick released too soon
        autopilot_check_motor_status = STATUS_MOTORS_ON;
      break;
    case STATUS_STOP_MOTORS:
      autopilot_motors_on = FALSE;
      autopilot_motors_on_counter = 0;
      if (!(THROTTLE_STICK_DOWN() && YAW_STICK_PUSHED())) // wait until stick released
        autopilot_check_motor_status = STATUS_MOTORS_OFF;
      break;
    default:
      break;
  };
}
#endif


void autopilot_on_rc_frame(void) {

  uint8_t new_autopilot_mode = 0;
  AP_MODE_OF_PPRZ(radio_control.values[RADIO_MODE], new_autopilot_mode);
  autopilot_set_mode(new_autopilot_mode);

#ifdef RADIO_KILL_SWITCH
  if (radio_control.values[RADIO_KILL_SWITCH] < 0)
    autopilot_set_mode(AP_MODE_KILL);
#endif

#ifdef AUTOPILOT_KILL_WITHOUT_AHRS
  if (!ahrs_is_aligned())
    autopilot_set_mode(AP_MODE_KILL);
#endif

  autopilot_check_motors_on();
  autopilot_check_in_flight();
  kill_throttle = !autopilot_motors_on;

  if (autopilot_mode > AP_MODE_FAILSAFE) {
    guidance_v_read_rc();
    guidance_h_read_rc(autopilot_in_flight);
  }

}
