// CpuInfo.h — CPUINFO structure populated from CPUID leaves; includes
//             family/model/stepping, hybrid architecture flag, and brand strings.
#pragma once

/*******************************************************************************
 * CpuInfo Structure
 ******************************************************************************/

typedef struct _CPUINFO
{
  UINT8 venString[64];
  UINT8 brandString[8];

  UINT32 f1;
  UINT32 maxf;
  UINT32 family;
  UINT32 model;
  UINT32 stepping;
 
  BOOLEAN HybridArch;  
  BOOLEAN ECore;

} CPUINFO;


/*******************************************************************************
 * GetCpuInfo
 ******************************************************************************/

void GetCpuInfo(CPUINFO* ci);