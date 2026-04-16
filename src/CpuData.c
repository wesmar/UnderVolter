// CpuData.c — Per-CPU microarchitecture table (gCpuConfigTable) and DetectCpu():
//             matches running CPU's CPUID family/model/stepping, probes BCLK via
//             OC Mailbox command 0x05, and sets gActiveCpuData for downstream use.
#include <Uefi.h>
#include <Library/UefiLib.h>
#include "UiConsole.h"

#include "CpuInfo.h"
#include "CpuData.h"
#include "OcMailbox.h"
#include "MiniLog.h"
#include "HwIntrinsicsX64.h"

CPUINFO gCpuInfo = { 0 };

// BCLK in kHz as reported by OC Mailbox command 0x05.
// Single value for the whole system — multi-socket platforms with per-package
// BCLKs are not a realistic use-case for this tool.
UINT32 gBCLK_bsp = 0;

/*******************************************************************************
 *
 ******************************************************************************/


/*******************************************************************************
 * CPU DATA + PER-CPU PATCHES/WORKAROUNDS
 ******************************************************************************/

///
/// Data Sources,
/// - https,//github.com/erpalma/throttled  (CPU IDs)
/// 

// ─── Per-microarchitecture configuration table ────────────────────────────────
// Fields per entry: {family,model,stepping}, uArch name, hasUnlimitedIccMaxFlag,
//   IccMaxBits (10 for pre-TGL, 11 for ADL+), VfPointsExposed, HasEcores, vtdt.
// Entry [0] is the "unknown CPU" fallback; gActiveCpuData defaults to it.
// Table is searched linearly by DetectCpu(); first exact match wins.
CPUCONFIGTABLE gCpuConfigTable[] = {

  //
  // Special entry for undetected CPUs

  { {0, 0, 0} , "Unknown", 0, 10, 0, 0, NULL },

  //
  // "Known" CPUs (not necessarily supported or tested)

  { {6, 26, 1} , "Nehalem", 0, 10, 0, 0, NULL },
  { {6, 26, 2} , "Nehalem-EP", 0, 10, 0, 0, NULL },
  { {6, 26, 4} , "Bloomfield", 0, 10, 0, 0, NULL },
  { {6, 28, 2} , "Silverthorne", 0, 10, 0, 0, NULL },
  { {6, 28, 10} , "PineView", 0, 10, 0, 0, NULL },
  { {6, 29, 0} , "Dunnington-6C", 0, 10, 0, 0, NULL },
  { {6, 29, 1} , "Dunnington", 0, 10, 0, 0, NULL },
  { {6, 30, 0} , "Lynnfield", 0, 10, 0, 0, NULL },
  { {6, 30, 5} , "Lynnfield_CPUID", 0, 10, 0, 0, NULL },
  { {6, 31, 1} , "Auburndale", 0, 10, 0, 0, NULL },
  { {6, 37, 2} , "Clarkdale", 0, 10, 0, 0, NULL },
  { {6, 38, 1} , "TunnelCreek", 0, 10, 0, 0, NULL },
  { {6, 39, 2} , "Medfield", 0, 10, 0, 0, NULL },
  { {6, 42, 2} , "SandyBridge", 0, 10, 0, 0, NULL },
  { {6, 42, 6} , "SandyBridge", 0, 10, 0, 0, NULL },
  { {6, 42, 7} , "Sandy Bridge-DT", 0, 10, 0, 0, NULL },
  { {6, 44, 1} , "Westmere-EP", 0, 10, 0, 0, NULL },
  { {6, 44, 2} , "Gulftown", 0, 10, 0, 0, NULL },
  { {6, 45, 5} , "Sandy Bridge-EP", 0, 10, 0, 0, NULL },
  { {6, 45, 6} , "Sandy Bridge-E", 0, 10, 0, 0, NULL },
  { {6, 46, 4} , "Beckton", 0, 10, 0, 0, NULL },
  { {6, 46, 5} , "Beckton", 0, 10, 0, 0, NULL },
  { {6, 46, 6} , "Beckton", 0, 10, 0, 0, NULL },
  { {6, 47, 2} , "Eagleton", 0, 10, 0, 0, NULL },
  { {6, 53, 1} , "Cloverview", 0, 10, 0, 0, NULL },
  { {6, 54, 1} , "Cedarview-D", 0, 10, 0, 0, NULL },
  { {6, 54, 9} , "Centerton", 0, 10, 0, 0, NULL },
  { {6, 55, 3} , "Bay Trail-D", 0, 10, 0, 0, NULL },
  { {6, 55, 8} , "Silvermont", 0, 10, 0, 0, NULL },
  { {6, 58, 9} , "Ivy Bridge-DT", 0, 10, 0, 0, NULL },
  { {6, 60, 3} , "Haswell-DT", 0, 10, 0, 0, NULL },
  { {6, 61, 4} , "Broadwell-U", 0, 10, 0, 0, NULL },
  { {6, 62, 3} , "IvyBridgeEP", 0, 10, 0, 0, NULL },
  { {6, 62, 4} , "Ivy Bridge-E", 0, 10, 0, 0, NULL },
  { {6, 63, 2} , "Haswell-EP", 0, 10, 0, 0, NULL },
  { {6, 69, 1} , "HaswellULT", 0, 10, 0, 0, NULL },
  { {6, 70, 1} , "Crystal Well-DT", 0, 10, 0, 0, NULL },
  { {6, 71, 1} , "Broadwell-H", 0, 10, 0, 0, NULL },
  { {6, 76, 3} , "Braswell", 0, 10, 0, 0, NULL },
  { {6, 77, 8} , "Avoton", 0, 10, 0, 0, NULL },
  { {6, 78, 3} , "Skylake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 79, 1} , "BroadwellE", 0, 10, 0, 0, NULL },
  { {6, 85, 4} , "SkylakeXeon", 0, 10, 0, 0, &vcfg_q_xyzlake_server },
  { {6, 85, 6} , "CascadeLakeSP", 0, 10, 0, 0, &vcfg_q_xyzlake_server },
  { {6, 85, 7} , "CascadeLakeXeon2", 0, 10, 0, 0, &vcfg_q_xyzlake_server },
  { {6, 86, 2} , "BroadwellDE", 0, 10, 0, 0, NULL },
  { {6, 86, 4} , "BroadwellDE", 0, 10, 0, 0, NULL },
  { {6, 87, 0} , "KnightsLanding", 0, 10, 0, 0, NULL },
  { {6, 87, 1} , "KnightsLanding", 0, 10, 0, 0, NULL },
  { {6, 90, 0} , "Moorefield", 0, 10, 0, 0, NULL },
  { {6, 92, 9} , "Apollo Lake", 0, 10, 0, 0, NULL },
  { {6, 93, 1} , "SoFIA", 0, 10, 0, 0, NULL },
  { {6, 94, 0} , "Skylake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 94, 3} , "Skylake-S", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 95, 1} , "Denverton", 0, 10, 0, 0, NULL },
  { {6, 102, 3} , "Cannon Lake-U", 0, 10, 0, 0, NULL },
  { {6, 117, 10} , "Spreadtrum", 0, 10, 0, 0, NULL },
  { {6, 106, 0} , "IceLake-SP", 0, 10, 1, 0, &vcfg_q_xyzlake_server },       // QEMU (debug)
  { {6, 106, 6} , "IceLake-SP", 0, 10, 0, 0, &vcfg_q_xyzlake_server },
  { {6, 122, 1} , "Gemini Lake-D", 0, 10, 0, 0, NULL },
  { {6, 122, 8} , "GoldmontPlus", 0, 10, 0, 0, NULL },
  { {6, 126, 5} , "IceLakeY", 0, 10, 0, 0, NULL },  
  { {6, 138, 1} , "Lakefield", 0, 10, 0, 0, NULL },
  { {6, 140, 1} , "TigerLake", 0, 10, 1, 0, &vcfg_q_tigerlake_client },
  { {6, 141, 1} , "TigerLake", 0, 10, 1, 0, &vcfg_q_tigerlake_client },
  { {6, 142, 9} , "Kabylake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 142, 10} , "Kabylake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 142, 11} , "WhiskeyLake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 142, 12} , "Comet Lake-U", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 156, 0} , "JasperLake", 0, 10, 0, 0, NULL },
  { {6, 158, 9} , "KabylakeG", 0, 10, 0, 0, NULL },
  { {6, 158, 10} , "Coffeelake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 158, 11} , "Coffeelake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 158, 12} , "CoffeeLake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 158, 13} , "CoffeeLake", 0, 10, 0, 0, &vcfg_q_xyzlake_client },
  { {6, 165, 2} , "CometLake", 0, 10, 1, 0, &vcfg_q_xyzlake_client },
  { {6, 165, 4} , "CometLake", 0, 10, 1, 0, &vcfg_q_xyzlake_client },
  { {6, 165, 5} , "CometLake-S", 0, 10, 1, 0, &vcfg_q_xyzlake_client },
  { {6, 166, 0} , "CometLake", 0, 10, 1, 0, &vcfg_q_xyzlake_client },
  
  { {6, 167, 0} , "RocketLake", 0, 10, 1, 0, &vcfg_q_xyzlake_client },       // RKL-S ES
  { {6, 167, 1} , "RocketLake", 0, 10, 1, 0, &vcfg_q_xyzlake_client },       // RKL-S QS/PRQ
  
  { {6, 151, 0} , "AlderLake", 0, 11, 1, 1, &vcfg_q_alderlake_client },      // (90670)
  { {6, 151, 1} , "AlderLake-S", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-S ES2 (90671)
  { {6, 151, 2} , "AlderLake-S", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-S QS/PRQ (90672)
  { {6, 151, 4} , "AlderLake-S", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-S (90674)
  { {6, 151, 5} , "AlderLake-S", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-S QS/PRQ (90675)
  { {6, 154, 2} , "AlderLake-H/P", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-H/P (906A2)
  { {6, 154, 3} , "AlderLake-H/P", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-H/P (906A3)
  { {6, 154, 4} , "AlderLake-H/P", 0, 11, 1, 1, &vcfg_q_alderlake_client },    // ADL-H/P (906A4)
  { {6, 154, 1} , "AlderLake",     0, 11, 1, 1, &vcfg_q_alderlake_client },

  //
  // Raptor Lake (13th Gen) — same FIVR/OCMB topology as Alder Lake

  { {6, 183, 1} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-S B0 desktop (B0700)
  { {6, 183, 2} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-S C0 desktop (B0702)
  { {6, 186, 2} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-P/H B0 mobile (BA02)
  { {6, 186, 3} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-P/HX C0 mobile (BA03)

  //
  // Raptor Lake Refresh (14th Gen) — die-identical to RPL, same topology

  { {6, 191, 2} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-S Refresh B0 desktop (BF02)
  { {6, 191, 3} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-S Refresh C0 desktop (BF03)
  { {6, 186, 4} , "RaptorLake",   0, 11, 1, 1, &vcfg_q_alderlake_client },    // RPL-HX Refresh mobile (BA04)

  //
  // Meteor Lake (Core Ultra 1xx, 14th Gen laptop)
  // Tile architecture: compute tile (P+E+Ring) + SoC tile (GT+SA)
  // OC Mailbox present but VR topology bit fields not confirmed — discovery disabled

  { {6, 170, 4} , "MeteorLake",   0, 11, 1, 1, &vcfg_q_meteorlake_client },   // MTL-M/P (Core Ultra 1xx H/U, AA04)

  //
  // Arrow Lake (Core Ultra 200S/HX, 15th Gen)
  // Disaggregated tiles: compute (Intel 20A) + SoC/IO (Intel 3 TSMC N6)
  // DLVR (Digital Linear Voltage Regulator) replaces external SVID VR on ARL-S.
  // OC Mailbox topology bit layout identical to MTL; VR addresses may differ.

  { {6, 197, 2} , "ArrowLake",    0, 11, 1, 1, &vcfg_q_arrowlake_client },    // ARL-S B0 desktop (C502, Core Ultra 200S)
  { {6, 198, 2} , "ArrowLake",    0, 11, 1, 1, &vcfg_q_arrowlake_client },    // ARL-HX B0 mobile  (C602, Core Ultra 200HX)
};


/*******************************************************************************
 * Active CPU Data (to be used)
 ******************************************************************************/

CPUCONFIGTABLE* gActiveCpuData = &gCpuConfigTable[0];

/*******************************************************************************
 * DetectCpu
 ******************************************************************************/

// Populate gCpuInfo via CPUID, read BCLK from OC Mailbox (cmd 0x05, fallback
// sub-command 0x01), then scan gCpuConfigTable for a matching entry.
// Returns TRUE when a known CPU is found; gActiveCpuData is always set.
BOOLEAN DetectCpu()
{
  //
  // CPUID

  GetCpuInfo(&gCpuInfo);

  //
  // Detect BIOS-limited maximum CPUID input value 
  // as it would interfere with our ability to detect features

  if (gCpuInfo.maxf <= 2) 
  {
    UiAsciiPrint("WARNING: possible maximum CPUID input value limitation detected!\n");
    UiAsciiPrint("This condition can interfere with CPU feature detection.\n");
    UiAsciiPrint("For ensuring correct operation, it is advisable to disable CPUID limit.\n");
    UiAsciiPrint("This is typically done in BIOS Setup.\n");
    UiAsciiPrint("\n");
  }
   

  //
  // BCLK

  gBCLK_bsp = 100000;

  {
    CpuMailbox box;
    MailboxBody* b = &box.b;
    UINT32 cmd = 0;

    OcMailbox_InitializeAsMSR(&box);

    MiniTraceEx("Reading BCLK frequency from OC Mailbox");

    cmd = OcMailbox_BuildInterface(0x5, 0, 0);

    if ((!EFI_ERROR(OcMailbox_ReadWrite(cmd, 0, &box)))&&((box.status == 0))) {
      gBCLK_bsp = b->box.data;
    }

    //
    // Try this in caes of failure

    if (!gBCLK_bsp) {

      cmd = OcMailbox_BuildInterface(0x5, 1, 0);

      if ((!EFI_ERROR(OcMailbox_ReadWrite(cmd, 0, &box))) && ((box.status == 0))) {
        gBCLK_bsp = b->box.data;
      }
    }

    //
    // QEMU and some hypervisors return 0 or an implausibly small value;
    // fall back to the standard 100 MHz BCLK (100 000 kHz).

    if (gBCLK_bsp < 5000) {
      gBCLK_bsp = 100000;
    }
  }

  //
  // Detect CPU

  for (UINTN ccnt=0;ccnt<sizeof(gCpuConfigTable)/sizeof(CPUCONFIGTABLE);ccnt++) {

    CPUCONFIGTABLE* pt = &gCpuConfigTable[ccnt];

    if ( 
      (gCpuInfo.model == pt->cpuType.model) && 
      (gCpuInfo.family == pt->cpuType.family) && 
      (gCpuInfo.stepping == pt->cpuType.stepping)) 
    {
      gActiveCpuData = pt;
      
      UiAsciiPrint("Detected CPU: %a, model: %u, family: %u, stepping: %u\n",
        pt->uArch, pt->cpuType.model, pt->cpuType.family, pt->cpuType.stepping);

      return TRUE;
    }
  }

  return FALSE;
}
