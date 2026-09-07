// Platform.c — CPU platform topology discovery and policy application:
//              enumerates packages/cores via EFI_MP_SERVICES_PROTOCOL, probes
//              VR topology and V/F data per domain, then programs MSR/MMIO targets.
#include <PiPei.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include "UiConsole.h"
#include <Library/MemoryAllocationLib.h>
#include <Protocol/MpService.h>

#include "Platform.h"
#include "MpDispatcher.h"
#include "VfCurve.h"
#include "MiniLog.h"

#include "TurboRatioLimits.h"
#include "PowerLimits.h"
#include "Constants.h"
#include "OcMailbox.h"
#include "DelayX86.h"
#include "PrintStats.h"
#include "CpuData.h"
#include "HwAccess.h"
#include <Library/BaseMemoryLib.h>
#include "Config.h"

/*******************************************************************************
 * Globals
 ******************************************************************************/

 //
 // Initialized at startup

extern EFI_MP_SERVICES_PROTOCOL* gMpServices;
extern UINT8 gPrintPackageConfig;
extern UINT8 gPostProgrammingOcLock;
extern PLATFORM* gPlatform;

/*******************************************************************************
 * CoreIdxMap
 ******************************************************************************/

UINTN gNumCores = 0;
VOID* gCorePtrs[MAX_CORES * MAX_PACKAGES] = { 0 };
UINTN gCoreApicIDs[MAX_CORES * MAX_PACKAGES] = { 0 };

/*******************************************************************************
 * ApicIdToCoreNumber
 ******************************************************************************/

// Linear scan of the APIC-ID array; returns 0 (BSP) when not found.
UINTN ApicIdToCoreNumber(const UINTN ApicID)
{
  for (UINTN ccount = 0; ccount < gNumCores; ccount++) {
    if (gCoreApicIDs[ccount] == ApicID) {
      return ccount;
    }
  }

  return 0;
}


/*******************************************************************************
 * DiscoverVRTopology
 ******************************************************************************/

// Query OC Mailbox command 0x04 for each voltage domain to discover
// VR address and SVID capability.  Domains flagged DomainSupportedForDiscovery=0
// are skipped (BIOS mailbox would be required for those).
EFI_STATUS DiscoverVRTopology(IN OUT PACKAGE* pkg)
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // We can use OC Mailbox Function 0x04 only if we:
  //   a) know how to parse the result
  // and
  //   b) CPU actually supports this
  //

  MiniTraceEx("Detecting VR Topology");

  for (UINTN didx = 0; didx < MAX_DOMAINS; didx++) {

    DOMAIN* dom = &pkg->planes[didx];

    dom->VRaddr = INVALID_VR_ADDR;
    dom->VRtype = NO_SVID_VR;

    if (gActiveCpuData->vtdt) {
      
      VOLTPLANEDESC* vguide = &gActiveCpuData->vtdt->doms[didx];

      //
      // Domain should exist, check if OC Mailbox can be used to probe it

      if (vguide->DomainExists) {

        if (vguide->DomainSupportedForDiscovery) {

          CpuMailbox box;
          MailboxBody* b = &box.b;

          //
          // Use command 0x04 to obtain VR info

          OcMailbox_InitializeAsMSR(&box);
          UINT32 cmd = OcMailbox_BuildInterface(0x04, 0x0, 0x0);
          status = OcMailbox_ReadWrite(cmd, 0, &box);

          if (!EFI_ERROR(status) && box.status == 0) {

            const UINT32 amask  = vguide->OCMB_VRAddr_DomainBitMask;
            const UINT32 tmask  = vguide->OCMB_VRsvid_DomainBitMask;
            const UINT32 ashift = vguide->OCMB_VRAddr_DomainBitShift;

            dom->VRaddr = (UINT8)((b->box.data & amask) >> ashift);
            dom->VRtype = (UINT8)((b->box.data & tmask) ? NO_SVID_VR : SVID_VR);

          }
        }
        else {

          //
          // Domain cannot be probed by OC Mailbox
          // This means either incomplete info, 
          // or BIOS PCODE Mailbox must be used

          MiniTraceEx("Domain 0x%x cannot be probed using OC Mailbox", didx);
        }
      }
      else {
        MiniTraceEx("Skipping nonexistent voltage domain 0x%x", didx);
      }
    }
    else {
      MiniTraceEx("CPU information does not contain VR Topology discovery information");
    }
  }

  return status;
}

/*******************************************************************************
 * DomainSupported
 ******************************************************************************/

// Determine whether voltage domain didx is present on this CPU.
// Prefers the table entry when available; falls back to checking HybridArch
// for the E-Core domain (index 5) since older tables may predate ADL.
BOOLEAN VoltageDomainExists(const UINT8 didx)
{
  //
  // Check if we have this info in the table

  if (gActiveCpuData->vtdt) {
    return gActiveCpuData->vtdt->doms[didx].DomainExists;
  }
  
  //
  // Guess...

  if (didx == ECORE) {

    //
    // Check if we run on hybrid CPU

    if (gPlatform->packages[0].CpuInfo.HybridArch) {
      return 1;
    }

    return 0;
  }

  return 1;
}


/*******************************************************************************
 * ProbePackage
 ******************************************************************************/

// First-time probe of a package: runs CPUID, discovers VR topology, reads
// V/F data for every present domain, and captures turbo ratio limits and
// power limit MSRs.  Idempotent: second call skips discovery, re-reads VF data.
EFI_STATUS EFIAPI ProbePackage(IN OUT PACKAGE* pkg)
{
  EFI_STATUS status = EFI_SUCCESS;

  if (pkg->probed != 1) {

    ///////////
    // CPUID //
    ///////////

    GetCpuInfo(&pkg->CpuInfo);

    //////////////////////////
    // Discover VR Topology //
    //////////////////////////

    status = DiscoverVRTopology(pkg);
    if (EFI_ERROR(status)) return status;
  }
  
  ////////////////////////
  // Initialize domains //
  ////////////////////////

  for (UINT8 didx = 0; didx < MAX_DOMAINS; didx++) {
    if (VoltageDomainExists(didx)) {
      DOMAIN* dom = pkg->planes + didx;

      if (pkg->probed != 1) {
        dom->parent = (void*)pkg;
      }
      
      status = IAPERF_ProbeDomainVF(didx, dom);
      if (EFI_ERROR(status)) return status;
    }
  }

  //
  // Turbo Ratio Limits (this assumes all packages are the same)

  pkg->TurboRatioLimits = GetTurboRatioLimits();

  //
  // cTDP Levels

  pkg->ConfigTdpControl = GetConfigTdpControl();

  GetCTDPLevel(
    &pkg->MaxCTDPLevel,
    &pkg->TdpControLock);

  //
  // Power Limits and units

  pkg->MsrPkgPowerLimits = GetPkgPowerLimits(
    &pkg->MsrPkgMaxTau,
    &pkg->MsrPkgMinPL1,
    &pkg->MsrPkgMaxPL1
  );

  pkg->PkgPowerUnits = GetPkgPowerUnits(
    &pkg->PkgTimeUnits,
    &pkg->PkgEnergyUnits);

  pkg->probed = 1;

  return status;
}

/*******************************************************************************
 * ProgramVFOverridesAndOCRatios
 ******************************************************************************/

// Execute on the calling CPU (dispatched per-core): program turbo ratio limits
// and per-domain V/F overrides (IccMax + voltage).  Must run on every logical
// CPU because isolated-core power-on scenarios use per-core programmed values.
EFI_STATUS EFIAPI ProgramVFOverridesAndOCRatios()
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // We will locate our core and package using 
  // per-CPU Local Storage

  CPUCORE* core = (CPUCORE*)GetCpuDataBlock();
  PACKAGE* pkg = (PACKAGE*)core->parent;
    
  //
  // Forced turbo ratios

  if (pkg->ForcedRatioForPCoreCounts) {
    ProgramMaxTurboRatios(pkg->ForcedRatioForPCoreCounts);
  }

  if (gCpuInfo.HybridArch) {
    if (pkg->ForcedRatioForECoreCounts) {
      ProgramEfficientCoreMaxTurboRatios(pkg->ForcedRatioForECoreCounts);
    }
  }

  //
  // Program V/F overrides

  for (UINT8 didx = 0; didx < MAX_DOMAINS; didx++) {
    if (VoltageDomainExists(didx)) {
      DOMAIN* dom = pkg->planes + didx;

      if (pkg->Program_VF_Overrides[didx] || pkg->Program_IccMax[didx] || pkg->Program_VF_Points[didx]) {
        status = IAPERF_ProgramDomainVF(didx, dom, pkg->Program_VF_Points[didx],
          pkg->Program_IccMax[didx], pkg->Program_VF_Overrides[didx]);
        if (EFI_ERROR(status)) return status;
      }
    }
  }

  return status;
}

/*******************************************************************************
 * ProgramPowerLimits
 ******************************************************************************/

// Execute on BSP of each package: write cTDP level, MSR PL1/PL2, PL3, PL4,
// PP0, and power control tweaks (EE Turbo, Race-To-Halt).
EFI_STATUS EFIAPI ProgramPowerLimits()
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // We will locate our core and package using 
  // per-CPU Local Storage

  CPUCORE* core = (CPUCORE*)GetCpuDataBlock();
  PACKAGE* pkg = (PACKAGE*)core->parent;

  //
  // Program Config TDP Params
  // with no lock 

  SetCTDPLevel(pkg->MaxCTDPLevel);

  ///////////////////
  // Power Limits  //
  /////////////////// 

  //
  // PL1 and PL2

  if (pkg->ProgramPL12_MSR) {

    SetPkgPowerLimit12(
      IO_MSR,
      pkg->MsrPkgMaxTau,
      pkg->MsrPkgMinPL1,
      pkg->MsrPkgMaxPL1,
      pkg->EnableMsrPkgPL1,
      pkg->EnableMsrPkgPL2,
      pkg->PkgTimeUnits,
      pkg->PkgEnergyUnits,
      pkg->PkgPowerUnits,
      pkg->ClampMsrPkgPL,
      pkg->MsrPkgPL_Time,
      pkg->MsrPkgPL1_Power,
      pkg->MsrPkgPL2_Power);
  }

  //
  // PL3

  if (pkg->ProgramPL3) {

    SetPlatformPowerLimit3(
      pkg->EnableMsrPkgPL3,
      pkg->PkgTimeUnits,
      pkg->PkgPowerUnits,
      pkg->MsrPkgPL3_Time,
      pkg->MsrPkgPL3_Power);
  }

  //
  // PL4

  if (pkg->ProgramPL4) {

    SetPlatformPowerLimit4(
      pkg->EnableMsrPkgPL4,
      pkg->MsrPkgPL4_Current);
  }

  //
  // PP0

  if (pkg->ProgramPP0) {

    SetPP0PowerLimit(
      pkg->MsrPkgMaxTau,
      pkg->MsrPkgMinPL1,
      pkg->MsrPkgMaxPL1,
      pkg->EnableMsrPkgPP0,
      pkg->PkgTimeUnits,
      pkg->PkgPowerUnits,
      pkg->ClampMsrPP0,
      pkg->MsrPkgPP0_Time,
      pkg->MsrPkgPP0_Power
    );

  }

  //
  // Power Control

  if (pkg->ProgramPowerTweaks) {
    ProgramPowerCtl(pkg->EnableEETurbo, pkg->EnableRaceToHalt);
  }


  return status;
}

/*******************************************************************************
 * ProgramPackageLocks_Stage2
 * Program package locks in the separate stage, after everything else is done
 ******************************************************************************/

EFI_STATUS EFIAPI ProgramPackageLocks_Stage2()
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // We will locate our core and package using 
  // per-CPU Local Storage

  CPUCORE* core = (CPUCORE*)GetCpuDataBlock();
  PACKAGE* pkg = (PACKAGE*)core->parent;

  //
  // Power Limits MMIO Lock

  if (pkg->ProgramPL12_MMIO) {
    SetPL12MMIOLock(pkg->LockMmioPkgPL12);
  }

  return status;
}

/*******************************************************************************
 * ProgramPowerLimits_Stage2
 ******************************************************************************/

// Stage-2 power limit programming: MMIO-mapped PL1/PL2 (via MCHBAR) and
// Platform (PSys) limits.  Separated from Stage-1 because MMIO access
// requires gMCHBAR to be resolved and some firmware initialises it late.
EFI_STATUS EFIAPI ProgramPowerLimits_Stage2()
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // We will locate our core and package using 
  // per-CPU Local Storage

  CPUCORE* core = (CPUCORE*)GetCpuDataBlock();
  PACKAGE* pkg = (PACKAGE*)core->parent;

  //
  // MMIO
  //
  // Power Limits (MMIO)

  if (pkg->ProgramPL12_MMIO)
  {
    SetPkgPowerLimit12(
      IO_MMIO,
      pkg->MsrPkgMaxTau,
      pkg->MsrPkgMinPL1,
      pkg->MsrPkgMaxPL1,
      pkg->EnableMmioPkgPL1,
      pkg->EnableMmioPkgPL2,
      pkg->PkgTimeUnits,
      pkg->PkgEnergyUnits,
      pkg->PkgPowerUnits,
      pkg->ClampMmioPkgPL,
      pkg->MmioPkgPL_Time,
      pkg->MmioPkgPL1_Power,
      pkg->MmioPkgPL2_Power);
  }

  //
  // Platform (PSys) Power Limits
  
  if (pkg->ProgramPL12_PSys) 
  {
    SetPlatformPowerLimit12(
      pkg->EnablePlatformPL1,
      pkg->EnablePlatformPL2,
      pkg->PkgTimeUnits,
      pkg->PkgPowerUnits,
      pkg->ClampPlatformPL,
      pkg->PlatformPL_Time,
      pkg->PlatformPL1_Power,
      pkg->PlatformPL2_Power);
  }

  return status;
}


/*******************************************************************************
 * DiscoverPackage
 ******************************************************************************/

// Walk the EFI logical-processor list and group threads into PACKAGE structs.
// Fills gCorePtrs[] and gCoreApicIDs[] for GS-base per-CPU dispatch.
// Physical-core detection uses an SMT-2 heuristic (thread index even or 0).
EFI_STATUS DetectPackages(IN OUT PLATFORM* psys)
{
  if (!psys || !gMpServices || !psys->LogicalProcessors) return EFI_INVALID_PARAMETER;
  if (psys->LogicalProcessors > MAX_CORES * MAX_PACKAGES) return EFI_UNSUPPORTED;
  UINT32 packageIds[MAX_PACKAGES];
  UINTN nPackages = 0;
  SetMem(gCorePtrs, sizeof(gCorePtrs), 0);
  for (UINTN tidx = 0; tidx < psys->LogicalProcessors; tidx++) {
    EFI_PROCESSOR_INFORMATION pi = { 0 };
    EFI_STATUS status = gMpServices->GetProcessorInfo(gMpServices, tidx, &pi);
    if (EFI_ERROR(status)) return status;
    if (!(pi.StatusFlag & PROCESSOR_ENABLED_BIT)) continue;
    UINTN pidx = 0;
    while (pidx < nPackages && packageIds[pidx] != pi.Location.Package) pidx++;
    if (pidx == nPackages) {
      if (nPackages == MAX_PACKAGES) return EFI_UNSUPPORTED;
      packageIds[nPackages++] = pi.Location.Package;
      PACKAGE* p = &psys->packages[pidx];
      p->parent = psys;
      p->idx = pidx;
      p->FirstCoreNumber = tidx;
      p->FirstCoreApicID = pi.ProcessorId;
    }
    PACKAGE* pac = &psys->packages[pidx];
    if (pac->LogicalCores >= MAX_CORES) return EFI_UNSUPPORTED;
    UINTN localIdx = pac->LogicalCores++;
    CPUCORE* core = &pac->Core[localIdx];
    core->LocalIdx = (UINT8)localIdx;
    core->AbsIdx = tidx;
    core->ApicID = pi.ProcessorId;
    core->PkgIdx = (UINT8)pidx;
    core->parent = pac;
    // Retain the workaround for firmware reporting all cores as thread IDs.
    core->IsPhysical = (pi.Location.Thread % 2 == 0);
    pac->PhysicalCores += core->IsPhysical;
    gCorePtrs[tidx] = core;
    gCoreApicIDs[tidx] = core->ApicID;
  }
  if (!nPackages || psys->BootProcessor >= psys->LogicalProcessors || !gCorePtrs[psys->BootProcessor]) return EFI_NOT_FOUND;
  psys->PkgCnt = nPackages;
  // Keep absolute firmware indices, including holes for disabled processors.
  gNumCores = psys->LogicalProcessors;
  return EFI_SUCCESS;
}

/*******************************************************************************
* ProbePackages
******************************************************************************/

EFI_STATUS EFIAPI ProbePackages(IN OUT PLATFORM* ppd)
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // For each package

  for (UINTN pidx = 0; pidx < ppd->PkgCnt; pidx++)
  {
    PACKAGE* pac = ppd->packages + pidx;

    //
    // Run on 1st core

    status = RunOnPackageOrCore(ppd, pac->FirstCoreNumber, (CPU_STATUS_PROCEDURE)ProbePackage, pac);

    if (EFI_ERROR(status)) {
      UiPrint(L"[ERROR] CPU package %u, status code: 0x%x\n",
        pac->FirstCoreNumber,
        status);

      return status;
    }
  }

  return status;
}

/*******************************************************************************
 * ProbeCores
 * Probes information from each and every CPU core
 ******************************************************************************/

EFI_STATUS EFIAPI ProbeCores(IN OUT PLATFORM* ppd)
{
  EFI_STATUS status = EFI_SUCCESS;

  //
  // Probe each package in its own context

  for (UINTN pidx = 0; pidx < ppd->PkgCnt; pidx++)
  {
    PACKAGE* pac = ppd->packages + pidx;

    for (UINTN cidx = 0; cidx < pac->LogicalCores; cidx++)
    {
      CPUCORE* core = &pac->Core[cidx];

      /* if (core->IsPhysical) */ {

        //
        // Note: we use NULL instead of a real callback, because only data
        // being collected per-core is going to be collected by the MP dispatch
        // "Ignite" call itself. Once we need more data from each core, there
        // will be separate callback to do so

        status = RunOnPackageOrCore(ppd,
          core->AbsIdx,
          (CPU_STATUS_PROCEDURE)NULL,
          pac
        );

        if (EFI_ERROR(status)) {
          UiPrint(L"[ERROR] CPU package %u, status code: 0x%x\n",
            pac->FirstCoreNumber,
            status);

          return status;
        }
      }
    }
  }

  return status;
}


/*******************************************************************************
* DiscoverPlatform
******************************************************************************/

EFI_STATUS EFIAPI DiscoverPlatform(IN OUT PLATFORM** ppsys)
{

  EFI_STATUS status = EFI_SUCCESS;

  //
  // Allocate memory that will hold platform info

  *ppsys = (PLATFORM*)AllocateZeroPool(sizeof(PLATFORM));

  if (!*ppsys) {
    return EFI_OUT_OF_RESOURCES;
  }

  PLATFORM* ppd = *ppsys;

  //
  // Get bootstrap CPU

  status = gMpServices->WhoAmI(gMpServices, &ppd->BootProcessor);

  if (EFI_ERROR(status)) {

    UiPrint(
      L"[ERROR] Unable to get bootstrap processor (error: 0x%x)\n", status);

    goto Error;
  }

  //
  // We start with the logical processor count

  status = gMpServices->GetNumberOfProcessors(
    gMpServices,
    &ppd->LogicalProcessors,
    &ppd->EnabledLogicalProcessors
  );

  if (EFI_ERROR(status)) {
    goto Error;
  }

  ppd->PkgCnt = 1;                    // Boot Processor

  //
  // Identify CPU packages
  // and their respective CPU cores 

  status = DetectPackages(ppd);
  if (EFI_ERROR(status)) goto Error;

  //
  // Collect information specific
  // to each CPU core - currently only hybrid architecture CPUs need this

  /*if (gCpuInfo->HybridArch)*/ {   // <-- remove when necessary
    status = ProbeCores(ppd);
    if (EFI_ERROR(status)) goto Error;
  }

  //
  // Probe each detected package and collect info
  
  status = ProbePackages(ppd);
  if (EFI_ERROR(status)) goto Error;
  
  return EFI_SUCCESS;

Error:
  if (*ppsys) {
    FreePool(*ppsys);
    *ppsys = NULL;
  }
  return status;
}

/*******************************************************************************
 * PrintPlatformInfo 
 ******************************************************************************/

VOID PrintPlatformInfo(IN PLATFORM* psys)
{
  PMUNUSED(psys);
}


/*******************************************************************************
 * ProgramCoreLocks
 ******************************************************************************/

EFI_STATUS EFIAPI ProgramCoreLocks()
{
  //
  // We will locate our core and package using 
  // per-CPU Local Storage

  CPUCORE* core = (CPUCORE*)GetCpuDataBlock();
  PACKAGE* pk = (PACKAGE*)core->parent;

  
  //
  // PL1/2 Lock (MSR)

  if (pk->ProgramPL12_MSR) {
    SetPL12MSRLock(pk->LockMsrPkgPL12);
  }

  //
  // PL3 Lock

  if (pk->ProgramPL3) {
    SetPL3Lock(pk->LockMsrPkgPL3);
  }

  // PL4 Lock

  if (pk->ProgramPL4) {
    SetPL4Lock(pk->LockMsrPkgPL4);
  }

  //
  // PP0 Lock

  if (pk->ProgramPP0) {
    SetPP0Lock(pk->LockMsrPP0);
  }

  // PSys Lock

  if (pk->ProgramPL12_PSys) {
    SetPSysLock(pk->LockPlatformPL);
  }

  //
  // cTDP Lock

  SetCTDPLock(pk->TdpControLock);

  //
  // Overclocking Lock

  if (gPostProgrammingOcLock) {
    IaCore_OcLock();
  }

  return EFI_SUCCESS;
}


/*******************************************************************************
 * TBD / TODO: Needs Rewrite
 ******************************************************************************/

EFI_STATUS
EFIAPI
StartupPlatformInit(
  IN EFI_SYSTEM_TABLE* SystemTable,
  IN OUT PLATFORM** Platform
) {
  PMUNUSED(SystemTable);

  EFI_STATUS status = EFI_SUCCESS;

  status = DiscoverPlatform(Platform);

  if (EFI_ERROR(status)) {
    return status;
  }

  PLATFORM* sys = *Platform;

  PrintPlatformInfo(sys);
  //PrintCoreInfo();

  return status;
}

/*******************************************************************************
 * TBD / TODO: Needs Rewrite
 ******************************************************************************/

EFI_STATUS EFIAPI ApplyPolicy(IN EFI_SYSTEM_TABLE* SystemTable,
  IN OUT PLATFORM* sys)
{
  PMUNUSED(SystemTable);
  EFI_STATUS status = EFI_SUCCESS;

  
  /////////////////
  // PROGRAMMING //
  /////////////////

  if (!sys || !gIniFound) return EFI_NOT_READY;
  ApplyComputerOwnersPolicy(sys);

  ///////////////////
  // VF Overrides  //
  // and OC ratios //
  ///////////////////
  
  //
  // Strictly speaking, we do not need to program every core
  // Performing programming once per package would be sufficient (except for
  // parameters not yet supported here anyway). However, there is one scenario 
  // where isolated core would use its own programmed settings (one would need 
  // to power off other cores in package, though)
  //
  // So we will program every core, for the sake of completeness...

  for (UINTN pidx = 0; pidx < sys->PkgCnt; pidx++) {

    PACKAGE* pk = sys->packages + pidx;

    for (UINTN cidx = 0; cidx < pk->LogicalCores; cidx++)
    {
      CPUCORE* core = pk->Core + cidx;
      status = RunOnPackageOrCore(sys, core->AbsIdx, (CPU_STATUS_PROCEDURE)ProgramVFOverridesAndOCRatios, NULL);
    if (EFI_ERROR(status)) return status;
    }
  }

  //////////////////
  // Power Limits //
  //////////////////

  for (UINTN pidx = 0; pidx < sys->PkgCnt; pidx++)
  {
    PACKAGE* pk = sys->packages + pidx;

    //
    // cTDP, MSR PL1/PL2, ...

    status = RunOnPackageOrCore(sys, pk->FirstCoreNumber, (CPU_STATUS_PROCEDURE)ProgramPowerLimits, NULL);
    if (EFI_ERROR(status)) return status;

    //
    // MMIO, PSys, ...

    status = RunOnPackageOrCore(sys, pk->FirstCoreNumber, (CPU_STATUS_PROCEDURE)ProgramPowerLimits_Stage2, NULL);
    if (EFI_ERROR(status)) return status;
  }


  /////////////////
  // Apply LOCKS //
  /////////////////

  //
  // MSR Locks

  for (UINTN tidx = 0; tidx < gNumCores; tidx++) {
    if (!gCorePtrs[tidx]) continue;
    status = RunOnPackageOrCore(sys, tidx, (CPU_STATUS_PROCEDURE)ProgramCoreLocks, NULL);
    if (EFI_ERROR(status)) return status;
  }

  //
  // MMIO locks

  for (UINTN pidx = 0; pidx < sys->PkgCnt; pidx++)
  {
    PACKAGE* pk = sys->packages + pidx;
    status = RunOnPackageOrCore(sys, pk->FirstCoreNumber, (CPU_STATUS_PROCEDURE)ProgramPackageLocks_Stage2, pk);
    if (EFI_ERROR(status)) return status;
  }

  ////////////////////
  // PRINT SETTINGS //
  ////////////////////

  {
    status = ProbePackages(sys);
    if (EFI_ERROR(status)) return status;

    PrintVFPoints(sys);
  }

  return status;
}

/*******************************************************************************
* GetCpuDataBlock
******************************************************************************/

// Return the CPUCORE* for the currently executing logical CPU by querying
// WhoAmI and indexing gCorePtrs[].  Used by dispatch callbacks to locate
// their package/domain context without passing explicit parameters.
VOID* GetCpuDataBlock()
{
  UINTN processorNumber = 0;
  if (!gMpServices || EFI_ERROR(gMpServices->WhoAmI(gMpServices, &processorNumber)) || processorNumber >= gNumCores) return NULL;
  VOID* coreStructAddr = gCorePtrs[processorNumber];
  return coreStructAddr;
}
