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
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include "gnttab.h"
#include "fdo.h"
#include "range_set.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "hash_table.h"

#define XENBUS_GNTTAB_ENTRY_PER_FRAME      (PAGE_SIZE / sizeof (grant_entry_v1_t))

// Xen requires that we avoid the first 8 entries of the table and
// we also reserve some more room for the crash kernel
#define XENBUS_GNTTAB_RESERVED_ENTRY_COUNT 32

#define XENBUS_GNTTAB_ENTRY_MAGIC 'DTNG'

#define MAXNAMELEN  128

struct _XENBUS_GNTTAB_CACHE {
    LIST_ENTRY              ListEntry;
    CHAR                    Name[MAXNAMELEN];
    PXENBUS_GNTTAB_CONTEXT  Context;
    VOID                    (*AcquireLock)(PVOID);
    VOID                    (*ReleaseLock)(PVOID);
    PVOID                   Argument;
    PXENBUS_CACHE           Cache;
};

struct _XENBUS_GNTTAB_ENTRY {
    ULONG               Magic;
    ULONG               Reference;
    grant_entry_v1_t    Entry;
};

typedef struct _XENBUS_GNTTAB_MAP_ENTRY {
    PMDL    Mdl;
    ULONG   MapHandles[1];
} XENBUS_GNTTAB_MAP_ENTRY, *PXENBUS_GNTTAB_MAP_ENTRY;

typedef struct _XENBUS_GNTTAB_FRAME {
    PMDL                        Mdl;
    grant_entry_v1_t            *Entry;
} XENBUS_GNTTAB_FRAME, *PXENBUS_GNTTAB_FRAME;

struct _XENBUS_GNTTAB_CONTEXT {
    PXENBUS_FDO                 Fdo;
    KSPIN_LOCK                  Lock;
    LONG                        References;
    ULONG                       MaximumFrameCount;
    PXENBUS_GNTTAB_FRAME        Frame;
    LONG                        FrameIndex;
    XENBUS_RANGE_SET_INTERFACE  RangeSetInterface;
    PXENBUS_RANGE_SET           RangeSet;
    XENBUS_CACHE_INTERFACE      CacheInterface;
    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackEarly;
    XENBUS_DEBUG_INTERFACE      DebugInterface;
    PXENBUS_DEBUG_CALLBACK      DebugCallback;
    PXENBUS_HASH_TABLE          MapTable;
    LIST_ENTRY                  List;
};

#define XENBUS_GNTTAB_TAG   'TTNG'

static FORCEINLINE PVOID
__GnttabAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_GNTTAB_TAG);
}

static FORCEINLINE VOID
__GnttabFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, XENBUS_GNTTAB_TAG);
}

static NTSTATUS
GnttabExpand(
    _In_ PXENBUS_GNTTAB_CONTEXT Context
    )
{
    ULONG                       Index;
    PXENBUS_GNTTAB_FRAME        Frame;
    PFN_NUMBER                  Pfn;
    PHYSICAL_ADDRESS            Address;
    LONGLONG                    Start;
    LONGLONG                    End;
    NTSTATUS                    status;

    Index = InterlockedIncrement(&Context->FrameIndex);

    status = STATUS_INSUFFICIENT_RESOURCES;
    ASSERT3U(Index, <=, Context->MaximumFrameCount);
    if (Index == Context->MaximumFrameCount)
        goto fail1;

    Frame = &Context->Frame[Index];
    Frame->Mdl = FdoHoleAllocate(Context->Fdo, 1);

    status = STATUS_NO_MEMORY;
    if (Frame->Mdl == NULL)
        goto fail2;

    Frame->Entry = Frame->Mdl->StartVa;

    Pfn = MmGetMdlPfnArray(Frame->Mdl)[0];

    status = MemoryAddToPhysmap(Pfn, XENMAPSPACE_grant_table, Index);
    if (!NT_SUCCESS(status))
        goto fail3;

    Address.QuadPart = Pfn << PAGE_SHIFT;

    LogPrintf(LOG_LEVEL_INFO,
              "GNTTAB: MAP XENMAPSPACE_grant_table[%d] @ %08x.%08x\n",
              Index,
              Address.HighPart,
              Address.LowPart);

    Start = __max(XENBUS_GNTTAB_RESERVED_ENTRY_COUNT,
                  Index * XENBUS_GNTTAB_ENTRY_PER_FRAME);
    End = ((Index + 1) * XENBUS_GNTTAB_ENTRY_PER_FRAME) - 1;

    status = XENBUS_RANGE_SET(Put,
                              &Context->RangeSetInterface,
                              Context->RangeSet,
                              Start,
                              End + 1 - Start);
    if (!NT_SUCCESS(status))
        goto fail4;

    Info("added references [%08llx - %08llx]\n", Start, End);

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    (VOID) MemoryRemoveFromPhysmap(Pfn);

fail3:
    Error("fail3\n");

    Frame->Entry = NULL;

    FdoHoleFree(Context->Fdo, Frame->Mdl);
    Frame->Mdl = NULL;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    (VOID) InterlockedDecrement(&Context->FrameIndex);

    return status;
}

static VOID
GnttabMap(
    _In_ PXENBUS_GNTTAB_CONTEXT Context
    )
{
    LONG                        Index;
    NTSTATUS                    status;

    for (Index = 0; Index <= Context->FrameIndex; Index++) {
        PXENBUS_GNTTAB_FRAME    Frame = &Context->Frame[Index];
        PFN_NUMBER              Pfn;
        PHYSICAL_ADDRESS        Address;

        Pfn = MmGetMdlPfnArray(Frame->Mdl)[0];

        status = MemoryAddToPhysmap(Pfn, XENMAPSPACE_grant_table, Index);
        ASSERT(NT_SUCCESS(status));

        Address.QuadPart = Pfn << PAGE_SHIFT;

        LogPrintf(LOG_LEVEL_INFO,
                  "GNTTAB: MAP XENMAPSPACE_grant_table[%d] @ %08x.%08x\n",
                  Index,
                  Address.HighPart,
                  Address.LowPart);
    }
}

static VOID
GnttabUnmap(
    _In_ PXENBUS_GNTTAB_CONTEXT Context
    )
{
    LONG                        Index;

    for (Index = Context->FrameIndex; Index >= 0; --Index) {
        PXENBUS_GNTTAB_FRAME    Frame = &Context->Frame[Index];
        PFN_NUMBER              Pfn;

        Pfn = MmGetMdlPfnArray(Frame->Mdl)[0];

        (VOID) MemoryRemoveFromPhysmap(Pfn);

        LogPrintf(LOG_LEVEL_INFO,
                  "GNTTAB: UNMAP XENMAPSPACE_grant_table[%d]\n",
                  Index);
    }
}

static VOID
GnttabContract(
    _In_ PXENBUS_GNTTAB_CONTEXT Context
    )
{
    LONG                        Index;
    NTSTATUS                    status;

    for (Index = Context->FrameIndex; Index >= 0; --Index) {
        PXENBUS_GNTTAB_FRAME    Frame = &Context->Frame[Index];
        LONGLONG                Start;
        LONGLONG                End;
        PFN_NUMBER              Pfn;

        Start = __max(XENBUS_GNTTAB_RESERVED_ENTRY_COUNT,
                      Index * XENBUS_GNTTAB_ENTRY_PER_FRAME);
        End = ((Index + 1) * XENBUS_GNTTAB_ENTRY_PER_FRAME) - 1;

        status = XENBUS_RANGE_SET(Get,
                                  &Context->RangeSetInterface,
                                  Context->RangeSet,
                                  Start,
                                  End + 1 - Start);
        ASSERT(NT_SUCCESS(status));

        Info("removed refrences [%08llx - %08llx]\n", Start, End);

        ASSERT(Frame->Mdl != NULL);
        Pfn = MmGetMdlPfnArray(Frame->Mdl)[0];

        (VOID) MemoryRemoveFromPhysmap(Pfn);

        Frame->Entry = NULL;

        FdoHoleFree(Context->Fdo, Frame->Mdl);
        Frame->Mdl = NULL;
    }

    Context->FrameIndex = -1;
}

static NTSTATUS
GnttabEntryCtor(
    _In_ PVOID              Argument,
    _In_ PVOID              Object
    )
{
    PXENBUS_GNTTAB_CACHE    Cache = Argument;
    PXENBUS_GNTTAB_CONTEXT  Context = Cache->Context;
    PXENBUS_GNTTAB_ENTRY    Entry = Object;
    LONGLONG                Reference;
    NTSTATUS                status;

again:
    status = XENBUS_RANGE_SET(Pop,
                              &Context->RangeSetInterface,
                              Context->RangeSet,
                              1,
                              &Reference);
    if (!NT_SUCCESS(status)) {
        status = GnttabExpand(Context);
        if (!NT_SUCCESS(status))
            goto fail1;

        goto again;
    }

    Entry->Magic = XENBUS_GNTTAB_ENTRY_MAGIC;
    Entry->Reference = (ULONG)Reference;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
GnttabEntryDtor(
    _In_ PVOID              Argument,
    _In_ PVOID              Object
    )
{
    PXENBUS_GNTTAB_CACHE    Cache = Argument;
    PXENBUS_GNTTAB_CONTEXT  Context = Cache->Context;
    PXENBUS_GNTTAB_ENTRY    Entry = Object;
    NTSTATUS                status;

    status = XENBUS_RANGE_SET(Put,
                              &Context->RangeSetInterface,
                              Context->RangeSet,
                              (LONGLONG)Entry->Reference,
                              1);
    ASSERT(NT_SUCCESS(status));
}

static VOID
GnttabAcquireLock(
    _In_ PVOID              Argument
    )
{
    PXENBUS_GNTTAB_CACHE    Cache = Argument;

    Cache->AcquireLock(Cache->Argument);
}

static VOID
GnttabReleaseLock(
    _In_ PVOID              Argument
    )
{
    PXENBUS_GNTTAB_CACHE    Cache = Argument;

    Cache->ReleaseLock(Cache->Argument);
}

static NTSTATUS
GnttabCreateCache(
    _In_ PINTERFACE             Interface,
    _In_ const CHAR             *Name,
    _In_ ULONG                  Reservation,
    _In_ ULONG                  Cap,
    _In_ VOID                   (*AcquireLock)(PVOID),
    _In_ VOID                   (*ReleaseLock)(PVOID),
    _In_ PVOID                  Argument,
    _Out_ PXENBUS_GNTTAB_CACHE  *Cache
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql;
    NTSTATUS                    status;

    *Cache = __GnttabAllocate(sizeof (XENBUS_GNTTAB_CACHE));

    status = STATUS_NO_MEMORY;
    if (*Cache == NULL)
        goto fail1;

    (*Cache)->Context = Context;

    status = RtlStringCbPrintfA((*Cache)->Name,
                                sizeof ((*Cache)->Name),
                                "%s_gnttab",
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Cache)->AcquireLock = AcquireLock;
    (*Cache)->ReleaseLock = ReleaseLock;
    (*Cache)->Argument = Argument;

    status = XENBUS_CACHE(Create,
                          &Context->CacheInterface,
                          (*Cache)->Name,
                          sizeof (XENBUS_GNTTAB_ENTRY),
                          Reservation,
                          Cap,
                          GnttabEntryCtor,
                          GnttabEntryDtor,
                          GnttabAcquireLock,
                          GnttabReleaseLock,
                          *Cache,
                          &(*Cache)->Cache);
    if (!NT_SUCCESS(status))
        goto fail3;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    InsertTailList(&Context->List, &(*Cache)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    (*Cache)->Argument = NULL;
    (*Cache)->ReleaseLock = NULL;
    (*Cache)->AcquireLock = NULL;

    RtlZeroMemory((*Cache)->Name, sizeof ((*Cache)->Name));

fail2:
    Error("fail2\n");

    (*Cache)->Context = NULL;

    ASSERT(IsZeroMemory(*Cache, sizeof (XENBUS_GNTTAB_CACHE)));
    __GnttabFree(*Cache);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabCreateCacheVersion1(
    _In_ PINTERFACE             Interface,
    _In_ const CHAR             *Name,
    _In_ ULONG                  Reservation,
    _In_ VOID                   (*AcquireLock)(PVOID),
    _In_ VOID                   (*ReleaseLock)(PVOID),
    _In_ PVOID                  Argument,
    _Out_ PXENBUS_GNTTAB_CACHE  *Cache
    )
{
    return GnttabCreateCache(Interface,
                             Name,
                             Reservation,
                             0,
                             AcquireLock,
                             ReleaseLock,
                             Argument,
                             Cache);
}

static VOID
GnttabDestroyCache(
    _In_ PINTERFACE             Interface,
    _In_ PXENBUS_GNTTAB_CACHE   Cache
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Interface->Context;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    RemoveEntryList(&Cache->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&Cache->ListEntry, sizeof (LIST_ENTRY));

    XENBUS_CACHE(Destroy,
                 &Context->CacheInterface,
                 Cache->Cache);
    Cache->Cache = NULL;

    Cache->Argument = NULL;
    Cache->ReleaseLock = NULL;
    Cache->AcquireLock = NULL;

    RtlZeroMemory(Cache->Name, sizeof (Cache->Name));

    Cache->Context = NULL;

    ASSERT(IsZeroMemory(Cache, sizeof (XENBUS_GNTTAB_CACHE)));
    __GnttabFree(Cache);
}

static NTSTATUS
GnttabPermitForeignAccess(
    _In_ PINTERFACE             Interface,
    _In_ PXENBUS_GNTTAB_CACHE   Cache,
    _In_ BOOLEAN                Locked,
    _In_ USHORT                 Domain,
    _In_ PFN_NUMBER             Pfn,
    _In_ BOOLEAN                ReadOnly,
    _Out_ PXENBUS_GNTTAB_ENTRY  *Entry
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Interface->Context;
    PXENBUS_GNTTAB_FRAME        Frame;
    ULONG                       Index;
    NTSTATUS                    status;

    *Entry = XENBUS_CACHE(Get,
                          &Context->CacheInterface,
                          Cache->Cache,
                          Locked);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (*Entry == NULL)
        goto fail1;

    ASSERT3U((*Entry)->Reference, >=, XENBUS_GNTTAB_RESERVED_ENTRY_COUNT);
    ASSERT3U((*Entry)->Reference, <, (Context->FrameIndex + 1) * XENBUS_GNTTAB_ENTRY_PER_FRAME);

    (*Entry)->Entry.flags = (ReadOnly) ? GTF_readonly : 0;
    (*Entry)->Entry.domid = Domain;

    (*Entry)->Entry.frame = (uint32_t)Pfn;
    ASSERT3U((*Entry)->Entry.frame, ==, Pfn);

    Frame = &Context->Frame[(*Entry)->Reference / XENBUS_GNTTAB_ENTRY_PER_FRAME];
    Index = (*Entry)->Reference % XENBUS_GNTTAB_ENTRY_PER_FRAME;

    Frame->Entry[Index] = (*Entry)->Entry;
    KeMemoryBarrier();

    Frame->Entry[Index].flags |= GTF_permit_access;
    KeMemoryBarrier();

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabRevokeForeignAccess(
    _In_ PINTERFACE             Interface,
    _In_ PXENBUS_GNTTAB_CACHE   Cache,
    _In_ BOOLEAN                Locked,
    _In_ PXENBUS_GNTTAB_ENTRY   Entry
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Interface->Context;
    PXENBUS_GNTTAB_FRAME        Frame;
    ULONG                       Index;
    volatile SHORT              *flags;
    ULONG                       Attempt;
    NTSTATUS                    status;

    ASSERT3U(Entry->Magic, ==, XENBUS_GNTTAB_ENTRY_MAGIC);
    ASSERT3U(Entry->Reference, >=, XENBUS_GNTTAB_RESERVED_ENTRY_COUNT);
    ASSERT3U(Entry->Reference, <, (Context->FrameIndex + 1) * XENBUS_GNTTAB_ENTRY_PER_FRAME);

    Frame = &Context->Frame[Entry->Reference / XENBUS_GNTTAB_ENTRY_PER_FRAME];
    Index = Entry->Reference % XENBUS_GNTTAB_ENTRY_PER_FRAME;

    flags = (volatile SHORT *)&Frame->Entry[Index].flags;

    Attempt = 0;
    while (Attempt++ < 100) {
        uint16_t    Old;
        uint16_t    New;

        Old = *flags;
        Old &= ~(GTF_reading | GTF_writing);

        New = Old & ~GTF_permit_access;

        if (InterlockedCompareExchange16(flags, New, Old) == Old)
            break;

        SchedYield();
    }

    status = STATUS_UNSUCCESSFUL;
    if (Attempt == 100)
        goto fail1;

    RtlZeroMemory(&Frame->Entry[Index], sizeof (grant_entry_v1_t));
    RtlZeroMemory(&Entry->Entry, sizeof (grant_entry_v1_t));

    XENBUS_CACHE(Put,
                 &Context->CacheInterface,
                 Cache->Cache,
                 Entry,
                 Locked);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static ULONG
GnttabGetReference(
    _In_ PINTERFACE             Interface,
    _In_ PXENBUS_GNTTAB_ENTRY   Entry
    )
{
    UNREFERENCED_PARAMETER(Interface);

    ASSERT3U(Entry->Magic, ==, XENBUS_GNTTAB_ENTRY_MAGIC);

    return (ULONG)Entry->Reference;
}

static NTSTATUS
GnttabQueryReference(
    _In_ PINTERFACE         Interface,
    _In_ ULONG              Reference,
    _Out_opt_ PPFN_NUMBER   Pfn,
    _Out_opt_ PBOOLEAN      ReadOnly
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Interface->Context;
    PXENBUS_GNTTAB_FRAME    Frame;
    ULONG                   Index;
    NTSTATUS                status;

    status = STATUS_INVALID_PARAMETER;
    if (Reference >= (Context->FrameIndex + 1) * XENBUS_GNTTAB_ENTRY_PER_FRAME)
        goto fail1;

    Frame = &Context->Frame[Reference / XENBUS_GNTTAB_ENTRY_PER_FRAME];
    Index = Reference % XENBUS_GNTTAB_ENTRY_PER_FRAME;

    if (Pfn != NULL)
        *Pfn = Frame->Entry[Index].frame;

    if (ReadOnly != NULL)
        *ReadOnly = (Frame->Entry[Index].flags & GTF_readonly) ? TRUE : FALSE;

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabMapForeignPages(
    _In_ PINTERFACE             Interface,
    _In_ USHORT                 Domain,
    _In_ ULONG                  NumberPages,
    _In_ PULONG                 References,
    _In_ BOOLEAN                ReadOnly,
    _Out_ PHYSICAL_ADDRESS      *Address
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Interface->Context;
    PMDL                        Mdl;
    LONG                        PageIndex;
    PHYSICAL_ADDRESS            PageAddress;
    PXENBUS_GNTTAB_MAP_ENTRY    MapEntry;
    NTSTATUS                    status;

    Mdl = FdoHoleAllocate(Context->Fdo, NumberPages);

    status = STATUS_NO_MEMORY;
    if (Mdl == NULL)
        goto fail1;

    MapEntry = __GnttabAllocate(FIELD_OFFSET(XENBUS_GNTTAB_MAP_ENTRY,
                                             MapHandles) +
                                (NumberPages * sizeof (ULONG)));

    status = STATUS_NO_MEMORY;
    if (MapEntry == NULL)
        goto fail2;

    MapEntry->Mdl = Mdl;

    Address->QuadPart = MmGetMdlPfnArray(Mdl)[0] << PAGE_SHIFT;
    PageAddress.QuadPart = Address->QuadPart;

    for (PageIndex = 0; PageIndex < (LONG)NumberPages; PageIndex++) {
        status = GrantTableMapForeignPage(Domain,
                                          References[PageIndex],
                                          PageAddress,
                                          ReadOnly,
                                          &MapEntry->MapHandles[PageIndex]);
        if (!NT_SUCCESS(status))
            goto fail3;

        PageAddress.QuadPart += PAGE_SIZE;
    }

    status = HashTableAdd(Context->MapTable,
                          (ULONG_PTR)Address->QuadPart,
                          (ULONG_PTR)MapEntry);
    if (!NT_SUCCESS(status))
        goto fail4;

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

    while (--PageIndex >= 0) {
        PageAddress.QuadPart -= PAGE_SIZE;
        (VOID) GrantTableUnmapForeignPage(MapEntry->MapHandles[PageIndex],
                                          PageAddress);
    }

    Address->QuadPart = 0;

    __GnttabFree(MapEntry);

fail2:
    Error("fail2\n");

    FdoHoleFree(Context->Fdo, Mdl);

fail1:
    Error("fail1: (%08x)\n", status);

    return status;
}

static NTSTATUS
GnttabUnmapForeignPages(
    _In_ PINTERFACE             Interface,
    _In_ PHYSICAL_ADDRESS       Address
    )
{
    PXENBUS_GNTTAB_CONTEXT      Context = Interface->Context;
    ULONG                       NumberPages;
    PHYSICAL_ADDRESS            PageAddress;
    ULONG                       PageIndex;
    PXENBUS_GNTTAB_MAP_ENTRY    MapEntry;
    PMDL                        Mdl;
    NTSTATUS                    status;

    status = HashTableLookup(Context->MapTable,
                             (ULONG_PTR)Address.QuadPart,
                             (PULONG_PTR)&MapEntry);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = HashTableRemove(Context->MapTable,
                             (ULONG_PTR)Address.QuadPart);
    if (!NT_SUCCESS(status))
        goto fail2;

    PageAddress.QuadPart = Address.QuadPart;

    Mdl = MapEntry->Mdl;
    NumberPages = Mdl->ByteCount >> PAGE_SHIFT;

    for (PageIndex = 0; PageIndex < NumberPages; PageIndex++) {
        status = GrantTableUnmapForeignPage(MapEntry->MapHandles[PageIndex],
                                            PageAddress);
        BUG_ON(!NT_SUCCESS(status));

        PageAddress.QuadPart += PAGE_SIZE;
    }

    __GnttabFree(MapEntry);

    FdoHoleFree(Context->Fdo, Mdl);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1: (%08x)\n", status);

    return status;
}

static VOID
GnttabSuspendCallbackEarly(
    _In_ PVOID              Argument
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Argument;

    GnttabMap(Context);
}

static VOID
GnttabDebugCallback(
    _In_ PVOID              Argument,
    _In_ BOOLEAN            Crashing
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Argument;
    LONG                    Index;

    UNREFERENCED_PARAMETER(Crashing);

    for (Index = 0; Index <= Context->FrameIndex; Index++) {
        PXENBUS_GNTTAB_FRAME    Frame = &Context->Frame[Index];
        PHYSICAL_ADDRESS        Address;

        Address.QuadPart = MmGetMdlPfnArray(Frame->Mdl)[0] << PAGE_SHIFT;

        XENBUS_DEBUG(Printf,
                    &Context->DebugInterface,
                     "[%u] Address = %08x.%08x\n",
                     Address.HighPart,
                     Address.LowPart);
    }
}

NTSTATUS
GnttabAcquire(
    _In_ PINTERFACE         Interface
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    status = GrantTableQuerySize(NULL, &Context->MaximumFrameCount);
    if (!NT_SUCCESS(status))
        goto fail1;

    LogPrintf(LOG_LEVEL_INFO,
              "GNTTAB: MAX FRAMES = %u\n",
              Context->MaximumFrameCount);

    Context->Frame = __GnttabAllocate(Context->MaximumFrameCount * sizeof (XENBUS_GNTTAB_FRAME));

    status = STATUS_NO_MEMORY;
    if (Context->Frame == NULL)
        goto fail2;

    Context->FrameIndex = -1;

    status = XENBUS_RANGE_SET(Acquire, &Context->RangeSetInterface);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = XENBUS_RANGE_SET(Create,
                              &Context->RangeSetInterface,
                              "gnttab",
                              &Context->RangeSet);
    if (!NT_SUCCESS(status))
        goto fail4;

    status = XENBUS_CACHE(Acquire, &Context->CacheInterface);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = XENBUS_SUSPEND(Acquire, &Context->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = XENBUS_SUSPEND(Register,
                            &Context->SuspendInterface,
                            SUSPEND_CALLBACK_EARLY,
                            GnttabSuspendCallbackEarly,
                            Context,
                            &Context->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail7;

    status = XENBUS_DEBUG(Acquire, &Context->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail8;

    status = XENBUS_DEBUG(Register,
                          &Context->DebugInterface,
                          __MODULE__ "|GNTTAB",
                          GnttabDebugCallback,
                          Context,
                          &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail9;

    /* Make sure at least the reserved refrences are present */
    status = GnttabExpand(Context);
    if (!NT_SUCCESS(status))
        goto fail10;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail10:
    Error("fail10\n");

    XENBUS_DEBUG(Deregister,
                 &Context->DebugInterface,
                 Context->DebugCallback);
    Context->DebugCallback = NULL;

fail9:
    Error("fail9\n");

    XENBUS_DEBUG(Release, &Context->DebugInterface);

fail8:
    Error("fail8\n");

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

fail7:
    Error("fail7\n");

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

fail6:
    Error("fail6\n");

    XENBUS_CACHE(Release, &Context->CacheInterface);

fail5:
    Error("fail5\n");

    GnttabContract(Context);
    ASSERT3S(Context->FrameIndex, ==, -1);

    XENBUS_RANGE_SET(Destroy,
                     &Context->RangeSetInterface,
                     Context->RangeSet);
    Context->RangeSet = NULL;

fail4:
    Error("fail4\n");

    XENBUS_RANGE_SET(Release, &Context->RangeSetInterface);

fail3:
    Error("fail3\n");

    Context->FrameIndex = 0;

    __GnttabFree(Context->Frame);
    Context->Frame = NULL;

fail2:
    Error("fail2\n");

    Context->MaximumFrameCount = 0;

fail1:
    Error("fail1 (%08x)\n", status);

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

VOID
GnttabRelease(
    _In_ PINTERFACE         Interface
    )
{
    PXENBUS_GNTTAB_CONTEXT  Context = Interface->Context;
    KIRQL                   Irql;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (--Context->References > 0)
        goto done;

    Trace("====>\n");

    if (!IsListEmpty(&Context->List))
        BUG("OUTSTANDING CACHES");

    XENBUS_DEBUG(Deregister,
                 &Context->DebugInterface,
                 Context->DebugCallback);
    Context->DebugCallback = NULL;

    XENBUS_DEBUG(Release, &Context->DebugInterface);

    XENBUS_SUSPEND(Deregister,
                   &Context->SuspendInterface,
                   Context->SuspendCallbackEarly);
    Context->SuspendCallbackEarly = NULL;

    XENBUS_SUSPEND(Release, &Context->SuspendInterface);

    XENBUS_CACHE(Release, &Context->CacheInterface);

    GnttabContract(Context);
    ASSERT3S(Context->FrameIndex, ==, -1);

    XENBUS_RANGE_SET(Destroy,
                     &Context->RangeSetInterface,
                     Context->RangeSet);
    Context->RangeSet = NULL;

    XENBUS_RANGE_SET(Release, &Context->RangeSetInterface);

    Context->FrameIndex = 0;

    __GnttabFree(Context->Frame);
    Context->Frame = NULL;

    Context->MaximumFrameCount = 0;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_GNTTAB_INTERFACE_V2   GnttabInterfaceVersion2 = {
    { sizeof (struct _XENBUS_GNTTAB_INTERFACE_V2), 2, NULL, NULL, NULL },
    GnttabAcquire,
    GnttabRelease,
    GnttabCreateCacheVersion1,
    GnttabPermitForeignAccess,
    GnttabRevokeForeignAccess,
    GnttabGetReference,
    GnttabDestroyCache,
    GnttabMapForeignPages,
    GnttabUnmapForeignPages
};

static struct _XENBUS_GNTTAB_INTERFACE_V3   GnttabInterfaceVersion3 = {
    { sizeof (struct _XENBUS_GNTTAB_INTERFACE_V3), 3, NULL, NULL, NULL },
    GnttabAcquire,
    GnttabRelease,
    GnttabCreateCacheVersion1,
    GnttabPermitForeignAccess,
    GnttabRevokeForeignAccess,
    GnttabGetReference,
    GnttabQueryReference,
    GnttabDestroyCache,
    GnttabMapForeignPages,
    GnttabUnmapForeignPages
};

static struct _XENBUS_GNTTAB_INTERFACE_V4   GnttabInterfaceVersion4 = {
    { sizeof (struct _XENBUS_GNTTAB_INTERFACE_V4), 4, NULL, NULL, NULL },
    GnttabAcquire,
    GnttabRelease,
    GnttabCreateCache,
    GnttabPermitForeignAccess,
    GnttabRevokeForeignAccess,
    GnttabGetReference,
    GnttabQueryReference,
    GnttabDestroyCache,
    GnttabMapForeignPages,
    GnttabUnmapForeignPages
};

NTSTATUS
GnttabInitialize(
    _In_ PXENBUS_FDO                Fdo,
    _Out_ PXENBUS_GNTTAB_CONTEXT    *Context
    )
{
    NTSTATUS                        status;

    Trace("====>\n");

    *Context = __GnttabAllocate(sizeof (XENBUS_GNTTAB_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    status = RangeSetGetInterface(FdoGetRangeSetContext(Fdo),
                                  XENBUS_RANGE_SET_INTERFACE_VERSION_MAX,
                                  (PINTERFACE)&(*Context)->RangeSetInterface,
                                  sizeof ((*Context)->RangeSetInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->RangeSetInterface.Interface.Context != NULL);

    status = CacheGetInterface(FdoGetCacheContext(Fdo),
                               XENBUS_CACHE_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&(*Context)->CacheInterface,
                               sizeof ((*Context)->CacheInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->CacheInterface.Interface.Context != NULL);

    status = SuspendGetInterface(FdoGetSuspendContext(Fdo),
                                 XENBUS_SUSPEND_INTERFACE_VERSION_MAX,
                                 (PINTERFACE)&(*Context)->SuspendInterface,
                                 sizeof ((*Context)->SuspendInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->SuspendInterface.Interface.Context != NULL);

    status = DebugGetInterface(FdoGetDebugContext(Fdo),
                               XENBUS_DEBUG_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&(*Context)->DebugInterface,
                               sizeof ((*Context)->DebugInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->DebugInterface.Interface.Context != NULL);

    InitializeListHead(&(*Context)->List);
    KeInitializeSpinLock(&(*Context)->Lock);

    status = HashTableCreate(&(*Context)->MapTable);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Context)->Fdo = Fdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
GnttabGetInterface(
    _In_ PXENBUS_GNTTAB_CONTEXT Context,
    _In_ ULONG                  Version,
    _Inout_ PINTERFACE          Interface,
    _In_ ULONG                  Size
    )
{
    NTSTATUS                    status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 2: {
        struct _XENBUS_GNTTAB_INTERFACE_V2  *GnttabInterface;

        GnttabInterface = (struct _XENBUS_GNTTAB_INTERFACE_V2 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_GNTTAB_INTERFACE_V2))
            break;

        *GnttabInterface = GnttabInterfaceVersion2;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 3: {
        struct _XENBUS_GNTTAB_INTERFACE_V3  *GnttabInterface;

        GnttabInterface = (struct _XENBUS_GNTTAB_INTERFACE_V3 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_GNTTAB_INTERFACE_V3))
            break;

        *GnttabInterface = GnttabInterfaceVersion3;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 4: {
        struct _XENBUS_GNTTAB_INTERFACE_V4  *GnttabInterface;

        GnttabInterface = (struct _XENBUS_GNTTAB_INTERFACE_V4 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_GNTTAB_INTERFACE_V4))
            break;

        *GnttabInterface = GnttabInterfaceVersion4;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}

ULONG
GnttabGetReferences(
    _In_ PXENBUS_GNTTAB_CONTEXT Context
    )
{
    return Context->References;
}

VOID
GnttabTeardown(
    _In_ PXENBUS_GNTTAB_CONTEXT Context
    )
{
    Trace("====>\n");

    Context->Fdo = NULL;

    HashTableDestroy(Context->MapTable);
    Context->MapTable = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    RtlZeroMemory(&Context->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    RtlZeroMemory(&Context->CacheInterface,
                  sizeof (XENBUS_CACHE_INTERFACE));

    RtlZeroMemory(&Context->RangeSetInterface,
                  sizeof (XENBUS_RANGE_SET_INTERFACE));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_GNTTAB_CONTEXT)));
    __GnttabFree(Context);

    Trace("<====\n");
}
