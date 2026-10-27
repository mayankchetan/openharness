// The device half of the host's debug commands: take a `debug.*` message off the cable, do it, answer it.
// The shapes are in docs/DEBUG_COMMANDS.md; the logic that can run without a board is in debug_cmd.h.
//
// Compiled in by default; -DDEVICE_NO_DEBUG_COMMANDS=1 removes it (main/CMakeLists.txt), and with it every
// hook in the renderer, the touch task and the message layer.
#pragma once

#include <stdbool.h>

typedef struct cJSON cJSON;

// Offer a decoded message to the debug commands. Returns true when `t` is one of theirs — answered, or
// refused with a `debug.error` — and false when it is not, so the caller carries on with its own
// vocabulary. Runs on the cable reader task.
//
// A screenshot is taken and sent from inside this call, on the reader task: roughly a second at USB
// full speed, during which the reader does not drain the port. The 32 KiB receive ring holds what a host
// sends meanwhile; see the sizing note in cable_link.c.
bool cable_debug_handle(const char *t, const cJSON *p);

// From the reader's tick: report an injected gesture that has finished (or failed). Cheap when idle.
void cable_debug_tick(void);
