#pragma once

#include <math.h>

enum class GateTerminalState {
  Unknown = 0,
  FullyOpen,
  FullyClosed
};

enum class GateMoveDirection {
  None = 0,
  Open = 1,
  Close = -1
};

enum class GateMoveBlockReason {
  None = 0,
  Busy,
  PendingStop,
  InvalidLimits,
  LimitOpenActive,
  LimitCloseActive,
  FullyOpen,
  FullyClosed
};

struct GateDecisionContext {
  bool moving = false;
  bool pendingStop = false;
  bool limitOpenActive = false;
  bool limitCloseActive = false;
  GateTerminalState terminalState = GateTerminalState::Unknown;
  float position = 0.0f;
  float maxDistance = 0.0f;
  int lastDirection = 1;
  // Set when the user manually stopped the gate during movement.
  // When true, the next toggle should always reverse direction
  // regardless of position proximity to endpoints.
  bool userStoppedDuringMove = false;
};

inline bool gateNearClosed(const GateDecisionContext& ctx, float eps = 0.01f) {
  return ctx.position <= eps;
}

inline bool gateNearOpen(const GateDecisionContext& ctx, float eps = 0.01f) {
  return ctx.maxDistance > 0.0f && ctx.position >= (ctx.maxDistance - eps);
}

inline GateMoveDirection resolveToggleDirection(const GateDecisionContext& ctx) {
  // 1. Hard limits always take precedence
  if (ctx.limitOpenActive && ctx.limitCloseActive) return GateMoveDirection::None;
  if (ctx.limitOpenActive) return GateMoveDirection::Close;
  if (ctx.limitCloseActive) return GateMoveDirection::Open;

  // 2. If the user manually stopped the gate during movement, ALWAYS reverse direction.
  //    This has higher priority than terminal state to handle the case where
  //    gate barely moved from OPEN limit (terminalState may still be FullyOpen
  //    due to race with refreshTerminalStateFromPosition) but user clearly
  //    wants to reverse.  The physical hard-limit check above is still the
  //    ultimate guard.
  if (ctx.userStoppedDuringMove) {
    return ctx.lastDirection >= 0 ? GateMoveDirection::Close : GateMoveDirection::Open;
  }

  // 3. Terminal states (gate reached an endpoint via soft limit or limit switch)
  if (ctx.terminalState == GateTerminalState::FullyOpen) return GateMoveDirection::Close;
  if (ctx.terminalState == GateTerminalState::FullyClosed) return GateMoveDirection::Open;

  // 4. Position-based fallbacks for idle/unknown states
  if (ctx.maxDistance <= 0.0f || gateNearClosed(ctx)) return GateMoveDirection::Open;
  if (gateNearOpen(ctx)) return GateMoveDirection::Close;

  // 5. Default: reverse last movement direction
  return ctx.lastDirection >= 0 ? GateMoveDirection::Close : GateMoveDirection::Open;
}

inline GateMoveBlockReason validateMoveDirection(const GateDecisionContext& ctx, GateMoveDirection dir) {
  if (dir == GateMoveDirection::None) return GateMoveBlockReason::InvalidLimits;
  if (ctx.moving) return GateMoveBlockReason::Busy;
  if (ctx.pendingStop) return GateMoveBlockReason::PendingStop;
  if (ctx.limitOpenActive && ctx.limitCloseActive) return GateMoveBlockReason::InvalidLimits;

  if (dir == GateMoveDirection::Open) {
    if (ctx.limitOpenActive) return GateMoveBlockReason::LimitOpenActive;
    // When user manually stopped mid-move, bypass stale FullyOpen terminal state.
    // The physical limit check above already guards the real hardware limit.
    if (!ctx.userStoppedDuringMove && ctx.terminalState == GateTerminalState::FullyOpen)
      return GateMoveBlockReason::FullyOpen;
  } else if (dir == GateMoveDirection::Close) {
    if (ctx.limitCloseActive) return GateMoveBlockReason::LimitCloseActive;
    if (!ctx.userStoppedDuringMove && ctx.terminalState == GateTerminalState::FullyClosed)
      return GateMoveBlockReason::FullyClosed;
  }

  return GateMoveBlockReason::None;
}
