#include <iostream>
#include <string>
#include <vector>

#include "../esp32/Src/gate_logic_rules.h"

namespace {
struct ScenarioGate {
  GateDecisionContext ctx;

  std::string commandToggle() {
    if (ctx.moving) {
      // User manually stopped during movement — set the flag
      // so the next toggle reverses direction.
      ctx.userStoppedDuringMove = true;
      ctx.moving = false;
      ctx.pendingStop = false;
      return "stop";
    }
    GateMoveDirection dir = resolveToggleDirection(ctx);
    if (dir == GateMoveDirection::Open) return commandOpen();
    if (dir == GateMoveDirection::Close) return commandClose();
    return "blocked";
  }

  std::string commandOpen() {
    GateMoveBlockReason block = validateMoveDirection(ctx, GateMoveDirection::Open);
    if (block != GateMoveBlockReason::None) return "blocked";
    ctx.moving = true;
    ctx.pendingStop = false;
    ctx.userStoppedDuringMove = false;
    ctx.lastDirection = 1;
    ctx.terminalState = GateTerminalState::Unknown;
    return "open";
  }

  std::string commandClose() {
    GateMoveBlockReason block = validateMoveDirection(ctx, GateMoveDirection::Close);
    if (block != GateMoveBlockReason::None) return "blocked";
    ctx.moving = true;
    ctx.pendingStop = false;
    ctx.userStoppedDuringMove = false;
    ctx.lastDirection = -1;
    ctx.terminalState = GateTerminalState::Unknown;
    return "close";
  }

  std::string obstacleTrip() {
    if (!ctx.moving || ctx.lastDirection >= 0) return "ignored";
    ctx.moving = false;
    ctx.pendingStop = false;
    return commandOpen();
  }

  void limitOpenHit() {
    ctx.moving = false;
    ctx.pendingStop = false;
    ctx.userStoppedDuringMove = false;
    ctx.limitOpenActive = true;
    ctx.limitCloseActive = false;
    ctx.terminalState = GateTerminalState::FullyOpen;
    ctx.lastDirection = 1;
    ctx.position = ctx.maxDistance;
  }

  void limitCloseHit() {
    ctx.moving = false;
    ctx.pendingStop = false;
    ctx.userStoppedDuringMove = false;
    ctx.limitOpenActive = false;
    ctx.limitCloseActive = true;
    ctx.terminalState = GateTerminalState::FullyClosed;
    ctx.lastDirection = -1;
    ctx.position = 0.0f;
  }

  void releaseLimits() {
    ctx.limitOpenActive = false;
    ctx.limitCloseActive = false;
  }
};

struct ScenarioResult {
  std::string name;
  bool ok;
  std::string detail;
};

ScenarioResult expect(bool cond, const std::string& name, const std::string& detail) {
  return {name, cond, detail};
}
}  // namespace

int main() {
  std::vector<ScenarioResult> results;

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitCloseHit();
    gate.releaseLimits();
    bool ok = gate.commandToggle() == "open";
    gate.limitOpenHit();
    gate.releaseLimits();
    ok = ok && gate.commandToggle() == "close";
    results.push_back(expect(ok, "closed_toggle_open_limit_toggle_close", "fullyClosed -> toggle -> OPEN -> limit open -> toggle -> CLOSE"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitOpenHit();
    gate.releaseLimits();
    bool ok = gate.commandToggle() == "close";
    gate.limitCloseHit();
    gate.releaseLimits();
    ok = ok && gate.commandToggle() == "open";
    results.push_back(expect(ok, "open_toggle_close_limit_toggle_open", "fullyOpen -> toggle -> CLOSE -> limit close -> toggle -> OPEN"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitCloseHit();
    gate.releaseLimits();
    bool ok = gate.commandOpen() == "open" && gate.commandOpen() == "blocked";
    results.push_back(expect(ok, "open_during_closeing_path_blocked", "OPEN command during active movement must not restart motion"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitOpenHit();
    gate.releaseLimits();
    bool ok = gate.commandClose() == "close" && gate.commandClose() == "blocked";
    results.push_back(expect(ok, "close_during_opening_path_blocked", "CLOSE command during active movement must not restart motion"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitCloseHit();
    gate.releaseLimits();
    bool ok = gate.commandOpen() == "open" && gate.commandToggle() == "stop";
    results.push_back(expect(ok, "toggle_while_moving_stops", "toggle during motion becomes stop"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitOpenHit();
    gate.releaseLimits();
    bool ok = gate.commandClose() == "close" && gate.obstacleTrip() == "open";
    results.push_back(expect(ok, "obstacle_during_close_reopens", "obstacle during CLOSE stops and reopens"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitCloseHit();
    gate.releaseLimits();
    bool ok = gate.commandOpen() == "open" && gate.obstacleTrip() == "ignored";
    results.push_back(expect(ok, "obstacle_during_open_ignored", "obstacle during OPEN is ignored"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitOpenHit();
    bool ok = validateMoveDirection(gate.ctx, GateMoveDirection::Open) == GateMoveBlockReason::FullyOpen;
    results.push_back(expect(ok, "open_limit_blocks_open", "active/open latched end blocks OPEN"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitCloseHit();
    bool ok = validateMoveDirection(gate.ctx, GateMoveDirection::Close) == GateMoveBlockReason::FullyClosed;
    results.push_back(expect(ok, "close_limit_blocks_close", "active/closed latched end blocks CLOSE"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.ctx.pendingStop = true;
    bool ok = gate.commandOpen() == "blocked" && gate.commandClose() == "blocked";
    results.push_back(expect(ok, "pending_stop_blocks_restart", "pendingStop rejects quick follow-up commands"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitOpenHit();
    gate.releaseLimits();
    bool ok = validateMoveDirection(gate.ctx, GateMoveDirection::Open) == GateMoveBlockReason::FullyOpen;
    ok = ok && gate.commandToggle() == "close";
    results.push_back(expect(ok, "released_open_limit_keeps_terminal_latch", "releasing OPEN limit keeps fully-open latch until movement away starts"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.limitCloseHit();
    gate.releaseLimits();
    bool ok = validateMoveDirection(gate.ctx, GateMoveDirection::Close) == GateMoveBlockReason::FullyClosed;
    ok = ok && gate.commandToggle() == "open";
    results.push_back(expect(ok, "released_close_limit_keeps_terminal_latch", "releasing CLOSE limit keeps fully-closed latch until movement away starts"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.ctx.limitOpenActive = true;
    gate.ctx.limitCloseActive = true;
    bool ok = gate.commandToggle() == "blocked";
    ok = ok && validateMoveDirection(gate.ctx, GateMoveDirection::Open) == GateMoveBlockReason::InvalidLimits;
    ok = ok && validateMoveDirection(gate.ctx, GateMoveDirection::Close) == GateMoveBlockReason::InvalidLimits;
    results.push_back(expect(ok, "both_limits_active_block_all_motion", "invalid simultaneous limits block toggle and direct motion"));
  }

  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    gate.ctx.position = 2.5f;
    gate.ctx.lastDirection = 1;
    bool ok = gate.commandToggle() == "close";
    gate.ctx.moving = false;
    gate.ctx.position = 2.5f;
    gate.ctx.lastDirection = -1;
    ok = ok && gate.commandToggle() == "open";
    results.push_back(expect(ok, "mid_position_toggle_flips_last_direction", "toggle from mid-travel prefers opposite of previous move"));
  }

  // --- Problem 2 regression test: user stops near endpoint then toggles ---
  // The gate should REVERSE even if still within 1cm of the open endpoint.
  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    // Gate is fully open, user presses toggle -> starts closing
    gate.limitOpenHit();
    gate.releaseLimits();
    bool ok = gate.commandToggle() == "close";
    // Gate is closing, position barely moved (still near open end)
    gate.ctx.position = 4.995f;  // within 0.01m of maxDistance
    // User presses toggle -> stops
    ok = ok && gate.commandToggle() == "stop";
    // User presses toggle again -> should REVERSE to open (not continue closing!)
    ok = ok && gate.commandToggle() == "open";
    results.push_back(expect(ok, "user_stop_near_open_reverses",
      "after user stop near open endpoint during closing, toggle must reverse to open"));
  }

  // Same test near the closed endpoint
  {
    ScenarioGate gate;
    gate.ctx.maxDistance = 5.0f;
    // Gate is fully closed, user presses toggle -> starts opening
    gate.limitCloseHit();
    gate.releaseLimits();
    bool ok = gate.commandToggle() == "open";
    // Gate is opening, position barely moved (still near closed end)
    gate.ctx.position = 0.005f;  // within 0.01m of 0
    // User presses toggle -> stops
    ok = ok && gate.commandToggle() == "stop";
    // User presses toggle again -> should REVERSE to close (not continue opening!)
    ok = ok && gate.commandToggle() == "close";
    results.push_back(expect(ok, "user_stop_near_closed_reverses",
      "after user stop near closed endpoint during opening, toggle must reverse to close"));
  }

  bool allOk = true;
  for (const auto& result : results) {
    allOk = allOk && result.ok;
    std::cout << (result.ok ? "[PASS] " : "[FAIL] ")
              << result.name << " :: " << result.detail << "\n";
  }

  std::cout << "summary: " << (allOk ? "PASS" : "FAIL")
            << " total=" << results.size() << "\n";
  return allOk ? 0 : 1;
}
