/* Copyright (c) Citrix Systems Inc.
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

#define	P2ROUNDUP(_x, _a)   \
        (-(-(_x) & -(_a)))

static FORCEINLINE LONG
__ffs(
    IN  unsigned long long  mask
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
    IN  ULONG   Leaf,
    OUT PULONG  EAX OPTIONAL,
    OUT PULONG  EBX OPTIONAL,
    OUT PULONG  ECX OPTIONAL,
    OUT PULONG  EDX OPTIONAL
    )
{
    int  Value[4] = {0};

    __cpuid(Value, Leaf);

    if (EAX)
        *EAX = Value[0];

    if (EBX)
        *EBX = Value[1];

    if (ECX)
        *ECX = Value[2];

    if (EDX)
        *EDX = Value[3];
}

static FORCEINLINE LONG
__InterlockedAdd(
    IN  LONG    *Value,
    IN  LONG    Delta
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
    IN  LONG    *Value,
    IN  LONG    Delta
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

#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
#define ALLOCATE_POOL(a, b, c) ExAllocatePool2(a, b, c)
#else
#define ALLOCATE_POOL(a, b, c) ExAllocatePoolWithTag(a, b, c)
#endif

__checkReturn
static FORCEINLINE PVOID
__AllocatePoolWithTag(
    IN  POOL_TYPE   PoolType,
    IN  SIZE_T      NumberOfBytes,
    IN  ULONG       Tag
    )
{
    PUCHAR          Buffer;

    __analysis_assume(PoolType == NonPagedPool ||
                      PoolType == PagedPool);

#pragma warning(suppress:28160) // annotation error
    Buffer = ALLOCATE_POOL(PoolType, NumberOfBytes, Tag);
    if (Buffer == NULL)
        return NULL;

    RtlZeroMemory(Buffer, NumberOfBytes);
    return Buffer;
}

static FORCEINLINE VOID
__FreePoolWithTag(
    IN  PVOID   Buffer,
    IN  ULONG   Tag
    )
{
    ExFreePoolWithTag(Buffer, Tag);
}

#define FAIL_WITH_ERROR(mdl, status) \
    { \
        /* Print an error message with the name of the failed function and the status code */ \
        Error("%s failed (%08x)\n", __FUNCTION__, status); \
        \
        /* If an MDL was allocated, free the pages and memory associated with it */ \
        if (mdl != NULL) { \
            MmFreePagesFromMdl(mdl); \
            ExFreePool(mdl); \
        } \
        \
        /* Return NULL to indicate failure */ \
        return NULL; \
    }

static FORCEINLINE PMDL
__AllocatePages(
    IN  ULONG           Count,
    IN  BOOLEAN         zeroInitialize
    )
{
    PHYSICAL_ADDRESS    LowAddress;
    PHYSICAL_ADDRESS    HighAddress;
    LARGE_INTEGER       SkipBytes;
    SIZE_T              TotalBytes;
    PMDL                Mdl;
    PUCHAR              MdlMappedSystemVa;

    LowAddress.QuadPart = 0ull;
    HighAddress.QuadPart = ~0ull;
    SkipBytes.QuadPart = 0ull;
    TotalBytes = (SIZE_T)PAGE_SIZE * Count;

    ULONG allocFlags = zeroInitialize ? MM_ALLOCATE_FULLY_REQUIRED : MM_DONT_ZERO_ALLOCATION;

    Mdl = MmAllocatePagesForMdlEx(LowAddress,
                                  HighAddress,
                                  SkipBytes,
                                  TotalBytes,
                                  MmCached,
                                  allocFlags);

    if (Mdl == NULL || Mdl->ByteCount < TotalBytes)
        FAIL_WITH_ERROR(Mdl, STATUS_NO_MEMORY);

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

    if (MdlMappedSystemVa == NULL)
        FAIL_WITH_ERROR(Mdl, STATUS_UNSUCCESSFUL);

    Mdl->StartVa = PAGE_ALIGN(MdlMappedSystemVa);

    ASSERT3U(Mdl->ByteOffset, ==, 0);
    ASSERT3P(Mdl->StartVa, ==, MdlMappedSystemVa);
    ASSERT3P(Mdl->MappedSystemVa, ==, MdlMappedSystemVa);

    if (zeroInitialize) {
        RtlZeroMemory(MdlMappedSystemVa, Mdl->ByteCount);
    }

    return Mdl;
}

/*	__AllocatePage was implemented with MmAllocatePagesForMdlEx using flag MM_DONT_ZERO_ALLOCATION in xeniface
	This inline function has been refatored and centralized with additional parameter zeroInitialize to account for this special usage
	But xeniface doesn't call this function at all.
	If it needs to be modified to use it, it would probably require using __AllocatePages(1, FALSE)	*/

#define __AllocatePage()    __AllocatePages(1, TRUE)

static FORCEINLINE VOID
__FreePages(
    IN	PMDL	Mdl
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

static FORCEINLINE PCHAR
__strtok_r(
    IN      PCHAR   Buffer,
    IN      PCHAR   Delimiter,
    IN OUT  PCHAR   *Context
    )
{
    PCHAR           Token;
    PCHAR           End;

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

static FORCEINLINE PWCHAR
__wcstok_r(
    IN      PWCHAR  Buffer,
    IN      PWCHAR  Delimiter,
    IN OUT  PWCHAR  *Context
    )
{
    PWCHAR          Token;
    PWCHAR          End;

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
    IN  CHAR    Character
    )
{
    if (Character < 'a' || Character > 'z')
        return Character;

    return 'A' + Character - 'a';
}

static FORCEINLINE CHAR
__tolower(
    IN  CHAR    Character
    )
{
    if (Character < 'A' || Character > 'Z')
        return Character;

    return 'a' + Character - 'A';
}

#endif  // _COMMON_UTIL_H
