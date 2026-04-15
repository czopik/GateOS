#ifndef EPS_H
#define EPS_H

#include <stdint.h>

// Hall-based position state for EPS mode
typedef struct {
  uint8_t hall_raw;
  uint8_t hall_stable;
  uint8_t debounce_cnt;
  int8_t  sector_index;
  int8_t  direction;           // -1, 0, +1
  uint16_t transitions;
  uint32_t last_transition_tick;
  uint32_t period_ticks;
  int16_t mech_angle_q4;       // mechanical angle [deg * 16]
  int16_t rpm;                 // signed mechanical rpm
  uint8_t valid;
} EpsPosState;

typedef enum {
  EPS_ST_IDLE = 0,
  EPS_ST_ALIGN,
  EPS_ST_OPEN_LOOP,
  EPS_ST_CLOSED_LOOP
} EpsState;

typedef struct {
  EpsPosState pos;
  EpsState state;
  uint32_t state_tick;
  uint32_t last_tick;
  uint32_t slew_tick_accum;
  uint32_t speed_tick_accum;
  int16_t cmd_ramped;
  int16_t open_loop_rpm;
  int32_t open_loop_elec_angle_q4;  // electrical angle [deg * 16]
  int16_t offset_mech_q4;
  uint8_t offset_valid;
  uint8_t pwm_enable;
} EpsCtrl;

void eps_ctrl_init(EpsCtrl *ctrl);
void eps_ctrl_update(EpsCtrl *ctrl,
                     uint8_t hall_state,
                     uint32_t tick,
                     uint8_t enable_req,
                     int16_t cmd_in,
                     uint8_t ctrl_mode_in,
                     int16_t *a_mech_angle_q4,
                     int16_t *cmd_out,
                     uint8_t *ctrl_mode_out,
                     uint8_t *pwm_enable_out);

#endif  // EPS_H
