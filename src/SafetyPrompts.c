// SafetyPrompts.c — User-facing safety gates: a countdown abort prompt and
//                   an unknown-CPU override dialog.  Both use the UiConsole
//                   text API and UEFI boot services for keyboard input and
//                   timer events.
#include "SafetyPrompts.h"
#include "ConsoleUi.h"
#include "UiConsole.h"
#include "CpuInfo.h"
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

extern UINT8 gEmergencyExit;
extern EFI_BOOT_SERVICES* gBS;
extern EFI_SYSTEM_TABLE*  gST;
extern CPUINFO gCpuInfo;

// If gEmergencyExit is set, display a 10-step countdown progress bar (~2 s
// total, polling at 10 ms intervals per step).  Returns TRUE immediately if
// the user presses ESC, aborting further programming.
BOOLEAN CheckForEmergencyExit(VOID)
{
  if (gEmergencyExit) {

    EFI_STATUS         Status;
    EFI_INPUT_KEY      Key;

    UiSetAttribute(EFI_WHITE);
    UiPrint(L" Voltage offset is the preferred method. Make sure your PC is stable. Press 'ESC' to skip voltage correction.\n\n");

    UiSetAttribute(EFI_LIGHTGRAY);

    for (UINTN i = 0; i < 10; i++) {
        UiSetAttribute(EFI_GREEN);
        DrawProgressBar((i + 1) * 10);
        
        for (UINTN j = 0; j < 20; j++) {
            Status = gBS->CheckEvent(gST->ConIn->WaitForKey);
            if (!EFI_ERROR(Status)) {
                gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
                if (Key.ScanCode == SCAN_ESC) {      
                    UiAsciiPrint("\n Aborting.\n");
                    return TRUE;
                }
            }
            gBS->Stall(10000); // 10ms delay
        }
    }
    UiAsciiPrint("\n");
  }

  return FALSE;
}

// Show a red-text warning that the detected CPU is not in CpuConfigTable.
// Opens a 30-second timer event and waits for either the timer or a keypress.
// Returns TRUE if the user presses F10 to override (proceed anyway); FALSE
// on timeout or any other key (programming is aborted by the caller).
BOOLEAN DisplayUnknownCpuWarning(VOID)
{
  EFI_STATUS         Status;
  EFI_EVENT          TimerEvent;
  EFI_EVENT          WaitList[2];
  EFI_INPUT_KEY      Key = {0, 0};   // initialise: ReadKeyStroke may not fill on timeout
  UINTN              Index;

  UiSetAttribute(EFI_RED);

  UiPrint(
    L"\n WARNING: Detected CPU (model: %u, family: %u, stepping: %u) is not known!\n"
    L" It is likely that proceeding further with hardware programming will result\n"
    L" result in unpredictable behavior or with the system hang/reboot.\n\n",
    gCpuInfo.model,
    gCpuInfo.family,
    gCpuInfo.stepping );

  UiPrint(
    L" If you are a BIOS engineer or otherwise familiar with the detected CPU params\n"
    L" please edit CpuData.c and extend it with the detected CPU model/family/stepping\n"
  );

  UiPrint(
    L" and its capabilities. Further changes to the UnderVolter code might be necessary.\n\n"
    L" Press F10 key within the next 30 seconds to IGNORE this warning.\n"
    L" Otherwise, UnderVolter will exit with no changes to the system.\n\n"
  );

  UiSetAttribute(EFI_LIGHTCYAN);

  Status = gBS->CreateEvent(
    EVT_TIMER, TPL_NOTIFY, NULL, NULL, &TimerEvent);

  Status = gBS->SetTimer(
    TimerEvent, TimerRelative, 300000000);

  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;

  Status = gBS->WaitForEvent(2, WaitList, &Index);

  if (!EFI_ERROR(Status) && Index == 1) {
    Status = EFI_TIMEOUT;
  }

  gBS->CloseEvent(TimerEvent);
  gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);

  if (Key.ScanCode == SCAN_F10) {
    UiAsciiPrint(
      " Overriding unknown CPU detection...\n");

    return TRUE;
  }
  else {
    UiAsciiPrint(
      " Programming aborted.\n");
  }

  return FALSE;
}
