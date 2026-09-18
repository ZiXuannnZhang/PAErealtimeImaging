000000014003a9c0: mov      qword ptr [rsp + 0x18], rbx                                ; 
000000014003a9c5: push     rbp                                                        ; 
000000014003a9c6: push     rsi                                                        ; 
000000014003a9c7: push     rdi                                                        ; 
000000014003a9c8: push     r12                                                        ; 
000000014003a9ca: push     r13                                                        ; 
000000014003a9cc: push     r14                                                        ; 
000000014003a9ce: push     r15                                                        ; 
000000014003a9d0: lea      rbp, [rsp - 0x2c0]                                         ; 
000000014003a9d8: sub      rsp, 0x3c0                                                 ; 
000000014003a9df: movaps   xmmword ptr [rsp + 0x3b0], xmm6                            ; 
000000014003a9e7: mov      rax, qword ptr [rip + 0x50fa52]                            ; 0x14054a440 
000000014003a9ee: xor      rax, rsp                                                   ; 
000000014003a9f1: mov      qword ptr [rbp + 0x2a0], rax                               ; 
000000014003a9f8: mov      rdi, rdx                                                   ; 
000000014003a9fb: mov      rbx, rcx                                                   ; 
000000014003a9fe: xor      r14d, r14d                                                 ; 
000000014003aa01: lea      rcx, [rbp - 0x80]                                          ; 
000000014003aa05: call     0x140014a00                                                ; 
000000014003aa0a: nop                                                                 ; 
000000014003aa0b: mov      rcx, qword ptr [rip + 0x5183ae]                            ; 0x140552dc0 
000000014003aa12: test     rcx, rcx                                                   ; 
000000014003aa15: jne      0x14003aa23                                                ; 
000000014003aa17: mov      esi, r14d                                                  ; 
000000014003aa1a: mov      r15d, r14d                                                 ; 
000000014003aa1d: mov      byte ptr [rsp + 0x40], cl                                  ; 
000000014003aa21: jmp      0x14003aa61                                                ; 
000000014003aa23: xor      r9d, r9d                                                   ; 
000000014003aa26: xor      r8d, r8d                                                   ; 
000000014003aa29: mov      edx, 0x147                                                 ; 
000000014003aa2e: call     qword ptr [rip + 0x423d6c]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003aa34: mov      rsi, rax                                                   ; 
000000014003aa37: test     eax, eax                                                   ; 
000000014003aa39: cmovs    esi, r14d                                                  ; 
000000014003aa3d: mov      r15d, esi                                                  ; 
000000014003aa40: mov      ecx, esi                                                   ; 
000000014003aa42: cmp      esi, 4                                                     ; 
000000014003aa45: sete     r14b                                                       ; 
000000014003aa49: mov      eax, esi                                                   ; 
000000014003aa4b: cmp      esi, 1                                                     ; 
000000014003aa4e: je       0x14003aa5c                                                ; 
000000014003aa50: cmp      eax, 4                                                     ; 
000000014003aa53: je       0x14003aa5c                                                ; 
000000014003aa55: mov      byte ptr [rsp + 0x40], 0                                   ; 
000000014003aa5a: jmp      0x14003aa61                                                ; 
000000014003aa5c: mov      byte ptr [rsp + 0x40], 1                                   ; 
000000014003aa61: cmp      esi, 2                                                     ; 
000000014003aa64: sete     r12b                                                       ; 
000000014003aa68: cmp      esi, 3                                                     ; 
000000014003aa6b: sete     r13b                                                       ; 
000000014003aa6f: cmp      esi, 2                                                     ; 
000000014003aa72: je       0x14003abce                                                ; 
000000014003aa78: xor      r8d, r8d                                                   ; 
000000014003aa7b: mov      rdx, rdi                                                   ; 
000000014003aa7e: lea      rcx, [rbp - 0x80]                                          ; 
000000014003aa82: call     0x14003b580                                                ; 
000000014003aa87: test     al, al                                                     ; 
000000014003aa89: jne      0x14003abce                                                ; 
000000014003aa8f: mov      r15, qword ptr [rdi + 0x10]                                ; 
000000014003aa93: movabs   rcx, 0x7ffffffffffffffe                                    ; 
000000014003aa9d: mov      rax, rcx                                                   ; 
000000014003aaa0: sub      rax, r15                                                   ; 
000000014003aaa3: cmp      rax, 0x16                                                  ; 
000000014003aaa7: jb       0x14003b514                                                ; 
000000014003aaad: mov      r12, rdi                                                   ; 
000000014003aab0: cmp      qword ptr [rdi + 0x18], 7                                  ; 
000000014003aab5: jbe      0x14003aaba                                                ; 
000000014003aab7: mov      r12, qword ptr [rdi]                                       ; 
000000014003aaba: xorps    xmm0, xmm0                                                 ; 
000000014003aabd: movups   xmmword ptr [rsp + 0x60], xmm0                             ; 
000000014003aac2: lea      r14, [r15 + 0x16]                                          ; 
000000014003aac6: mov      ebx, 7                                                     ; 
000000014003aacb: lea      rsi, [rsp + 0x60]                                          ; 
000000014003aad0: cmp      r14, rbx                                                   ; 
000000014003aad3: jbe      0x14003ab02                                                ; 
000000014003aad5: mov      rbx, r14                                                   ; 
000000014003aad8: or       rbx, 7                                                     ; 
000000014003aadc: cmp      rbx, rcx                                                   ; 
000000014003aadf: jbe      0x14003aba6                                                ; 
000000014003aae5: mov      rbx, rcx                                                   ; 
000000014003aae8: movabs   rcx, 0x7fffffffffffffff                                    ; 
000000014003aaf2: add      rcx, rcx                                                   ; 
000000014003aaf5: call     0x140002690                                                ; 
000000014003aafa: mov      rsi, rax                                                   ; 
000000014003aafd: mov      qword ptr [rsp + 0x60], rax                                ; 
000000014003ab02: mov      qword ptr [rsp + 0x70], r14                                ; 
000000014003ab07: mov      qword ptr [rsp + 0x78], rbx                                ; 
000000014003ab0c: movups   xmm0, xmmword ptr [rip + 0x431fa5]                         ; 0x14046cab8 '当前重建参数无法作为实时 DAS 标定基础：'
000000014003ab13: movups   xmmword ptr [rsi], xmm0                                    ; 
000000014003ab16: movups   xmm1, xmmword ptr [rip + 0x431fab]                         ; 0x14046cac8 '作为实时 DAS 标定基础：'
000000014003ab1d: movups   xmmword ptr [rsi + 0x10], xmm1                             ; 
000000014003ab21: movsd    xmm0, qword ptr [rip + 0x431faf]                           ; 0x14046cad8 ' 标定基础：'
000000014003ab29: movsd    qword ptr [rsi + 0x20], xmm0                               ; 
000000014003ab2e: mov      eax, dword ptr [rip + 0x431fac]                            ; 0x14046cae0 
000000014003ab34: mov      dword ptr [rsi + 0x28], eax                                ; 
000000014003ab37: lea      r8, [r15 + r15]                                            ; 
000000014003ab3b: lea      rcx, [rsi + 0x2c]                                          ; 
000000014003ab3f: mov      rdx, r12                                                   ; 
000000014003ab42: call     0x140450030                                                ; 
000000014003ab47: xor      ebx, ebx                                                   ; 
000000014003ab49: mov      word ptr [rsi + r14*2], bx                                 ; 
000000014003ab4e: lea      rdx, [rsp + 0x60]                                          ; 
000000014003ab53: mov      rcx, rdi                                                   ; 
000000014003ab56: call     0x140018ce0                                                ; 
000000014003ab5b: mov      rdx, qword ptr [rsp + 0x78]                                ; 
000000014003ab60: cmp      rdx, 7                                                     ; 
000000014003ab64: jbe      0x14003b480                                                ; 
000000014003ab6a: lea      rdx, [rdx*2 + 2]                                           ; 
000000014003ab72: mov      rcx, qword ptr [rsp + 0x60]                                ; 
000000014003ab77: mov      rax, rcx                                                   ; 
000000014003ab7a: cmp      rdx, 0x1000                                                ; 
000000014003ab81: jb       0x14003ab9c                                                ; 
000000014003ab83: add      rdx, 0x27                                                  ; 
000000014003ab87: mov      rcx, qword ptr [rcx - 8]                                   ; 
000000014003ab8b: sub      rax, rcx                                                   ; 
000000014003ab8e: sub      rax, 8                                                     ; 
000000014003ab92: cmp      rax, 0x1f                                                  ; 
000000014003ab96: ja       0x14003b520                                                ; 
000000014003ab9c: call     0x1404117bc                                                ; 
000000014003aba1: jmp      0x14003b480                                                ; 
000000014003aba6: mov      eax, 0xa                                                   ; 
000000014003abab: cmp      rbx, rax                                                   ; 
000000014003abae: cmovb    rbx, rax                                                   ; 
000000014003abb2: lea      rcx, [rbx + 1]                                             ; 
000000014003abb6: movabs   rax, 0x7fffffffffffffff                                    ; 
000000014003abc0: cmp      rcx, rax                                                   ; 
000000014003abc3: ja       0x14003b51a                                                ; 
000000014003abc9: jmp      0x14003aaf2                                                ; 
000000014003abce: mov      rdx, qword ptr [rip + 0x518143]                            ; 0x140552d18 
000000014003abd5: lea      rcx, [rsp + 0x60]                                          ; 
000000014003abda: call     0x140064990                                                ; 
000000014003abdf: lea      rcx, [rbx + 8]                                             ; 
000000014003abe3: mov      rdx, rax                                                   ; 
000000014003abe6: call     0x140018ce0                                                ; 
000000014003abeb: mov      rdx, qword ptr [rsp + 0x78]                                ; 
000000014003abf0: cmp      rdx, 7                                                     ; 
000000014003abf4: jbe      0x14003ac2d                                                ; 
000000014003abf6: lea      rdx, [rdx*2 + 2]                                           ; 
000000014003abfe: mov      rcx, qword ptr [rsp + 0x60]                                ; 
000000014003ac03: mov      rax, rcx                                                   ; 
000000014003ac06: cmp      rdx, 0x1000                                                ; 
000000014003ac0d: jb       0x14003ac28                                                ; 
000000014003ac0f: add      rdx, 0x27                                                  ; 
000000014003ac13: mov      rcx, qword ptr [rcx - 8]                                   ; 
000000014003ac17: sub      rax, rcx                                                   ; 
000000014003ac1a: sub      rax, 8                                                     ; 
000000014003ac1e: cmp      rax, 0x1f                                                  ; 
000000014003ac22: ja       0x14003b535                                                ; 
000000014003ac28: call     0x1404117bc                                                ; 
000000014003ac2d: xor      r9d, r9d                                                   ; 
000000014003ac30: xor      r8d, r8d                                                   ; 
000000014003ac33: mov      edx, 0xf0                                                  ; 
000000014003ac38: mov      rcx, qword ptr [rip + 0x5180e1]                            ; 0x140552d20 
000000014003ac3f: call     qword ptr [rip + 0x423b5b]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003ac45: cmp      rax, 1                                                     ; 
000000014003ac49: sete     al                                                         ; 
000000014003ac4c: mov      byte ptr [rbx + 0x28], al                                  ; 
000000014003ac4f: xor      r9d, r9d                                                   ; 
000000014003ac52: xor      r8d, r8d                                                   ; 
000000014003ac55: mov      edx, 0xf0                                                  ; 
000000014003ac5a: mov      rcx, qword ptr [rip + 0x5180c7]                            ; 0x140552d28 
000000014003ac61: call     qword ptr [rip + 0x423b39]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003ac67: cmp      rax, 1                                                     ; 
000000014003ac6b: sete     al                                                         ; 
000000014003ac6e: mov      byte ptr [rbx + 0x29], al                                  ; 
000000014003ac71: mov      rdx, qword ptr [rip + 0x5180c0]                            ; 0x140552d38 
000000014003ac78: lea      rcx, [rsp + 0x60]                                          ; 
000000014003ac7d: call     0x140064990                                                ; 
000000014003ac82: lea      rcx, [rbx + 0x30]                                          ; 
000000014003ac86: mov      rdx, rax                                                   ; 
000000014003ac89: call     0x140018ce0                                                ; 
000000014003ac8e: mov      rdx, qword ptr [rsp + 0x78]                                ; 
000000014003ac93: cmp      rdx, 7                                                     ; 
000000014003ac97: jbe      0x14003acd0                                                ; 
000000014003ac99: lea      rdx, [rdx*2 + 2]                                           ; 
000000014003aca1: mov      rcx, qword ptr [rsp + 0x60]                                ; 
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
000000014003adfe: cmp      rax, 1                                                     ; 
000000014003ae02: sete     al                                                         ; 
000000014003ae05: mov      byte ptr [rbx + 0x120], al                                 ; 
000000014003ae0b: xor      r9d, r9d                                                   ; 
000000014003ae0e: xor      r8d, r8d                                                   ; 
000000014003ae11: mov      edx, 0xf0                                                  ; 
000000014003ae16: mov      rcx, qword ptr [rip + 0x51824b]                            ; 0x140553068 
000000014003ae1d: call     qword ptr [rip + 0x42397d]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003ae23: cmp      rax, 1                                                     ; 
000000014003ae27: sete     al                                                         ; 
000000014003ae2a: mov      byte ptr [rbx + 0x140], al                                 ; 
000000014003ae30: mov      dword ptr [rsp + 0x50], 2                                  ; 
000000014003ae38: mov      dword ptr [rsp + 0x48], r14d                               ; 
000000014003ae3d: mov      rcx, qword ptr [rip + 0x518264]                            ; 0x1405530a8 
000000014003ae44: test     rcx, rcx                                                   ; 
000000014003ae47: jne      0x14003ae4e                                                ; 
000000014003ae49: mov      edx, r14d                                                  ; 
000000014003ae4c: jmp      0x14003ae68                                                ; 
000000014003ae4e: xor      r9d, r9d                                                   ; 
000000014003ae51: xor      r8d, r8d                                                   ; 
000000014003ae54: mov      edx, 0x147                                                 ; 
000000014003ae59: call     qword ptr [rip + 0x423941]                                 ; 0x14045e7a0 USER32.dll!SendMessageW
000000014003ae5f: mov      rdx, rax                                                   ; 
000000014003ae62: test     eax, eax                                                   ; 
000000014003ae64: cmovs    edx, r14d                                                  ; 
000000014003ae68: mov      dword ptr [rsp + 0x58], edx                                ; 
000000014003ae6c: lea      rcx, [rsp + 0x48]                                          ; 
000000014003ae71: lea      rax, [rsp + 0x58]                                          ; 
000000014003ae76: test     edx, edx                                                   ; 
000000014003ae78: cmovns   rcx, rax                                                   ; 
000000014003ae7c: lea      rax, [rsp + 0x50]                                          ; 
000000014003ae81: cmp      edx, 2                                                     ; 
000000014003ae84: cmovle   rax, rcx                                                   ; 
000000014003ae88: mov      eax, dword ptr [rax]                                       ; 
000000014003ae8a: mov      dword ptr [rbx + 0x15c], eax                               ; 
000000014003ae90: xorps    xmm6, xmm6                                                 ; 
000000014003ae93: movsd    qword ptr [rsp + 0x58], xmm6                               ; 
000000014003ae99: movsd    qword ptr [rsp + 0x48], xmm6                               ; 
000000014003ae9f: lea      rdx, [rsp + 0x48]                                          ; 
000000014003aea4: mov      rcx, qword ptr [rip + 0x517ebd]                            ; 0x140552d68 
000000014003aeab: call     0x14008a2a0                                                ; 
000000014003aeb0: test     al, al                                                     ; 
000000014003aeb2: je       0x14003b46b                                                ; 
000000014003aeb8: movsd    xmm0, qword ptr [rsp + 0x48]                               ; 
000000014003aebe: call     0x14042c110                                                ; 
000000014003aec3: mov      dword ptr [rbx + 4], eax                                   ; 
000000014003aec6: movsd    qword ptr [rsp + 0x48], xmm6                               ; 
000000014003aecc: lea      rdx, [rsp + 0x48]                                          ; 
000000014003aed1: mov      rcx, qword ptr [rip + 0x517e70]                            ; 0x140552d48 
000000014003aed8: call     0x14008a2a0                                                ; 
000000014003aedd: test     al, al                                                     ; 
000000014003aedf: je       0x14003b46b                                                ; 
000000014003aee5: movsd    xmm0, qword ptr [rsp + 0x48]                               ; 
000000014003aeeb: call     0x14042c110                                                ; 
000000014003aef0: mov      dword ptr [rbx + 0x50], eax                                ; 
000000014003aef3: movsd    qword ptr [rsp + 0x48], xmm6                               ; 
000000014003aef9: lea      rdx, [rsp + 0x48]                                          ; 
000000014003aefe: mov      rcx, qword ptr [rip + 0x517e53]                            ; 0x140552d58 
000000014003af05: call     0x14008a2a0                                                ; 
000000014003af0a: test     al, al                                                     ; 
000000014003af0c: je       0x14003b46b                                                ; 
000000014003af12: movsd    xmm0, qword ptr [rsp + 0x48]                               ; 
000000014003af18: call     0x14042c110                                                ; 
000000014003af1d: mov      dword ptr [rbx + 0x54], eax                                ; 
000000014003af20: mov      rdx, rbx                                                   ; 
000000014003af23: mov      rcx, qword ptr [rip + 0x517e4e]                            ; 0x140552d78 
000000014003af2a: call     0x14008a360                                                ; 
000000014003af2f: test     al, al                                                     ; 
000000014003af31: je       0x14003b46b                                                ; 
000000014003af37: lea      rdx, [rbx + 0x5c]                                          ; 
000000014003af3b: mov      rcx, qword ptr [rip + 0x517e56]                            ; 0x140552d98 
000000014003af42: call     0x14008a360                                                ; 
000000014003af47: test     al, al                                                     ; 
000000014003af49: je       0x14003b46b                                                ; 
000000014003af4f: lea      rdx, [rbx + 0x60]                                          ; 
000000014003af53: mov      rcx, qword ptr [rip + 0x517e4e]                            ; 0x140552da8 
000000014003af5a: call     0x14008a360                                                ; 
000000014003af5f: test     al, al                                                     ; 
000000014003af61: je       0x14003b46b                                                ; 
000000014003af67: lea      rdx, [rbx + 0x68]                                          ; 
000000014003af6b: mov      rcx, qword ptr [rip + 0x517e6e]                            ; 0x140552de0 
000000014003af72: call     0x14008a360                                                ; 
000000014003af77: test     al, al                                                     ; 
000000014003af79: je       0x14003b46b                                                ; 
000000014003af7f: lea      rdx, [rbx + 0x6c]                                          ; 
000000014003af83: mov      rcx, qword ptr [rip + 0x517e66]                            ; 0x140552df0 
000000014003af8a: call     0x14008a360                                                ; 
000000014003af8f: test     al, al                                                     ; 
000000014003af91: je       0x14003b46b                                                ; 
000000014003af97: lea      rdx, [rbx + 0x1b8]                                         ; 
000000014003af9e: mov      rcx, qword ptr [rip + 0x517eb3]                            ; 0x140552e58 
000000014003afa5: call     0x14008a360                                                ; 
000000014003afaa: test     al, al                                                     ; 
000000014003afac: je       0x14003b46b                                                ; 
000000014003afb2: lea      rdx, [rsp + 0x58]                                          ; 
000000014003afb7: mov      rcx, qword ptr [rip + 0x517ec2]                            ; 0x140552e80 
000000014003afbe: call     0x14008a2a0                                                ; 
000000014003afc3: test     al, al                                                     ; 
000000014003afc5: je       0x14003b46b                                                ; 
000000014003afcb: lea      rdx, [rbx + 0xe0]                                          ; 
000000014003afd2: mov      rcx, qword ptr [rip + 0x517faf]                            ; 0x140552f88 
000000014003afd9: call     0x14008a360                                                ; 
000000014003afde: test     al, al                                                     ; 
000000014003afe0: je       0x14003b46b                                                ; 
000000014003afe6: lea      rdx, [rbx + 0xe4]                                          ; 
000000014003afed: mov      rcx, qword ptr [rip + 0x517fa4]                            ; 0x140552f98 
000000014003aff4: call     0x14008a360                                                ; 
000000014003aff9: test     al, al                                                     ; 
000000014003affb: je       0x14003b46b                                                ; 
000000014003b001: lea      rdx, [rbx + 0xe8]                                          ; 
000000014003b008: mov      rcx, qword ptr [rip + 0x517f99]                            ; 0x140552fa8 
000000014003b00f: call     0x14008a2a0                                                ; 
000000014003b014: test     al, al                                                     ; 
000000014003b016: je       0x14003b46b                                                ; 
000000014003b01c: lea      rdx, [rbx + 0xf0]                                          ; 
000000014003b023: mov      rcx, qword ptr [rip + 0x517f8e]                            ; 0x140552fb8 
000000014003b02a: call     0x14008a2a0                                                ; 
000000014003b02f: test     al, al                                                     ; 
000000014003b031: je       0x14003b46b                                                ; 
000000014003b037: lea      rdx, [rbx + 0xf8]                                          ; 
000000014003b03e: mov      rcx, qword ptr [rip + 0x517f83]                            ; 0x140552fc8 
000000014003b045: call     0x14008a2a0                                                ; 
000000014003b04a: test     al, al                                                     ; 
000000014003b04c: je       0x14003b46b                                                ; 
000000014003b052: lea      rdx, [rbx + 0x108]                                         ; 
000000014003b059: mov      rcx, qword ptr [rip + 0x517f88]                            ; 0x140552fe8 
000000014003b060: call     0x14008a2a0                                                ; 
000000014003b065: test     al, al                                                     ; 
000000014003b067: je       0x14003b46b                                                ; 
000000014003b06d: lea      rdx, [rbx + 0x110]                                         ; 
000000014003b074: mov      rcx, qword ptr [rip + 0x517f85]                            ; 0x140553000 
000000014003b07b: call     0x14008a360                                                ; 
000000014003b080: test     al, al                                                     ; 
000000014003b082: je       0x14003b46b                                                ; 
000000014003b088: lea      rdx, [rbx + 0x118]                                         ; 
000000014003b08f: mov      rcx, qword ptr [rip + 0x517f7a]                            ; 0x140553010 
000000014003b096: call     0x14008a2a0                                                ; 
000000014003b09b: test     al, al                                                     ; 
000000014003b09d: je       0x14003b46b                                                ; 
000000014003b0a3: lea      rdx, [rbx + 0x128]                                         ; 
000000014003b0aa: mov      rcx, qword ptr [rip + 0x517f77]                            ; 0x140553028 
000000014003b0b1: call     0x14008a2a0                                                ; 
000000014003b0b6: test     al, al                                                     ; 
000000014003b0b8: je       0x14003b46b                                                ; 
000000014003b0be: lea      rdx, [rbx + 0x130]                                         ; 
000000014003b0c5: mov      rcx, qword ptr [rip + 0x517f74]                            ; 0x140553040 
000000014003b0cc: call     0x14008a2a0                                                ; 
000000014003b0d1: test     al, al                                                     ; 
000000014003b0d3: je       0x14003b46b                                                ; 
000000014003b0d9: lea      rdx, [rbx + 0x138]                                         ; 
000000014003b0e0: mov      rcx, qword ptr [rip + 0x517f71]                            ; 0x140553058 
000000014003b0e7: call     0x14008a2a0                                                ; 
000000014003b0ec: test     al, al                                                     ; 
000000014003b0ee: je       0x14003b46b                                                ; 
000000014003b0f4: lea      rdx, [rbx + 0x148]                                         ; 
000000014003b0fb: mov      rcx, qword ptr [rip + 0x517f76]                            ; 0x140553078 
000000014003b102: call     0x14008a2a0                                                ; 
000000014003b107: test     al, al                                                     ; 
000000014003b109: je       0x14003b46b                                                ; 
000000014003b10f: lea      rdx, [rbx + 0x150]                                         ; 
000000014003b116: mov      rcx, qword ptr [rip + 0x517f6b]                            ; 0x140553088 
000000014003b11d: call     0x14008a2a0                                                ; 
000000014003b122: test     al, al                                                     ; 
000000014003b124: je       0x14003b46b                                                ; 
000000014003b12a: lea      rdx, [rbx + 0x158]                                         ; 
000000014003b131: mov      rcx, qword ptr [rip + 0x517f60]                            ; 0x140553098 
000000014003b138: call     0x14008a360                                                ; 
000000014003b13d: test     al, al                                                     ; 
000000014003b13f: je       0x14003b46b                                                ; 
000000014003b145: lea      rdx, [rbx + 0x160]                                         ; 
000000014003b14c: mov      rcx, qword ptr [rip + 0x517f65]                            ; 0x1405530b8 
000000014003b153: call     0x14008a2a0                                                ; 
000000014003b158: test     al, al                                                     ; 
000000014003b15a: je       0x14003b46b                                                ; 
000000014003b160: lea      r14, [rbx + 0x100]                                         ; 
000000014003b167: cmp      esi, 1                                                     ; 
000000014003b16a: jne      0x14003b2cc                                                ; 
000000014003b170: movsd    xmm0, qword ptr [rbp + 0xe0]                               ; 
000000014003b178: movsd    qword ptr [r14], xmm0                                      ; 
000000014003b17d: movsd    xmm0, qword ptr [rsp + 0x58]                               ; 
000000014003b183: mulsd    xmm0, qword ptr [rip + 0x43ab05]                           ; 0x140475c90 
000000014003b18b: movsd    qword ptr [rbx + 0x88], xmm0                               ; 
000000014003b193: mov      eax, dword ptr [rbx + 0x6c]                                ; 
000000014003b196: mov      dword ptr [rbx + 0x74], eax                                ; 
000000014003b199: mov      rdx, qword ptr [rip + 0x517cc8]                            ; 0x140552e68 
000000014003b1a0: lea      rcx, [rsp + 0x60]                                          ; 
000000014003b1a5: call     0x140064990                                                ; 
000000014003b1aa: lea      rcx, [rbx + 0x90]                                          ; 
000000014003b1b1: mov      rdx, rax                                                   ; 
000000014003b1b4: call     0x140018ce0                                                ; 
000000014003b1b9: mov      rdx, qword ptr [rsp + 0x78]                                ; 
000000014003b1be: cmp      rdx, 7                                                     ; 
000000014003b1c2: jbe      0x14003b1fb                                                ; 
000000014003b1c4: lea      rdx, [rdx*2 + 2]                                           ; 
000000014003b1cc: mov      rcx, qword ptr [rsp + 0x60]                                ; 
000000014003b1d1: mov      rax, rcx                                                   ; 
000000014003b1d4: cmp      rdx, 0x1000                                                ; 
000000014003b1db: jb       0x14003b1f6                                                ; 
000000014003b1dd: add      rdx, 0x27                                                  ; 
000000014003b1e1: mov      rcx, qword ptr [rcx - 8]                                   ; 
000000014003b1e5: sub      rax, rcx                                                   ; 
000000014003b1e8: sub      rax, 8                                                     ; 
000000014003b1ec: cmp      rax, 0x1f                                                  ; 
000000014003b1f0: ja       0x14003b55f                                                ; 
000000014003b1f6: call     0x1404117bc                                                ; 
000000014003b1fb: cmp      esi, 2                                                     ; 
000000014003b1fe: je       0x14003b467                                                ; 
000000014003b204: cmp      byte ptr [rsp + 0x40], 0                                   ; 
000000014003b209: je       0x14003b32e                                                ; 
000000014003b20f: lea      rdx, [rbx + 0x1ac]                                         ; 
000000014003b216: mov      rcx, qword ptr [rip + 0x517beb]                            ; 0x140552e08 
000000014003b21d: call     0x14008a360                                                ; 
000000014003b222: test     al, al                                                     ; 
000000014003b224: je       0x14003b31c                                                ; 
000000014003b22a: lea      rdx, [rbx + 0x1b0]                                         ; 
000000014003b231: mov      rcx, qword ptr [rip + 0x517be0]                            ; 0x140552e18 
000000014003b238: call     0x14008a360                                                ; 
000000014003b23d: test     al, al                                                     ; 
000000014003b23f: je       0x14003b31c                                                ; 
000000014003b245: lea      rdx, [rbx + 0x1b4]                                         ; 
000000014003b24c: mov      rcx, qword ptr [rip + 0x517bd5]                            ; 0x140552e28 
000000014003b253: call     0x14008a360                                                ; 
000000014003b258: test     al, al                                                     ; 
000000014003b25a: je       0x14003b31c                                                ; 
000000014003b260: mov      qword ptr [rsp + 0x48], rdi                                ; 
000000014003b265: lea      r8, [rbx + 0x16c]                                          ; 
000000014003b26c: lea      r9, [rip + 0x4318f5]                                       ; 0x14046cb68 '64 路物理通道延时'
000000014003b273: mov      rdx, qword ptr [rip + 0x517bbe]                            ; 0x140552e38 
000000014003b27a: lea      rcx, [rsp + 0x48]                                          ; 
000000014003b27f: call     0x140020a60                                                ; 
000000014003b284: test     al, al                                                     ; 
000000014003b286: je       0x14003b2ff                                                ; 
000000014003b288: lea      r8, [rbx + 0x18c]                                          ; 
000000014003b28f: lea      r9, [rip + 0x4318ea]                                       ; 0x14046cb80 '64 路时隙内部延时'
000000014003b296: mov      rdx, qword ptr [rip + 0x517bab]                            ; 0x140552e48 
000000014003b29d: lea      rcx, [rsp + 0x48]                                          ; 
000000014003b2a2: call     0x140020a60                                                ; 
000000014003b2a7: test     al, al                                                     ; 
000000014003b2a9: je       0x14003b2ff                                                ; 
000000014003b2ab: cmp      esi, 1                                                     ; 
000000014003b2ae: jne      0x14003b372                                                ; 
000000014003b2b4: mov      dword ptr [rbx + 0x6c], esi                                ; 
000000014003b2b7: mov      dword ptr [rbx + 0x74], esi                                ; 
000000014003b2ba: movzx    eax, byte ptr [rbp + 0xb0]                                 ; 
000000014003b2c1: mov      byte ptr [rbx + 0x1bc], al                                 ; 
000000014003b2c7: jmp      0x14003b388                                                ; 
000000014003b2cc: mov      rax, qword ptr [rbx + 0xf8]                                ; 
000000014003b2d3: mov      qword ptr [r14], rax                                       ; 
000000014003b2d6: mov      rdx, r14                                                   ; 
000000014003b2d9: mov      rcx, qword ptr [rip + 0x517cf8]                            ; 0x140552fd8 
000000014003b2e0: call     0x14008a2a0                                                ; 
000000014003b2e5: test     al, al                                                     ; 
000000014003b2e7: jne      0x14003b17d                                                ; 
000000014003b2ed: mov      r8d, 0xc                                                   ; 
000000014003b2f3: lea      rdx, [rip + 0x431816]                                      ; 0x14046cb10 '扫描步长必须是有效数值。'
000000014003b2fa: jmp      0x14003b478                                                ; 
000000014003b2ff: cmp      qword ptr [rdi + 0x10], 0                                  ; 
000000014003b304: jne      0x14003b480                                                ; 
000000014003b30a: mov      r8d, 0x18                                                  ; 
000000014003b310: lea      rdx, [rip + 0x431881]                                      ; 0x14046cb98 '64 路延时必须正好填写 8 个逗号分隔的整数。'
000000014003b317: jmp      0x14003b478                                                ; 
000000014003b31c: mov      r8d, 0x19                                                  ; 
000000014003b322: lea      rdx, [rip + 0x431807]                                      ; 0x14046cb30 '64 路复用起点、时隙长度和有效截止点必须是整数。'
000000014003b329: jmp      0x14003b478                                                ; 
000000014003b32e: movups   xmm0, xmmword ptr [rbp + 0x58]                             ; 
000000014003b332: movups   xmmword ptr [rbx + 0x16c], xmm0                            ; 
000000014003b339: movups   xmm1, xmmword ptr [rbp + 0x68]                             ; 
000000014003b33d: movups   xmmword ptr [rbx + 0x17c], xmm1                            ; 
000000014003b344: movaps   xmm0, xmmword ptr [rbp + 0x80]                             ; 
000000014003b34b: movups   xmmword ptr [rbx + 0x18c], xmm0                            ; 
000000014003b352: movaps   xmm1, xmmword ptr [rbp + 0x90]                             ; 
000000014003b359: movups   xmmword ptr [rbx + 0x19c], xmm1                            ; 
000000014003b360: mov      eax, dword ptr [rbp + 0x54]                                ; 
000000014003b363: mov      dword ptr [rbx + 0x1ac], eax                               ; 
000000014003b369: mov      eax, dword ptr [rbp + 0x4c]                                ; 
000000014003b36c: mov      dword ptr [rbx + 0x1b0], eax                               ; 
000000014003b372: movzx    eax, byte ptr [rbp + 0xb0]                                 ; 
000000014003b379: mov      byte ptr [rbx + 0x1bc], al                                 ; 
000000014003b37f: cmp      esi, 3                                                     ; 
000000014003b382: je       0x14003b432                                                ; 
000000014003b388: lea      rax, [rbx + 0xc8]                                          ; 
000000014003b38f: lea      rdx, [rbx + 0xb0]                                          ; 
000000014003b396: lea      rcx, [rbx + 0x90]                                          ; 
000000014003b39d: mov      qword ptr [rsp + 0x30], rdi                                ; 
000000014003b3a2: mov      qword ptr [rsp + 0x28], rax                                ; 
000000014003b3a7: mov      qword ptr [rsp + 0x20], rdx                                ; 
000000014003b3ac: movsd    xmm3, qword ptr [rbx + 0x108]                              ; 
000000014003b3b4: movsd    xmm2, qword ptr [r14]                                      ; 
000000014003b3b9: mov      edx, dword ptr [rbx + 0x78]                                ; 
000000014003b3bc: call     0x14010e320                                                ; 
000000014003b3c1: test     al, al                                                     ; 
000000014003b3c3: jne      0x14003b432                                                ; 
000000014003b3c5: mov      rdx, qword ptr [rdi + 0x10]                                ; 
000000014003b3c9: movabs   rcx, 0x7ffffffffffffffe                                    ; 
000000014003b3d3: sub      rcx, rdx                                                   ; 
000000014003b3d6: cmp      rcx, 0xe                                                   ; 
000000014003b3da: jb       0x14003b514                                                ; 
000000014003b3e0: mov      rax, rdi                                                   ; 
000000014003b3e3: cmp      qword ptr [rdi + 0x18], 7                                  ; 
000000014003b3e8: jbe      0x14003b3ed                                                ; 
000000014003b3ea: mov      rax, qword ptr [rdi]                                       ; 
000000014003b3ed: mov      qword ptr [rsp + 0x30], rdx                                ; 
000000014003b3f2: mov      qword ptr [rsp + 0x28], rax                                ; 
000000014003b3f7: mov      qword ptr [rsp + 0x20], 0xe                                ; 
000000014003b400: lea      r9, [rip + 0x4317c9]                                       ; 0x14046cbd0 '实时 DAS 配准文件无效：'
000000014003b407: mov      r8, rdi                                                    ; 
000000014003b40a: movzx    edx, byte ptr [rsp + 0x40]                                 ; 
000000014003b40f: lea      rcx, [rsp + 0x60]                                          ; 
000000014003b414: call     0x140011480                                                ; 
000000014003b419: lea      rdx, [rsp + 0x60]                                          ; 
000000014003b41e: mov      rcx, rdi                                                   ; 
000000014003b421: call     0x140018ce0                                                ; 
000000014003b426: lea      rcx, [rsp + 0x60]                                          ; 
000000014003b42b: call     0x1400281b0                                                ; 
000000014003b430: jmp      0x14003b480                                                ; 
000000014003b432: mov      eax, dword ptr [rbx + 0x78]                                ; 
000000014003b435: cmp      eax, 8                                                     ; 
000000014003b438: jne      0x14003b44e                                                ; 
000000014003b43a: cmp      dword ptr [rbx], 4                                         ; 
000000014003b43d: je       0x14003b467                                                ; 
000000014003b43f: mov      r8d, 0x17                                                  ; 
000000014003b445: lea      rdx, [rip + 0x4317a4]                                      ; 0x14046cbf0 '8 路实时 DAS 要求 4 张双通道采集卡。'
000000014003b44c: jmp      0x14003b478                                                ; 
000000014003b44e: cmp      eax, 0x40                                                  ; 
000000014003b451: jne      0x14003b467                                                ; 
000000014003b453: cmp      dword ptr [rbx], 4                                         ; 
000000014003b456: je       0x14003b467                                                ; 
000000014003b458: mov      r8d, 0x1d                                                  ; 
000000014003b45e: lea      rdx, [rip + 0x4317bb]                                      ; 0x14046cc20 '64 路时分复用实时 DAS 仍要求 4 张双通道采集卡。'
000000014003b465: jmp      0x14003b478                                                ; 
000000014003b467: mov      bl, 1                                                      ; 
000000014003b469: jmp      0x14003b482                                                ; 
000000014003b46b: mov      r8d, 0x11                                                  ; 
000000014003b471: lea      rdx, [rip + 0x431670]                                      ; 0x14046cae8 '实时 DAS 参数中存在无效数字。'
000000014003b478: mov      rcx, rdi                                                   ; 
000000014003b47b: call     0x140030650                                                ; 
000000014003b480: xor      bl, bl                                                     ; 
000000014003b482: lea      rcx, [rbp + 0x280]                                         ; 
000000014003b489: call     0x140027a90                                                ; 
000000014003b48e: lea      rcx, [rbp + 0x1c8]                                         ; 
000000014003b495: call     0x140027ec0                                                ; 
000000014003b49a: lea      rcx, [rbp + 0x130]                                         ; 
000000014003b4a1: call     0x140027bd0                                                ; 
000000014003b4a6: lea      rcx, [rbp + 0xb8]                                          ; 
000000014003b4ad: call     0x140027a90                                                ; 
000000014003b4b2: lea      rcx, [rbp + 0x28]                                          ; 
000000014003b4b6: call     0x1400281b0                                                ; 
000000014003b4bb: lea      rcx, [rbp - 0x10]                                          ; 
000000014003b4bf: call     0x1400281b0                                                ; 
000000014003b4c4: lea      rcx, [rbp - 0x38]                                          ; 
000000014003b4c8: call     0x1400281b0                                                ; 
000000014003b4cd: lea      rcx, [rbp - 0x58]                                          ; 
000000014003b4d1: call     0x1400281b0                                                ; 
000000014003b4d6: lea      rcx, [rbp - 0x78]                                          ; 
000000014003b4da: call     0x1400281b0                                                ; 
000000014003b4df: movzx    eax, bl                                                    ; 
000000014003b4e2: mov      rcx, qword ptr [rbp + 0x2a0]                               ; 
000000014003b4e9: xor      rcx, rsp                                                   ; 
000000014003b4ec: call     0x140411cb0                                                ; 
000000014003b4f1: mov      rbx, qword ptr [rsp + 0x410]                               ; 
000000014003b4f9: movaps   xmm6, xmmword ptr [rsp + 0x3b0]                            ; 
000000014003b501: add      rsp, 0x3c0                                                 ; 
000000014003b508: pop      r15                                                        ; 
000000014003b50a: pop      r14                                                        ; 
000000014003b50c: pop      r13                                                        ; 
000000014003b50e: pop      r12                                                        ; 
000000014003b510: pop      rdi                                                        ; 
000000014003b511: pop      rsi                                                        ; 
000000014003b512: pop      rbp                                                        ; 
000000014003b513: ret                                                                 ; 
000000014003b514: call     0x140028720                                                ; 
000000014003b519: nop                                                                 ; 
000000014003b51a: call     0x1400278e0                                                ; 
000000014003b51f: int3                                                                ; 
000000014003b520: mov      qword ptr [rsp + 0x20], rbx                                ; 
000000014003b525: xor      r9d, r9d                                                   ; 
000000014003b528: xor      r8d, r8d                                                   ; 
000000014003b52b: xor      edx, edx                                                   ; 
000000014003b52d: xor      ecx, ecx                                                   ; 
000000014003b52f: call     0x140417b84                                                ; 
000000014003b534: int3                                                                ; 
000000014003b535: xor      ecx, ecx                                                   ; 
000000014003b537: mov      qword ptr [rsp + 0x20], rcx                                ; 
000000014003b53c: xor      r9d, r9d                                                   ; 
000000014003b53f: xor      r8d, r8d                                                   ; 
000000014003b542: xor      edx, edx                                                   ; 
000000014003b544: call     0x140417b84                                                ; 
000000014003b549: int3                                                                ; 
000000014003b54a: xor      ecx, ecx                                                   ; 
000000014003b54c: mov      qword ptr [rsp + 0x20], rcx                                ; 
000000014003b551: xor      r9d, r9d                                                   ; 
000000014003b554: xor      r8d, r8d                                                   ; 
000000014003b557: xor      edx, edx                                                   ; 
000000014003b559: call     0x140417b84                                                ; 
000000014003b55e: int3                                                                ; 
000000014003b55f: xor      ecx, ecx                                                   ; 
000000014003b561: mov      qword ptr [rsp + 0x20], rcx                                ; 
000000014003b566: xor      r9d, r9d                                                   ; 
000000014003b569: xor      r8d, r8d                                                   ; 
000000014003b56c: xor      edx, edx                                                   ; 
000000014003b56e: call     0x140417b84                                                ; 
000000014003b573: int3                                                                ; 