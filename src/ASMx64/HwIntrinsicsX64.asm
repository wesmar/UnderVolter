; Hardware intrinsics and precise MSR/MMIO exception recovery for Microsoft x64.
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
    pushfq
    cli                           ; disable interrupts for the RMW sequence
    lock inc qword ptr [rax]
    mov rax, [rax]
    popfq
    ret
AtomicIncrementU64 ENDP

; rcx: pointer to UINT32 counter — returns new value in eax
PUBLIC AtomicIncrementU32
AtomicIncrementU32 PROC
    mov r8, rcx
    pushfq
    cli
    lock inc dword ptr [r8]
    mov eax, dword ptr [r8]
    popfq
    ret
AtomicIncrementU32 ENDP

; rcx: pointer to UINT64 counter — decremented atomically; returns new value in rax
PUBLIC AtomicDecrementU64
AtomicDecrementU64 PROC
    mov r8, rcx
    pushfq
    cli
    lock dec qword ptr [r8]
    mov rax, [r8]
    popfq
    ret
AtomicDecrementU64 ENDP

; rcx: pointer to UINT32 counter — returns new value in eax
PUBLIC AtomicDecrementU32
AtomicDecrementU32 PROC
    mov r8, rcx
    pushfq
    cli
    lock dec dword ptr [r8]
    mov eax, dword ptr [r8]
    popfq
    ret
AtomicDecrementU32 ENDP

; Recover only at explicitly labelled MSR/MMIO accesses. Unrelated exceptions
; retain the firmware handler, original exception frame, registers and CR2.
; No C callback, segment-register reload, or guessed instruction length is used.
SAFE_PROBE_ISR MACRO vec, hasError
    PUBLIC monkey_isr_&vec
monkey_isr_&vec:
    push rax
    push rcx
    mov rax, qword ptr [rsp + 16 + hasError * 8]
    lea rcx, SafeReadMsr64_access
    cmp rax, rcx
    je recover_read_msr_&vec
    lea rcx, SafeWriteMsr64_access
    cmp rax, rcx
    je recover_write_msr_&vec
    lea rcx, SafeMmioRead32_access
    cmp rax, rcx
    je recover_read_mmio_&vec
    lea rcx, SafeMmioWrite32_access
    cmp rax, rcx
    je recover_write_mmio_&vec
    lea rcx, SafeMmioOr32_read_access
    cmp rax, rcx
    je recover_or_mmio_&vec
    lea rcx, SafeMmioOr32_write_access
    cmp rax, rcx
    je recover_or_mmio_&vec
    pop rcx
    pop rax
    jmp qword ptr [gOriginalIsr&vec]
recover_read_msr_&vec:
    lea rcx, SafeReadMsr64_error
    jmp recover_done_&vec
recover_write_msr_&vec:
    lea rcx, SafeWriteMsr64_error
    jmp recover_done_&vec
recover_read_mmio_&vec:
    lea rcx, SafeMmioRead32_error
    jmp recover_done_&vec
recover_write_mmio_&vec:
    lea rcx, SafeMmioWrite32_error
    jmp recover_done_&vec
recover_or_mmio_&vec:
    lea rcx, SafeMmioOr32_error
recover_done_&vec:
    mov qword ptr [rsp + 16 + hasError * 8], rcx
    pop rcx
    pop rax
    IF hasError
        add rsp, 8
    ENDIF
    iretq
ENDM

SAFE_PROBE_ISR 6, 0
SAFE_PROBE_ISR 13, 1
SAFE_PROBE_ISR 14, 1

PUBLIC GetPciExpressBaseAddress
GetPciExpressBaseAddress PROC
    push rdx
    pushfq
    cli
    mov eax, 80000060h
    mov dx, 0cf8h
    out dx, eax
    mov dx, 0cfch
    in eax, dx
    popfq
    pop rdx
    ret
GetPciExpressBaseAddress ENDP

PUBLIC SafeReadMsr64
SafeReadMsr64 PROC
    mov r11, rdx
SafeReadMsr64_access::
    rdmsr
    shl rdx, 32
    or rax, rdx
    test r11, r11
    jz read_msr_done
    mov dword ptr [r11], 0
read_msr_done:
    ret
SafeReadMsr64_error::
    xor eax, eax
    test r11, r11
    jz read_msr_done
    mov dword ptr [r11], 1
    ret
SafeReadMsr64 ENDP

PUBLIC SafeWriteMsr64
SafeWriteMsr64 PROC
    mov rax, rdx
    shr rdx, 32
SafeWriteMsr64_access::
    wrmsr
    xor eax, eax
    ret
SafeWriteMsr64_error::
    mov eax, 1
    ret
SafeWriteMsr64 ENDP

PUBLIC SafeMmioWrite32
SafeMmioWrite32 PROC
    test ecx, 3
    jnz SafeMmioWrite32_error
    mov ecx, ecx
SafeMmioWrite32_access::
    mov dword ptr [rcx], edx
    xor eax, eax
    ret
SafeMmioWrite32_error::
    mov eax, 1
    ret
SafeMmioWrite32 ENDP

PUBLIC SafeMmioRead32
SafeMmioRead32 PROC
    test ecx, 3
    jnz SafeMmioRead32_error
    mov ecx, ecx
SafeMmioRead32_access::
    mov eax, dword ptr [rcx]
    test rdx, rdx
    jz read_mmio_done
    mov dword ptr [rdx], 0
read_mmio_done:
    ret
SafeMmioRead32_error::
    xor eax, eax
    test rdx, rdx
    jz read_mmio_done
    mov dword ptr [rdx], 1
    ret
SafeMmioRead32 ENDP

PUBLIC SafeMmioOr32
SafeMmioOr32 PROC
    test ecx, 3
    jnz SafeMmioOr32_error
    mov ecx, ecx
SafeMmioOr32_read_access::
    mov eax, dword ptr [rcx]
    or eax, edx
SafeMmioOr32_write_access::
    mov dword ptr [rcx], eax
    xor eax, eax
    ret
SafeMmioOr32_error::
    mov eax, 1
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
PUBLIC gOriginalIsr6
PUBLIC gOriginalIsr13
PUBLIC gOriginalIsr14
gOriginalIsr6 DQ 0
gOriginalIsr13 DQ 0
gOriginalIsr14 DQ 0
END
