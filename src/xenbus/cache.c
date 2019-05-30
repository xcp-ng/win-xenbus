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

typedef struct _XENBUS_CACHE_SLAB {
    ULONG           Magic;
    PXENBUS_CACHE   Cache;
    LIST_ENTRY      ListEntry;
    ULONG           Count;
    ULONG           Allocated;
    UCHAR           Buffer[1];
} XENBUS_CACHE_SLAB, *PXENBUS_CACHE_SLAB;

#define BITS_PER_ULONG (sizeof (ULONG) * 8)
#define MINIMUM_OBJECT_SIZE (PAGE_SIZE / BITS_PER_ULONG)

C_ASSERT(sizeof (XENBUS_CACHE_SLAB) <= MINIMUM_OBJECT_SIZE);

#define MAXNAMELEN  128

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
    ULONG                   Count;
    PXENBUS_CACHE_MAGAZINE  Magazine;
    ULONG                   MagazineCount;
};

struct _XENBUS_CACHE_CONTEXT {
    PXENBUS_FDO             Fdo;
    KSPIN_LOCK              Lock;
    LONG                    References;
    XENBUS_DEBUG_INTERFACE  DebugInterface;
    PXENBUS_DEBUG_CALLBACK  DebugCallback;
    LIST_ENTRY              List;
};

#define CACHE_TAG   'HCAC'

static FORCEINLINE PVOID
__CacheAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, CACHE_TAG);
}

static FORCEINLINE VOID
__CacheFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, CACHE_TAG);
}

static FORCEINLINE VOID
__drv_requiresIRQL(DISPATCH_LEVEL)
__CacheAcquireLock(
    IN  PXENBUS_CACHE   Cache
    )
{
    Cache->AcquireLock(Cache->Argument);
}

static FORCEINLINE VOID
__drv_requiresIRQL(DISPATCH_LEVEL)
__CacheReleaseLock(
    IN  PXENBUS_CACHE   Cache
    )
{
    Cache->ReleaseLock(Cache->Argument);
}

static FORCEINLINE NTSTATUS
__drv_requiresIRQL(DISPATCH_LEVEL)
__CacheCtor(
    IN  PXENBUS_CACHE   Cache,
    IN  PVOID           Object
    )
{
    return Cache->Ctor(Cache->Argument, Object);
}

static FORCEINLINE VOID
__drv_requiresIRQL(DISPATCH_LEVEL)
__CacheDtor(
    IN  PXENBUS_CACHE   Cache,
    IN  PVOID           Object
    )
{
    Cache->Dtor(Cache->Argument, Object);
}

static PVOID
CacheGetObjectFromMagazine(
    IN  PXENBUS_CACHE_MAGAZINE  Magazine
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
    IN  PXENBUS_CACHE_MAGAZINE  Magazine,
    IN  PVOID                   Object
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

static VOID
CacheInsertSlab(
    IN  PXENBUS_CACHE       Cache,
    IN  PXENBUS_CACHE_SLAB  Slab
    )
{
#define INSERT_BEFORE(_Cursor, _New)            \
        do {                                    \
            (_New)->Blink = (_Cursor)->Blink;   \
            (_Cursor)->Blink->Flink = (_New);   \
                                                \
            (_Cursor)->Blink = (_New);          \
            (_New)->Flink = (_Cursor);          \
        } while (FALSE)

    PLIST_ENTRY Cursor;

    for (Cursor = Cache->SlabList.Flink;
         Cursor != &Cache->SlabList;
         Cursor = Cursor->Flink) {
        PXENBUS_CACHE_SLAB  Next;

        Next = CONTAINING_RECORD(Cursor, XENBUS_CACHE_SLAB, ListEntry);

        if (Next->Allocated > Slab->Allocated) {
            INSERT_BEFORE(Cursor, &Slab->ListEntry);
            return;
        }
    }

    InsertTailList(&Cache->SlabList, &Slab->ListEntry);

#undef  INSERT_BEFORE
}

// Must be called with lock held
static PXENBUS_CACHE_SLAB
CacheCreateSlab(
    IN  PXENBUS_CACHE   Cache
    )
{
    PXENBUS_CACHE_SLAB  Slab;
    ULONG               NumberOfBytes;
    ULONG               Count;
    LARGE_INTEGER       LowAddress;
    LARGE_INTEGER       HighAddress;
    LARGE_INTEGER       Boundary;
    LONG                Index;
    NTSTATUS            status;

    NumberOfBytes = P2ROUNDUP(FIELD_OFFSET(XENBUS_CACHE_SLAB, Buffer) +
                              Cache->Size,
                              PAGE_SIZE);
    Count = (NumberOfBytes - FIELD_OFFSET(XENBUS_CACHE_SLAB, Buffer)) /
            Cache->Size;
    ASSERT(Count != 0);

    status = STATUS_INSUFFICIENT_RESOURCES;
    if (Cache->Count + Count > Cache->Cap)
        goto fail1;

    LowAddress.QuadPart = 0ull;
    HighAddress.QuadPart = ~0ull;
    Boundary.QuadPart = 0ull;

    Slab = MmAllocateContiguousMemorySpecifyCacheNode((SIZE_T)NumberOfBytes,
                                                      LowAddress,
                                                      HighAddress,
                                                      Boundary,
                                                      MmCached,
                                                      MM_ANY_NODE_OK);

    status = STATUS_NO_MEMORY;
    if (Slab == NULL)
        goto fail2;

    RtlZeroMemory(Slab, NumberOfBytes);

    Slab->Magic = XENBUS_CACHE_SLAB_MAGIC;
    Slab->Cache = Cache;
    Slab->Count = Count;

    for (Index = 0; Index < (LONG)Slab->Count; Index++) {
        PVOID Object = (PVOID)&Slab->Buffer[Index * Cache->Size];

        status = __CacheCtor(Cache, Object);
        if (!NT_SUCCESS(status))
            goto fail3;
    }

    CacheInsertSlab(Cache, Slab);
    Cache->Count += Count;

    return Slab;

fail3:
    Error("fail3\n");

    while (--Index >= 0) {
        PVOID Object = (PVOID)&Slab->Buffer[Index * Cache->Size];

        __CacheDtor(Cache, Object);
    }

    MmFreeContiguousMemory(Slab);

fail2:
    Error("fail2\n");

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;
}

// Must be called with lock held
static VOID
CacheDestroySlab(
    IN  PXENBUS_CACHE       Cache,
    IN  PXENBUS_CACHE_SLAB  Slab
    )
{
    LONG                    Index;

    ASSERT3U(Slab->Allocated, ==, 0);

    ASSERT3U(Cache->Count, >=, Slab->Count);
    Cache->Count -= Slab->Count;
    RemoveEntryList(&Slab->ListEntry);

    Index = Slab->Count;
    while (--Index >= 0) {
        PVOID Object = (PVOID)&Slab->Buffer[Index * Cache->Size];

        __CacheDtor(Cache, Object);
    }

    MmFreeContiguousMemory(Slab);
}

// Must be called with lock held
static PVOID
CacheGetObjectFromSlab(
    IN  PXENBUS_CACHE_SLAB  Slab
    )
{
    PXENBUS_CACHE           Cache;
    ULONG                   Free;
    ULONG                   Index;
    ULONG                   Set;

    Cache = Slab->Cache;

    Free = ~Slab->Allocated;
    if (!_BitScanForward(&Index, Free) || Index >= Slab->Count)
        return NULL;

    Set = InterlockedBitTestAndSet((LONG *)&Slab->Allocated, Index);
    ASSERT(!Set);

    return (PVOID)&Slab->Buffer[Index * Cache->Size];
}

// Must be called with lock held
static VOID
CachePutObjectToSlab(
    IN  PXENBUS_CACHE_SLAB  Slab,
    IN  PVOID               Object
    )
{
    PXENBUS_CACHE           Cache;
    ULONG                   Index;

    Cache = Slab->Cache;

    Index = (ULONG)((PUCHAR)Object - &Slab->Buffer[0]) / Cache->Size;
    BUG_ON(Index >= Slab->Count);

    (VOID) InterlockedBitTestAndReset((LONG *)&Slab->Allocated, Index);
}

static PVOID
CacheGet(
    IN  PINTERFACE          Interface,
    IN  PXENBUS_CACHE       Cache,
    IN  BOOLEAN             Locked
    )
{
    KIRQL                   Irql;
    ULONG                   Index;
    PXENBUS_CACHE_MAGAZINE  Magazine;
    PVOID                   Object;
    PLIST_ENTRY             ListEntry;

    UNREFERENCED_PARAMETER(Interface);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Index = KeGetCurrentProcessorNumberEx(NULL);

    ASSERT3U(Index, <, Cache->MagazineCount);
    Magazine = &Cache->Magazine[Index];

    Object = CacheGetObjectFromMagazine(Magazine);
    if (Object != NULL)
        goto done;

    if (!Locked)
        __CacheAcquireLock(Cache);

    for (ListEntry = Cache->SlabList.Flink;
         ListEntry != &Cache->SlabList;
         ListEntry = ListEntry->Flink) {
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        Object = CacheGetObjectFromSlab(Slab);
        if (Object != NULL)
            break;
    }

    if (Object == NULL) {
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CacheCreateSlab(Cache);
        if (Slab != NULL)
            Object = CacheGetObjectFromSlab(Slab);
    }

    if (!Locked)
        __CacheReleaseLock(Cache);

done:
    KeLowerIrql(Irql);

    return Object;
}

static VOID
CachePut(
    IN  PINTERFACE          Interface,
    IN  PXENBUS_CACHE       Cache,
    IN  PVOID               Object,
    IN  BOOLEAN             Locked
    )
{
    KIRQL                   Irql;
    ULONG                   Index;
    PXENBUS_CACHE_MAGAZINE  Magazine;
    PXENBUS_CACHE_SLAB      Slab;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Interface);

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

    if (Slab->Allocated == 0) {
        CacheDestroySlab(Cache, Slab);
    } else {
        /* Re-insert to keep slab list ordered */
        RemoveEntryList(&Slab->ListEntry);
        CacheInsertSlab(Cache, Slab);
    }

    if (!Locked)
        __CacheReleaseLock(Cache);

done:
    KeLowerIrql(Irql);
}

static FORCEINLINE NTSTATUS
__CacheFill(
    IN  PXENBUS_CACHE   Cache
    )
{
    KIRQL               Irql;
    NTSTATUS            status;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __CacheAcquireLock(Cache);

    while (Cache->Count < Cache->Reservation) {
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CacheCreateSlab(Cache);

        status = STATUS_NO_MEMORY;
        if (Slab == NULL)
            goto fail1;
    }

    __CacheReleaseLock(Cache);
    KeLowerIrql(Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    while (!IsListEmpty(&Cache->SlabList)) {
        PLIST_ENTRY         ListEntry = Cache->SlabList.Flink;
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        CacheDestroySlab(Cache, Slab);
    }
    ASSERT3U(Cache->Count, ==, 0);

    __CacheReleaseLock(Cache);
    KeLowerIrql(Irql);

    return status;
}

static FORCEINLINE VOID
__CacheEmpty(
    IN  PXENBUS_CACHE   Cache
    )
{
    KIRQL               Irql;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    __CacheAcquireLock(Cache);

    while (!IsListEmpty(&Cache->SlabList)) {
        PLIST_ENTRY         ListEntry = Cache->SlabList.Flink;
        PXENBUS_CACHE_SLAB  Slab;

        Slab = CONTAINING_RECORD(ListEntry, XENBUS_CACHE_SLAB, ListEntry);

        CacheDestroySlab(Cache, Slab);
    }
    ASSERT3U(Cache->Count, ==, 0);

    __CacheReleaseLock(Cache);
    KeLowerIrql(Irql);
}

static FORCEINLINE VOID
__CacheFlushMagazines(
    IN  PXENBUS_CACHE   Cache
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
    IN  PINTERFACE          Interface,
    IN  const CHAR          *Name,
    IN  ULONG               Size,
    IN  ULONG               Reservation,
    IN  ULONG               Cap,
    IN  NTSTATUS            (*Ctor)(PVOID, PVOID),
    IN  VOID                (*Dtor)(PVOID, PVOID),
    IN  VOID                (*AcquireLock)(PVOID),
    IN  VOID                (*ReleaseLock)(PVOID),
    IN  PVOID               Argument,
    OUT PXENBUS_CACHE       *Cache
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Interface->Context;
    KIRQL                   Irql;
    NTSTATUS                status;

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

    Size = __max(Size, MINIMUM_OBJECT_SIZE);
    Size = P2ROUNDUP(Size, sizeof (ULONG_PTR));

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

    status = STATUS_INVALID_PARAMETER;
    if ((*Cache)->Reservation > (*Cache)->Cap)
        goto fail3;

    if ((*Cache)->Reservation != 0) {
        status = __CacheFill(*Cache);
        if (!NT_SUCCESS(status))
            goto fail4;
    }

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

    __CacheEmpty(*Cache);

fail4:
    Error("fail4\n");

fail3:
    Error("fail3\n");

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
    IN  PINTERFACE          Interface,
    IN  const CHAR          *Name,
    IN  ULONG               Size,
    IN  ULONG               Reservation,
    IN  NTSTATUS            (*Ctor)(PVOID, PVOID),
    IN  VOID                (*Dtor)(PVOID, PVOID),
    IN  VOID                (*AcquireLock)(PVOID),
    IN  VOID                (*ReleaseLock)(PVOID),
    IN  PVOID               Argument,
    OUT PXENBUS_CACHE       *Cache
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
    IN  PINTERFACE          Interface,
    IN  PXENBUS_CACHE       Cache
    )
{
    PXENBUS_CACHE_CONTEXT   Context = Interface->Context;
    KIRQL                   Irql;

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

    __CacheEmpty(Cache);
    Cache->Reservation = 0;

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
    IN  PVOID               Argument,
    IN  BOOLEAN             Crashing
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
                         "- %s: Count = %d (Reservation = %d)\n",
                         Cache->Name,
                         Cache->Count,
                         Cache->Reservation);
        }
    }
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
    IN  PINTERFACE          Interface
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
    IN  PXENBUS_FDO             Fdo,
    OUT PXENBUS_CACHE_CONTEXT   *Context
    )
{
    NTSTATUS                    status;

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

    (*Context)->Fdo = Fdo;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
CacheGetInterface(
    IN      PXENBUS_CACHE_CONTEXT   Context,
    IN      ULONG                   Version,
    IN OUT  PINTERFACE              Interface,
    IN      ULONG                   Size
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
    IN  PXENBUS_CACHE_CONTEXT   Context
    )
{
    return Context->References;
}

VOID
CacheTeardown(
    IN  PXENBUS_CACHE_CONTEXT   Context
    )
{
    Trace("====>\n");

    Context->Fdo = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_CACHE_CONTEXT)));
    __CacheFree(Context);

    Trace("<====\n");
}
