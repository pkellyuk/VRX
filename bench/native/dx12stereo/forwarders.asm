; Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
; Windows D3D12 private/PIX exports are forwarded without guessing signatures.
; Preserve all Win64 argument registers and the caller's stack, then tail-jump.
EXTERN ResolveSystemOrdinal:PROC
.code
FORWARD MACRO name, ordinal
name PROC FRAME
    sub rsp, 0A8h
    .allocstack 0A8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movdqu [rsp+40h], xmm0
    movdqu [rsp+50h], xmm1
    movdqu [rsp+60h], xmm2
    movdqu [rsp+70h], xmm3
    movdqu [rsp+80h], xmm4
    movdqu [rsp+90h], xmm5
    mov ecx, ordinal
    call ResolveSystemOrdinal
    mov r11, rax
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    movdqu xmm0, [rsp+40h]
    movdqu xmm1, [rsp+50h]
    movdqu xmm2, [rsp+60h]
    movdqu xmm3, [rsp+70h]
    movdqu xmm4, [rsp+80h]
    movdqu xmm5, [rsp+90h]
    add rsp, 0A8h
    jmp r11
name ENDP
ENDM
FORWARD D3D12PrivateOrdinal99, 99
FORWARD SetAppCompatStringPointer, 100
FORWARD D3D12CoreCreateLayeredDevice, 103
FORWARD D3D12CoreGetLayeredDeviceSize, 104
FORWARD D3D12CoreRegisterLayers, 105
FORWARD D3D12DeviceRemovedExtendedData, 108
FORWARD D3D12PIXEventsReplaceBlock, 111
FORWARD D3D12PIXGetThreadInfo, 112
FORWARD D3D12PIXNotifyWakeFromFenceSignal, 113
FORWARD D3D12PIXReportCounter, 114
FORWARD GetBehaviorValue, 117
END
