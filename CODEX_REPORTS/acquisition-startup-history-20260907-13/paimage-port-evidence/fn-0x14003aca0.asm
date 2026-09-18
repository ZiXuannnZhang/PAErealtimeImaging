000000014003aca0: add      byte ptr [rax - 0x75], cl                                  ; 
000000014003aca3: and      al, 0x60                                                   ; 
000000014003aca6: mov      rax, rcx                                                   ; 
000000014003aca9: cmp      rdx, 0x1000                                                ; 
000000014003acb0: jb       0x14003accb                                                ; 
000000014003acb2: add      rdx, 0x27                                                  ; 
000000014003acb6: mov      rcx, qword ptr [rcx - 8]                                   ; 
000000014003acba: sub      rax, rcx                                                   ; 
000000014003acbd: sub      rax, 8                                                     ; 
000000014003acc1: cmp      rax, 0x1f                                                  ; 
000000014003acc5: ja       0x14003b54a                                                ; 
000000014003accb: call     0x1404117bc                                                ; 
000000014003acd0: mov      rcx, qword ptr [rip + 0x5180b1]                            ; 0x140552d88 
000000014003acd7: xor      r8d, r8d                                                   ; 
000000014003acda: test     rcx, rcx                                                   ; 
000000014003acdd: jne      0x14003ace4                                                ; 
000000014003acdf: mov      eax, r8d                                                   ; 
000000014003ace2: jmp      0x14003acfe                                                ; 
000000014003ace4: xor      r9d, r9d                                                   ; 
000000014003ace7: mov      edx, 0x147                                                 ; 
000000014003acec: call     qword ptr [rip + 0x423aae]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003acf2: test     eax, eax                                                   ; 
000000014003acf4: mov      r8d, 0                                                     ; 
000000014003acfa: cmovs    eax, r8d                                                   ; 
000000014003acfe: mov      ecx, 0x10                                                  ; 
000000014003ad03: mov      edx, 0x20                                                  ; 
000000014003ad08: cmp      eax, 1                                                     ; 
000000014003ad0b: cmove    ecx, edx                                                   ; 
000000014003ad0e: mov      dword ptr [rbx + 0x64], ecx                                ; 
000000014003ad11: mov      rcx, qword ptr [rip + 0x5180b8]                            ; 0x140552dd0 
000000014003ad18: test     rcx, rcx                                                   ; 
000000014003ad1b: jne      0x14003ad22                                                ; 
000000014003ad1d: mov      eax, r8d                                                   ; 
000000014003ad20: jmp      0x14003ad3d                                                ; 
000000014003ad22: xor      r9d, r9d                                                   ; 
000000014003ad25: xor      r8d, r8d                                                   ; 
000000014003ad28: mov      edx, 0x147                                                 ; 
000000014003ad2d: call     qword ptr [rip + 0x423a6d]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003ad33: test     eax, eax                                                   ; 
000000014003ad35: mov      ecx, 0                                                     ; 
000000014003ad3a: cmovs    eax, ecx                                                   ; 
000000014003ad3d: cmp      eax, 1                                                     ; 
000000014003ad40: setne    al                                                         ; 
000000014003ad43: mov      byte ptr [rbx + 0x70], al                                  ; 
000000014003ad46: mov      byte ptr [rbx + 0x84], r12b                                ; 
000000014003ad4d: mov      byte ptr [rbx + 0x85], r13b                                ; 
000000014003ad54: mov      eax, 8                                                     ; 
000000014003ad59: mov      ecx, 0x40                                                  ; 
000000014003ad5e: cmp      byte ptr [rsp + 0x40], 0                                   ; 
000000014003ad63: cmovne   eax, ecx                                                   ; 
000000014003ad66: mov      dword ptr [rbx + 0x78], eax                                ; 
000000014003ad69: mov      byte ptr [rbx + 0x7c], r14b                                ; 
000000014003ad6d: mov      eax, 0x3e8                                                 ; 
000000014003ad72: cmp      r15d, 4                                                    ; 
000000014003ad76: mov      r14d, 0                                                    ; 
000000014003ad7c: cmova    eax, r14d                                                  ; 
000000014003ad80: mov      dword ptr [rbx + 0x80], eax                                ; 
000000014003ad86: mov      dword ptr [rsp + 0x48], 2                                  ; 
000000014003ad8e: mov      dword ptr [rsp + 0x50], r14d                               ; 
000000014003ad93: mov      rcx, qword ptr [rip + 0x51832e]                            ; 0x1405530c8 
000000014003ad9a: test     rcx, rcx                                                   ; 
000000014003ad9d: jne      0x14003ada4                                                ; 
000000014003ad9f: mov      edx, r14d                                                  ; 
000000014003ada2: jmp      0x14003adbe                                                ; 
000000014003ada4: xor      r9d, r9d                                                   ; 
000000014003ada7: xor      r8d, r8d                                                   ; 
000000014003adaa: mov      edx, 0x147                                                 ; 
000000014003adaf: call     qword ptr [rip + 0x4239eb]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003adb5: mov      rdx, rax                                                   ; 
000000014003adb8: test     eax, eax                                                   ; 
000000014003adba: cmovs    edx, r14d                                                  ; 
000000014003adbe: mov      dword ptr [rsp + 0x58], edx                                ; 
000000014003adc2: lea      rcx, [rsp + 0x50]                                          ; 
000000014003adc7: lea      rax, [rsp + 0x58]                                          ; 
000000014003adcc: test     edx, edx                                                   ; 
000000014003adce: cmovns   rcx, rax                                                   ; 
000000014003add2: lea      rax, [rsp + 0x48]                                          ; 
000000014003add7: cmp      edx, 2                                                     ; 
000000014003adda: cmovle   rax, rcx                                                   ; 
000000014003adde: mov      eax, dword ptr [rax]                                       ; 
000000014003ade0: mov      dword ptr [rbx + 0x168], eax                               ; 
000000014003ade6: xor      r9d, r9d                                                   ; 
000000014003ade9: xor      r8d, r8d                                                   ; 
000000014003adec: mov      edx, 0xf0                                                  ; 
000000014003adf1: mov      rcx, qword ptr [rip + 0x518220]                            ; 0x140553018 
000000014003adf8: call     qword ptr [rip + 0x4239a2]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003adfe: .byte    0x48                                                       ; 
000000014003adff: .byte    0x83                                                       ; 