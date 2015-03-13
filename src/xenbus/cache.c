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
#include <ntstrsafe.h>
#include <stdlib.h>
#include <util.h>

#include "thread.h"
#include "cache.h"
#include "dbg_print.h"
#include "assert.h"

extern ULONG
NTAPI
RtlRandomEx (
    __inout PULONG Seed
    );

typedef struct _XENBUS_CACHE_OBJECT_HEADER {
    ULONG       Magic;

#define XENBUS_CACHE_OBJECT_HEADER_MAGIC 'EJBO'

    LIST_ENTRY  ListEntry;
} XENBUS_CACHE_OBJECT_HEADER, *PXENBUS_CACHE_OBJECT_HEADER;

#define XENBUS_CACHE_MAGAZINE_SLOTS   6

typedef struct _XENBUS_CACHE_MAGAZINE {
    PVOID   Slot[XENBUS_CACHE_MAGAZINE_SLOTS];
} XENBUS_CACHE_MAGAZINE, *PXENBUS_CACHE_MAGAZINE;

typedef struct _XENBUS_CACHE_FIST {
    LONG    Defer;
    ULONG   Probability;
    ULONG   Seed;
} XENBUS_CACHE_FIST, *PXENBUS_CACHE_FIST;

#define MAXNAMELEN  128

struct _XENBUS_CACHE {
    LIST_ENTRY              ListEntry;
    CHAR                    Name[MAXNAMELEN];
    ULONG                   Size;
    ULONG                   Reservation;
    NTSTATUS                (*Ctor)(PVOID, PVOID);
    VOID                    (*Dtor)(PVOID, PVOID);
    VOID                    (*AcquireLock)(PVOID);
    VOID                    (*ReleaseLock)(PVOID);
    PVOID                   Argument;
    LIST_ENTRY              GetList;
    PLIST_ENTRY             PutList;
    LONG                    Count;
    XENBUS_CACHE_MAGAZINE   Magazine[MAXIMUM_PROCESSORS];
    XENBUS_CACHE_FIST       FIST;
};

struct _XENBUS_CACHE_CONTEXT {
    PXENBUS_FDO             Fdo;
    KSPIN_LOCK              Lock;
    LONG                    References;
    XENBUS_DEBUG_INTERFACE  DebugInterface;
    PXENBUS_DEBUG_CALLBACK  DebugCallback;
    XENBUS_STORE_INTERFACE  StoreInterface;
    PXENBUS_THREAD          MonitorThread;
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
    ExFreePoolWithTag(Buffer, CACHE_TAG);
}

static VOID
CacheSwizzle(
    IN  PXENBUS_CACHE   Cache
    )
{
    PLIST_ENTRY         List;

    List = InterlockedExchangePointer(&Cache->PutList, NULL);

    // Not really a doubly-linked list; it's actually a singly-linked
    // list via the Flink field.
    while (List != NULL) {
        PLIST_ENTRY                 Next;
        PXENBUS_CACHE_OBJECT_HEADER Header;

        Next = List->Flink;
        List->Flink = NULL;
        ASSERT3P(List->Blink, ==, NULL);

        Header = CONTAINING_RECORD(List,
                                   XENBUS_CACHE_OBJECT_HEADER,
                                   ListEntry);
        ASSERT3U(Header->Magic, ==, XENBUS_CACHE_OBJECT_HEADER_MAGIC);

        InsertTailList(&Cache->GetList, &Header->ListEntry);

        List = Next;
    }
}

static PVOID
CacheCreateObject(
    IN  PXENBUS_CACHE           Cache
    )
{
    PXENBUS_CACHE_OBJECT_HEADER Header;
    PVOID                       Object;
    NTSTATUS                    status;

    Header = __CacheAllocate(sizeof (XENBUS_CACHE_OBJECT_HEADER) +
                             Cache->Size);

    status = STATUS_NO_MEMORY;
    if (Header == NULL)
        goto fail1;

    Header->Magic = XENBUS_CACHE_OBJECT_HEADER_MAGIC;

    Object = Header + 1;

    status = Cache->Ctor(Cache->Argument, Object);
    if (!NT_SUCCESS(status))
        goto fail2;

    return Object;

fail2:
    Error("fail2\n");

    Header->Magic = 0;

    ASSERT(IsZeroMemory(Header, sizeof (XENBUS_CACHE_OBJECT_HEADER)));
    __CacheFree(Header);

fail1:
    Error("fail1 (%08x)\n", status);

    return NULL;    
}

static FORCEINLINE
__drv_savesIRQL
__drv_raisesIRQL(DISPATCH_LEVEL)
KIRQL
__CacheAcquireLock(
    IN  PXENBUS_CACHE   Cache
    )
{
    KIRQL               Irql;

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Cache->AcquireLock(Cache->Argument);

    return Irql;
}

static FORCEINLINE VOID
__drv_requiresIRQL(DISPATCH_LEVEL)
__CacheReleaseLock(
    IN  PXENBUS_CACHE               Cache,
    IN  __drv_restoresIRQL KIRQL    Irql
    )
{
    Cache->ReleaseLock(Cache->Argument);
    KeLowerIrql(Irql);
}

static PVOID
CacheGetObjectFromList(
    IN  PXENBUS_CACHE           Cache,
    IN  BOOLEAN                 Locked
    )
{
    LONG                        Count;
    PLIST_ENTRY                 ListEntry;
    PXENBUS_CACHE_OBJECT_HEADER Header;
    PVOID                       Object;
    KIRQL                       Irql = PASSIVE_LEVEL;
    NTSTATUS                    status;

    Count = InterlockedDecrement(&Cache->Count);

    status = STATUS_NO_MEMORY;
    if (Count < 0)
        goto fail1;

    if (!Locked)
        Irql = __CacheAcquireLock(Cache);

    if (IsListEmpty(&Cache->GetList))
        CacheSwizzle(Cache);

    ListEntry = RemoveHeadList(&Cache->GetList);
    ASSERT(ListEntry != &Cache->GetList);

    if (!Locked)
        __CacheReleaseLock(Cache, Irql);

    RtlZeroMemory(ListEntry, sizeof (LIST_ENTRY));

    Header = CONTAINING_RECORD(ListEntry,
                               XENBUS_CACHE_OBJECT_HEADER,
                               ListEntry);
    ASSERT3U(Header->Magic, ==, XENBUS_CACHE_OBJECT_HEADER_MAGIC);

    Object = Header + 1;

    return Object;

fail1:
    (VOID) InterlockedIncrement(&Cache->Count);

    return NULL;    
}

static VOID
CachePutObjectToList(
    IN  PXENBUS_CACHE           Cache,
    IN  PVOID                   Object,
    IN  BOOLEAN                 Locked
    )
{
    PXENBUS_CACHE_OBJECT_HEADER Header;
    PLIST_ENTRY                 Old;
    PLIST_ENTRY                 New;

    ASSERT(Object != NULL);

    Header = Object;
    --Header;
    ASSERT3U(Header->Magic, ==, XENBUS_CACHE_OBJECT_HEADER_MAGIC);

    ASSERT(IsZeroMemory(&Header->ListEntry, sizeof (LIST_ENTRY)));

    if (!Locked) {
        New = &Header->ListEntry;

        do {
            Old = Cache->PutList;
            New->Flink = Old;
        } while (InterlockedCompareExchangePointer(&Cache->PutList,
                                                   New,
                                                   Old) != Old);
    } else {
        InsertTailList(&Cache->GetList, &Header->ListEntry);
    }

    KeMemoryBarrier();

    (VOID) InterlockedIncrement(&Cache->Count);
}

static PVOID
CacheGetObjectFromMagazine(
    IN  PXENBUS_CACHE       Cache,
    IN  ULONG               Cpu
    )
{
    PXENBUS_CACHE_MAGAZINE  Magazine;
    ULONG                   Index;

    Magazine = &Cache->Magazine[Cpu];

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

static BOOLEAN
CachePutObjectToMagazine(
    IN  PXENBUS_CACHE       Cache,
    IN  ULONG               Cpu,
    IN  PVOID               Object
    )
{
    PXENBUS_CACHE_MAGAZINE  Magazine;
    ULONG                   Index;

    Magazine = &Cache->Magazine[Cpu];

    for (Index = 0; Index < XENBUS_CACHE_MAGAZINE_SLOTS; Index++) {
        if (Magazine->Slot[Index] == NULL) {
            Magazine->Slot[Index] = Object;
            return TRUE;
        }
    }

    return FALSE;
}

static PVOID
CacheGet(
    IN  PINTERFACE      Interface,
    IN  PXENBUS_CACHE   Cache,
    IN  BOOLEAN         Locked
    )
{
    KIRQL               Irql;
    ULONG               Cpu;
    PVOID               Object;

    UNREFERENCED_PARAMETER(Interface);

    if (Cache->FIST.Probability != 0) {
        LONG    Defer;

        Defer = InterlockedDecrement(&Cache->FIST.Defer);

        if (Defer <= 0) {
            ULONG   Random = RtlRandomEx(&Cache->FIST.Seed);
            ULONG   Threshold = (MAXLONG / 100) * Cache->FIST.Probability;

            if (Random < Threshold)
                return NULL;
        }
    }

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Cpu = KeGetCurrentProcessorNumber();

    Object = CacheGetObjectFromMagazine(Cache, Cpu);
    if (Object != NULL)
        goto done;

    Object = CacheGetObjectFromList(Cache, Locked);
    if (Object != NULL)
        goto done;

    Object = CacheCreateObject(Cache);

done:
    KeLowerIrql(Irql);

    return Object;
}

static VOID
CachePut(
    IN  PINTERFACE      Interface,
    IN  PXENBUS_CACHE   Cache,
    IN  PVOID           Object,
    IN  BOOLEAN         Locked
    )
{
    KIRQL               Irql;
    ULONG               Cpu;

    UNREFERENCED_PARAMETER(Interface);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);
    Cpu = KeGetCurrentProcessorNumber();

    if (CachePutObjectToMagazine(Cache, Cpu, Object))
        goto done;

    CachePutObjectToList(Cache, Object, Locked);

done:
    KeLowerIrql(Irql);
}

static FORCEINLINE VOID
CacheFlushMagazines(
    IN  PXENBUS_CACHE   Cache
    )
{
    ULONG               Cpu;

    for (Cpu = 0; Cpu < MAXIMUM_PROCESSORS; Cpu++) {
        PVOID   Object;

        while ((Object = CacheGetObjectFromMagazine(Cache, Cpu)) != NULL)
            CachePutObjectToList(Cache, Object, TRUE);
    }
}

static VOID
CacheDestroyObject(
    IN  PXENBUS_CACHE           Cache,
    IN  PVOID                   Object
    )
{
    PXENBUS_CACHE_OBJECT_HEADER Header;

    Header = Object;
    --Header;
    ASSERT3U(Header->Magic, ==, XENBUS_CACHE_OBJECT_HEADER_MAGIC);

    Cache->Dtor(Cache->Argument, Object);

    Header->Magic = 0;

    ASSERT(IsZeroMemory(Header, sizeof (XENBUS_CACHE_OBJECT_HEADER)));
    __CacheFree(Header);
}

static NTSTATUS
CacheGetFISTEntries(
    IN  PXENBUS_CACHE_CONTEXT   Context,
    IN  PXENBUS_CACHE           Cache
    )
{
    CHAR                        Node[sizeof ("FIST/cache/") + MAXNAMELEN];
    PCHAR                       Buffer;
    LARGE_INTEGER               Now;
    NTSTATUS                    status;

    status = XENBUS_STORE(Acquire, &Context->StoreInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = RtlStringCbPrintfA(Node,
                                sizeof (Node),
                                "FIST/cache/%s",
                                Cache->Name);
    ASSERT(NT_SUCCESS(status));

    status = XENBUS_STORE(Read,
                          &Context->StoreInterface,
                          NULL,
                          Node,
                          "defer",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Cache->FIST.Defer = 0;
    } else {
        Cache->FIST.Defer = (ULONG)strtol(Buffer, NULL, 0);

        XENBUS_STORE(Free,
                     &Context->StoreInterface,
                     Buffer);
    }

    status = XENBUS_STORE(Read,
                          &Context->StoreInterface,
                          NULL,
                          Node,
                          "probability",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Cache->FIST.Probability = 0;
    } else {
        Cache->FIST.Probability = (ULONG)strtol(Buffer, NULL, 0);

        XENBUS_STORE(Free,
                     &Context->StoreInterface,
                     Buffer);
    }

    if (Cache->FIST.Probability > 100)
        Cache->FIST.Probability = 100;

    if (Cache->FIST.Probability != 0)
        Info("%s: Defer = %d Probability = %d\n",
             Cache->Name,
             Cache->FIST.Defer,
             Cache->FIST.Probability);

    KeQuerySystemTime(&Now);
    Cache->FIST.Seed = Now.LowPart;

    XENBUS_STORE(Release, &Context->StoreInterface);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;    
}

static NTSTATUS
CacheFill(
    IN  PXENBUS_CACHE   Cache,
    IN  ULONG           Count
    )
{
    while (Count != 0) {
        PVOID   Object = CacheCreateObject(Cache);

        if (Object == NULL)
            break;

        CachePutObjectToList(Cache, Object, FALSE);
        --Count;
    }

    return (Count == 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static VOID
CacheSpill(
    IN  PXENBUS_CACHE   Cache,
    IN  ULONG           Count
    )
{
    while (Count != 0) {
        PVOID   Object = CacheGetObjectFromList(Cache, FALSE);

        if (Object == NULL)
            break;

        CacheDestroyObject(Cache, Object);
        --Count;
    }
}

static NTSTATUS
CacheCreate(
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

    (*Cache)->Size = Size;
    (*Cache)->Ctor = Ctor;
    (*Cache)->Dtor = Dtor;
    (*Cache)->AcquireLock = AcquireLock;
    (*Cache)->ReleaseLock = ReleaseLock;
    (*Cache)->Argument = Argument;

    status = CacheGetFISTEntries(Context, *Cache);
    if (!NT_SUCCESS(status))
        goto fail3;

    InitializeListHead(&(*Cache)->GetList);

    status =  CacheFill(*Cache, Reservation);
    if (!NT_SUCCESS(status))
        goto fail4;

    (*Cache)->Reservation = Reservation;

    KeAcquireSpinLock(&Context->Lock, &Irql);
    InsertTailList(&Context->List, &(*Cache)->ListEntry);
    KeReleaseSpinLock(&Context->Lock, Irql);

    Trace("<====\n");

    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");

    RtlZeroMemory(&(*Cache)->GetList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&(*Cache)->FIST, sizeof (XENBUS_CACHE_FIST));

fail3:
    Error("fail3\n");

    (*Cache)->Argument = NULL;
    (*Cache)->ReleaseLock = NULL;
    (*Cache)->AcquireLock = NULL;
    (*Cache)->Dtor = NULL;
    (*Cache)->Ctor = NULL;
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

    Cache->Reservation = 0;
    CacheFlushMagazines(Cache);
    CacheSpill(Cache, Cache->Count);

    ASSERT3U(Cache->Count, ==, 0);

    RtlZeroMemory(&Cache->GetList, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Cache->FIST, sizeof (XENBUS_CACHE_FIST));

    Cache->Argument = NULL;
    Cache->ReleaseLock = NULL;
    Cache->AcquireLock = NULL;
    Cache->Dtor = NULL;
    Cache->Ctor = NULL;
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

#define TIME_US(_us)        ((_us) * 10)
#define TIME_MS(_ms)        (TIME_US((_ms) * 1000))
#define TIME_S(_s)          (TIME_MS((_s) * 1000))
#define TIME_RELATIVE(_t)   (-(_t))

#define XENBUS_CACHE_MONITOR_PERIOD 5

static NTSTATUS
CacheMonitor(
    IN  PXENBUS_THREAD      Self,
    IN  PVOID               _Context
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
            ULONG           Count;

            Cache = CONTAINING_RECORD(ListEntry, XENBUS_CACHE, ListEntry);

            Count = Cache->Count;

            if (Count < Cache->Reservation)
                CacheFill(Cache, Cache->Reservation - Count);
            else if (Count > Cache->Reservation)
                CacheSpill(Cache, (Count - Cache->Reservation + 1) / 2);
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

    status = StoreGetInterface(FdoGetStoreContext(Fdo),
                               XENBUS_STORE_INTERFACE_VERSION_MAX,
                               (PINTERFACE)&(*Context)->StoreInterface,
                               sizeof ((*Context)->StoreInterface));
    ASSERT(NT_SUCCESS(status));
    ASSERT((*Context)->StoreInterface.Interface.Context != NULL);

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

    RtlZeroMemory(&(*Context)->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&(*Context)->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

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
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    return status;
}   

VOID
CacheTeardown(
    IN  PXENBUS_CACHE_CONTEXT   Context
    )
{
    Trace("====>\n");

    Context->Fdo = NULL;

    ThreadAlert(Context->MonitorThread);
    ThreadJoin(Context->MonitorThread);
    Context->MonitorThread = NULL;

    RtlZeroMemory(&Context->Lock, sizeof (KSPIN_LOCK));
    RtlZeroMemory(&Context->List, sizeof (LIST_ENTRY));

    RtlZeroMemory(&Context->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Context->DebugInterface,
                  sizeof (XENBUS_DEBUG_INTERFACE));

    ASSERT(IsZeroMemory(Context, sizeof (XENBUS_CACHE_CONTEXT)));
    __CacheFree(Context);

    Trace("<====\n");
}
