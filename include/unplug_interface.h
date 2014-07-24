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

/*! \file unplug_interface.h
    \brief XENFILT UNPLUG Interface

    This interface provides a primitive to re-unplug emulated devices,
    which is required on resume-from-suspend
*/

#ifndef _XENFILT_UNPLUG_INTERFACE_H
#define _XENFILT_UNPLUG_INTERFACE_H

#ifndef _WINDLL

/*! \typedef XENFILT_UNPLUG_ACQUIRE
    \brief Acquire a reference to the UNPLUG interface

    \param Interface The interface header
*/  
typedef NTSTATUS
(*XENFILT_UNPLUG_ACQUIRE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENFILT_UNPLUG_RELEASE
    \brief Release a reference to the UNPLUG interface

    \param Interface The interface header
*/  
typedef VOID
(*XENFILT_UNPLUG_RELEASE)(
    IN  PINTERFACE  Interface
    );

/*! \typedef XENFILT_UNPLUG_REPLAY
    \brief Re-unplug emulated devices that were previously unplugged
    at boot time

    \param Interface The interface header
*/  
typedef VOID
(*XENFILT_UNPLUG_REPLAY)(
    IN  PINTERFACE  Interface
    );

// {D5657CFD-3DB5-4A23-A94F-61FD89247FE7}
DEFINE_GUID(GUID_XENFILT_UNPLUG_INTERFACE,
0xd5657cfd, 0x3db5, 0x4a23, 0xa9, 0x4f, 0x61, 0xfd, 0x89, 0x24, 0x7f, 0xe7);

/*! \struct _XENFILT_UNPLUG_INTERFACE_V1
    \brief UNPLUG interface version 1
*/
struct _XENFILT_UNPLUG_INTERFACE_V1 {
    INTERFACE               Interface;
    XENFILT_UNPLUG_ACQUIRE  Acquire;
    XENFILT_UNPLUG_RELEASE  Release;
    XENFILT_UNPLUG_REPLAY   Replay;
};

typedef struct _XENFILT_UNPLUG_INTERFACE_V1 XENFILT_UNPLUG_INTERFACE, *PXENFILT_UNPLUG_INTERFACE;

/*! \def XENFILT_UNPLUG
    \brief Macro at assist in method invocation
*/
#define XENFILT_UNPLUG(_Method, _Interface, ...)    \
    (_Interface)-> ## _Method((PINTERFACE)(_Interface), __VA_ARGS__)

#endif  // _WINDLL

#define XENFILT_UNPLUG_INTERFACE_VERSION_MIN  1
#define XENFILT_UNPLUG_INTERFACE_VERSION_MAX  1

#endif  // _XENFILT_UNPLUG_INTERFACE_H

