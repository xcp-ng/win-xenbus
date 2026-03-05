                        page    ,132
                        title   Hypercall Thunks

                        .code

                        ; uintptr_t __stdcall hypercall2_vmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2);
                        public hypercall2_vmcall
hypercall2_vmcall       proc
                        push    rdi
                        push    rsi
                        mov     eax, ecx                            ; ord
                        mov     rdi, rdx                            ; arg1
                        mov     rsi, r8                             ; arg2
                        vmcall
                        pop     rsi
                        pop     rdi
                        ret
hypercall2_vmcall       endp

                        ; uintptr_t __stdcall hypercall2_vmmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2);
                        public hypercall2_vmmcall
hypercall2_vmmcall      proc
                        push    rdi
                        push    rsi
                        mov     eax, ecx                            ; ord
                        mov     rdi, rdx                            ; arg1
                        mov     rsi, r8                             ; arg2
                        vmmcall
                        pop     rsi
                        pop     rdi
                        ret
hypercall2_vmmcall      endp

                        ; uintptr_t __stdcall hypercall3_vmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2,
                        ;     uintptr_t   arg3);
                        public hypercall3_vmcall
hypercall3_vmcall       proc
                        push    rdi
                        push    rsi
                        mov     eax, ecx                            ; ord
                        mov     rdi, rdx                            ; arg1
                        mov     rsi, r8                             ; arg2
                        mov     rdx, r9                             ; arg3
                        vmcall
                        pop     rsi
                        pop     rdi
                        ret
hypercall3_vmcall       endp

                        ; uintptr_t __stdcall hypercall3_vmmcall(
                        ;     uint32_t    ord,
                        ;     uintptr_t   arg1,
                        ;     uintptr_t   arg2,
                        ;     uintptr_t   arg3);
                        public hypercall3_vmmcall
hypercall3_vmmcall      proc
                        push    rdi
                        push    rsi
                        mov     eax, ecx                            ; ord
                        mov     rdi, rdx                            ; arg1
                        mov     rsi, r8                             ; arg2
                        mov     rdx, r9                             ; arg3
                        vmmcall
                        pop     rsi
                        pop     rdi
                        ret
hypercall3_vmmcall      endp

                        end
