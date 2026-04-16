// MiniLog.h — Optional compile-time tracing overlay: when ENABLE_MINILOG_TRACING
//             is defined, MiniTrace/MiniTraceEx render per-CPU diagnostic lines
//             directly to the GOP framebuffer (no UEFI console required).
#if defined(_MSC_VER)
#define UNUSED
#else
#define UNUSED __attribute__((unused))
#endif

/*******************************************************************************
 * Configuration
 ******************************************************************************/

#include "Config.h"

/*******************************************************************************
 * MiniLogEntry - 128 bits, can be cmpxchg16b-ed
 ******************************************************************************/

typedef struct _MiniLogEntry {
  UINT8   operId;
  UINT8   pkgIdx;
  UINT8   coreIdx;
  UINT8   dangerous;
  UINT32  param1;
  UINT64  param2;
} MiniLogEntry;

/*******************************************************************************
 * Operation IDs
 ******************************************************************************/

#define MINILOG_OPID_FREE_MSG                                   0x00
#define MINILOG_OPID_RDMSR64                                    0x01
#define MINILOG_OPID_WRMSR64                                    0x02
#define MINILOG_OPID_MMIO_READ32                                0x03
#define MINILOG_OPID_MMIO_WRITE32                               0x04
#define MINILOG_OPID_MMIO_OR32                                  0x05

/*******************************************************************************
 * Log Codes
 ******************************************************************************/

#define MINILOG_LOGCODE_TRACE                                   0x00

/*******************************************************************************
 *
 ******************************************************************************/

#ifdef ENABLE_MINILOG_TRACING

void InitializeTrace();

void MiniTrace(
  const UINT8  operId,  
  const UINT8  dangerous,
  const UINT32 param1,
  const UINT64 param2
);

void MiniTraceEx(
  IN  CONST CHAR8* format,
  ...
);

#else

#define InitializeTrace()
#define MiniTrace(a, b, c, d)

static void UNUSED MiniTraceEx(
  IN  CONST CHAR8* format,
  ...
) {
  
}


#endif