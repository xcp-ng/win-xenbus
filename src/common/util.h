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

#ifndef _COMMON_UTIL_H
#define _COMMON_UTIL_H

#include <ntddk.h>
#include <intrin.h>

#include "assert.h"

#define	P2ROUNDUP(_t, _x, _a)   \
        (-(-((_t)(_x)) & -(((_t)(_a)))))

static FORCEINLINE LONG
__ffs(
    _In_ unsigned long long mask
    )
{
    unsigned char           *array = (unsigned char *)&mask;
    unsigned int            byte;
    unsigned int            bit;
    unsigned char           val;

    val = 0;

    byte = 0;
    while (byte < 8) {
        val = array[byte];

        if (val != 0)
            break;

        byte++;
    }
    if (byte == 8)
        return -1;

    bit = 0;
    while (bit < 8) {
        if (val & 0x01)
            break;

        val >>= 1;
        bit++;
    }

    return (byte * 8) + bit;
}

#define __ffu(_mask)  \
        __ffs(~(_mask))

static FORCEINLINE VOID
__CpuId(
    _In_ ULONG          Leaf,
    _Out_opt_ PULONG    EAX,
    _Out_opt_ PULONG    EBX,
    _Out_opt_ PULONG    ECX,
    _Out_opt_ PULONG    EDX
    )
{
    int         Value[4] = {0};

    __cpuid(Value, Leaf);

    if (EAX)
        *EAX = (ULONG)Value[0];

    if (EBX)
        *EBX = (ULONG)Value[1];

    if (ECX)
        *ECX = (ULONG)Value[2];

    if (EDX)
        *EDX = (ULONG)Value[3];
}

static FORCEINLINE LONG
__InterlockedAdd(
    _In_ LONG   *Value,
    _In_ LONG   Delta
    )
{
    LONG        New;
    LONG        Old;

    do {
        Old = *Value;
        New = Old + Delta;
    } while (InterlockedCompareExchange(Value, New, Old) != Old);

    return New;
}

static FORCEINLINE LONG
__InterlockedSubtract(
    _In_ LONG   *Value,
    _In_ LONG   Delta
    )
{
    LONG        New;
    LONG        Old;

    do {
        Old = *Value;
        New = Old - Delta;
    } while (InterlockedCompareExchange(Value, New, Old) != Old);

    return New;
}

_Check_return_
static FORCEINLINE PVOID
__AllocatePoolWithTag(
    _In_ POOL_TYPE  PoolType,
    _In_ SIZE_T     NumberOfBytes,
    _In_ ULONG      Tag
    )
{
    PUCHAR          Buffer;

    __analysis_assume(PoolType == NonPagedPool ||
                      PoolType == PagedPool);

    if (NumberOfBytes == 0)
        return NULL;

#if (_MSC_VER >= 1928) // VS 16.9 (EWDK 20344 or later)
    Buffer = ExAllocatePoolUninitialized(PoolType, NumberOfBytes, Tag);
#else
#pragma warning(suppress:28160) // annotation error
    Buffer = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
#endif
    if (Buffer == NULL)
        return NULL;

    RtlZeroMemory(Buffer, NumberOfBytes);
    return Buffer;
}

static FORCEINLINE VOID
__FreePoolWithTag(
    _In_ PVOID  Buffer,
    _In_ ULONG  Tag
    )
{
    ExFreePoolWithTag(Buffer, Tag);
}

static FORCEINLINE PMDL
__AllocatePages(
    _In_ ULONG          Count,
    _In_ BOOLEAN        Contiguous
    )
{
    PHYSICAL_ADDRESS    LowAddress;
    PHYSICAL_ADDRESS    HighAddress;
    LARGE_INTEGER       SkipBytes;
    SIZE_T              TotalBytes;
    ULONG               Flags;
    PMDL                Mdl;
    PUCHAR              MdlMappedSystemVa;
    NTSTATUS            status;

    LowAddress.QuadPart = 0ull;
    HighAddress.QuadPart = ~0ull;
    TotalBytes = (SIZE_T)PAGE_SIZE * Count;

    if (Contiguous) {
        SkipBytes.QuadPart = TotalBytes;
        Flags = MM_ALLOCATE_REQUIRE_CONTIGUOUS_CHUNKS;
    } else {
        SkipBytes.QuadPart = 0ull;
        Flags = MM_ALLOCATE_FULLY_REQUIRED;
    }

    Mdl = MmAllocatePagesForMdlEx(LowAddress,
                                  HighAddress,
                                  SkipBytes,
                                  TotalBytes,
                                  MmCached,
                                  Flags);

    status = STATUS_NO_MEMORY;
    if (Mdl == NULL)
        goto fail1;

    if (Mdl->ByteCount < TotalBytes)
        goto fail2;

    ASSERT((Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA |
                             MDL_PARTIAL_HAS_BEEN_MAPPED |
                             MDL_PARTIAL |
                             MDL_PARENT_MAPPED_SYSTEM_VA |
                             MDL_SOURCE_IS_NONPAGED_POOL |
                             MDL_IO_SPACE)) == 0);

    MdlMappedSystemVa = MmMapLockedPagesSpecifyCache(Mdl,
                                                     KernelMode,
                                                     MmCached,
                                                     NULL,
                                                     FALSE,
                                                     NormalPagePriority);

    status = STATUS_UNSUCCESSFUL;
    if (MdlMappedSystemVa == NULL)
        goto fail3;

    Mdl->StartVa = PAGE_ALIGN(MdlMappedSystemVa);

    ASSERT3U(Mdl->ByteOffset, ==, 0);
    ASSERT3P(Mdl->StartVa, ==, MdlMappedSystemVa);
    ASSERT3P(Mdl->MappedSystemVa, ==, MdlMappedSystemVa);

    return Mdl;

fail3:
fail2:
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);

fail1:
    return NULL;
}

#define __AllocatePage()    __AllocatePages(1, FALSE)

static FORCEINLINE VOID
__FreePages(
    _In_ PMDL   Mdl
    )
{
    PUCHAR	MdlMappedSystemVa;

    ASSERT(Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA);
    MdlMappedSystemVa = Mdl->MappedSystemVa;

    MmUnmapLockedPages(MdlMappedSystemVa, Mdl);

    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
}

#define __FreePage(_Mdl)    __FreePages(_Mdl)

static FORCEINLINE PSTR
__strtok_r(
    _In_opt_ PSTR   Buffer,
    _In_ PSTR       Delimiter,
    _Inout_ PSTR    *Context
    )
{
    PSTR            Token;
    PSTR            End;

    if (Buffer != NULL)
        *Context = Buffer;

    Token = *Context;

    if (Token == NULL)
        return NULL;

    while (*Token != '\0' &&
           strchr(Delimiter, *Token) != NULL)
        Token++;

    if (*Token == '\0')
        return NULL;

    End = Token + 1;
    while (*End != '\0' &&
           strchr(Delimiter, *End) == NULL)
        End++;

    if (*End != '\0')
        *End++ = '\0';

    *Context = End;

    return Token;
}

static FORCEINLINE PWSTR
__wcstok_r(
    _In_opt_ PWSTR  Buffer,
    _In_ PWSTR      Delimiter,
    _Inout_ PWSTR   *Context
    )
{
    PWSTR           Token;
    PWSTR           End;

    if (Buffer != NULL)
        *Context = Buffer;

    Token = *Context;

    if (Token == NULL)
        return NULL;

    while (*Token != L'\0' &&
           wcschr(Delimiter, *Token) != NULL)
        Token++;

    if (*Token == L'\0')
        return NULL;

    End = Token + 1;
    while (*End != L'\0' &&
           wcschr(Delimiter, *End) == NULL)
        End++;

    if (*End != L'\0')
        *End++ = L'\0';

    *Context = End;

    return Token;
}

static FORCEINLINE CHAR
__toupper(
    _In_ CHAR   Character
    )
{
    if (Character < 'a' || Character > 'z')
        return Character;

    return 'A' + Character - 'a';
}

static FORCEINLINE CHAR
__tolower(
    _In_ CHAR   Character
    )
{
    if (Character < 'A' || Character > 'Z')
        return Character;

    return 'a' + Character - 'A';
}

#endif  // _COMMON_UTIL_H
