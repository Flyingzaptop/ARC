OPTION CASEMAP:NONE
EXTERN g_hostState:BYTE
EXTERN g_exitState:BYTE

.code

; EntryState layout is checked with static_assert in replay.cpp.
StartIsolated PROC
    mov r10, rcx
    lea r11, g_hostState
    mov [r11+0], rsp
    mov [r11+8], rbx
    mov [r11+16], rbp
    mov [r11+24], rsi
    mov [r11+32], rdi
    mov [r11+40], r12
    mov [r11+48], r13
    mov [r11+56], r14
    mov [r11+64], r15
    stmxcsr DWORD PTR [r11+72]
    movdqu XMMWORD PTR [r11+80], xmm6
    movdqu XMMWORD PTR [r11+96], xmm7
    movdqu XMMWORD PTR [r11+112], xmm8
    movdqu XMMWORD PTR [r11+128], xmm9
    movdqu XMMWORD PTR [r11+144], xmm10
    movdqu XMMWORD PTR [r11+160], xmm11
    movdqu XMMWORD PTR [r11+176], xmm12
    movdqu XMMWORD PTR [r11+192], xmm13
    movdqu XMMWORD PTR [r11+208], xmm14
    movdqu XMMWORD PTR [r11+224], xmm15

    ldmxcsr DWORD PTR [r10+144]
    movdqu xmm0, XMMWORD PTR [r10+160]
    movdqu xmm1, XMMWORD PTR [r10+176]
    movdqu xmm2, XMMWORD PTR [r10+192]
    movdqu xmm3, XMMWORD PTR [r10+208]
    movdqu xmm4, XMMWORD PTR [r10+224]
    movdqu xmm5, XMMWORD PTR [r10+240]
    movdqu xmm6, XMMWORD PTR [r10+256]
    movdqu xmm7, XMMWORD PTR [r10+272]
    movdqu xmm8, XMMWORD PTR [r10+288]
    movdqu xmm9, XMMWORD PTR [r10+304]
    movdqu xmm10, XMMWORD PTR [r10+320]
    movdqu xmm11, XMMWORD PTR [r10+336]
    movdqu xmm12, XMMWORD PTR [r10+352]
    movdqu xmm13, XMMWORD PTR [r10+368]
    movdqu xmm14, XMMWORD PTR [r10+384]
    movdqu xmm15, XMMWORD PTR [r10+400]
    push QWORD PTR [r10+136]
    popfq
    mov rax, [r10+8]
    mov rcx, [r10+16]
    mov rdx, [r10+24]
    mov rbx, [r10+32]
    mov rbp, [r10+48]
    mov rsi, [r10+56]
    mov rdi, [r10+64]
    mov r8, [r10+72]
    mov r9, [r10+80]
    mov r12, [r10+104]
    mov r13, [r10+112]
    mov r14, [r10+120]
    mov r15, [r10+128]
    mov rsp, [r10+40]
    mov r11, [r10+0]
    mov [rsp-8], r11
    mov r11, [r10+96]
    mov r10, [r10+88]
    jmp QWORD PTR [rsp-8]
StartIsolated ENDP

ReturnStub PROC
    mov QWORD PTR [g_exitState+8], rax
    lea rax, g_exitState
    mov [rax+16], rcx
    mov [rax+24], rdx
    mov [rax+32], rbx
    mov [rax+40], rsp
    mov [rax+48], rbp
    mov [rax+56], rsi
    mov [rax+64], rdi
    mov [rax+72], r8
    mov [rax+80], r9
    mov [rax+88], r10
    mov [rax+96], r11
    mov [rax+104], r12
    mov [rax+112], r13
    mov [rax+120], r14
    mov [rax+128], r15
    pushfq
    pop rcx
    mov [rax+136], rcx
    stmxcsr DWORD PTR [rax+144]
    movdqu XMMWORD PTR [rax+160], xmm0
    movdqu XMMWORD PTR [rax+176], xmm1
    movdqu XMMWORD PTR [rax+192], xmm2
    movdqu XMMWORD PTR [rax+208], xmm3
    movdqu XMMWORD PTR [rax+224], xmm4
    movdqu XMMWORD PTR [rax+240], xmm5
    movdqu XMMWORD PTR [rax+256], xmm6
    movdqu XMMWORD PTR [rax+272], xmm7
    movdqu XMMWORD PTR [rax+288], xmm8
    movdqu XMMWORD PTR [rax+304], xmm9
    movdqu XMMWORD PTR [rax+320], xmm10
    movdqu XMMWORD PTR [rax+336], xmm11
    movdqu XMMWORD PTR [rax+352], xmm12
    movdqu XMMWORD PTR [rax+368], xmm13
    movdqu XMMWORD PTR [rax+384], xmm14
    movdqu XMMWORD PTR [rax+400], xmm15
    lea r11, g_hostState
    ldmxcsr DWORD PTR [r11+72]
    movdqu xmm6, XMMWORD PTR [r11+80]
    movdqu xmm7, XMMWORD PTR [r11+96]
    movdqu xmm8, XMMWORD PTR [r11+112]
    movdqu xmm9, XMMWORD PTR [r11+128]
    movdqu xmm10, XMMWORD PTR [r11+144]
    movdqu xmm11, XMMWORD PTR [r11+160]
    movdqu xmm12, XMMWORD PTR [r11+176]
    movdqu xmm13, XMMWORD PTR [r11+192]
    movdqu xmm14, XMMWORD PTR [r11+208]
    movdqu xmm15, XMMWORD PTR [r11+224]
    mov rbx, [r11+8]
    mov rbp, [r11+16]
    mov rsi, [r11+24]
    mov rdi, [r11+32]
    mov r12, [r11+40]
    mov r13, [r11+48]
    mov r14, [r11+56]
    mov r15, [r11+64]
    mov rsp, [r11+0]
    ret
ReturnStub ENDP
END
