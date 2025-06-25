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

#ifndef _XENFILT_PDO_H
#define _XENFILT_PDO_H

#include <ntddk.h>
#include <emulated_interface.h>

#include "driver.h"
#include "types.h"

typedef struct _XENFILT_PDO XENFILT_PDO, *PXENFILT_PDO;

extern VOID
PdoSetDevicePnpState(
    _In_ PXENFILT_PDO       Pdo,
    _In_ DEVICE_PNP_STATE   State
    );

extern DEVICE_PNP_STATE
PdoGetDevicePnpState(
    _In_ PXENFILT_PDO   Pdo
    );

extern PDEVICE_OBJECT
PdoGetPhysicalDeviceObject(
    _In_ PXENFILT_PDO   Pdo
    );

extern BOOLEAN
PdoIsMissing(
    _In_ PXENFILT_PDO   Pdo
    );

extern VOID
PdoSetMissing(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PCSTR          Reason
    );

extern BOOLEAN
PdoIsMasked(
    _In_ PXENFILT_PDO   Pdo
    );

extern PDEVICE_OBJECT
PdoGetDeviceObject(
    _In_ PXENFILT_PDO   Pdo
    );

extern NTSTATUS
PdoCreate(
    _In_ PXENFILT_FDO                   Fdo,
    _In_ PDEVICE_OBJECT                 PhysicalDeviceObject,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type
    );

extern VOID
PdoResume(
    _In_ PXENFILT_PDO   Pdo
    );

extern VOID
PdoSuspend(
    _In_ PXENFILT_PDO   Pdo
    );

extern VOID
PdoDestroy(
    _In_ PXENFILT_PDO   Pdo
    );

extern NTSTATUS
PdoDispatch(
    _In_ PXENFILT_PDO   Pdo,
    _In_ PIRP           Irp
    );

#endif  // _XENFILT_PDO_H
