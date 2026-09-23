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
; Separate translated blocks exercise candidate replacement after the first 64.
; Distinct addresses stress bounded discovery; all share one simple oracle.
ARC_LATE MACRO label_name
PUBLIC label_name
label_name PROC
    mov rax, rcx
    lea rax, [rax+1]
    ret
label_name ENDP
ENDM

ARC_LATE arc_late_region_00
ARC_LATE arc_late_region_01
ARC_LATE arc_late_region_02
ARC_LATE arc_late_region_03
ARC_LATE arc_late_region_04
ARC_LATE arc_late_region_05
ARC_LATE arc_late_region_06
ARC_LATE arc_late_region_07
ARC_LATE arc_late_region_08
ARC_LATE arc_late_region_09
ARC_LATE arc_late_region_10
ARC_LATE arc_late_region_11
ARC_LATE arc_late_region_12
ARC_LATE arc_late_region_13
ARC_LATE arc_late_region_14
ARC_LATE arc_late_region_15
ARC_LATE arc_late_region_16
ARC_LATE arc_late_region_17
ARC_LATE arc_late_region_18
ARC_LATE arc_late_region_19
ARC_LATE arc_late_region_20
ARC_LATE arc_late_region_21
ARC_LATE arc_late_region_22
ARC_LATE arc_late_region_23
ARC_LATE arc_late_region_24
ARC_LATE arc_late_region_25
ARC_LATE arc_late_region_26
ARC_LATE arc_late_region_27
ARC_LATE arc_late_region_28
ARC_LATE arc_late_region_29
ARC_LATE arc_late_region_30
ARC_LATE arc_late_region_31
ARC_LATE arc_late_region_32
ARC_LATE arc_late_region_33
ARC_LATE arc_late_region_34
ARC_LATE arc_late_region_35
ARC_LATE arc_late_region_36
ARC_LATE arc_late_region_37
ARC_LATE arc_late_region_38
ARC_LATE arc_late_region_39
ARC_LATE arc_late_region_40
ARC_LATE arc_late_region_41
ARC_LATE arc_late_region_42
ARC_LATE arc_late_region_43
ARC_LATE arc_late_region_44
ARC_LATE arc_late_region_45
ARC_LATE arc_late_region_46
ARC_LATE arc_late_region_47
ARC_LATE arc_late_region_48
ARC_LATE arc_late_region_49
ARC_LATE arc_late_region_50
ARC_LATE arc_late_region_51
ARC_LATE arc_late_region_52
ARC_LATE arc_late_region_53
ARC_LATE arc_late_region_54
ARC_LATE arc_late_region_55
ARC_LATE arc_late_region_56
ARC_LATE arc_late_region_57
ARC_LATE arc_late_region_58
ARC_LATE arc_late_region_59
ARC_LATE arc_late_region_60
ARC_LATE arc_late_region_61
ARC_LATE arc_late_region_62
ARC_LATE arc_late_region_63
ARC_LATE arc_late_region_64
ARC_LATE arc_late_region_65
ARC_LATE arc_late_region_66
ARC_LATE arc_late_region_67
ARC_LATE arc_late_region_68
ARC_LATE arc_late_region_69
ARC_LATE arc_late_region_70
ARC_LATE arc_late_region_71
ARC_LATE arc_late_region_72
ARC_LATE arc_late_region_73
ARC_LATE arc_late_region_74
ARC_LATE arc_late_region_75
ARC_LATE arc_late_region_76
ARC_LATE arc_late_region_77
ARC_LATE arc_late_region_78
ARC_LATE arc_late_region_79
ARC_LATE arc_late_region_080
ARC_LATE arc_late_region_081
ARC_LATE arc_late_region_082
ARC_LATE arc_late_region_083
ARC_LATE arc_late_region_084
ARC_LATE arc_late_region_085
ARC_LATE arc_late_region_086
ARC_LATE arc_late_region_087
ARC_LATE arc_late_region_088
ARC_LATE arc_late_region_089
ARC_LATE arc_late_region_090
ARC_LATE arc_late_region_091
ARC_LATE arc_late_region_092
ARC_LATE arc_late_region_093
ARC_LATE arc_late_region_094
ARC_LATE arc_late_region_095
ARC_LATE arc_late_region_096
ARC_LATE arc_late_region_097
ARC_LATE arc_late_region_098
ARC_LATE arc_late_region_099
ARC_LATE arc_late_region_100
ARC_LATE arc_late_region_101
ARC_LATE arc_late_region_102
ARC_LATE arc_late_region_103
ARC_LATE arc_late_region_104
ARC_LATE arc_late_region_105
ARC_LATE arc_late_region_106
ARC_LATE arc_late_region_107
ARC_LATE arc_late_region_108
ARC_LATE arc_late_region_109
ARC_LATE arc_late_region_110
ARC_LATE arc_late_region_111
ARC_LATE arc_late_region_112
ARC_LATE arc_late_region_113
ARC_LATE arc_late_region_114
ARC_LATE arc_late_region_115
ARC_LATE arc_late_region_116
ARC_LATE arc_late_region_117
ARC_LATE arc_late_region_118
ARC_LATE arc_late_region_119
ARC_LATE arc_late_region_120
ARC_LATE arc_late_region_121
ARC_LATE arc_late_region_122
ARC_LATE arc_late_region_123
ARC_LATE arc_late_region_124
ARC_LATE arc_late_region_125
ARC_LATE arc_late_region_126
ARC_LATE arc_late_region_127
ARC_LATE arc_late_region_128
ARC_LATE arc_late_region_129
ARC_LATE arc_late_region_130
ARC_LATE arc_late_region_131
ARC_LATE arc_late_region_132
ARC_LATE arc_late_region_133
ARC_LATE arc_late_region_134
ARC_LATE arc_late_region_135
ARC_LATE arc_late_region_136
ARC_LATE arc_late_region_137
ARC_LATE arc_late_region_138
ARC_LATE arc_late_region_139
ARC_LATE arc_late_region_140
ARC_LATE arc_late_region_141
ARC_LATE arc_late_region_142
ARC_LATE arc_late_region_143
ARC_LATE arc_late_region_144
ARC_LATE arc_late_region_145
ARC_LATE arc_late_region_146
ARC_LATE arc_late_region_147
ARC_LATE arc_late_region_148
ARC_LATE arc_late_region_149
ARC_LATE arc_late_region_150
ARC_LATE arc_late_region_151
ARC_LATE arc_late_region_152
ARC_LATE arc_late_region_153
ARC_LATE arc_late_region_154
ARC_LATE arc_late_region_155
ARC_LATE arc_late_region_156
ARC_LATE arc_late_region_157
ARC_LATE arc_late_region_158
ARC_LATE arc_late_region_159
ARC_LATE arc_late_region_160
ARC_LATE arc_late_region_161
ARC_LATE arc_late_region_162
ARC_LATE arc_late_region_163
ARC_LATE arc_late_region_164
ARC_LATE arc_late_region_165
ARC_LATE arc_late_region_166
ARC_LATE arc_late_region_167
ARC_LATE arc_late_region_168
ARC_LATE arc_late_region_169
ARC_LATE arc_late_region_170
ARC_LATE arc_late_region_171
ARC_LATE arc_late_region_172
ARC_LATE arc_late_region_173
ARC_LATE arc_late_region_174
ARC_LATE arc_late_region_175
ARC_LATE arc_late_region_176
ARC_LATE arc_late_region_177
ARC_LATE arc_late_region_178
ARC_LATE arc_late_region_179
ARC_LATE arc_late_region_180
ARC_LATE arc_late_region_181
ARC_LATE arc_late_region_182
ARC_LATE arc_late_region_183
ARC_LATE arc_late_region_184
ARC_LATE arc_late_region_185
ARC_LATE arc_late_region_186
ARC_LATE arc_late_region_187
ARC_LATE arc_late_region_188
ARC_LATE arc_late_region_189
ARC_LATE arc_late_region_190
ARC_LATE arc_late_region_191
ARC_LATE arc_late_region_192
ARC_LATE arc_late_region_193
ARC_LATE arc_late_region_194
ARC_LATE arc_late_region_195
ARC_LATE arc_late_region_196
ARC_LATE arc_late_region_197
ARC_LATE arc_late_region_198
ARC_LATE arc_late_region_199
ARC_LATE arc_late_region_200
ARC_LATE arc_late_region_201
ARC_LATE arc_late_region_202
ARC_LATE arc_late_region_203
ARC_LATE arc_late_region_204
ARC_LATE arc_late_region_205
ARC_LATE arc_late_region_206
ARC_LATE arc_late_region_207
ARC_LATE arc_late_region_208
ARC_LATE arc_late_region_209

END
