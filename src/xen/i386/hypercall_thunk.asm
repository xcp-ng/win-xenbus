                        page    ,132
                        title   Hypercall Thunks

                        .686p
                        .model  FLAT
                        .code

                        ; uintptr_t __stdcall hypercall2_vmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2);
                        public _hypercall2_vmcall@12
_hypercall2_vmcall@12   proc
                        push    ebp
                        mov     ebp, esp
                        push    ebx
                        mov     eax, [ebp + 08h]                ; ord
                        mov     ebx, [ebp + 0ch]                ; arg1
                        mov     ecx, [ebp + 10h]                ; arg2
                        vmcall
                        pop     ebx
                        leave
                        ret     0Ch
_hypercall2_vmcall@12   endp

                        ; uintptr_t __stdcall hypercall2_vmmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2);
                        public _hypercall2_vmmcall@12
_hypercall2_vmmcall@12  proc
                        push    ebp
                        mov     ebp, esp
                        push    ebx
                        mov     eax, [ebp + 08h]                ; ord
                        mov     ebx, [ebp + 0ch]                ; arg1
                        mov     ecx, [ebp + 10h]                ; arg2
                        vmmcall
                        pop     ebx
                        leave
                        ret     0Ch
_hypercall2_vmmcall@12  endp

                        ; uintptr_t __stdcall hypercall3_vmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2,
                        ;     uintptr_t   arg3);
                        public _hypercall3_vmcall@16
_hypercall3_vmcall@16   proc
                        push    ebp
                        mov     ebp, esp
                        push    ebx
                        mov     eax, [ebp + 08h]                ; ord
                        mov     ebx, [ebp + 0ch]                ; arg1
                        mov     ecx, [ebp + 10h]                ; arg2
                        mov     edx, [ebp + 14h]                ; arg3
                        vmcall
                        pop     ebx
                        leave
                        ret     10h
_hypercall3_vmcall@16   endp

                        ; uintptr_t __stdcall hypercall3_vmmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2,
                        ;     uintptr_t   arg3);
                        public _hypercall3_vmmcall@16
_hypercall3_vmmcall@16  proc
                        push    ebp
                        mov     ebp, esp
                        push    ebx
                        mov     eax, [ebp + 08h]                ; ord
                        mov     ebx, [ebp + 0ch]                ; arg1
                        mov     ecx, [ebp + 10h]                ; arg2
                        mov     edx, [ebp + 14h]                ; arg3
                        vmmcall
                        pop     ebx
                        leave
                        ret     10h
_hypercall3_vmmcall@16  endp

                        end
