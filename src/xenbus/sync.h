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

#ifndef _XENBUS_SYNC_H
#define _XENBUS_SYNC_H

#include <ntddk.h>

typedef VOID
(*SYNC_CALLBACK)(
    _In_ PVOID  Arguement,
    _In_ ULONG  Cpu
    );

extern
_IRQL_requires_max_(DISPATCH_LEVEL)
_IRQL_raises_(DISPATCH_LEVEL)
VOID
SyncCapture(
    _In_opt_ PVOID          Argument,
    _In_opt_ SYNC_CALLBACK  Early,
    _In_opt_ SYNC_CALLBACK  Late
    );

extern
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_raises_(HIGH_LEVEL)
VOID
SyncDisableInterrupts(
    _At_(*Irql, _IRQL_saves_)
    _Out_ PKIRQL    Irql
    );

extern
_IRQL_requires_(HIGH_LEVEL)
VOID
SyncRunEarly(
    VOID
    );

extern
_IRQL_requires_(HIGH_LEVEL)
VOID
SyncEnableInterrupts(
    _In_ _IRQL_restores_ KIRQL  Irql
    );

extern
_IRQL_requires_(DISPATCH_LEVEL)
VOID
SyncRunLate(
    VOID
    );

extern
_IRQL_requires_(DISPATCH_LEVEL)
VOID
SyncRelease(
    VOID
    );

#endif  // _XENBUS_SYNC_H
