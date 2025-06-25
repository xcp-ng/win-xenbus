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

/*! \file unplug_interface.h
    \brief XENBUS UNPLUG Interface

    This interface provides a method to request emulated device unplug
*/

#ifndef _XENBUS_UNPLUG_INTERFACE_H
#define _XENBUS_UNPLUG_INTERFACE_H

#ifndef _WINDLL

/*! \typedef XENBUS_UNPLUG_ACQUIRE
    \brief Acquire a reference to the UNPLUG interface

    \param Interface The interface header
*/
typedef NTSTATUS
(*XENBUS_UNPLUG_ACQUIRE)(
    _In_ PINTERFACE Interface
    );

/*! \typedef XENBUS_UNPLUG_RELEASE
    \brief Release a reference to the UNPLUG interface

    \param Interface The interface header
*/
typedef VOID
(*XENBUS_UNPLUG_RELEASE)(
    _In_ PINTERFACE Interface
    );

/*! \enum _XENBUS_UNPLUG_DEVICE_TYPE
    \brief Type of device to be unplugged
*/
typedef enum _XENBUS_UNPLUG_DEVICE_TYPE {
    XENBUS_UNPLUG_DEVICE_TYPE_INVALID = 0,
    XENBUS_UNPLUG_DEVICE_TYPE_NICS,     /*!< NICs */
    XENBUS_UNPLUG_DEVICE_TYPE_DISKS,    /*!< Disks */
} XENBUS_UNPLUG_DEVICE_TYPE, *PXENBUS_UNPLUG_DEVICE_TYPE;

/*! \typedef XENBUS_UNPLUG_REQUEST
    \brief Request unplug of a type of emulated device

    \param Interface The interface header
    \param Type The type of device
    \param Make Set to TRUE if the request is being made, FALSE if it is
           being revoked.
*/
typedef VOID
(*XENBUS_UNPLUG_REQUEST)(
    _In_ PINTERFACE                 Interface,
    _In_ XENBUS_UNPLUG_DEVICE_TYPE  Type,
    _In_ BOOLEAN                    Make
    );

/*! \typedef XENBUS_UNPLUG_IS_REQUESTED
    \brief Has a type of emulated device been unplugged

    \param Interface The interface header
    \param Type The type of device

    \return TRUE The type of device has been unplugged this boot.
*/
typedef BOOLEAN
(*XENBUS_UNPLUG_IS_REQUESTED)(
    _In_ PINTERFACE                 Interface,
    _In_ XENBUS_UNPLUG_DEVICE_TYPE  Type
    );

/*! \typedef XENBUS_UNPLUG_BOOT_EMULATED
    \brief Should the boot disk be emulated

    \param Interface The interface header
*/
typedef BOOLEAN
(*XENBUS_UNPLUG_BOOT_EMULATED)(
    _In_ PINTERFACE                 Interface
    );

/*! \typedef XENBUS_UNPLUG_REBOOT
    \brief Request a reboot to complete setup

    \param Interface The interface header
    \param Module The module name requesting a reboot
*/
typedef VOID
(*XENBUS_UNPLUG_REBOOT)(
    _In_ PINTERFACE                 Interface,
    _In_ PCHAR                      Module
    );

// {73db6517-3d06-4937-989f-199b7501e229}
DEFINE_GUID(GUID_XENBUS_UNPLUG_INTERFACE,
0x73db6517, 0x3d06, 0x4937, 0x98, 0x9f, 0x19, 0x9b, 0x75, 0x01, 0xe2, 0x29);

/*! \struct _XENBUS_UNPLUG_INTERFACE_V1
    \brief UNPLUG interface version 1
    \ingroup interfaces
*/
struct _XENBUS_UNPLUG_INTERFACE_V1 {
    INTERFACE               Interface;
    XENBUS_UNPLUG_ACQUIRE   UnplugAcquire;
    XENBUS_UNPLUG_RELEASE   UnplugRelease;
    XENBUS_UNPLUG_REQUEST   UnplugRequest;
};

/*! \struct _XENBUS_UNPLUG_INTERFACE_V2
    \brief UNPLUG interface version 2
    \ingroup interfaces
*/
struct _XENBUS_UNPLUG_INTERFACE_V2 {
    INTERFACE                   Interface;
    XENBUS_UNPLUG_ACQUIRE       UnplugAcquire;
    XENBUS_UNPLUG_RELEASE       UnplugRelease;
    XENBUS_UNPLUG_REQUEST       UnplugRequest;
    XENBUS_UNPLUG_IS_REQUESTED  UnplugIsRequested;
};

/*! \struct _XENBUS_UNPLUG_INTERFACE_V3
    \brief UNPLUG interface version 3
    \ingroup interfaces
*/
struct _XENBUS_UNPLUG_INTERFACE_V3 {
    INTERFACE                   Interface;
    XENBUS_UNPLUG_ACQUIRE       UnplugAcquire;
    XENBUS_UNPLUG_RELEASE       UnplugRelease;
    XENBUS_UNPLUG_REQUEST       UnplugRequest;
    XENBUS_UNPLUG_IS_REQUESTED  UnplugIsRequested;
    XENBUS_UNPLUG_BOOT_EMULATED UnplugBootEmulated;
    XENBUS_UNPLUG_REBOOT        UnplugReboot;
};

typedef struct _XENBUS_UNPLUG_INTERFACE_V3 XENBUS_UNPLUG_INTERFACE, *PXENBUS_UNPLUG_INTERFACE;

/*! \def XENBUS_UNPLUG
    \brief Macro at assist in method invocation
*/
#define XENBUS_UNPLUG(_Method, _Interface, ...)    \
    (_Interface)->Unplug ## _Method((PINTERFACE)(_Interface), __VA_ARGS__)

#endif  // _WINDLL

#define XENBUS_UNPLUG_INTERFACE_VERSION_MIN  1
#define XENBUS_UNPLUG_INTERFACE_VERSION_MAX  3

#endif  // _XENBUS_UNPLUG_INTERFACE_H
