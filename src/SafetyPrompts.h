// SafetyPrompts.h — User-facing safety gates: countdown abort prompt and
//                   unknown-CPU warning with forced confirmation before programming.
#ifndef _SAFETY_PROMPTS_H_
#define _SAFETY_PROMPTS_H_

#include <Uefi.h>

// Displays a 10-second countdown and returns TRUE if ESC was pressed (abort requested).
BOOLEAN CheckForEmergencyExit(VOID);

// Warns user of unrecognised CPUID and waits up to 30 s for F10 confirmation.
// Returns TRUE if user chose to proceed despite the warning.
BOOLEAN DisplayUnknownCpuWarning(VOID);

#endif
