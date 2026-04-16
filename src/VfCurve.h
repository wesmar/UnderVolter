// VfCurve.h — OC Mailbox V/F curve probe and programming interface:
//             per-domain IccMax, legacy VF override, and per-point VF curve writes.
#pragma once

#include "Platform.h"

/*******************************************************************************
 * MSRs used by VfCurve and related modules
 ******************************************************************************/

#define MSR_OC_MAILBOX                  0x150
#define MSR_FLEX_RATIO                  0x194
#define MSR_TURBO_RATIO_LIMIT           0x1AD
#define MSR_TURBO_RATIO_LIMIT_ECORE     0x650
#define MSR_POWER_CONTROL               0x1FC
#define MSR_VR_CURRENT_CONFIG           0x601
#define MSR_PACKAGE_POWER_SKU_UNIT      0x606
#define MSR_PACKAGE_POWER_LIMIT         0x610
#define MSR_PKG_POWER_INFO              0x614
#define MSR_PL3_CONTROL                 0x615
#define MSR_PP0_POWER_LIMIT             0x638
#define MSR_PLATFORM_POWER_LIMIT        0x65C
#define MSR_CONFIG_TDP_CONTROL          0x64B

 /*******************************************************************************
  * MCHBAR Registers
  ******************************************************************************/

#define MMIO_PACKAGE_POWER_LIMIT        0x59A0
#define MMIO_PACKAGE_POWER_LIMIT_HI     0x59A4

/*******************************************************************************
 *
 ******************************************************************************/

EFI_STATUS EFIAPI IAPERF_ProbeDomainVF(
  IN const UINT8 domain, OUT DOMAIN *desc );

/*******************************************************************************
 *
 ******************************************************************************/

EFI_STATUS EFIAPI IAPERF_ProgramDomainVF(IN const UINT8 domIdx,
  IN OUT DOMAIN* dom, 
  IN const UINT8 programVfPoints,
  IN const UINT8 programIccMax);

/*******************************************************************************
 *
 ******************************************************************************/

VOID IaCore_OcLock(VOID);
