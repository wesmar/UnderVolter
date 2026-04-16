// InterruptHook.c — IDT monkey-patching for safe-ASM mode.  Installs stub ISRs
//                   (monkey_isr_N) for selected exception vectors so that faults
//                   triggered by speculative rdmsr/wrmsr are caught and signalled
//                   via CR2 rather than causing a triple-fault.  The original IDT
//                   is saved to gBackupIDT[] and restored by RemoveAllInterruptOverrides.
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include "HwIntrinsicsX64.h"

/*******************************************************************************
* Interrupt Descriptor Table (IDT) and its corresponding register (IDTR)
* See: https://wiki.osdev.org/Interrupt_Descriptor_Table and
* Intel 64 and IA-32 Architectures Software Developer�s Manual, Vol 3A, 5.10
******************************************************************************/

 
/*******************************************************************************
* type_addr: https://wiki.osdev.org/Interrupt_Descriptor_Table
*
* Example:   8E = 10001110
*                 |   ||||
*                 |   Gate type = Interrupt Gate
*                 Used / Unused = Switch it on!!
******************************************************************************/

/////////
// IDT //
/////////

#pragma pack (1)
typedef union {
  struct {
    UINT32  OffsetLow   : 16;
    UINT32  Selector    : 16;
    UINT32  Reserved1   : 8;
    UINT32  TypeAddr    : 8;
    UINT32  OffsetHigh  : 16;
    UINT32  OffsetUpper : 32;
    UINT32  Reserved2   : 32;
  } u1;
  struct {
    UINT64  q[2];
  } u64;
} IDT;

//////////
// IDTR //
//////////

typedef struct _IDTR {
  UINT16  Limit;
  UINT64  Base;
} IDTR;
#pragma pack ()

/*******************************************************************************
* This is a list of interrupts we want to redirect to our ISRs. Remaining IRQs
* will go to the handlers set by UEFI firmware or whoever else operates this.
* 
* NOTE: for voltage stability testing, it is probably a good idea to override
* all exception ISRs (0x00 to 0x1F) as you probably do not want some serious 
* code written 'for revenue' to deal with garbage made by glitched CPU (SOP for 
* any grownup code: reboot immediately, maybe even call tactical police on you)
*******************************************************************************/

typedef struct _ISROVERRIDE {
  UINT8 vector;                             ///< Vector # we want to override
  VOID* isr;                                ///< Address of the ISR
} ISROVERRIDE;

//////////////////////////////////
// Example list for MSR probing //
//////////////////////////////////

ISROVERRIDE isrlist_probingmode[] = {
  { 0x05,   &monkey_isr_5, },               ///< (#BR) Bound Range Exceeded
  { 0x06,   &monkey_isr_6, },               ///< (#UD) Invalid Opcode
  { 0x08,   &monkey_isr_8, },               ///< (#DF) Double Fault
  { 0x0D,   &monkey_isr_13, },              ///< (#GP) General Protection Fault
  { 0x12,   &monkey_isr_18, },              ///< (#MC) Machine Check
};

/*******************************************************************************
* Backup table will be used when completing the hook
*******************************************************************************/

IDT gBackupIDT[256] = { 0 };                ///< Backup of original IDT
IDT* sysIDT;

IDTR origIDTR = { 0 };
UINT32 gISRsPatched = 0;

/*******************************************************************************
* PatchIDTEntry
******************************************************************************/

// Overwrite the IDT entry at isro->vector with the address of isro->isr.
// TypeAddr 0x8E = present (bit7) + DPL0 (bits 6:5=00) + Interrupt Gate (0xE).
// The 64-bit ISR address is split across OffsetLow (15:0), OffsetHigh (31:16),
// and OffsetUpper (63:32) fields as per the x86-64 IDT descriptor layout.
VOID PatchIDTEntry(IDT* idtBase, ISROVERRIDE* isro)
{
  //
  // Locate the position in IDT for this ISR
  
  IDT* dest = idtBase + isro->vector;

  //
  // And patch...

  UINT64 isrAddr = (UINT64)isro->isr;

  dest->u1.OffsetLow = (UINT16) isrAddr;
  dest->u1.OffsetHigh = (UINT16)(isrAddr >> 16);
  dest->u1.OffsetUpper = (UINT32)(isrAddr >> 32);
  dest->u1.TypeAddr = 0x8E;                         ///< See explanation above
}

/*******************************************************************************
* UnpatchIDTEntry
******************************************************************************/

// Restore one IDT entry to its original value from gBackupIDT[].
VOID UnpatchIDTEntry(IDT* idtBase, ISROVERRIDE* isro)
{
  //
  // Unpatch 

  IDT* dest = idtBase + isro->vector;
  IDT* orig = gBackupIDT + isro->vector;

  CopyMem(dest, orig, sizeof(IDT));
}


/*******************************************************************************
* ApplyISRPatchTable
******************************************************************************/

// Apply or remove the entire ISROVERRIDE list (doUnapply=0 to install,
// doUnapply=1 to restore).  Interrupts are disabled for the duration.
// Sets gISRsPatched=1 after patching so RemoveAllInterruptOverrides can tell
// that a valid backup exists.
VOID ApplyISRPatchTable( ISROVERRIDE *isrs,
  const INTN isrCount,
  const UINT8 doUnapply)
{
  //
  // Probably a good idea to disable interrupts now...
  
  DisableInterruptsOnThisCpu();

  ///////////////////
  // (Un)patch the //
  // entire list   //
  ///////////////////
  
  //
  // Compiler will optimize the loop and move the if() out
  // Let's not make the code uglier than it already is...
    
  for (INTN idx = 0; idx < isrCount; idx++) {
    if (!doUnapply) {
      PatchIDTEntry(sysIDT, isrs + idx);
    }
    else {
      UnpatchIDTEntry(sysIDT, isrs + idx);
    }    
  }

  //
  // And... action
  // (or crash)
  
  gISRsPatched = 1;
  
  EnableInterruptsOnThisCpu();
}


/*******************************************************************************
* InstallSafeAsmExceptionHandler
******************************************************************************/

// Save the current IDT to gBackupIDT[], then install the monkey ISRs from
// isrlist_probingmode.  Called before any speculative MSR/MMIO probe so that
// #GP, #UD, #DF, #BR, and #MC are redirected to stub handlers that store the
// faulting vector in CR2 for detection by HwAccess.c.
VOID InstallSafeAsmExceptionHandler(VOID)
{
  //
  // Locate current IDTR

  GetCurrentIdtr(&origIDTR);

  //
  // Copy current IDT table to backup
  // so we can restore it after we are done

  sysIDT = (IDT *)origIDTR.Base;
  
  CopyMem( &gBackupIDT[0], sysIDT, sizeof(IDT) * 256);

  //
  // Now that we have backups, we can start hacking
  // I mean patching...
 
  ApplyISRPatchTable(isrlist_probingmode,
    sizeof(isrlist_probingmode) / sizeof(ISROVERRIDE), 0);
    
}

/******************************************************************************
* RemoveAllInterruptOverrides
******************************************************************************/

// Bulk-restore all 256 IDT entries from gBackupIDT[] with interrupts disabled.
// No-op if gISRsPatched is 0 (backup was never taken).
VOID RemoveAllInterruptOverrides(VOID)
{
  //
  // gISRsPatched == 1 means there is a valid IDT backup
  
  if (gISRsPatched) {

    DisableInterruptsOnThisCpu();

    CopyMem(sysIDT, &gBackupIDT[0], sizeof(IDT) * 256);

    gISRsPatched = 0;

    EnableInterruptsOnThisCpu();
  }
}
