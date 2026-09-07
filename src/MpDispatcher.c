// MpDispatcher.c — Multi-processor dispatch helpers: wraps EFI_MP_SERVICES
//                  to run a caller-supplied procedure on a specific AP or on
//                  all processors concurrently.  Each AP is initialised via
//                  ProcessorIgnite, which sets the GS base to the per-CPU
//                  CPUCORE block before invoking the caller's function.
#include <PiPei.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include "UiConsole.h"

//
// Protocols

#include <Protocol/MpService.h>

//
// UnderVolter

#include "MpDispatcher.h"
#include "HwAccess.h"
#include <Library/UefiBootServicesTableLib.h>     // gBS

//
// Initialized at startup

extern EFI_MP_SERVICES_PROTOCOL* gMpServices;

/*******************************************************************************
 *
 ******************************************************************************/


/*******************************************************************************
 * ProcessorIgnite
 ******************************************************************************/

typedef struct _IgniteContext
{
  UINTN CpuNumber;
  VOID* userParam;  
  EFI_AP_PROCEDURE userProc;
  CPU_STATUS_PROCEDURE statusProc;
  EFI_STATUS result;
} IgniteContext;

// EFI_AP_PROCEDURE shim: runs on each AP before the caller's procedure.
// Sets the GS base to this CPU's CPUCORE block (found via GetCpuDataBlock),
// populates CpuInfo and IsECore, records the logical processor index from
// WhoAmI for validation, then calls the user procedure (if non-NULL).
VOID EFIAPI ProcessorIgnite(VOID* params)
{
    
  IgniteContext* pic = (IgniteContext*)params;

  VOID* coreStructAddr = GetCpuDataBlock();
  if (!coreStructAddr) { pic->result = EFI_NOT_FOUND; return; }
  VOID* oldGsBase = GetCpuGSBase();

  //
  // Set the GS register to point to coreStructAddr

  SetCpuGSBase(coreStructAddr);

  ///
  /// Populate CPU core-specific info
  /// 
  
  CPUCORE* core = (CPUCORE*)coreStructAddr;
  
  GetCpuInfo(&core->CpuInfo);
  core->IsECore = core->CpuInfo.ECore;

  //
  // Debug
  {
    UINTN processorNumber = 0;
    gMpServices->WhoAmI(gMpServices, &processorNumber);
    core->ValidateIdx = (UINT32)processorNumber;
  }  
  
  if (pic->userProc) {

    ///
    /// Execute user's call (if supplied)
    ///

    pic->userProc(pic->userParam);
  }

  if (pic->statusProc) pic->result = pic->statusProc(pic->userParam);
  SetCpuGSBase(oldGsBase);
}


/*******************************************************************************
 * RunOnPackageOrCore
 ******************************************************************************/

// Dispatch proc(param) to the logical processor identified by CpuNumber using
// StartupThisAP (1 s timeout).  Falls back to in-place execution via
// ProcessorIgnite when CpuNumber == BSP or gMpServices is unavailable.
EFI_STATUS EFIAPI RunOnPackageOrCore(
  const IN PLATFORM* Platform,
  const IN UINTN CpuNumber,
  const IN CPU_STATUS_PROCEDURE proc,
  IN VOID* param OPTIONAL)
{
  EFI_STATUS status = EFI_SUCCESS;
  if (!gMpServices || !Platform || CpuNumber >= gNumCores || !gCorePtrs[CpuNumber]) return EFI_INVALID_PARAMETER;

  if (gMpServices) {
    if (CpuNumber != Platform->BootProcessor) {

      IgniteContext ctx = { 0 };
      
      ctx.userParam = param;
      ctx.statusProc = proc;
      ctx.CpuNumber = CpuNumber;

      status = gMpServices->StartupThisAP(
        gMpServices,
        ProcessorIgnite,
        CpuNumber,
        NULL,
        1000000,
        &ctx,
        NULL
      );

      if (EFI_ERROR(status)) {
        UiPrint(L"[ERROR] Unable to execute on CPU %u,"
          "status code: 0x%x\n", CpuNumber, status);
      }

      return EFI_ERROR(status) ? status : ctx.result;
    }
  }

  //
  // Platform has no MP services OR we are running on the desired package
  // ... so instead of dispatching, we will just do the work now

  {
    IgniteContext ctx = { 0 };

    ctx.userParam = param;
    ctx.statusProc = proc;
    ctx.CpuNumber = CpuNumber;

    ProcessorIgnite(&ctx);
    status = ctx.result;
  }
  

  //
  // ... and that's that

  return status;
}

/*******************************************************************************
 *
 ******************************************************************************/

// Run proc(param) on every logical processor.
// runConcurrent=TRUE: APs start via StartupAllAPs with WaitEvent, then BSP
//   executes its copy in parallel; caller blocks until the EFI event fires.
// runConcurrent=FALSE: APs run serially (SingleThread=TRUE), then BSP runs.
// Falls back to BSP-only execution if gMpServices is unavailable.
EFI_STATUS EFIAPI RunOnAllProcessors(
  const IN EFI_AP_PROCEDURE proc,
  const BOOLEAN runConcurrent,                  // FALSE = serial AP execution
  IN VOID* param OPTIONAL)
{
  if (!proc || !gMpServices) return EFI_INVALID_PARAMETER;
  EFI_EVENT mpEvent = NULL;
  IgniteContext ctx = { 0 };
  ctx.userParam = param;
  ctx.userProc = proc;
  EFI_STATUS status = EFI_SUCCESS;
  if (runConcurrent) {
    // A plain signalable event can be waited on; EVT_NOTIFY_SIGNAL cannot.
    status = gBS->CreateEvent(0, TPL_APPLICATION, NULL, NULL, &mpEvent);
    if (EFI_ERROR(status)) return status;
  }
  status = gMpServices->StartupAllAPs(gMpServices, ProcessorIgnite,
    !runConcurrent, mpEvent, 0, &ctx, NULL);
  // EFI_NOT_STARTED is normal for a BSP-only machine.
  if (status == EFI_NOT_STARTED) {
    if (mpEvent) gBS->CloseEvent(mpEvent);
    mpEvent = NULL;
    status = EFI_SUCCESS;
  }
  if (EFI_ERROR(status)) {
    if (mpEvent) gBS->CloseEvent(mpEvent);
    return status;
  }
  proc(param);
  if (mpEvent) {
    UINTN eventIdx;
    status = gBS->WaitForEvent(1, &mpEvent, &eventIdx);
    gBS->CloseEvent(mpEvent);
  }
  return status;
}
