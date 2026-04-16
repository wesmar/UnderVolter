// CpuInfo.c — Populate a CPUINFO structure from CPUID leaves:
//             brand string (0x80000002-4), vendor string and max leaf (0x0),
//             family/model/stepping (0x1), hybrid arch flag (0x7 EDX bit15),
//             and E-Core detection via leaf 0x1A core type field.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include "HwIntrinsicsX64.h"
#include "CpuInfo.h"
#include "Constants.h"

/*******************************************************************************
 * 
 ******************************************************************************/

#define CPUID_EAX   0
#define CPUID_EBX   1
#define CPUID_ECX   2
#define CPUID_EDX   3

/*******************************************************************************
 * 
 ******************************************************************************/

extern void* memset(void* str, int c, UINTN n);

/*******************************************************************************
 * GetCpuInfo
 ******************************************************************************/

// Fill *ci by executing the relevant CPUID leaves on the calling logical CPU.
// Extended family/model correction (Intel SDM Vol.2 Table 3-8):
//   model  |= extended_model  << 4  (always for family 6 and 15)
//   family |= extended_family << 8  (only for family 15)
// Hybrid detection: CPUID 0x7 EDX bit15 = 1 means the package has mixed core
// types.  CPUID 0x1A EAX[31:24] == 0x20 identifies an E-Core (Gracemont).
void GetCpuInfo(CPUINFO* ci)
{
  memset(ci, 0, sizeof(CPUINFO));

  //////////////////
  // Brand String //
  //////////////////
  
  UINT32* venstr = (UINT32 *)ci->venString;
  UINT32* brandstr = (UINT32*)ci->brandString;

  // Leaves 0x80000002-4 return 16 bytes of brand string each (48 bytes total)
  AsmCpuidRegisters(0x80000002, venstr);
  AsmCpuidRegisters(0x80000003, venstr+4);
  AsmCpuidRegisters(0x80000004, venstr+12);

  ///////////////////
  // Vendor String //
  ///////////////////
  
  UINT32 regs[4] = {0};

  AsmCpuidRegisters(0, regs);
  
  UINT32 hscall = ci->maxf = regs[CPUID_EAX];
  
  brandstr[0] = regs[CPUID_EBX];
  brandstr[1] = regs[CPUID_EDX];
  brandstr[2] = regs[CPUID_ECX];
 
  AsmCpuidRegisters(0x01, regs);

  ci->f1 = regs[CPUID_EAX];
  ci->stepping =  regs[CPUID_EAX] & 0x0000000F;
  ci->family =   (UINT32)(regs[CPUID_EAX] & 0x00000F00) >> 8;
  ci->model =    (UINT32)(regs[CPUID_EAX] & 0x000000F0) >> 4;

  if ((ci->family == 0xF) || (ci->family == 0x6)) {
    ci->model  |= (UINT32)((regs[CPUID_EAX] & 0x0000F0000) >> 12);
    ci->family |= (UINT32)((regs[CPUID_EAX] & 0x00FF00000) >> 16);
  }
  
  ///////////////////////////////////
  // Hybrid Architecture Detection //
  ///////////////////////////////////
  
  AsmCpuidRegisters(0x7, regs);

  ci->ECore = 0;
  ci->HybridArch = ((regs[CPUID_EDX] & bit15u32)) ? 1 : 0;

  if (ci->HybridArch) {
    if (hscall >= 0x1A) {
      // CPUID 0x1A (Hybrid Information): EAX[31:24] = core type
      //   0x40 = P-Core (Redwood Cove / Golden Cove)
      //   0x20 = E-Core (Gracemont / Crestmont)
      AsmCpuidRegisters(0x1A, regs);

      UINT32 ct = ((regs[CPUID_EAX] & 0xFF000000) >> 24);

      if (ct == 0x20) {
        ci->ECore = 1;
      }
    }
  }
}