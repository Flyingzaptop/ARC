.const
ALIGN 16
xmm_pattern0 db 00h,11h,22h,33h,44h,55h,66h,77h,88h,99h,0aah,0bbh,0cch,0ddh,0eeh,0ffh
xmm_pattern1 db 0ffh,0eeh,0ddh,0cch,0bbh,0aah,99h,88h,77h,66h,55h,44h,33h,22h,11h,00h

.code
PUBLIC arc_native_region
arc_native_region PROC
    ; Independent input slices: RCX dirties nodes 0,2; RDX dirties 1,2.
    lea rax, [rcx+7]
    lea r10, [rdx+9]
    lea rax, [rax+r10*2]
    ret
arc_native_region ENDP

PUBLIC arc_native_side_effect
arc_native_side_effect PROC
    mov rax, rcx
    mov [rdx], rax
    ret
arc_native_side_effect ENDP

; void arc_native_state_probe(uint64 a, uint64 b, NativeProbe* out)
; Preserve ABI nonvolatile registers while measuring the region's exact state.
PUBLIC arc_native_state_probe
arc_native_state_probe PROC FRAME
    push rbx
    .pushreg rbx
    push r12
    .pushreg r12
    sub rsp, 68h
    .allocstack 68h
    .endprolog

    mov [rsp+20h], r8
    mov rbx, 1122334455667788h
    mov r12, 8877665544332211h
    stmxcsr dword ptr [rsp+38h]
    fnstcw word ptr [rsp+40h]
    movdqu xmm0, xmmword ptr [xmm_pattern0]
    movdqu xmm1, xmmword ptr [xmm_pattern1]
    mov [rsp+30h], rsp
    fld1
    cmp rcx, rcx
    pushfq
    pop r11
    mov [rsp+28h], r11
    call arc_native_region

    mov r9, [rsp+20h]
    mov [r9], rax
    mov [r9+8], r10
    pushfq
    pop r11
    mov rax, [rsp+28h]
    mov [r9+16], rax
    mov [r9+24], r11
    mov rax, [rsp+30h]
    mov [r9+32], rax
    mov [r9+40], rsp
    mov eax, dword ptr [rsp+38h]
    mov dword ptr [r9+48], eax
    stmxcsr dword ptr [r9+52]
    movdqu xmmword ptr [r9+56], xmm0
    movdqu xmmword ptr [r9+72], xmm1
    mov rax, 1122334455667788h
    mov [r9+88], rax
    mov [r9+96], rbx
    mov rax, 8877665544332211h
    mov [r9+104], rax
    mov [r9+112], r12
    mov [r9+120], rcx
    mov [r9+128], rdx
    fstp qword ptr [r9+136]
    mov ax, word ptr [rsp+40h]
    mov word ptr [r9+144], ax
    fnstcw word ptr [r9+146]

    add rsp, 68h
    pop r12
    pop rbx
    ret
arc_native_state_probe ENDP
END
