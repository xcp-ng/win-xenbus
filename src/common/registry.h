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

#ifndef _COMMON_REGISTRY_H
#define _COMMON_REGISTRY_H

#include <ntddk.h>

extern NTSTATUS
RegistryInitialize(
    _In_opt_ PDRIVER_OBJECT DrvObj,
    _In_ PUNICODE_STRING    Path
    );

extern VOID
RegistryTeardown(
    VOID
    );

extern NTSTATUS
RegistryOpenParametersKey(
    _In_ ACCESS_MASK    DesiredAccess,
    _Out_ PHANDLE       Key
    );

extern NTSTATUS
RegistryOpenKey(
    _In_opt_ HANDLE         Parent,
    _In_ PUNICODE_STRING    Path,
    _In_ ACCESS_MASK        DesiredAccess,
    _Out_ PHANDLE           Key
    );

extern NTSTATUS
RegistryCreateKey(
    _In_opt_ HANDLE         Parent,
    _In_ PUNICODE_STRING    Path,
    _In_ ULONG              Options,
    _Out_ PHANDLE           Key
    );

extern NTSTATUS
RegistryOpenServiceKey(
    _In_ ACCESS_MASK    DesiredAccess,
    _Out_ PHANDLE       Key
    );

extern NTSTATUS
RegistryCreateServiceKey(
    _Out_ PHANDLE   Key
    );

extern NTSTATUS
RegistryOpenSoftwareKey(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ACCESS_MASK    DesiredAccess,
    _Out_ PHANDLE       Key
    );

extern NTSTATUS
RegistryOpenHardwareKey(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ACCESS_MASK    DesiredAccess,
    _Out_ PHANDLE       Key
    );

extern NTSTATUS
RegistryOpenSubKey(
    _In_opt_ HANDLE     Key,
    _In_ PSTR           Name,
    _In_ ACCESS_MASK    DesiredAccess,
    _Out_ PHANDLE       SubKey
    );

extern NTSTATUS
RegistryCreateSubKey(
    _In_opt_ HANDLE Key,
    _In_ PSTR       Name,
    _In_ ULONG      Options,
    _Out_ PHANDLE   SubKey
    );

extern NTSTATUS
RegistryDeleteSubKey(
    _In_ HANDLE     Key,
    _In_ PSTR       Name
    );

extern NTSTATUS
RegistryEnumerateSubKeys(
    _In_ HANDLE     Key,
    _In_ NTSTATUS   (*Callback)(PVOID, HANDLE, PANSI_STRING),
    _In_ PVOID      Context
    );

extern NTSTATUS
RegistryEnumerateValues(
    _In_ HANDLE     Key,
    _In_ NTSTATUS   (*Callback)(PVOID, HANDLE, PANSI_STRING, ULONG),
    _In_ PVOID      Context
    );

extern NTSTATUS
RegistryDeleteValue(
    _In_ HANDLE     Key,
    _In_ PSTR       Name
    );

extern NTSTATUS
RegistryQueryDwordValue(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _Out_ PULONG        Value
    );

extern NTSTATUS
RegistryQueryQwordValue(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _Out_ PULONGLONG    Value
    );

extern NTSTATUS
RegistryUpdateDwordValue(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _In_ ULONG          Value
    );

extern NTSTATUS
RegistryQuerySzValue(
    _In_ HANDLE             Key,
    _In_ PSTR               Name,
    _Out_opt_ PULONG        Type,
    _Outptr_ PANSI_STRING   *Array
    );

extern NTSTATUS
RegistryQueryBinaryValue(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _Outptr_ PVOID      *Buffer,
    _Out_ PULONG        Length
    );

extern NTSTATUS
RegistryUpdateBinaryValue(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _In_ PVOID          Buffer,
    _In_ ULONG          Length
    );

extern NTSTATUS
RegistryQueryKeyName(
    _In_ HANDLE             Key,
    _Outptr_ PANSI_STRING   *Array
    );

extern NTSTATUS
RegistryQuerySystemStartOption(
    _In_ PSTR               Name,
    _Outptr_ PANSI_STRING   *Option
    );

extern VOID
RegistryFreeSzValue(
    _In_ PANSI_STRING   Array
    );

extern VOID
RegistryFreeBinaryValue(
    _In_ PVOID          Buffer
    );

extern NTSTATUS
RegistryUpdateSzValue(
    _In_ HANDLE         Key,
    _In_ PSTR           Name,
    _In_ ULONG          Type,
    _In_ PANSI_STRING   Array
    );

extern VOID
RegistryCloseKey(
    _In_ HANDLE Key
    );

#endif  // _COMMON_REGISTRY_H
