00000001401505f4: mov      qword ptr [rsp + 0x58], rdi                                ; 
00000001401505f9: mov      eax, 2                                                     ; 
00000001401505fe: nop                                                                 ; 
0000000140150600: mov      edi, dword ptr [rbx]                                       ; 
0000000140150602: movzx    ecx, r12w                                                  ; 
0000000140150606: mov      qword ptr [rsp + 0x40], r13                                ; 
000000014015060b: mov      word ptr [rsp + 0x38], ax                                  ; 
0000000140150610: call     qword ptr [rip + 0x30e47a]                                 ; 0x14045ea90 WS2_32.dll!htons
0000000140150616: mov      ecx, edi                                                   ; 
0000000140150618: mov      word ptr [rsp + 0x3a], ax                                  ; 
000000014015061d: call     qword ptr [rip + 0x30e465]                                 ; 0x14045ea88 WS2_32.dll!htonl
0000000140150623: mov      dword ptr [rsp + 0x28], 0x10                               ; 
000000014015062b: xor      r9d, r9d                                                   ; 
000000014015062e: mov      dword ptr [rsp + 0x3c], eax                                ; 
0000000140150632: mov      r8d, 0x3a                                                  ; 
0000000140150638: lea      rax, [rsp + 0x38]                                          ; 
000000014015063d: mov      rdx, r15                                                   ; 
0000000140150640: mov      rcx, r14                                                   ; 
0000000140150643: mov      qword ptr [rsp + 0x20], rax                                ; 
0000000140150648: call     qword ptr [rip + 0x30e462]                                 ; 0x14045eab0 WS2_32.dll!sendto
000000014015064e: cmp      eax, 0x3a                                                  ; 
0000000140150651: jne      0x140150657                                                ; 
0000000140150653: inc      esi                                                        ; 
0000000140150655: jmp      0x140150661                                                ; 
0000000140150657: call     qword ptr [rip + 0x30e47b]                                 ; 0x14045ead8 WS2_32.dll!WSAGetLastError
000000014015065d: mov      dword ptr [rsp + 0x34], eax                                ; 
0000000140150661: add      rbx, 4                                                     ; 
0000000140150665: mov      eax, 2                                                     ; 
000000014015066a: cmp      rbx, rbp                                                   ; 
000000014015066d: jne      0x140150600                                                ; 
000000014015066f: mov      rdi, qword ptr [rsp + 0x58]                                ; 
0000000140150674: mov      dword ptr [rsp + 0x30], esi                                ; 