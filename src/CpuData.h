// CpuData.h — Per-microarchitecture CPU configuration table: CPUID triplets,
//             VR topology templates, IccMax bit-width, and V/F curve capabilities.
#pragma once

#include "Platform.h"

/*******************************************************************************
 * VR address sentinel and type flags
 ******************************************************************************/

#define INVALID_VR_ADDR                                                 0xFF
#define NO_SVID_VR                                                      0x01
#define SVID_VR                                                         0x00

/*******************************************************************************
 * VR Configuration Templates (per domain, used during topo discovery)
 ******************************************************************************/

typedef struct _VOLTPLANEDESC {

  UINT8 DomainExists;
  UINT8 DomainSupportedForDiscovery;

  //
  // For SKUs with OC Mailbox support for VR topology discovery

  UINT32 OCMB_VRAddr_DomainBitMask;         // Bit mask for VR Addr
  UINT32 OCMB_VRAddr_DomainBitShift;        // bits to shift 

  UINT32 OCMB_VRsvid_DomainBitMask;         // Bit mask for VR SVID capa bit
  UINT32 OCMB_VRsvid_DomainBitShift;        // bits to shift

  CHAR8 FriendlyNameShort[32];

} VOLTPLANEDESC;

/*******************************************************************************
 * VR Configuration
 ******************************************************************************/

typedef struct _VOLTCFGTEMPLATE
{
  VOLTPLANEDESC doms[MAX_DOMAINS];

} VOLTCFGTEMPLATE;

/*******************************************************************************
 * 
 ******************************************************************************/

typedef struct _CPUTYPE {
  UINT32 family;
  UINT32 model;
  UINT32 stepping;
} CPUTYPE;

typedef struct _CPUCONFIGTABLE {

  CPUTYPE cpuType;
  CHAR8 uArch[32];

  BOOLEAN hasUnlimitedIccMaxFlag;   // Extra bit controlling unlimited IccMax
  UINT8 IccMaxBits;                 // ADL=11bits, RKL and lower=10bits
  UINT8 VfPointsExposed;            // OC Mailbox exposes V/F points (param2)
  UINT8 HasEcores;                  // Has E Cores

  VOLTCFGTEMPLATE* vtdt;            // Template for VR topo discovery (or null)

} CPUCONFIGTABLE;

/*******************************************************************************
 * DetectCpu
 ******************************************************************************/

BOOLEAN DetectCpu();

/*******************************************************************************
 * Detected CPU Data
 ******************************************************************************/

extern CPUCONFIGTABLE* gActiveCpuData;
extern CPUINFO gCpuInfo;
extern UINT32 gBCLK_bsp;

/*******************************************************************************
 * Available VR discovery templates
 ******************************************************************************/

extern VOLTCFGTEMPLATE vcfg_q_xyzlake_client;       // SKL/CFL/CML/RKL Client
extern VOLTCFGTEMPLATE vcfg_q_xyzlake_server;       // SKX/CLX HEDT/WS/Server
extern VOLTCFGTEMPLATE vcfg_q_tigerlake_client;     // TGL Client
extern VOLTCFGTEMPLATE vcfg_q_alderlake_client;     // ADL/RPL Client
extern VOLTCFGTEMPLATE vcfg_q_meteorlake_client;    // MTL Client (tile arch)
extern VOLTCFGTEMPLATE vcfg_q_arrowlake_client;     // ARL Client (tile arch, DLVR)
