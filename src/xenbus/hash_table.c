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
#include <stdarg.h>
#include <xen.h>
#include <util.h>

#include "hash_table.h"
#include "dbg_print.h"
#include "assert.h"

typedef struct _XENBUS_HASH_TABLE_NODE {
    LIST_ENTRY  ListEntry;
    ULONG_PTR   Key;
    ULONG_PTR   Value;
} XENBUS_HASH_TABLE_NODE, *PXENBUS_HASH_TABLE_NODE;

typedef struct _XENBUS_HASH_TABLE_BUCKET {
    LONG        Lock;
    LIST_ENTRY  List;
} XENBUS_HASH_TABLE_BUCKET, *PXENBUS_HASH_TABLE_BUCKET;

#define XENBUS_HASH_TABLE_NR_BUCKETS \
    (1 << (sizeof (UCHAR) * 8))

struct _XENBUS_HASH_TABLE {
    XENBUS_HASH_TABLE_BUCKET    Bucket[XENBUS_HASH_TABLE_NR_BUCKETS];
};

#define XENBUS_HASH_TABLE_TAG   'HSAH'

static FORCEINLINE PVOID
__HashTableAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, XENBUS_HASH_TABLE_TAG);
}

static FORCEINLINE VOID
__HashTableFree(
    IN  PVOID   Buffer
    )
{
    ExFreePoolWithTag(Buffer, XENBUS_HASH_TABLE_TAG);
}

static ULONG
HashTableHash(
    IN  ULONG_PTR   Key
    )
{
    PUCHAR          Array = (PUCHAR)&Key;
    ULONG           Accumulator;
    ULONG           Index;

    Accumulator = 0;

    for (Index = 0; Index < sizeof (ULONG_PTR); Index++) {
        ULONG   Overflow;

        Accumulator = (Accumulator << 4) + Array[Index];

        Overflow = Accumulator & 0x00000f00;
        if (Overflow != 0) {
            Accumulator ^= Overflow >> 8;
            Accumulator ^= Overflow;
        }
    }

    ASSERT3U(Accumulator, <, XENBUS_HASH_TABLE_NR_BUCKETS);

    return Accumulator;
}

static
_IRQL_requires_max_(HIGH_LEVEL)
_IRQL_saves_
_IRQL_raises_(HIGH_LEVEL)
KIRQL
__HashTableBucketLock(
    IN  PXENBUS_HASH_TABLE_BUCKET   Bucket,
    IN  BOOLEAN                     Writer
    )
{
    KIRQL                           Irql;

    KeRaiseIrql(HIGH_LEVEL, &Irql);

    for (;;) {
        LONG    Lock;
        LONG    Readers;
        LONG    Writers;
        LONG    Old;
        LONG    New;

        KeMemoryBarrier();

        Lock = Bucket->Lock;
        Readers = Lock >> 1;
        Writers = Lock & 1;

        // There must be no existing writer
        Old = Readers << 1;

        if (Writer) 
            Writers++;
        else
            Readers++;

        New = (Readers << 1) | (Writers & 1);

        if (InterlockedCompareExchange(&Bucket->Lock, New, Old) != Old)
            continue;

        //
        // We are done if we're not a writer, or there are no readers
        // left.
        //
        if (!Writer || Readers == 0)
            break;
    }

    return Irql;
}

#define HashTableBucketLock(_Bucket, _Writer, _Irql)            \
    do {                                                        \
        *(_Irql) = __HashTableBucketLock((_Bucket), (_Writer)); \
    } while (FALSE)

static
__drv_requiresIRQL(HIGH_LEVEL)
VOID
HashTableBucketUnlock(
    IN  PXENBUS_HASH_TABLE_BUCKET   Bucket,
    IN  BOOLEAN                     Writer,
    IN  __drv_restoresIRQL KIRQL    Irql
    )
{
    for (;;) {
        LONG    Lock;
        LONG    Readers;
        LONG    Writers;
        LONG    Old;
        LONG    New;

        KeMemoryBarrier();

        Lock = Bucket->Lock;
        Readers = Lock >> 1;
        Writers = Lock & 1;

        Old = (Readers << 1) | (Writers & 1);

        if (Writer) {
            ASSERT(Writers != 0);
            --Writers;
        } else {
            --Readers;
        }

        New = (Readers << 1) | (Writers & 1);

        if (InterlockedCompareExchange(&Bucket->Lock, New, Old) == Old)
            break;
    }

    KeLowerIrql(Irql);
}

NTSTATUS
HashTableAdd(
    IN  PXENBUS_HASH_TABLE      Table,
    IN  ULONG_PTR               Key,
    IN  ULONG_PTR               Value
    )
{
    PXENBUS_HASH_TABLE_NODE     Node;
    PXENBUS_HASH_TABLE_BUCKET   Bucket;
    KIRQL                       Irql;
    NTSTATUS                    status;

    Node = __HashTableAllocate(sizeof (XENBUS_HASH_TABLE_NODE));

    status = STATUS_NO_MEMORY;
    if (Node == NULL)
        goto fail1;

    Node->Key = Key;
    Node->Value = Value;

    Bucket = &Table->Bucket[HashTableHash(Key)];
    
    HashTableBucketLock(Bucket, TRUE, &Irql);
    InsertTailList(&Bucket->List, &Node->ListEntry);
    HashTableBucketUnlock(Bucket, TRUE, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
HashTableRemove(
    IN  PXENBUS_HASH_TABLE      Table,
    IN  ULONG_PTR               Key
    )
{
    PXENBUS_HASH_TABLE_BUCKET   Bucket;
    PLIST_ENTRY                 ListEntry;
    PXENBUS_HASH_TABLE_NODE     Node;
    KIRQL                       Irql;
    NTSTATUS                    status;

    Bucket = &Table->Bucket[HashTableHash(Key)];
    
    HashTableBucketLock(Bucket, TRUE, &Irql);

    ListEntry = Bucket->List.Flink;
    while (ListEntry != &Bucket->List) {
        Node = CONTAINING_RECORD(ListEntry, XENBUS_HASH_TABLE_NODE, ListEntry);

        if (Node->Key == Key)
            goto found;
    }

    HashTableBucketUnlock(Bucket, TRUE, Irql);

    status = STATUS_OBJECT_NAME_NOT_FOUND;
    goto fail1;

found:
    RemoveEntryList(ListEntry);

    HashTableBucketUnlock(Bucket, TRUE, Irql);

    __HashTableFree(Node);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
HashTableLookup(
    IN  PXENBUS_HASH_TABLE      Table,
    IN  ULONG_PTR               Key,
    OUT PULONG_PTR              Value
    )
{
    PXENBUS_HASH_TABLE_BUCKET   Bucket;
    PLIST_ENTRY                 ListEntry;
    PXENBUS_HASH_TABLE_NODE     Node;
    KIRQL                       Irql;
    NTSTATUS                    status;

    Bucket = &Table->Bucket[HashTableHash(Key)];
    
    HashTableBucketLock(Bucket, FALSE, &Irql);

    ListEntry = Bucket->List.Flink;
    while (ListEntry != &Bucket->List) {
        Node = CONTAINING_RECORD(ListEntry, XENBUS_HASH_TABLE_NODE, ListEntry);

        if (Node->Key == Key)
            goto found;
    }

    HashTableBucketUnlock(Bucket, FALSE, Irql);

    status = STATUS_OBJECT_NAME_NOT_FOUND;
    goto fail1;

found:
    *Value = Node->Value;

    HashTableBucketUnlock(Bucket, FALSE, Irql);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

NTSTATUS
HashTableCreate(
    OUT PXENBUS_HASH_TABLE  *Table
    )
{
    ULONG                   Index;
    NTSTATUS                status;

    *Table = __HashTableAllocate(sizeof (XENBUS_HASH_TABLE));

    status = STATUS_NO_MEMORY;
    if (*Table == NULL)
        goto fail1;

    for (Index = 0; Index < XENBUS_HASH_TABLE_NR_BUCKETS; Index++) {
        PXENBUS_HASH_TABLE_BUCKET   Bucket = &(*Table)->Bucket[Index];

        InitializeListHead(&Bucket->List);
    }

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
HashTableDestroy(
    IN  PXENBUS_HASH_TABLE  Table
    )
{
    ULONG                   Index;

    for (Index = 0; Index < XENBUS_HASH_TABLE_NR_BUCKETS; Index++) {
        PXENBUS_HASH_TABLE_BUCKET   Bucket = &Table->Bucket[Index];

        ASSERT(IsListEmpty(&Bucket->List));
        RtlZeroMemory(&Bucket->List, sizeof (LIST_ENTRY));
    }

    ASSERT(IsZeroMemory(Table, sizeof (XENBUS_HASH_TABLE)));
    __HashTableFree(Table);
}

