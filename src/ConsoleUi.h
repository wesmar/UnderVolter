// ConsoleUi.h — Startup animation (lightning bolt + oscilloscope) and
//               progress bar drawn directly to the GOP framebuffer.
#ifndef _CONSOLE_UI_H_
#define _CONSOLE_UI_H_

#include <Uefi.h>

#define PROGRESS_BAR_WIDTH 100

VOID RunStartupAnimation(VOID);
VOID UiClearAnimationArea(VOID);
VOID DrawProgressBar(IN UINTN ProgressPercentage);

#endif
