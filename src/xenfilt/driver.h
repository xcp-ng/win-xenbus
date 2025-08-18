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

#ifndef _XENFILT_DRIVER_H
#define _XENFILT_DRIVER_H

extern PDRIVER_OBJECT
DriverGetDriverObject(
    VOID
    );

extern VOID
DriverAcquireMutex(
    VOID
     );

extern VOID
DriverReleaseMutex(
    VOID
     );

extern NTSTATUS
DriverGetActive(
    _In_ PCSTR              Key,
    _Outptr_result_z_ PSTR  *Value
    );

_On_failure_(_Post_satisfies_(*Precedence == 0))
extern NTSTATUS
DriverGetPrecedence(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _Out_ PULONG        Precedence
    );

typedef enum _XENFILT_FILTER_STATE {
    XENFILT_FILTER_ENABLED = 0,
    XENFILT_FILTER_PENDING,
    XENFILT_FILTER_DISABLED
} XENFILT_FILTER_STATE, *PXENFILT_FILTER_STATE;

VOID
DriverSetFilterState(
    VOID
    );

XENFILT_FILTER_STATE
DriverGetFilterState(
    VOID
    );

extern NTSTATUS
DriverQueryId(
    _In_ PDEVICE_OBJECT     PhysicalDeviceObject,
    _In_ BUS_QUERY_ID_TYPE  Type,
    _Outptr_result_z_ PSTR  *Id
    );

extern NTSTATUS
DriverQueryDeviceText(
    _In_ PDEVICE_OBJECT     LowerDeviceObject,
    _In_ DEVICE_TEXT_TYPE   Type,
    _Outptr_result_z_ PSTR  *Text
    );

#include "emulated.h"

PXENFILT_EMULATED_CONTEXT
DriverGetEmulatedContext(
    VOID
    );

typedef struct _XENFILT_FDO XENFILT_FDO, *PXENFILT_FDO;
typedef struct _XENFILT_PDO XENFILT_PDO, *PXENFILT_PDO;

#include "pdo.h"
#include "fdo.h"

extern VOID
DriverAddFunctionDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    );

extern VOID
DriverRemoveFunctionDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    );

#pragma warning(push)
#pragma warning(disable:4201) // nonstandard extension used : nameless struct/union

typedef struct _XENFILT_DX {
    PDEVICE_OBJECT      DeviceObject;
    DEVICE_OBJECT_TYPE  Type;

    DEVICE_PNP_STATE    DevicePnpState;
    DEVICE_PNP_STATE    PreviousDevicePnpState;

    SYSTEM_POWER_STATE  SystemPowerState;
    DEVICE_POWER_STATE  DevicePowerState;

    PSTR                DeviceID;
    PSTR                InstanceID;
    PSTR                LocationInformation;

    IO_REMOVE_LOCK      RemoveLock;

    LIST_ENTRY          ListEntry;

    union {
        PXENFILT_FDO    Fdo;
        PXENFILT_PDO    Pdo;
    };
} XENFILT_DX, *PXENFILT_DX;

#pragma warning(pop)

#endif  // _XENFILT_DRIVER_H
