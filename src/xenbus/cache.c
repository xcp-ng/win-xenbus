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
#include <procgrp.h>
#include <ntstrsafe.h>
#include <stdlib.h>

#include "thread.h"
#include "cache.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

extern ULONG
NTAPI
RtlRandomEx (
    __inout PULONG Seed
    );

#define XENBUS_CACHE_MAGAZINE_SLOTS   6

typedef struct _XENBUS_CACHE_MAGAZINE {
    PVOID   Slot[XENBUS_CACHE_MAGAZINE_SLOTS];
} XENBUS_CACHE_MAGAZINE, *PXENBUS_CACHE_MAGAZINE;

#define XENBUS_CACHE_SLAB_MAGIC 'BALS'

typedef struct _XENBUS_CACHE_MASK {
    ULONG   Size;
    ULONG   Count;
    ULONG   Mask[1];
} XENBUS_CACHE_MASK, *PXENBUS_CACHE_MASK;

#define BITS_PER_ULONG  (sizeof (ULONG) * 8)

typedef struct _XENBUS_CACHE_SLAB {
    ULONG               Magic;
    PXENBUS_CACHE       Cache;
    LIST_ENTRY          ListEntry;
    PXENBUS_CACHE_MASK  Constructed;
    PXENBUS_CACHE_MASK  Allocated;
    UCHAR               Buffer[1];
} XENBUS_CACHE_SLAB, *PXENBUS_CACHE_SLAB;

#define MAXNAMELEN      128

struct _XENBUS_CACHE {
    LIST_ENTRY              ListEntry;
    CHAR                    Name[MAXNAMELEN];
    ULONG                   Size;
    ULONG                   Reservation;
    ULONG                   Cap;
    NTSTATUS                (*Ctor)(PVOID, PVOID);
    VOID                    (*Dtor)(PVOID, PVOID);
    VOID                    (*AcquireLock)(PVOID);
    VOID                    (*ReleaseLock)(PVOID);
    PVOID                   Argument;
    LIST_ENTRY              SlabList;
    PLIST_ENTRY             Cursor;
    ULONG                   Count;
    PXENBUS_CACHE_MAGAZINE  Magazine;
    ULONG                   MagazineCount;
    LONG                    CurrentSlabs;
    LONG                    MaximumSlabs;
    LONG                    CurrentObjects;
    LONG                    MaximumObjects;
};

struct _XENBUS_CACHE_CONTEXT {
    PXENBUS_FDO             Fdo;
    KSPIN_LOCK              Lock;
    LONG                    References;
    XENBUS_DEBUG_INTERFACE  DebugInterface;
    PXENBUS_DEBUG_CALLBACK  DebugCallback;
    PXENBUS_THREAD          MonitorThread;
    LIST_ENTRY              List;
};

#define CACHE_TAG   'HCAC'

static FORCEINLINE PVOID
__CacheAllocate(
    _In_ ULONG  Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, CACHE_TAG);
}

static FORCEINLINE VOID
__CacheFree(
    _In_ PVOID  Buffer
    )
{
    __FreePoolWithTag(Buffer, CACHE_TAG);
}

static FORCEINLINE VOID
_IRQL_requires_(DISPATCH_LEVEL)
__CacheAcquireLock(
    _In_ PXENBUS_CACHE  Cache
    )
{
    Cache->AcquireLock(Cache->Argument);
}

static FORCEINLINE VOID
_IRQL_requires_(DISPATCH_LEVEL)
__CacheReleaseLock(
    _In_ PXENBUS_CACHE  Cache
    )
{
    Cache->ReleaseLock(Cache->Argument);
}

static FORCEINLINE NTSTATUS
_IRQL_requires_(DISPATCH_LEVEL)
__CacheCtor(
    _In_ PXENBUS_CACHE  Cache,
    _In_ PVOID          Object
    )
{
    return Cache->Ctor(Cache->Argument, Object);
}

static FORCEINLINE VOID
_IRQL_requires_(DISPATCH_LEVEL)
__CacheDtor(
    _In_ PXENBUS_CACHE  Cache,
    _In_ PVOID          Object
    )
{
    Cache->Dtor(Cache->Argument, Object);
}

static PVOID
CacheGetObjectFromMagazine(
    _In_ PXENBUS_CACHE_MAGAZINE Magazine
    )
{
    ULONG                       Index;

    for (Index = 0; Index < XENBUS_CACHE_MAGAZINE_SLOTS; Index++) {
        PVOID   Object;

        if (Magazine->Slot[Index] != NULL) {
            Object = Magazine->Slot[Index];
            Magazine->Slot[Index] = NULL;

            return Object;
        }
    }

    return NULL;
}

static NTSTATUS
CachePutObjectToMagazine(
    _In_ PXENBUS_CACHE_MAGAZINE Magazine,
    _In_ PVOID                  Object
    )
{
    ULONG                       Index;

    for (Index = 0; Index < XENBUS_CACHE_MAGAZINE_SLOTS; Index++) {
        if (Magazine->Slot[Index] == NULL) {
            Magazine->Slot[Index] = Object;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_UNSUCCESSFUL;
}

static PXENBUS_CACHE_MASK
CacheMaskCreate(
    _In_ ULONG          Size
    )
{
    ULONG               NumberOfBytes;
    PXENBUS_CACHE_MASK  Mask;

    NumberOfBytes = FIELD_OFFSET(XENBUS_CACHE_MASK, Mask) +
        (P2ROUNDUP(ULONG, Size, BITS_PER_ULONG) / 8);

    Mask = __CacheAllocate(NumberOfBytes);
    if (Mask == NULL)
        goto fail1;

    Mask->Size = Size;

    return Mask;

fail1:
    return NULL;
}

static VOID
CacheMaskDestroy(
    _In_ PXENBUS_CACHE_MASK Mask
    )
{
    ASSERT(Mask->Count == 0);
    __CacheFree(Mask);
}

static FORCEINLINE VOID
__CacheMaskSet(
    _In_ PXENBUS_CACHE_MASK Mask,
    _In_ ULONG              Bit
    )
{
    ULONG                   Index = Bit / BITS_PER_ULONG;
    ULONG                   Value = 1u << (Bit % BITS_PER_ULONG);

    ASSERT3U(Bit, <, Mask->Size);

    ASSERT(!(Mask->Mask[Index] & Value));
    Mask->Mask[Index] |= Value;
    ASSERT(Mask->Count < Mask->Size);
    Mask->Count++;
}

static FORCEINLINE BOOLEAN
__CacheMaskTest(
    _In_ PXENBUS_CACHE_MASK Mask,
    _In_ ULONG              Bit
    )
{
    ULONG                   Index = Bit / BITS_PER_ULONG;
    ULONG                   Value = 1u << (Bit % BITS_PER_ULONG);

    ASSERT3U(Bit, <, Mask->Size);

    return (Mask->Mask[Index] & Value) ? TRUE : FALSE;
}

static FORCEINLINE VOID
__CacheMaskClear(
    _In_ PXENBUS_CACHE_MASK Mask,
    _In_ ULONG              Bit
    )
{
    ULONG                   Index = Bit / BITS_PER_ULONG;
    ULONG                   Value = 1u << (Bit % BITS_PER_ULONG);

    ASSERT3U(Bit, <, Mask->Size);

    ASSERT(Mask->Count != 0);
    --Mask->Count;
    ASSERT(Mask->Mask[Index] & Value);
    Mask->Mask[Index] &= ~Value;
}

static ULONG
CacheMaskSize(
    _In_ PXENBUS_CACHE_MASK Mask
    )
{
    return Mask->Size;
}

static ULONG
CacheMaskCount(
    _In_ PXENBUS_CACHE_MASK Mask
    )
{
    return Mask->Count;
}

static VOID
CacheInsertSlab(
    _In_ PXENBUS_CACHE      Cache,
    _In_ PXENBUS_CACHE_SLAB New
    )
{
#define INSERT_BEFORE(_ListEntry, _New)             \
        do {                                        \
            (_New)->Blink = (_ListEntry)->Blink;    \
            (_ListEntry)->Blink->Flink = (_New);    \
                                                    \
            (_ListEntry)->Blink = (_New);           \
            (_New)->Flink = (_ListEntry);           \
        } while (FALSE)

    PLIST_ENTRY             ListEntry;

    ASSERT(CacheMaskCount(New->Allocated) < CacheMaskSize(New->Allocated));

    Cache->Cursor = NULL;

    for (ListEntry = Cache->SlabList.Flink;
         ListEntry != &Cache->SlabList;
         ListEntry = ListEntry->Flink) {
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        if (CacheMaskCount(Slab->Allocated) < CacheMaskCount(New->Allocated)) {
            INSERT_BEFORE(ListEntry, &New->ListEntry);
            goto done;
        }

        if (CacheMaskCount(Slab->Allocated) < CacheMaskSize(Slab->Allocated) &&
            Cache->Cursor == NULL)
            Cache->Cursor = ListEntry;
    }

    InsertTailList(&Cache->SlabList, &New->ListEntry);

done:
    if (Cache->Cursor == NULL) {
        //
        // A newly inserted slab has either just been created, or has just had
        // an object freed back to it. Either will it should never be full.
        //
        ASSERT(CacheMaskCount(New->Allocated) < CacheMaskSize(New->Allocated));
        Cache->Cursor = &New->ListEntry;
    }

#undef  INSERT_BEFORE
}

#if DBG
static VOID
CacheAudit(
    _In_ PXENBUS_CACHE  Cache
    )
{
    ULONG               Count = ULONG_MAX;
    PLIST_ENTRY         ListEntry;

    //
    // The cursor should point at the first slab that is not fully
    // occupied.
    //
    for (ListEntry = Cache->SlabList.Flink;
         ListEntry != &Cache->SlabList;
         ListEntry = ListEntry->Flink) {
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        if (CacheMaskCount(Slab->Allocated) < CacheMaskSize(Slab->Allocated)) {
            ASSERT3P(Cache->Cursor, ==, ListEntry);
            break;
        }
    }

    // Slabs should be kept in order of maximum to minimum occupancy
    for (ListEntry = Cache->SlabList.Flink;
         ListEntry != &Cache->SlabList;
         ListEntry = ListEntry->Flink) {
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        ASSERT3U(CacheMaskCount(Slab->Allocated), <=, Count);

        Count = CacheMaskCount(Slab->Allocated);
    }
}
#else
#define CacheAudit(_Cache) ((VOID)(_Cache))
#endif

// Must be called with lock held
static NTSTATUS
CacheCreateSlab(
    _In_ PXENBUS_CACHE  Cache
    )
{
    PXENBUS_CACHE_SLAB  Slab;
    ULONG               NumberOfBytes;
    ULONG               Count;
    LONG                SlabCount;
    NTSTATUS            status;

    NumberOfBytes = P2ROUNDUP(ULONG,
                              FIELD_OFFSET(XENBUS_CACHE_SLAB, Buffer) +
                              Cache->Size,
                              PAGE_SIZE);
    Count = (NumberOfBytes - FIELD_OFFSET(XENBUS_CACHE_SLAB, Buffer)) /
            Cache->Size;
    ASSERT(Count != 0);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (Cache->Count + Count > Cache->Cap)
        goto fail1;

    Slab = __CacheAllocate(NumberOfBytes);
    ASSERT3P(Slab, ==, PAGE_ALIGN(Slab));

    status = STATUS_NO_MEMORY;
    if (Slab == NULL)
        goto fail2;

    RtlZeroMemory(Slab, NumberOfBytes);

    Slab->Magic = XENBUS_CACHE_SLAB_MAGIC;
    Slab->Cache = Cache;

    Slab->Constructed = CacheMaskCreate(Count);
    if (Slab->Constructed == NULL)
        goto fail3;

    Slab->Allocated = CacheMaskCreate(Count);
    if (Slab->Allocated == NULL)
        goto fail4;

    CacheInsertSlab(Cache, Slab);
    Cache->Count += Count;

    SlabCount = InterlockedIncrement(&Cache->CurrentSlabs);
    if (SlabCount > Cache->MaximumSlabs)
        Cache->MaximumSlabs = SlabCount;

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    CacheMaskDestroy(Slab->Constructed);

fail3:
    Error("fail3\n");

    __CacheFree(Slab);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

// Must be called with lock held
static VOID
CacheDestroySlab(
    _In_ PXENBUS_CACHE      Cache,
    _In_ PXENBUS_CACHE_SLAB Slab
    )
{
    LONG                    Index;

    ASSERT3U(Cache->Count, >=, CacheMaskSize(Slab->Allocated));
    Cache->Count -= CacheMaskSize(Slab->Allocated);

    //
    // The cursor slab should always be the first slab in the list that is not
    // fully occupied. If we are destroying it then clearly it is empty, but
    // it may be one of several empty slabs. Set the cursor to the current
    // cursor's Flink so that it will either point at the next empty slab, or
    // the list anchor if there are no more empty slabs.
    //
    if (Cache->Cursor == &Slab->ListEntry)
        Cache->Cursor = Slab->ListEntry.Flink;

    RemoveEntryList(&Slab->ListEntry);
    CacheAudit(Cache);

    Index = CacheMaskSize(Slab->Constructed);
    while (--Index >= 0) {
        PVOID Object = (PVOID)&Slab->Buffer[Index * Cache->Size];

        if (__CacheMaskTest(Slab->Constructed, Index)) {
            __CacheDtor(Cache, Object);
            __CacheMaskClear(Slab->Constructed, Index);
        }
    }

    ASSERT(Cache->CurrentSlabs != 0);
    InterlockedDecrement(&Cache->CurrentSlabs);

    CacheMaskDestroy(Slab->Allocated);
    CacheMaskDestroy(Slab->Constructed);
    __CacheFree(Slab);
}

// Must be called with lock held
static PVOID
CacheGetObjectFromSlab(
    _In_ PXENBUS_CACHE_SLAB Slab
    )
{
    PXENBUS_CACHE           Cache;
    ULONG                   Index;
    PVOID                   Object;
    NTSTATUS                status;

    Cache = Slab->Cache;

    ASSERT(CacheMaskCount(Slab->Allocated) <= CacheMaskSize(Slab->Allocated));

    status = STATUS_NO_MEMORY;
    if (CacheMaskCount(Slab->Allocated) == CacheMaskSize(Slab->Allocated))
	    goto fail1;

    //
    // If there are unallocated but constructed objects then look for one of those,
    // otherwise look for a free unconstructed object. (NOTE: The 'Constructed' mask
    // should always be contiguous).
    //
    Index = (CacheMaskCount(Slab->Allocated) < CacheMaskCount(Slab->Constructed)) ?
        0 : CacheMaskCount(Slab->Constructed);

    while (Index < CacheMaskSize(Slab->Allocated)) {
        if (!__CacheMaskTest(Slab->Allocated, Index))
            break;

        Index++;
    }

    Object = (PVOID)&Slab->Buffer[Index * Cache->Size];
    ASSERT3U(Index, ==, (ULONG)((PUCHAR)Object - &Slab->Buffer[0]) /
             Cache->Size);

    if (!__CacheMaskTest(Slab->Constructed, Index)) {
        status = __CacheCtor(Cache, Object);
        if (!NT_SUCCESS(status))
            goto fail2;

        __CacheMaskSet(Slab->Constructed, Index);
    }

    __CacheMaskSet(Slab->Allocated, Index);

    return Object;

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

// Must be called with lock held
static VOID
CachePutObjectToSlab(
    _In_ PXENBUS_CACHE_SLAB Slab,
    _In_ PVOID              Object
    )
{
    PXENBUS_CACHE           Cache;
    ULONG                   Index;

    Cache = Slab->Cache;

    Index = (ULONG)((PUCHAR)Object - &Slab->Buffer[0]) / Cache->Size;
    BUG_ON(Index >= CacheMaskSize(Slab->Allocated));

    __CacheMaskClear(Slab->Allocated, Index);
}

static PVOID
CacheGet(
    _In_ PINTERFACE         Interface,
    _In_ PXENBUS_CACHE      Cache,
    _In_ BOOLEAN            Locked
    )
{
    KIRQL                   Irql;
    ULONG                   Index;
    PXENBUS_CACHE_MAGAZINE  Magazine;
    PVOID                   Object;
    LONG                    ObjectCount;

    UNREFERENCED_PARAMETER(Interface);

    ASSERT(Cache != NULL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Index = KeGetCurrentProcessorNumberEx(NULL);

    ASSERT3U(Index, <, Cache->MagazineCount);
    Magazine = &Cache->Magazine[Index];

    Object = CacheGetObjectFromMagazine(Magazine);
    if (Object != NULL)
        goto done;

    if (!Locked)
        __CacheAcquireLock(Cache);

again:
    if (Cache->Cursor != &Cache->SlabList) {
        PLIST_ENTRY         ListEntry = Cache->Cursor;
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        Object = CacheGetObjectFromSlab(Slab);
        ASSERT(Object != NULL);

        //
        // If the slab is now fully occupied, ove the cursor on to the next
        // slab. If there are no more slabed then Flink will be pointing at
        // Cache->SlabList so we will create a new slab next time round, if
        // necessary.
        //
        if (CacheMaskCount(Slab->Allocated) == CacheMaskSize(Slab->Allocated))
            Cache->Cursor = Slab->ListEntry.Flink;
    } else {
        NTSTATUS status;

        ASSERT3P(Cache->Cursor, ==, &Cache->SlabList);

        status = CacheCreateSlab(Cache);
        if (NT_SUCCESS(status)) {
            ASSERT(Cache->Cursor != &Cache->SlabList);
            goto again;
        }
    }

    CacheAudit(Cache);

    if (!Locked)
        __CacheReleaseLock(Cache);

done:
    if (Object != NULL) {
        ObjectCount = InterlockedIncrement(&Cache->CurrentObjects);
        if (ObjectCount > Cache->MaximumObjects)
            Cache->MaximumObjects = ObjectCount;
    }

    KeLowerIrql(Irql);

    return Object;
}

static VOID
CachePut(
    _In_ PINTERFACE         Interface,
    _In_ PXENBUS_CACHE      Cache,
    _In_ PVOID              Object,
    _In_ BOOLEAN            Locked
    )
{
    KIRQL                   Irql;
    ULONG                   Index;
    PXENBUS_CACHE_MAGAZINE  Magazine;
    PXENBUS_CACHE_SLAB      Slab;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Interface);

    ASSERT(Cache != NULL);
    ASSERT(Object != NULL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Index = KeGetCurrentProcessorNumberEx(NULL);

    ASSERT3U(Index, <, Cache->MagazineCount);
    Magazine = &Cache->Magazine[Index];

    status = CachePutObjectToMagazine(Magazine, Object);

    if (NT_SUCCESS(status))
        goto done;

    Slab = (PXENBUS_CACHE_SLAB)PAGE_ALIGN(Object);
    ASSERT3U(Slab->Magic, ==, XENBUS_CACHE_SLAB_MAGIC);

    if (!Locked)
        __CacheAcquireLock(Cache);

    CachePutObjectToSlab(Slab, Object);

    //
    // To maintain the order, and the invariant that the cursor always points,
    // to the first slab with available space we must remove this slab from the
    // list and re-insert it at it's (now) correct location.
    //
    RemoveEntryList(&Slab->ListEntry);
    CacheInsertSlab(Cache, Slab);

    CacheAudit(Cache);

    if (!Locked)
        __CacheReleaseLock(Cache);

done:
    ASSERT(Cache->CurrentObjects != 0);
    InterlockedDecrement(&Cache->CurrentObjects);

    KeLowerIrql(Irql);
}

static NTSTATUS
CacheFill(
    _In_ PXENBUS_CACHE  Cache,
    _In_ ULONG          Count
    )
{
    KIRQL               Irql;
    NTSTATUS            status;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __CacheAcquireLock(Cache);

    status = STATUS_SUCCESS;
    while (Cache->Count < Count) {
        status = CacheCreateSlab(Cache);
        if (!NT_SUCCESS(status))
            break;
    }

    CacheAudit(Cache);

    __CacheReleaseLock(Cache);
    KeLowerIrql(Irql);

    return status;
}

static VOID
CacheSpill(
    _In_ PXENBUS_CACHE  Cache,
    _In_ ULONG          Count
    )
{
    KIRQL               Irql;
    PLIST_ENTRY         ListEntry;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __CacheAcquireLock(Cache);

    if (Cache->Count <= Count)
        goto done;

    while (!IsListEmpty(&Cache->SlabList)) {
        PXENBUS_CACHE_SLAB  Slab;

        ListEntry = Cache->SlabList.Blink;
        ASSERT(ListEntry != &Cache->SlabList);

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        //
        // Slabs are kept in order of maximum to minimum occupancy so we know
        // that if the last slab in the list is not empty, then none of the
        // slabs before it will be empty.
        //
        if (CacheMaskCount(Slab->Allocated) != 0)
            break;

        ASSERT(Cache->Count >= CacheMaskSize(Slab->Allocated));
        if (Cache->Count - CacheMaskSize(Slab->Allocated) < Count)
            break;

        CacheDestroySlab(Cache, Slab);
    }

    CacheAudit(Cache);

done:
    __CacheReleaseLock(Cache);
    KeLowerIrql(Irql);
}

static FORCEINLINE VOID
__CacheFlushMagazines(
    _In_ PXENBUS_CACHE  Cache
    )
{
    KIRQL               Irql;
    ULONG               Index;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __CacheAcquireLock(Cache);

    for (Index = 0; Index < Cache->MagazineCount; Index++) {
        PXENBUS_CACHE_MAGAZINE  Magazine = &Cache->Magazine[Index];
        PVOID                   Object;

        while ((Object = CacheGetObjectFromMagazine(Magazine)) != NULL) {
            PXENBUS_CACHE_SLAB  Slab;

            Slab = (PXENBUS_CACHE_SLAB)PAGE_ALIGN(Object);
            ASSERT3U(Slab->Magic, ==, XENBUS_CACHE_SLAB_MAGIC);

            CachePutObjectToSlab(Slab, Object);
        }
    }

    __CacheReleaseLock(Cache);
    KeLowerIrql(Irql);
}

static NTSTATUS
CacheCreate(
    _In_ PINTERFACE         Interface,
    _In_ PCSTR              Name,
    _In_ ULONG              Size,
    _In_ ULONG              Reservation,
    _In_ ULONG              Cap,
    _In_ NTSTATUS           (*Ctor)(PVOID, PVOID),
    _In_ VOID               (*Dtor)(PVOID, PVOID),
    _In_ VOID               (*AcquireLock)(PVOID),
    _In_ VOID               (*ReleaseLock)(PVOID),
    _In_ PVOID              Argument,
    _Outptr_ PXENBUS_CACHE  *Cache
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    ASSERT(Name != NULL);
    ASSERT(Size != 0);
    ASSERT(Ctor != NULL);
    ASSERT(Dtor != NULL);
    ASSERT(AcquireLock != NULL);
    ASSERT(ReleaseLock != NULL);
    ASSERT(Cache != NULL);

    Trace("====> (%s)\n", Name);

    *Cache = __CacheAllocate(sizeof (XENBUS_CACHE));

    status = STATUS_NO_MEMORY;
    if (*Cache == NULL)
        goto fail1;

    status = RtlStringCbPrintfA((*Cache)->Name,
                                sizeof ((*Cache)->Name),
                                "%s",
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    Size = P2ROUNDUP(ULONG, Size, sizeof (ULONG_PTR));

    if (Cap == 0)
        Cap = ULONG_MAX;

    (*Cache)->Size = Size;
    (*Cache)->Reservation = Reservation;
    (*Cache)->Cap = Cap;
    (*Cache)->Ctor = Ctor;
    (*Cache)->Dtor = Dtor;
    (*Cache)->AcquireLock = AcquireLock;
    (*Cache)->ReleaseLock = ReleaseLock;
    (*Cache)->Argument = Argument;

    InitializeListHead(&(*Cache)->SlabList);
    (*Cache)->Cursor = &(*Cache)->SlabList;

    status = STATUS_INVALID_PARAMETER;
    if ((*Cache)->Reservation > (*Cache)->Cap)
        goto fail3;

    status = CacheFill(*Cache, (*Cache)->Reservation);
    if (!NT_SUCCESS(status))
        goto fail4;

    (*Cache)->MagazineCount = KeQueryMaximumProcessorCountEx(ALL_PROCESSOR_GROUPS);
    (*Cache)->Magazine = __CacheAllocate(sizeof (XENBUS_CACHE_MAGAZINE) * (*Cache)->MagazineCount);

    status = STATUS_NO_MEMORY;
    if ((*Cache)->Magazine == NULL)
        goto fail5;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    InsertTailList(&Context->List, &(*Cache)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    (*Cache)->MagazineCount = 0;

    CacheSpill(*Cache, 0);

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

    (*Cache)->Cursor = NULL;
    ASSERT(IsListEmpty(&(*Cache)->SlabList));
    RtlZeroMemory(&(*Cache)->SlabList, sizeof (LIST_ENTRY));

    (*Cache)->Argument = NULL;
    (*Cache)->ReleaseLock = NULL;
    (*Cache)->AcquireLock = NULL;
    (*Cache)->Dtor = NULL;
    (*Cache)->Ctor = NULL;
    (*Cache)->Cap = 0;
    (*Cache)->Reservation = 0;
    (*Cache)->Size = 0;

fail2:
    Error("fail2\n");

    RtlZeroMemory((*Cache)->Name, sizeof ((*Cache)->Name));

    ASSERT(IsZeroMemory(*Cache, sizeof (XENBUS_CACHE)));
    __CacheFree(*Cache);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static NTSTATUS
CacheCreateVersion1(
    _In_ PINTERFACE         Interface,
    _In_ PCSTR              Name,
    _In_ ULONG              Size,
    _In_ ULONG              Reservation,
    _In_ NTSTATUS           (*Ctor)(PVOID, PVOID),
    _In_ VOID               (*Dtor)(PVOID, PVOID),
    _In_ VOID               (*AcquireLock)(PVOID),
    _In_ VOID               (*ReleaseLock)(PVOID),
    _In_ PVOID              Argument,
    _Outptr_ PXENBUS_CACHE  *Cache
    )
{
    return CacheCreate(Interface,
                       Name,
                       Size,
                       Reservation,
                       0,
                       Ctor,
                       Dtor,
                       AcquireLock,
                       ReleaseLock,
                       Argument,
                       Cache);
}

static VOID
CacheDestroy(
    _In_ PINTERFACE         Interface,
    _In_ PXENBUS_CACHE      Cache
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Interface->Context;
    KIRQL                   Irql;

    ASSERT(Cache != NULL);

    Trace("====> (%s)\n", Cache->Name);

    KeAcquireSpinLock(&Context->Lock, &Irql);
    RemoveEntryList(&Cache->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    RtlZeroMemory(&Cache->ListEntry, sizeof (LIST_ENTRY));

    __CacheFlushMagazines(Cache);

    ASSERT(IsZeroMemory(Cache->Magazine, sizeof (XENBUS_CACHE_MAGAZINE) * Cache->MagazineCount));
    __CacheFree(Cache->Magazine);
    Cache->Magazine = NULL;
    Cache->MagazineCount = 0;

    CacheSpill(Cache, 0);

    ASSERT(Cache->CurrentObjects == 0);
    Cache->MaximumObjects = 0;

    ASSERT(Cache->CurrentSlabs == 0);
    Cache->MaximumSlabs = 0;

    Cache->Cursor = NULL;
    ASSERT(IsListEmpty(&Cache->SlabList));
    RtlZeroMemory(&Cache->SlabList, sizeof (LIST_ENTRY));

    Cache->Argument = NULL;
    Cache->ReleaseLock = NULL;
    Cache->AcquireLock = NULL;
    Cache->Dtor = NULL;
    Cache->Ctor = NULL;
    Cache->Cap = 0;
    Cache->Reservation = 0;
    Cache->Size = 0;

    RtlZeroMemory(Cache->Name, sizeof (Cache->Name));

    ASSERT(IsZeroMemory(Cache, sizeof (XENBUS_CACHE)));
    __CacheFree(Cache);

    Trace("<====\n");
}

static VOID
CacheDebugCallback(
    _In_ PVOID              Argument,
    _In_ BOOLEAN            Crashing
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Argument;

    UNREFERENCED_PARAMETER(Crashing);

    if (!IsListEmpty(&Context->List)) {
        PLIST_ENTRY ListEntry;

        XENBUS_DEBUG(Printf,
                     &Context->DebugInterface,
                     "CACHES:\n");

        for (ListEntry = Context->List.Flink;
             ListEntry != &Context->List;
             ListEntry = ListEntry->Flink) {
            PXENBUS_CACHE   Cache;

            Cache = CONTAINING_RECORD(ListEntry, XENBUS_CACHE, ListEntry);

            XENBUS_DEBUG(Printf,
                         &Context->DebugInterface,
                         "- %s: Count = %d, Reservation = %d, Objects = %d / %d, Slabs = %d / %d\n",
                         Cache->Name,
                         Cache->Count,
                         Cache->Reservation,
                         Cache->CurrentObjects,
                         Cache->MaximumObjects,
                         Cache->CurrentSlabs,
                         Cache->MaximumSlabs);
        }
    }
}

#define TIME_US(_us)        ((_us) * 10)
#define TIME_MS(_ms)        (TIME_US((_ms) * 1000))
#define TIME_S(_s)          (TIME_MS((_s) * 1000))
#define TIME_RELATIVE(_t)   (-(_t))

#define XENBUS_CACHE_MONITOR_PERIOD 5

static NTSTATUS
CacheMonitor(
    _In_ PXENBUS_THREAD     Self,
    _In_ PVOID              _Context
    )
{
    PXENBUS_CACHE_CONTEXT   Context = _Context;
    PKEVENT                 Event;
    LARGE_INTEGER           Timeout;
    PLIST_ENTRY             ListEntry;

    Trace("====>\n");

    Event = ThreadGetEvent(Self);

    Timeout.QuadPart = TIME_RELATIVE(TIME_S(XENBUS_CACHE_MONITOR_PERIOD));

    for (;;) {
        KIRQL   Irql;

        (VOID) KeWaitForSingleObject(Event,
                                     Executive,
                                     KernelMode,
                                     FALSE,
                                     &Timeout);
        KeClearEvent(Event);

        if (ThreadIsAlerted(Self))
            break;

        KeAcquireSpinLock(&Context->Lock, &Irql);

        if (Context->References == 0)
            goto loop;

        for (ListEntry = Context->List.Flink;
             ListEntry != &Context->List;
             ListEntry = ListEntry->Flink) {
            PXENBUS_CACHE   Cache;

            Cache = CONTAINING_RECORD(ListEntry, XENBUS_CACHE, ListEntry);

            if (Cache->Count < Cache->Reservation)
                CacheFill(Cache, Cache->Reservation);
            else if (Cache->Count > Cache->Reservation)
                CacheSpill(Cache,
                           __max(Cache->Reservation, (Cache->Count / 2)));
        }

loop:
        KeReleaseSpinLock(&Context->Lock, Irql);
    }

    Trace("====>\n");

    return STATUS_SUCCESS;
}

static NTSTATUS
CacheAcquire(
    PINTERFACE              Interface
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

    KeAcquireSpinLock(&Context->Lock, &Irql);

    if (Context->References++ != 0)
        goto done;

    Trace("====>\n");

    status = XENBUS_DEBUG(Acquire, &Context->DebugInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_DEBUG(Register,
                          &Context->DebugInterface,
                          __MODULE__ "|CACHE",
                          CacheDebugCallback,
                          Context,
                          &Context->DebugCallback);
    if (!NT_SUCCESS(status))
        goto fail2;

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    XENBUS_DEBUG(Release, &Context->DebugInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    --Context->References;
    ASSERT3U(Context->References, ==, 0);
    KeReleaseSpinLock(&Context->Lock, Irql);

    return status;
}

VOID
CacheRelease(
    _In_ PINTERFACE         Interface
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Interface->Context;
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

    Trace("<====\n");

done:
    KeReleaseSpinLock(&Context->Lock, Irql);
}

static struct _XENBUS_CACHE_INTERFACE_V1 CacheInterfaceVersion1 = {
    { sizeof (struct _XENBUS_CACHE_INTERFACE_V1), 1, NULL, NULL, NULL },
    CacheAcquire,
    CacheRelease,
    CacheCreateVersion1,
    CacheGet,
    CachePut,
    CacheDestroy
};

static struct _XENBUS_CACHE_INTERFACE_V2 CacheInterfaceVersion2 = {
    { sizeof (struct _XENBUS_CACHE_INTERFACE_V2), 2, NULL, NULL, NULL },
    CacheAcquire,
    CacheRelease,
    CacheCreate,
    CacheGet,
    CachePut,
    CacheDestroy
};

NTSTATUS
CacheInitialize(
    _In_ PXENBUS_FDO                Fdo,
    _Outptr_ PXENBUS_CACHE_CONTEXT  *Context
    )
{
    NTSTATUS                        status;

    Trace("====>\n");

    *Context = __CacheAllocate(sizeof (XENBUS_CACHE_CONTEXT));

    status = STATUS_NO_MEMORY;
    if (*Context == NULL)
        goto fail1;

    status = DebugGetInterface(FdoGetDebugContext(Fdo),
                               XENBUS_DEBUG_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&(*Context)->DebugInterface,
                               sizeof ((*Context)->DebugInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->DebugInterface.Interface.Context != NULL);

    InitializeListHead(&(*Context)->List);
    KeInitializeSpinLock(&(*Context)->Lock);

    status = ThreadCreate(CacheMonitor, *Context, &(*Context)->MonitorThread);
    if (!NT_SUCCESS(status))
        goto fail2;

    (*Context)->Fdo = Fdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    RtlZeroMemory(&(*Context)->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&(*Context)->List, sizeof (LIST_ENTRY));

    RtlZeroMemory(&(*Context)->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    __CacheFree(*Context);
    *Context = NULL;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
CacheGetInterface(
    _In_ PXENBUS_CACHE_CONTEXT      Context,
    _In_ ULONG                      Version,
    _Inout_ PINTERFACE              Interface,
    _In_ ULONG                      Size
    )
{
    NTSTATUS                        status;

    ASSERT(Context != NULL);

    switch (Version) {
    case 1: {
        struct _XENBUS_CACHE_INTERFACE_V1   *CacheInterface;

        CacheInterface = (struct _XENBUS_CACHE_INTERFACE_V1 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_CACHE_INTERFACE_V1))
            break;

        *CacheInterface = CacheInterfaceVersion1;

        ASSERT3U(Interface->Version, ==, Version);
        Interface->Context = Context;

        status = STATUS_SUCCESS;
        break;
    }
    case 2: {
        struct _XENBUS_CACHE_INTERFACE_V2   *CacheInterface;

        CacheInterface = (struct _XENBUS_CACHE_INTERFACE_V2 *)Interface;

        status = STATUS_BUFFER_OVERFLOW;
        if (Size < sizeof (struct _XENBUS_CACHE_INTERFACE_V2))
            break;

        *CacheInterface = CacheInterfaceVersion2;

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
CacheGetReferences(
    _In_ PXENBUS_CACHE_CONTEXT  Context
    )
{
    return Context->References;
}

VOID
CacheTeardown(
    _In_ PXENBUS_CACHE_CONTEXT  Context
    )
{
    Trace("====>\n");

    Context->Fdo = NULL;

    ThreadAlert(Context->MonitorThread);
    ThreadJoin(Context->MonitorThread);
    Context->MonitorThread = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_CACHE_CONTEXT)));
    __CacheFree(Context);

    Trace("<====\n");
}
