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

/*! \file pvdevice_interface.h
    \brief XENFILT PVDEVICE Interface

    This interface provides primitives to determine whether a pvdevice
    device is active (and claim the active device id if not)
*/

#ifndef _XENFILT_PVDEVICE_INTERFACE_H
#define _XENFILT_PVDEVICE_INTERFACE_H

#ifndef _WINDLL

/*! \typedef XENFILT_PVDEVICE_ACQUIRE
    \brief Acquire a reference to the PVDEVICE interface

    \param Interface The interface header
*/
typedef NTSTATUS
(*XENFILT_PVDEVICE_ACQUIRE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENFILT_PVDEVICE_RELEASE
    \brief Release a reference to the PVDEVICE interface

    \param Interface The interface header
*/
typedef VOID
(*XENFILT_PVDEVICE_RELEASE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENFILT_PVDEVICE_GET_ACTIVE
    \brief Get the active device instance

    \param Interface The interface header
    \param DeviceID A buffer of length MAXNAMELEN to receive the device id
    \param InstanceID A buffer of length MAXNAMELEN to receive the instance id
*/
typedef NTSTATUS
(*XENFILT_PVDEVICE_GET_ACTIVE)(
    IN  PVOID   Context,
    OUT PCHAR   DeviceID,
    OUT PCHAR   InstanceID
    );

/*! \typedef XENFILT_PVDEVICE_SET_ACTIVE
    \brief Set the active device instance

    \param Interface The interface header
    \param DeviceID Buffer containing the device id
    \param InstanceID Buffer containing the instance id
*/
typedef NTSTATUS
(*XENFILT_PVDEVICE_SET_ACTIVE)(
    IN  PVOID   Context,
    IN  PCHAR   DeviceID,
    IN  PCHAR   InstanceID
    );

/*! \typedef XENFILT_PVDEVICE_CLEAR_ACTIVE
    \brief Clear the active device instance

    \param Interface The interface header
*/
typedef NTSTATUS
(*XENFILT_PVDEVICE_CLEAR_ACTIVE)(
    IN  PVOID   Context
    );

// {7d09b250-898f-4fea-b7fa-e0490e46f95f}
DEFINE_GUID(GUID_XENFILT_PVDEVICE_INTERFACE,
0x7d09b250, 0x898f, 0x4fea, 0xb7, 0xfa, 0xe0, 0x49, 0x0e, 0x46, 0xf9, 0x5f);

/*! \struct _XENFILT_PVDEVICE_INTERFACE_V1
    \brief PVDEVICE interface version 1
    \ingroup interfaces
*/
struct _XENFILT_PVDEVICE_INTERFACE_V1 {
    INTERFACE                       Interface;
    XENFILT_PVDEVICE_ACQUIRE        PvdeviceAcquire;
    XENFILT_PVDEVICE_RELEASE        PvdeviceRelease;
    XENFILT_PVDEVICE_GET_ACTIVE     PvdeviceGetActive;
    XENFILT_PVDEVICE_SET_ACTIVE     PvdeviceSetActive;
    XENFILT_PVDEVICE_CLEAR_ACTIVE   PvdeviceClearActive;
};

typedef struct _XENFILT_PVDEVICE_INTERFACE_V1 XENFILT_PVDEVICE_INTERFACE, *PXENFILT_PVDEVICE_INTERFACE;

/*! \def XENFILT_PVDEVICE
    \brief Macro at assist in method invocation
*/
#define XENFILT_PVDEVICE(_Method, _Interface, ...)    \
    (_Interface)->Pvdevice ## _Method((PINTERFACE)(_Interface), __VA_ARGS__)

#endif  // _WINDLL

#define XENFILT_PVDEVICE_INTERFACE_VERSION_MIN  1
#define XENFILT_PVDEVICE_INTERFACE_VERSION_MAX  1

#endif  // _XENFILT_PVDEVICE_INTERFACE_H
