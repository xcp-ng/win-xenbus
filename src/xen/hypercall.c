/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#include <ntddk.h>
#include <xen.h>
#include <intrin.h>

#include "hypercall.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

typedef enum _HYPERCALL_INSTRUCTION {
    HYPERCALL_INSTRUCTION_UNKNOWN,
    HYPERCALL_INSTRUCTION_VMCALL,
    HYPERCALL_INSTRUCTION_VMMCALL,
} HYPERCALL_INSTRUCTION;

typedef struct _CPU_VENDOR_DATA {
    ULONG                   EBX;
    ULONG                   ECX;
    ULONG                   EDX;
    HYPERCALL_INSTRUCTION   Instruction;
} CPU_VENDOR_DATA;

static const CPU_VENDOR_DATA    HypercallVendorData[] = {
    // Note that the vendor data goes EBX-ECX-EDX
    {
        // "GenuineIntel"
        0x756E6547, 0x6C65746E, 0x49656E69,
        HYPERCALL_INSTRUCTION_VMCALL
    },
    {
        // "AuthenticAMD"
        0x68747541, 0x444D4163, 0x69746E65,
        HYPERCALL_INSTRUCTION_VMMCALL
    },
    {
        // "CentaurHauls"
        0x746E6543, 0x736C7561, 0x48727561,
        HYPERCALL_INSTRUCTION_VMCALL
    },
    {
        // "  Shanghai  "
        0x68532020, 0x20206961, 0x68676E61,
        HYPERCALL_INSTRUCTION_VMCALL
    },
    {
        // "HygonGenuine"
        0x6F677948, 0x656E6975, 0x6E65476E,
        HYPERCALL_INSTRUCTION_VMMCALL
    },
};

static HYPERCALL_INSTRUCTION    HypercallInstruction
    = HYPERCALL_INSTRUCTION_UNKNOWN;

NTSTATUS
HypercallInitialize(
    VOID
    )
{
    ULONG                   XenBaseLeaf = 0x40000000;
    ULONG                   EAX = 'DEAD';
    ULONG                   EBX = 'DEAD';
    ULONG                   ECX = 'DEAD';
    ULONG                   EDX = 'DEAD';
    HYPERCALL_INSTRUCTION   Instruction = HYPERCALL_INSTRUCTION_UNKNOWN;
    ULONG                   Index;

    for (;;) {
        CHAR    Signature[13] = {0};

        __CpuId(XenBaseLeaf, &EAX, &EBX, &ECX, &EDX);
        *((PULONG)(Signature + 0)) = EBX;
        *((PULONG)(Signature + 4)) = ECX;
        *((PULONG)(Signature + 8)) = EDX;

        if (strcmp(Signature, "XenVMMXenVMM") == 0 &&
            EAX >= XenBaseLeaf + 2)
            break;

        XenBaseLeaf += 0x100;

        if (XenBaseLeaf > 0x40000100) {
            LogPrintf(LOG_LEVEL_INFO,
                      "XEN: BASE CPUID LEAF NOT FOUND\n");
            return STATUS_NOT_SUPPORTED;
        }
    }

    LogPrintf(LOG_LEVEL_INFO,
              "XEN: BASE CPUID LEAF @ %08x\n",
              XenBaseLeaf);

    __CpuId(0, &EAX, &EBX, &ECX, &EDX);
    for (Index = 0; Index < ARRAYSIZE(HypercallVendorData); Index++) {
        const CPU_VENDOR_DATA   *CurrentData = &HypercallVendorData[Index];

        if (EBX == CurrentData->EBX &&
            ECX == CurrentData->ECX &&
            EDX == CurrentData->EDX) {
            Instruction = CurrentData->Instruction;
            break;
        }
    }

    if (Instruction == HYPERCALL_INSTRUCTION_UNKNOWN) {
        LogPrintf(LOG_LEVEL_INFO,
                  "XEN: CANNOT DETECT HYPERCALL INSTRUCTION\n");
        return STATUS_NOT_SUPPORTED;
    }

    HypercallInstruction = Instruction;
    return STATUS_SUCCESS;
}

extern uintptr_t __stdcall hypercall2_vmcall(
    uint32_t    ord,
    uintptr_t   arg1,
    uintptr_t   arg2);
extern uintptr_t __stdcall hypercall2_vmmcall(
    uint32_t    ord,
    uintptr_t   arg1,
    uintptr_t   arg2);
extern uintptr_t __stdcall hypercall3_vmcall(
    uint32_t    ord,
    uintptr_t   arg1,
    uintptr_t   arg2,
    uintptr_t   arg3);
extern uintptr_t __stdcall hypercall3_vmmcall(
    uint32_t    ord,
    uintptr_t   arg1,
    uintptr_t   arg2,
    uintptr_t   arg3);

LONG_PTR
__Hypercall(
    ULONG       Ordinal,
    ULONG       Count,
    ...
    )
{
    va_list     Arguments;
    ULONG_PTR   Value;

    va_start(Arguments, Count);
    switch (Count) {
    case 2: {
        uint32_t   ord = Ordinal;
        uintptr_t  arg1 = va_arg(Arguments, ULONG_PTR);
        uintptr_t  arg2 = va_arg(Arguments, ULONG_PTR);

        switch (HypercallInstruction) {
        case HYPERCALL_INSTRUCTION_VMCALL:
            Value = hypercall2_vmcall(ord, arg1, arg2);
            break;
        case HYPERCALL_INSTRUCTION_VMMCALL:
            Value = hypercall2_vmmcall(ord, arg1, arg2);
            break;
        default:
            BUG("NO HYPERCALL INSTRUCTION");
        }
        break;
    }
    case 3: {
        uint32_t   ord = Ordinal;
        uintptr_t  arg1 = va_arg(Arguments, ULONG_PTR);
        uintptr_t  arg2 = va_arg(Arguments, ULONG_PTR);
        uintptr_t  arg3 = va_arg(Arguments, ULONG_PTR);

        switch (HypercallInstruction) {
        case HYPERCALL_INSTRUCTION_VMCALL:
            Value = hypercall3_vmcall(ord, arg1, arg2, arg3);
            break;
        case HYPERCALL_INSTRUCTION_VMMCALL:
            Value = hypercall3_vmmcall(ord, arg1, arg2, arg3);
            break;
        default:
            BUG("NO HYPERCALL INSTRUCTION");
        }
        break;
    }
    default:
        ASSERT(FALSE);
        Value = 0;
    }
    va_end(Arguments);

    return Value;
}

VOID
HypercallTeardown(
    VOID
    )
{
}
