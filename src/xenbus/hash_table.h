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

#ifndef _XENBUS_HASH_TABLE_H
#define _XENBUS_HASH_TABLE_H

#include <ntddk.h>

typedef struct _XENBUS_HASH_TABLE XENBUS_HASH_TABLE, *PXENBUS_HASH_TABLE;

extern NTSTATUS
HashTableAdd(
    _In_ PXENBUS_HASH_TABLE Table,
    _In_ ULONG_PTR          Key,
    _In_ ULONG_PTR          Value
    );

extern NTSTATUS
HashTableRemove(
    _In_ PXENBUS_HASH_TABLE Table,
    _In_ ULONG_PTR          Key
    );

extern NTSTATUS
HashTableLookup(
    _In_ PXENBUS_HASH_TABLE Table,
    _In_ ULONG_PTR          Key,
    _Out_ PULONG_PTR        Value
    );

extern NTSTATUS
HashTableCreate(
    _Outptr_ PXENBUS_HASH_TABLE *Table
    );

extern VOID
HashTableDestroy(
    _In_ PXENBUS_HASH_TABLE Table
    );

#endif  // _XENBUS_HASH_TABLE_H
