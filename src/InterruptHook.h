// InterruptHook.h — IDT patching for "safe ASM" mode: redirects CPU exceptions
//                   (GP, UD, DF, BR, MC) to recoverable ISRs so that probing faults
//                   don't hard-crash the system.
#pragma once

/*******************************************************************************
* InstallSafeAsmExceptionHandler — backup IDT and install monkey ISR table
******************************************************************************/

VOID InstallSafeAsmExceptionHandler(VOID);

/*******************************************************************************
* RemoveAllInterruptOverrides — restore original IDT entries from backup
******************************************************************************/

VOID RemoveAllInterruptOverrides(VOID);
