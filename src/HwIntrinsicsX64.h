// HwIntrinsicsX64.h — Declarations for ASM-implemented hardware primitives:
//                     MSR/MMIO safe access, CPUID, IDT management, atomic ops,
//                     PCI config-space access, and monkey ISR entry points.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*******************************************************************************
 * Below calls are implemented in HwIntrinsicsX64.asm
 ******************************************************************************/

//
// CLI

VOID EFIAPI DisableInterruptsOnThisCpu(VOID);

//
// STI

VOID EFIAPI EnableInterruptsOnThisCpu(VOID);

//
// RDMSR

UINT64 EFIAPI SafeReadMsr64(
    const UINT32 msr_idx,
    UINT32* is_err);

//
// WRMSR

UINT32 EFIAPI SafeWriteMsr64(
    const UINT32 msr_idx,
    const UINT64 value);

//
// MMIO_READ32

UINT32 EFIAPI SafeMmioRead32(
  const UINT32 addr, 
  UINT32* is_err);

//
// MMIO_OR32

UINT32 EFIAPI SafeMmioOr32(
  const UINT32 addr, 
  const UINT32 value);

//
// MMIO_WRITE32

UINT32 EFIAPI SafeMmioWrite32(
  const UINT32 addr,
  const UINT32 value);

//
// SIDT

VOID EFIAPI GetCurrentIdtr(VOID* pidtr);

//
// CPUID

UINT32 EFIAPI AsmCpuidRegisters(UINT32 func, UINT32 *regs);
UINT32 EFIAPI AsmCpuidRegistersEx(UINT32 func, UINT32 subfunc, UINT32 *regs);


UINT32 EFIAPI GetPciExpressBaseAddress(VOID);

UINT64 EFIAPI AtomicIncrementU64(UINT64 *val);
UINT64 EFIAPI AtomicDecrementU64(UINT64* val);

UINT32 EFIAPI AtomicIncrementU32(UINT32* val);
UINT32 EFIAPI AtomicDecrementU32(UINT32* val);


/*******************************************************************************
 * ISR entry points in SaferAsm.asm
 ******************************************************************************/

extern VOID* monkey_isr_0;
extern VOID* monkey_isr_1;
extern VOID* monkey_isr_2;
extern VOID* monkey_isr_3;
extern VOID* monkey_isr_4;
extern VOID* monkey_isr_5;
extern VOID* monkey_isr_6;
extern VOID* monkey_isr_7;
extern VOID* monkey_isr_8;
extern VOID* monkey_isr_9;
extern VOID* monkey_isr_10;
extern VOID* monkey_isr_11;
extern VOID* monkey_isr_12;
extern VOID* monkey_isr_13;
extern VOID* monkey_isr_14;
extern VOID* monkey_isr_15;
extern VOID* monkey_isr_16;
extern VOID* monkey_isr_17;
extern VOID* monkey_isr_18;
extern VOID* monkey_isr_19;
extern VOID* monkey_isr_20;
extern VOID* monkey_isr_21;
extern VOID* monkey_isr_22;
extern VOID* monkey_isr_23;
extern VOID* monkey_isr_24;
extern VOID* monkey_isr_25;
extern VOID* monkey_isr_26;
extern VOID* monkey_isr_27;
extern VOID* monkey_isr_28;
extern VOID* monkey_isr_29;
extern VOID* monkey_isr_30;
extern VOID* monkey_isr_31;

#ifdef __cplusplus
}
#endif