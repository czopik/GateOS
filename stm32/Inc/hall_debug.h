#ifndef HALL_DEBUG_H
#define HALL_DEBUG_H

#include <stdint.h>

#define HALL_SEQ_LEN 32
#define HALL_EVT_BUF 8

typedef struct {
  uint32_t ts_ms;
  uint8_t from_state;
  uint8_t to_state;
  uint32_t dt_us;
  int8_t dir;
} HallEvent;

typedef struct {
  uint8_t last_state;
  uint8_t prev_state;
  uint8_t has_state;
  int8_t direction;
  uint16_t transitions;
  uint16_t bad_jumps;
  uint16_t timeouts;
  uint16_t unique_mask;
  uint32_t last_transition_tick;
  uint32_t last_dt_ticks;
  int16_t rpm_hall;
  uint8_t seq_buf[HALL_SEQ_LEN];
  uint8_t seq_head;
  uint8_t seq_len;
  uint8_t timeout_active;
  HallEvent events[HALL_EVT_BUF];
  uint8_t ev_head;
  uint8_t ev_tail;
} HallDebug;

extern HallDebug hall_dbg_left;
extern HallDebug hall_dbg_right;
extern volatile uint16_t hall_states_per_rev;

void hall_debug_init(HallDebug *dbg);
void hall_debug_reset(HallDebug *dbg);
void hall_debug_update(HallDebug *dbg, uint8_t hall_state, uint32_t tick, uint8_t dbg_level);
void hall_debug_check_timeout(HallDebug *dbg, uint32_t tick, uint32_t timeout_ticks);
uint8_t hall_debug_pop_event(HallDebug *dbg, HallEvent *out);
uint8_t hall_debug_unique_count(const HallDebug *dbg);

#endif  // HALL_DEBUG_H
