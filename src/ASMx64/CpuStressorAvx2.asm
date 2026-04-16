; CpuStressorAvx2.asm — AVX2 mixed-workload stress kernel (ComboHell):
;   Exercises integer VPXOR chains (ymm0–ymm3, ymm11–ymm14) and FP VRCPPS/VDPPS
;   (ymm8–ymm10) in alternating waves to maximise execution-unit pressure across
;   the vector pipeline.  After each outer iteration the integer results are
;   compared against a shadow copy (ymm11–ymm14); any mismatch increments a
;   shared error counter (LOCK XADD).  The kernel runs for ComboHell_MaxRuns outer
;   loops of 0x10000000 inner iterations, or until ComboHell_StopRequestPtr != 0.

.CODE
ALIGN 16

PUBLIC RunAvx2StressKernel

; rcx: pointer to cbhell_ymm_input (11 × 32-byte aligned YMM values):
;        [rcx+  0] .. [rcx+224] → YMM0–YMM7  integer working set (VPXOR inputs)
;        [rcx+256] .. [rcx+320] → YMM8–YMM10 FP working set (VRCPPS/VDPPS)
; r10: ComboHell_StopRequestPtr — pointer to stop-flag DWORD (checked each outer loop)
; r11: ComboHell_ErrorCounterPtr — pointer to 64-bit error counter (LOCK XADD)
; rdx: outer-loop run counter (0-based, checked against ComboHell_MaxRuns)
; esi: inner-loop downcounter (0x10000000 iterations per outer run)
; ymm11–ymm14: shadow copies of ymm0–ymm3 for post-run comparison
RunAvx2StressKernel PROC
    push rsi

    xor  rdx, rdx                          ; outer run counter = 0
    mov  r10, ComboHell_StopRequestPtr     ; cache stop-flag pointer
    mov  r11, ComboHell_ErrorCounterPtr    ; cache error counter pointer

    ; Load integer working set into ymm0–ymm7 (inputs for VPXOR chains)
    vmovdqa ymm0, YMMWORD PTR [rcx]
    vmovdqa ymm1, YMMWORD PTR [rcx + 32]
    vmovdqa ymm2, YMMWORD PTR [rcx + 64]
    vmovdqa ymm3, YMMWORD PTR [rcx + 96]
    vmovdqa ymm4, YMMWORD PTR [rcx + 128]
    vmovdqa ymm5, YMMWORD PTR [rcx + 160]
    vmovdqa ymm6, YMMWORD PTR [rcx + 192]
    vmovdqa ymm7, YMMWORD PTR [rcx + 224]

    ; Load FP working set into ymm8–ymm10 (inputs for VRCPPS/VDPPS)
    vmovaps ymm8,  YMMWORD PTR [rcx + 256]
    vmovaps ymm9,  YMMWORD PTR [rcx + 288]
    vmovaps ymm10, YMMWORD PTR [rcx + 320]

    ; Initialise shadow copies for correctness tracking
    vmovdqa ymm11, ymm0
    vmovdqa ymm12, ymm1
    vmovdqa ymm13, ymm2
    vmovdqa ymm14, ymm3

    mov esi, 10000000h                     ; 268M inner iterations per outer run

combohell:
    ; Reload FP input each iteration — prevents compiler-style loop hoisting and
    ; keeps the load unit occupied alongside the execution ports
    vmovaps ymm15, YMMWORD PTR [rcx]

    ; Integer XOR chain A: ymm0–ymm3 ← ymm0–ymm3 XOR ymm4–ymm7
    vpxor ymm0, ymm4, ymm0
    vpxor ymm1, ymm5, ymm1
    vpxor ymm2, ymm6, ymm2
    vpxor ymm3, ymm7, ymm3

    ; FP reciprocal estimate — exercises the FP divide/recip unit
    vrcpps ymm15, ymm15

    ; Integer XOR chain B (shadow): ymm11–ymm14 same transform as chain A
    vpxor ymm11, ymm4, ymm11
    vpxor ymm12, ymm5, ymm12
    vpxor ymm13, ymm6, ymm13
    vpxor ymm14, ymm7, ymm14

    ; FP dot product (all lanes, imm8=0xFF: all inputs used, all outputs written)
    ; — exercises the FP multiply-add path and packs results across all 8 floats
    vdpps ymm8, ymm9, ymm10, 0FFh

    sub esi, 1
    jnz combohell                          ; inner loop: 0x10000000 iterations

    add rdx, 1
    cmp rdx, ComboHell_MaxRuns
    jg done_label                          ; exceeded max outer runs → exit clean

errcheck:
    ; Compare chains A and B element-wise (32-bit lanes); all-ones mask = no error
    vpcmpeqd ymm11, ymm0, ymm11
    vpcmpeqd ymm12, ymm1, ymm12
    vpcmpeqd ymm13, ymm2, ymm13
    vpcmpeqd ymm14, ymm3, ymm14

    ; AND all four comparison results: ymm11 = all-ones only if every lane matched
    vpand ymm11, ymm11, ymm12
    vpand ymm11, ymm11, ymm13
    vpand ymm11, ymm11, ymm14

    ; Extract byte mask: 0xFFFFFFFF means all 32 bytes (8 dwords × 4 bytes) matched
    vpmovmskb r8d, ymm11

    cmp r8d, 0FFFFFFFFh
    jne cmperr                             ; any mismatch → error path

    ; No error: refresh shadow copies from current A-chain state
    vmovdqa ymm11, ymm0
    vmovdqa ymm12, ymm1
    vmovdqa ymm13, ymm2
    vmovdqa ymm14, ymm3

    ; Check stop flag before starting next outer run
    mov  r9d, DWORD PTR [r10]
    test r9d, r9d
    jz combohell                           ; stop flag clear → continue

    xor eax, eax                           ; clean exit (stop requested, no errors)

done_label:
    pop rsi
    ret

cmperr:
    ; Atomically increment the shared error counter (other APs may fault concurrently)
    mov r8, 1
    lock xadd QWORD PTR [r11], r8

    mov r8, ComboHell_TerminateOnError
    test r8, r8
    jnz errcleanup                         ; hard-stop mode: poison stop flag and exit

    ; Soft-error mode: keep running unless stop flag is set
    mov  r9d, DWORD PTR [r10]
    test r9d, r9d
    jz combohell

seterr:
    mov eax, 0BADDC0DEh                    ; sentinel return value indicating errors detected
    jmp done_label

errcleanup:
    ; Hard-stop: write 1 into *ComboHell_StopRequestPtr so all APs exit after this run
    mov  QWORD PTR [r10], 1
    jmp  seterr
RunAvx2StressKernel ENDP

.DATA
ALIGN 16

; Pointer to a DWORD stop-flag; non-zero requests kernel exit after current outer run
PUBLIC ComboHell_StopRequestPtr
ComboHell_StopRequestPtr DQ 0

; Pointer to a UINT64 error counter; incremented atomically on any XOR chain mismatch
PUBLIC ComboHell_ErrorCounterPtr
ComboHell_ErrorCounterPtr DQ 0

; Maximum number of outer runs (each = 0x10000000 inner iterations); 0 = unlimited
PUBLIC ComboHell_MaxRuns
ComboHell_MaxRuns DQ 0

; If non-zero, poison the stop flag and return 0xBADDC0DE on first error (hard-stop mode)
PUBLIC ComboHell_TerminateOnError
ComboHell_TerminateOnError DQ 0

ComboHell_SavedRdx DQ 0    ; reserved (unused in current implementation)
ComboHell_SavedRax DQ 0    ; reserved (unused in current implementation)

END