; Pass-through thunk shared by every opengl32 export / GL proc we don't wrap by hand.
; On entry r10 = ProcEntry* (first field = real function pointer); the caller's
; arguments are untouched. Normally this is a single compare + tail jump; while
; g_slowPath is set it reports the call to LogGenericCall first.

EXTERN g_slowPath:BYTE
EXTERN LogGenericCall:PROC

.code

GenericThunk PROC
    cmp byte ptr [g_slowPath], 0
    jne GenericSlow
    jmp qword ptr [r10]
GenericThunk ENDP

GenericSlow PROC FRAME
    mov [rsp+8], rcx            ; spill integer args into our home area
    mov [rsp+10h], rdx
    mov [rsp+18h], r8
    mov [rsp+20h], r9
    sub rsp, 68h                ; 20h shadow + 4 xmm + r10, keeps rsp 16-aligned
    .allocstack 68h
    .endprolog
    movdqu xmmword ptr [rsp+20h], xmm0
    movdqu xmmword ptr [rsp+30h], xmm1
    movdqu xmmword ptr [rsp+40h], xmm2
    movdqu xmmword ptr [rsp+50h], xmm3
    mov [rsp+60h], r10
    mov rcx, r10                ; LogGenericCall(entry, intArgs, xmmArgs)
    lea rdx, [rsp+70h]
    lea r8, [rsp+20h]
    call LogGenericCall
    movdqu xmm0, xmmword ptr [rsp+20h]
    movdqu xmm1, xmmword ptr [rsp+30h]
    movdqu xmm2, xmmword ptr [rsp+40h]
    movdqu xmm3, xmmword ptr [rsp+50h]
    mov rcx, [rsp+70h]
    mov rdx, [rsp+78h]
    mov r8, [rsp+80h]
    mov r9, [rsp+88h]
    mov r10, [rsp+60h]
    add rsp, 68h
    jmp qword ptr [r10]
GenericSlow ENDP

END
