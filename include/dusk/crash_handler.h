#pragma once

namespace dusk::crash_handler {

void install();

// Per-frame liveness signal for the hang watchdog. Call once per game frame.
// If no heartbeat arrives for the watchdog's stall window, the watchdog
// suspends the main thread, writes a symbolicated "HUNG" backtrace to the log,
// and terminates — turning an infinite loop (which produces no exception and
// otherwise dies traceless) into an actionable stack. No-op off Windows.
void heartbeat();

}  // namespace dusk::crash_handler
