; HwIntrinsicsX64.asm — x86-64 hardware intrinsics for UnderVolter:
;   - IDT helpers (GetCurrentIdtr, interrupt enable/disable)
;   - Atomic increment/decrement (CLI/LOCK/STI, 32 and 64-bit)
;   - Monkey-ISR stubs (EXCEPT_ISR_* macros): write vector# into CR2 as a
;     sentinel, push dummy error code where absent, fall through SafeIsrCommon
;   - SafeIsrCommon: saves all GPRs, segment regs, CR3; encodes fault sentinel
;     in CR2 (MAGIC_HIDWORD:MAGIC_LODWORD) for detection by the C fault handlers;
;     optionally calls gSafeCIsrFunctionPointer; restores and IRETQ
;   - GetPciExpressBaseAddress: PCI I/O port CF8/CFC read of PCIE BAR register
;   - SafeReadMsr64 / SafeWriteMsr64: RDMSR/WRMSR with CR2 fault detection
;   - SafeMmioRead32 / SafeMmioWrite32 / SafeMmioOr32: MMIO with alignment check
;     and CR2 fault detection; all reject unaligned addresses (return error=1)
;   - memset / memcpy: REP STOSB / REP MOVSB wrappers (no libc)
;   - AsmCpuidRegisters / AsmCpuidRegistersEx: CPUID with full register capture
;
; Sentinel encoding:
;   On fault the monkey ISR writes the vector number into CR2, then jumps to
;   SafeIsrCommon.  SafeIsrCommon stores MAGIC_HIDWORD:MAGIC_LODWORD into CR2.
;   After IRETQ the C wrapper reads CR2; if lo-dword == MAGIC_LODWORD, a fault
;   occurred and the error out-parameter is set to 1.

MAGIC_HIDWORD   EQU 0baad0000h  ; upper 32 bits of the CR2 fault sentinel
MAGIC_LODWORD   EQU 0deadc0deh  ; lower 32 bits of the CR2 fault sentinel
MAGIC_MASK32HI  EQU 0ffff0000h  ; mask for the upper half of CR2 32-bit window
MAGIC_MASKVECHI EQU 0000000ffh  ; mask for the vector# byte stored by monkey ISRs

.CODE
ALIGN 8

; rcx: pointer to IDTR structure (Limit:UINT16, Base:UINT64) — caller-allocated
PUBLIC GetCurrentIdtr
GetCurrentIdtr PROC
    sidt FWORD PTR [rcx]          ; store 10-byte IDTR descriptor at *rcx
    ret
GetCurrentIdtr ENDP

PUBLIC DisableInterruptsOnThisCpu
DisableInterruptsOnThisCpu PROC
    cli
    ret
DisableInterruptsOnThisCpu ENDP

PUBLIC EnableInterruptsOnThisCpu
EnableInterruptsOnThisCpu PROC
    sti
    ret
EnableInterruptsOnThisCpu ENDP

; rcx: pointer to UINT64 counter — incremented atomically; returns new value in rax
PUBLIC AtomicIncrementU64
AtomicIncrementU64 PROC
    mov rax, rcx
    cli                           ; disable interrupts for the RMW sequence
    lock inc qword ptr [rax]
    mov rax, [rax]
    sti
    ret
AtomicIncrementU64 ENDP

; rcx: pointer to UINT32 counter — returns new value in eax
PUBLIC AtomicIncrementU32
AtomicIncrementU32 PROC
    mov r8, rcx
    cli
    lock inc dword ptr [r8]
    mov eax, dword ptr [r8]
    sti
    ret
AtomicIncrementU32 ENDP

; rcx: pointer to UINT64 counter — decremented atomically; returns new value in rax
PUBLIC AtomicDecrementU64
AtomicDecrementU64 PROC
    mov r8, rcx
    cli
    lock dec qword ptr [r8]
    mov rax, [r8]
    sti
    ret
AtomicDecrementU64 ENDP

; rcx: pointer to UINT32 counter — returns new value in eax
PUBLIC AtomicDecrementU32
AtomicDecrementU32 PROC
    mov r8, rcx
    cli
    lock dec dword ptr [r8]
    mov eax, dword ptr [r8]
    sti
    ret
AtomicDecrementU32 ENDP

; Monkey-ISR stubs: each stub stores the vector number in CR2 (lo byte) as a
; pre-fault marker, then pushes a fake error code (0) if the CPU does not push
; one automatically, and jumps to SafeIsrCommon.
;
; EXCEPT_ISR_ERRCODE_ABSENT: no hardware error code (most exceptions)
; EXCEPT_ISR_ERRCODE_PUSHED: hardware pushes an error code (#DF, #GP, #PF, etc.)
; ISR_GENERIC_INTERRUPT: software/hardware IRQs (vectors 0x20–0xFF)

EXCEPT_ISR_ERRCODE_ABSENT MACRO vec
    PUBLIC monkey_isr_&vec
    ALIGN 8
monkey_isr_&vec:
    push rax
    mov rax, vec                  ; tag CR2 with the vector# (pre-fault marker)
    mov cr2, rax
    pop rax
    push 0                        ; synthesize missing error code
    jmp SafeIsrCommon
ENDM

EXCEPT_ISR_ERRCODE_PUSHED MACRO vec
    PUBLIC monkey_isr_&vec
    ALIGN 8
monkey_isr_&vec:
    push rax
    mov rax, vec
    mov cr2, rax
    pop rax
    jmp SafeIsrCommon             ; hardware already pushed error code
ENDM

ISR_GENERIC_INTERRUPT MACRO vec
    PUBLIC monkey_isr_&vec
    ALIGN 8
monkey_isr_&vec:
    push rax
    mov rax, vec
    mov cr2, rax
    pop rax
    push 0
    jmp SafeIsrCommon
ENDM

; ERROR_HANDLER_REG / ERROR_HANDLER_MEM: inline CR2 sentinel check used inside
; Safe{Write,Read}Msr64 and SafeMmio* after the potentially faulting instruction.
; If CR2 lo-dword matches MAGIC_LODWORD, a fault occurred: clear CR2, set error.

ERROR_HANDLER_REG MACRO lbl, reg
    lbl&_err_handler:
    mov   r8, cr2
    cmp   r8d, MAGIC_LODWORD      ; did the monkey-ISR leave the sentinel?
    jne   lbl&_noerr
    xor   r8, r8
    mov   cr2, r8                 ; clear sentinel
    mov   reg, 1                  ; signal error to caller
    jmp   lbl&_done
ENDM

ERROR_HANDLER_MEM MACRO lbl, memptr
    lbl&_err_handler:
    mov   r8, cr2
    cmp   r8d, MAGIC_LODWORD
    jne   lbl&_noerr
    xor   r8, r8
    mov   cr2, r8
    mov   DWORD PTR [memptr], 1
    jmp   lbl&_done
ENDM

EXCEPT_ISR_ERRCODE_ABSENT       0
EXCEPT_ISR_ERRCODE_ABSENT       1
EXCEPT_ISR_ERRCODE_ABSENT       2
EXCEPT_ISR_ERRCODE_ABSENT       3
EXCEPT_ISR_ERRCODE_ABSENT       4
EXCEPT_ISR_ERRCODE_ABSENT       5
EXCEPT_ISR_ERRCODE_ABSENT       6
EXCEPT_ISR_ERRCODE_ABSENT       7
EXCEPT_ISR_ERRCODE_PUSHED       8
EXCEPT_ISR_ERRCODE_ABSENT       9
EXCEPT_ISR_ERRCODE_PUSHED       10
EXCEPT_ISR_ERRCODE_PUSHED       11
EXCEPT_ISR_ERRCODE_PUSHED       12
EXCEPT_ISR_ERRCODE_PUSHED       13
EXCEPT_ISR_ERRCODE_PUSHED       14
EXCEPT_ISR_ERRCODE_ABSENT       15
EXCEPT_ISR_ERRCODE_ABSENT       16
EXCEPT_ISR_ERRCODE_ABSENT       17
EXCEPT_ISR_ERRCODE_ABSENT       18
EXCEPT_ISR_ERRCODE_ABSENT       19
EXCEPT_ISR_ERRCODE_ABSENT       20
EXCEPT_ISR_ERRCODE_ABSENT       21
EXCEPT_ISR_ERRCODE_ABSENT       22
EXCEPT_ISR_ERRCODE_ABSENT       23
EXCEPT_ISR_ERRCODE_ABSENT       24
EXCEPT_ISR_ERRCODE_ABSENT       25
EXCEPT_ISR_ERRCODE_ABSENT       26
EXCEPT_ISR_ERRCODE_ABSENT       27
EXCEPT_ISR_ERRCODE_ABSENT       28
EXCEPT_ISR_ERRCODE_ABSENT       29
EXCEPT_ISR_ERRCODE_ABSENT       30
EXCEPT_ISR_ERRCODE_ABSENT       31

i = 20h
REPT 100h-20h
    ISR_GENERIC_INTERRUPT %i
    i = i + 1
ENDM

; SafeIsrCommon — common ISR body reached from all monkey_isr_N stubs.
; Saves all GPRs (rax–r15), DS, ES, FS, GS, and CR3 on the stack.
; Then encodes the fault sentinel into CR2:
;   CR2 hi32 = (original CR2 hi16 masked) | MAGIC_HIDWORD
;   CR2 lo32 = MAGIC_LODWORD
; If gSafeCIsrFunctionPointer is non-NULL, calls it with RSP as argument
; (pointer to the saved-register frame).  Restores all registers and IRETQ.
ALIGN   8
SafeIsrCommon:
    push rax                      ; save all caller-preserved and GPRs
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push 0                        ; alignment / reserved slot

    xor rax, rax
    mov ax, ds
    push rax

    mov ax, es
    push rax

    push fs
    push gs

    push 0                        ; reserved slot

    mov rax, cr3
    push rax                      ; save CR3 (page table base)

    ; Encode fault sentinel in CR2 so C wrappers can detect it after IRETQ
    mov rax, cr2
    and eax, MAGIC_MASK32HI       ; preserve upper bits of the original CR2 value
    or  eax, MAGIC_HIDWORD        ; tag hi-dword
    shl rax, 32
    mov eax, MAGIC_LODWORD        ; tag lo-dword
    mov cr2, rax

    mov r10, gSafeCIsrFunctionPointer

    test r10, r10
    jz no_c_handler_ptr

    mov rdi, rsp                  ; rdi = pointer to saved frame (C calling conv 1st arg)
    call r10

no_c_handler_ptr:
    add rsp, 8
    pop rax

    pop rax
    mov cr3, rax

    pop gs
    pop fs
 
    pop rax
    mov es, ax

    pop rax
    mov ds, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    iretq


PUBLIC GetPciExpressBaseAddress
GetPciExpressBaseAddress PROC
    push rdx
    cli
    mov eax, 80000060h
    mov dx, 0cf8h
    out dx, eax
    mov dx, 0cfch
    in eax, dx
    sti
    pop rdx
    ret
GetPciExpressBaseAddress ENDP

PUBLIC SafeReadMsr64
SafeReadMsr64 PROC
    mov     r11, rdx
    xor     r10d, r10d
    rdmsr
    
    mov   r8, cr2
    cmp   r8d, MAGIC_LODWORD
    jne   rdmsr64_noerr
    xor   r8, r8
    mov   cr2, r8
    mov   r10d, 1
rdmsr64_noerr:
    shl   rdx, 32
    or    rax, rdx
    test  r11, r11
    jz    rdmsr64_justret
    mov   dword ptr [r11], r10d
rdmsr64_justret:
    ret
SafeReadMsr64 ENDP

PUBLIC SafeWriteMsr64
SafeWriteMsr64 PROC
    mov rax, rdx
    shr rdx, 32
    wrmsr

    ERROR_HANDLER_REG SafeWriteMsr64, eax

SafeWriteMsr64_noerr:
    xor eax, eax
SafeWriteMsr64_done:
    ret
SafeWriteMsr64 ENDP

PUBLIC SafeMmioWrite32    
SafeMmioWrite32 PROC
    mov esi, ecx
    and esi, 3
    jnz SafeMmioWrite32_done
    mov dword ptr [ecx], edx
    ERROR_HANDLER_REG SafeMmioWrite32, eax
SafeMmioWrite32_noerr:    
    xor eax, eax
SafeMmioWrite32_done:
    ret
SafeMmioWrite32 ENDP

PUBLIC SafeMmioRead32
SafeMmioRead32 PROC
    xor r9d, r9d
    mov esi, ecx
    and esi, 3
    jnz smr_error
    mov eax, dword ptr [ecx]
    
    mov   r8, cr2
    cmp   r8d, MAGIC_LODWORD
    jne   smr_done
    xor   r8, r8
    mov   cr2, r8
smr_error:
    mov   r9d, 1
smr_done:
    test rdx, rdx
    jz smr_end
    mov dword ptr [rdx], r9d
smr_end:
    ret
SafeMmioRead32 ENDP

PUBLIC SafeMmioOr32    
SafeMmioOr32 PROC
    mov esi, ecx
    and esi, 3
    jnz smo_error

    mov edi, dword ptr [ecx]
    or edi, edx
    mov dword ptr [ecx], edi

    mov   r8, cr2
    cmp   r8d, MAGIC_LODWORD
    jne   smo_noerror

    xor   r8, r8
    mov   cr2, r8    
smo_noerror:    
    xor eax, eax
    jmp smo_done
smo_error:    
    mov eax, 1
smo_done:
    ret
SafeMmioOr32 ENDP

PUBLIC memset
memset PROC
    push    rdi
    mov     eax, edx
    mov     rdi, rcx
    mov     r9, rcx
    mov     rcx, r8
    rep     stosb
    mov     rax, r9
    pop     rdi
    ret
memset ENDP

PUBLIC memcpy
memcpy PROC
    push    rsi
    push    rdi
    mov     r9, rcx
    mov     rdi, rcx
    mov     rsi, rdx
    mov     rcx, r8
    rep     movsb
    mov     rax, r9
    pop     rdi
    pop     rsi
    ret
memcpy ENDP

PUBLIC AsmCpuidRegisters
AsmCpuidRegisters PROC
    push    rbx
    mov     r10, rdx
    mov     eax, ecx
    xor     ecx, ecx
    push    rax
    cpuid
    mov     dword ptr [r10],    eax
    mov     dword ptr [r10+4],  ebx
    mov     dword ptr [r10+8],  ecx 
    mov     dword ptr [r10+12], edx 
    pop     rax
    pop     rbx
    ret
AsmCpuidRegisters ENDP

PUBLIC AsmCpuidRegistersEx
AsmCpuidRegistersEx PROC
    push    rbx        
    mov     eax, ecx
    mov     ecx, edx
    push    rax
    cpuid
    mov     dword ptr [r8],    eax
    mov     dword ptr [r8+4],  ebx
    mov     dword ptr [r8+8],  ecx 
    mov     dword ptr [r8+12], edx 
    pop     rax
    pop     rbx
    ret
AsmCpuidRegistersEx ENDP

.DATA
PUBLIC gSafeCIsrFunctionPointer
gSafeCIsrFunctionPointer DQ 0

END
