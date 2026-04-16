// CpuDataVR.c — VR topology discovery templates (VOLTCFGTEMPLATE) for each
//               supported microarchitecture: per-domain OC Mailbox bit masks for
//               VR address extraction and SVID capability detection (cmd 0x04).
#include <Uefi.h>
#include <Library/UefiLib.h>

#include "CpuInfo.h"
#include "CpuData.h"

// ─── OC Mailbox command 0x04 response bit layout (per domain) ────────────────
// The 32-bit data field returned by cmd 0x04 packs VR address and SVID flags
// for all domains into a single DWORD.  Each VOLTPLANEDESC entry holds the
// domain-specific bit mask and shift to extract:
//   VR address  = (data & OCMB_VRAddr_DomainBitMask) >> OCMB_VRAddr_DomainBitShift
//   SVID cap    = (data & OCMB_VRsvid_DomainBitMask) != 0  → 0=SVID, 1=no SVID
// Bit positions differ between client (SKL/CFL/CML/RKL) and server (SKX/CLX)
// topologies because the server omits GT domains entirely.
/*******************************************************************************
 * Templates for VR Topology Discovery
 ******************************************************************************/

/////////////////////////
/// CFL/CML/RKL Client //
/////////////////////////

VOLTCFGTEMPLATE vcfg_q_xyzlake_client = 
{

  {

    ///
    /// IA Cores
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x1E0,                          // Bit mask for retrieving VR Addr (OCMB)
      5,                              // VR Addr bits to shift right

      0x200,                          // Bit mask for asking if this VR is SVID
      9,                              // VR svid capa bits to shift right

      "CORE",                         // Friendly Name (Short)
    },

    ///
    /// GT Slice
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0xF00000,                       // Bit mask for retrieving VR Addr (OCMB)
      20,                             // VR Addr bits to shift right

      0x1000000,                      // Bit mask for asking if this VR is SVID
      24,                             // VR svid capa bits to shift right

      "GTSLICE",
    },

    ///
    /// Ring / Cache
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x3C00,                         // Bit mask for retrieving VR Addr (OCMB)
      10,                             // VR Addr bits to shift right

      0x4000,                         // Bit mask for asking if this VR is SVID
      14,                             // VR svid capa bits to shift right

      "RING",
    },

    ///
    /// GT Unslice
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x78000,                        // Bit mask for retrieving VR Addr (OCMB)
      15,                             // VR Addr bits to shift right

      0x80000,                        // Bit mask for asking if this VR is SVID
      19,                             // VR svid capa bits to shift right

      "GTUNSLICE",
    },

    ///
    /// UNCORE / SA
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0xF,                            // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0x10,                           // Bit mask for asking if this VR is SVID
      0x4,                            // VR svid capa bits to shift right

      "UNCORE",
    },

    ///
    ///
    /// 

    {
      0x00,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RESERVED",
    }
  }
};

//////////////////////
/// SKX/CLX HEDT/WS //
//////////////////////

VOLTCFGTEMPLATE vcfg_q_xyzlake_server =
{

  {

    ///
    /// IA Cores
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x1E0,                          // Bit mask for retrieving VR Addr (OCMB)
      5,                              // VR Addr bits to shift right

      0x200,                          // Bit mask for asking if this VR is SVID
      9,                              // VR svid capa bits to shift right

      "CORE",                         // Friendly Name (Short)
    },

    ///
    /// 
    /// 

    {
      0x00,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RESERVED",
    },

    ///
    /// Ring / Cache
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x3C00,                         // Bit mask for retrieving VR Addr (OCMB)
      10,                             // VR Addr bits to shift right

      0x4000,                         // Bit mask for asking if this VR is SVID
      14,                             // VR svid capa bits to shift right

      "RING",
    },

    ///
    /// 
    /// 

    {
      0x00,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RESERVED",
    },

    ///
    /// UNCORE / SA
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0xF,                            // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0x10,                           // Bit mask for asking if this VR is SVID
      0x4,                            // VR svid capa bits to shift right

      "UNCORE",
    },

    ///
    ///
    /// 

    {
      0x00,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RESERVED",
    }
  }
};


///////////////////////////////////////////////////////////
/// TGL — VR topology incomplete, IccMax programming N/A //
/// TGL uses a more complex multi-VR arrangement;        //
/// VR addresses not yet confirmed, discovery disabled.  //
/////////////////////////////////////////////////////////// 

VOLTCFGTEMPLATE vcfg_q_tigerlake_client =
{

  {

    ///
    /// IA Cores
    /// 

    {
      0x01,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "CORE",                         // Friendly Name (Short)
    },

    ///
    /// GT
    /// 

    {
      0x01,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "GT",
    },

    ///
    /// Ring / Cache
    /// 

    {
      0x01,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RING",
    },

    ///
    /// RESERVED
    /// 

    {
      0x00,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RESERVED",
    },

    ///
    /// UNCORE / SA
    /// 

    {
      0x01,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0x0,                            // Bit mask for asking if this VR is SVID
      0x0,                            // VR svid capa bits to shift right

      "UNCORE",
    },

    ///
    /// RESERVED
    /// 

    {
      0x00,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0,                              // Bit mask for retrieving VR Addr (OCMB)
      0,                              // VR Addr bits to shift right

      0,                              // Bit mask for asking if this VR is SVID
      0,                              // VR svid capa bits to shift right

      "RESERVED",
    }
  }
};


///////////////////////////////////////////////////////////////////////////
/// Meteor Lake / Arrow Lake Client (tile architecture, E-Cores)       //
///                                                                     //
/// OC Mailbox 0x1A (Get VR Topology) bit layout for MTL/ARL:         //
///   Each domain N uses 5 bits in the 64-bit response (EAX + EDX):   //
///   VR Addr bits: [8+N*5 .. 11+N*5]  mask = 0xF << (8+N*5)         //
///   SVID flag bit: [12+N*5]           mask = 0x1 << (12+N*5)        //
///                                                                     //
///   N=0 P-Core, N=1 GT, N=2 Ring, N=3 Uncore, N=4 E-Core,          //
///   N=5 GT Unslice/Media (SoC tile)                                  //
///                                                                     //
///   N=4 SVID bit [32] and N=5 fully [33..37] fall in EDX.           //
///   VOLTPLANEDESC uses UINT32 → N=5 discovery disabled,             //
///   N=4 VR addr (bits 28-31) discoverable; SVID set to 0.           //
///   Arrow Lake: DLVR used (SVID=0 expected for most domains).        //
///                                                                     //
/// Source: community analysis (throttled, intel-undervolt) —          //
///         not official Intel spec; treat as best-effort.             //
///////////////////////////////////////////////////////////////////////////

VOLTCFGTEMPLATE vcfg_q_meteorlake_client = {
  {
    /// IACORE — P-Core compute tile (N=0)
    /// VR Addr: bits [11:8]  shift=8,  mask=0x00000F00
    /// SVID:    bit  [12]    shift=12, mask=0x00001000
    { 0x01, 0x01, 0x00000F00,  8, 0x00001000, 12, "PCORE" },

    /// GTSLICE — Graphics Tile (N=1)
    /// VR Addr: bits [16:13] shift=13, mask=0x0001E000
    /// SVID:    bit  [17]    shift=17, mask=0x00020000
    { 0x01, 0x01, 0x0001E000, 13, 0x00020000, 17, "GTSLICE" },

    /// RING — Compute Ring (N=2)
    /// VR Addr: bits [21:18] shift=18, mask=0x003C0000
    /// SVID:    bit  [22]    shift=22, mask=0x00400000
    { 0x01, 0x01, 0x003C0000, 18, 0x00400000, 22, "RING" },

    /// GTUNSLICE — Media/SoC GT (N=5, bits 33-37 → EDX)
    /// Exceeds 32-bit EAX — discovery disabled.
    /// Voltage offset via MSR 0x150 still applies.
    { 0x01, 0x00, 0, 0, 0, 0, "GTUNSLICE" },

    /// UNCORE — SoC tile System Agent (N=3)
    /// VR Addr: bits [26:23] shift=23, mask=0x07800000
    /// SVID:    bit  [27]    shift=27, mask=0x08000000
    { 0x01, 0x01, 0x07800000, 23, 0x08000000, 27, "UNCORE" },

    /// ECORE — Compute E-Cores (N=4)
    /// VR Addr: bits [31:28] shift=28, mask=0xF0000000  (fits in EAX)
    /// SVID:    bit  [32]    → EDX bit 0 — exceeds UINT32.
    ///          SVID mask=0; ARL uses DLVR (non-SVID) anyway.
    { 0x01, 0x01, 0xF0000000, 28, 0, 0, "ECORE" },
  }
};

///////////////////////////////////////////////////////////////////////////
/// Arrow Lake Client (tile arch, DLVR)                                //
///                                                                     //
/// Bit layout identical to Meteor Lake (same N*5 formula).            //
/// Key difference: ARL-S uses DLVR instead of external SVID VR —     //
/// SVID flags returned by 0x1A may be 0 for all domains.             //
/// VR addresses may differ from MTL (DLVR controller IDs).           //
/// GT Unslice (N=5) and E-Core SVID (N=4 bit 32) still in EDX.      //
/// Separate template reserved for future ARL-specific adjustments     //
/// (e.g. Panther Lake tile mapping changes, DLVR quirks).            //
///////////////////////////////////////////////////////////////////////////

VOLTCFGTEMPLATE vcfg_q_arrowlake_client = {
  {
    /// IACORE — P-Core compute tile (N=0)
    { 0x01, 0x01, 0x00000F00,  8, 0x00001000, 12, "PCORE" },

    /// GTSLICE — Graphics Tile (N=1)
    { 0x01, 0x01, 0x0001E000, 13, 0x00020000, 17, "GTSLICE" },

    /// RING — Compute Ring (N=2)
    { 0x01, 0x01, 0x003C0000, 18, 0x00400000, 22, "RING" },

    /// GTUNSLICE — Media/SoC GT (N=5 → EDX), discovery disabled
    { 0x01, 0x00, 0, 0, 0, 0, "GTUNSLICE" },

    /// UNCORE — SoC tile System Agent (N=3)
    { 0x01, 0x01, 0x07800000, 23, 0x08000000, 27, "UNCORE" },

    /// ECORE — Compute E-Cores (N=4, VR addr in EAX, SVID in EDX)
    { 0x01, 0x01, 0xF0000000, 28, 0, 0, "ECORE" },
  }
};

//////////////////////////
/// Alder Lake Client  ///
//////////////////////////

VOLTCFGTEMPLATE vcfg_q_alderlake_client = {

  {

    ///
    /// IA Cores
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0xF00,                          // Bit mask for retrieving VR Addr (OCMB)
      8,                              // VR Addr bits to shift right

      0x1000,                         // Bit mask for asking if this VR is SVID
      12,                             // VR svid capa bits to shift right

      "PCORE",                        // Friendly name - short
    },

    ///
    /// GT Slice
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x1E000,                        // Bit mask for retrieving VR Addr (OCMB)
      13,                             // VR Addr bits to shift right

      0x800000,                       // Bit mask for asking if this VR is SVID
      17,                             // VR svid capa bits to shift right

      "GTSLICE",
    },

    ///
    /// Ring / Cache
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0xF00,                          // Bit mask for retrieving VR Addr (OCMB)
      8,                              // VR Addr bits to shift right

      0x1000,                         // Bit mask for asking if this VR is SVID
      12,                             // VR svid capa bits to shift right

      "RING",
    },

    ///
    /// GT Unslice
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0x1E000,                        // Bit mask for retrieving VR Addr (OCMB)
      13,                             // VR Addr bits to shift right

      0x800000,                       // Bit mask for asking if this VR is SVID
      17,                             // VR svid capa bits to shift right

      "GTUNSLICE",
    },

    ///
    /// UNCORE / SA
    /// 

    {
      0x01,                           // Domain exists?
      0x00,                           // Domain Supported for OCMB discovery?

      0x0,                            // Bit mask for retrieving VR Addr (OCMB)
      0x0,                            // VR Addr bits to shift right

      0x0,                            // Bit mask for asking if this VR is SVID
      0x0,                            // VR svid capa bits to shift right

      "UNCORE",
    },

    ///
    /// E-Cores
    /// 

    {
      0x01,                           // Domain exists?
      0x01,                           // Domain Supported for OCMB discovery?

      0xF00,                          // Bit mask for retrieving VR Addr (OCMB)
      8,                              // VR Addr bits to shift right

      0x1000,                         // Bit mask for asking if this VR is SVID
      12,                             // VR svid capa bits to shift right

      "ECORE",
    }
  }
};

