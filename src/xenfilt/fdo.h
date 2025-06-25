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

#ifndef _XENFILT_FDO_H
#define _XENFILT_FDO_H

#include <ntddk.h>
#include <emulated_interface.h>

#include "driver.h"
#include "types.h"

typedef struct _XENFILT_FDO XENFILT_FDO, *PXENFILT_FDO;

extern NTSTATUS
FdoCreate(
    _In_ PDEVICE_OBJECT                 PhysicalDeviceObject,
    _In_ XENFILT_EMULATED_OBJECT_TYPE   Type
    );

extern VOID
FdoDestroy(
    _In_ PXENFILT_FDO   Fdo
    );

extern PSTR
FdoGetPrefix(
    _In_ PXENFILT_FDO   Fdo
    );

extern VOID
FdoAddPhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PXENFILT_PDO   Pdo
    );

extern VOID
FdoRemovePhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PXENFILT_PDO   Pdo
    );

extern VOID
FdoAcquireMutex(
    _In_ PXENFILT_FDO   Fdo
    );

extern VOID
FdoReleaseMutex(
    _In_ PXENFILT_FDO   Fdo
    );

extern PDEVICE_OBJECT
FdoGetDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    );

extern PDEVICE_OBJECT
FdoGetPhysicalDeviceObject(
    _In_ PXENFILT_FDO   Fdo
    );

extern BOOLEAN
FdoHasEnumerated(
    _In_ PXENFILT_FDO   Fdo
    );

extern NTSTATUS
FdoDispatch(
    _In_ PXENFILT_FDO   Fdo,
    _In_ PIRP           Irp
    );

#endif  // _XENFILT_FDO_H
