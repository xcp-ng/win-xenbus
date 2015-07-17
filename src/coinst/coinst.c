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

#define INITGUID

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <stdio.h>
#include <stdlib.h>
#include <strsafe.h>
#include <malloc.h>
#include <assert.h>

#include <version.h>
#include <revision.h>

__user_code;

#define MAXIMUM_BUFFER_SIZE 1024

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Driver)    \
        SERVICES_KEY ## "\\" ## #_Driver

#define PARAMETERS_KEY(_Driver) \
        SERVICE_KEY(_Driver) ## "\\Parameters"

#define CONTROL_KEY "SYSTEM\\CurrentControlSet\\Control"

#define CLASS_KEY   \
        CONTROL_KEY ## "\\Class"

#define ENUM_KEY    "SYSTEM\\CurrentControlSet\\Enum"

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    IN  const CHAR  *Format,
    IN  ...
    )
{
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    SP_LOG_TOKEN        LogToken;
    DWORD               Category;
    DWORD               Flags;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer, MAXIMUM_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    LogToken = SetupGetThreadLogToken();
    Category = TXTLOG_VENDOR;
    Flags = TXTLOG_DETAILS;

    SetupWriteTextLog(LogToken, Category, Flags, Buffer);
    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    __analysis_assume(Length < MAXIMUM_BUFFER_SIZE);
    __analysis_assume(Length >= 2);
    Buffer[Length] = '\0';
    Buffer[Length - 1] = '\n';
    Buffer[Length - 2] = '\r';

    OutputDebugString(Buffer);
}

#define Log(_Format, ...) \
        __Log(__MODULE__ "|" __FUNCTION__ ": " _Format, __VA_ARGS__)

static PTCHAR
GetErrorMessage(
    IN  DWORD   Error
    )
{
    PTCHAR      Message;
    ULONG       Index;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  Error,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&Message,
                  0,
                  NULL);

    for (Index = 0; Message[Index] != '\0'; Index++) {
        if (Message[Index] == '\r' || Message[Index] == '\n') {
            Message[Index] = '\0';
            break;
        }
    }

    return Message;
}

static FORCEINLINE const CHAR *
__FunctionName(
    IN  DI_FUNCTION Function
    )
{
#define _NAME(_Function)        \
        case DIF_ ## _Function: \
            return #_Function;

    switch (Function) {
    _NAME(INSTALLDEVICE);
    _NAME(REMOVE);
    _NAME(SELECTDEVICE);
    _NAME(ASSIGNRESOURCES);
    _NAME(PROPERTIES);
    _NAME(FIRSTTIMESETUP);
    _NAME(FOUNDDEVICE);
    _NAME(SELECTCLASSDRIVERS);
    _NAME(VALIDATECLASSDRIVERS);
    _NAME(INSTALLCLASSDRIVERS);
    _NAME(CALCDISKSPACE);
    _NAME(DESTROYPRIVATEDATA);
    _NAME(VALIDATEDRIVER);
    _NAME(MOVEDEVICE);
    _NAME(DETECT);
    _NAME(INSTALLWIZARD);
    _NAME(DESTROYWIZARDDATA);
    _NAME(PROPERTYCHANGE);
    _NAME(ENABLECLASS);
    _NAME(DETECTVERIFY);
    _NAME(INSTALLDEVICEFILES);
    _NAME(ALLOW_INSTALL);
    _NAME(SELECTBESTCOMPATDRV);
    _NAME(REGISTERDEVICE);
    _NAME(NEWDEVICEWIZARD_PRESELECT);
    _NAME(NEWDEVICEWIZARD_SELECT);
    _NAME(NEWDEVICEWIZARD_PREANALYZE);
    _NAME(NEWDEVICEWIZARD_POSTANALYZE);
    _NAME(NEWDEVICEWIZARD_FINISHINSTALL);
    _NAME(INSTALLINTERFACES);
    _NAME(DETECTCANCEL);
    _NAME(REGISTER_COINSTALLERS);
    _NAME(ADDPROPERTYPAGE_ADVANCED);
    _NAME(ADDPROPERTYPAGE_BASIC);
    _NAME(TROUBLESHOOTER);
    _NAME(POWERMESSAGEWAKE);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _NAME
}

static BOOLEAN
OpenEnumKey(
    OUT PHKEY   EnumKey
    )
{
    HRESULT     Error;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         ENUM_KEY,
                         0,
                         KEY_READ,
                         EnumKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
OpenBusKey(
    IN  PTCHAR  BusKeyName,
    OUT PHKEY   BusKey
    )
{
    BOOLEAN     Success;
    HKEY        EnumKey;
    HRESULT     Error;

    Success = OpenEnumKey(&EnumKey);
    if (!Success)
        goto fail1;

    Error = RegOpenKeyEx(EnumKey,
                         BusKeyName,
                         0,
                         KEY_READ,
                         BusKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    RegCloseKey(EnumKey);

    return TRUE;

fail2:
    Log("fail2");

    RegCloseKey(EnumKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
GetDeviceKeyName(
    IN  PTCHAR  BusKeyName,
    IN  PTCHAR  DeviceKeyPrefix,
    OUT PTCHAR  *DeviceKeyName
    )
{
    BOOLEAN     Success;
    HKEY        BusKey;
    HRESULT     Error;
    DWORD       SubKeys;
    DWORD       MaxSubKeyLength;
    DWORD       SubKeyLength;
    PTCHAR      SubKeyName;
    DWORD       Index;

    Success = OpenBusKey(BusKeyName, &BusKey);
    if (!Success)
        goto fail1;

    Error = RegQueryInfoKey(BusKey,
                            NULL,
                            NULL,
                            NULL,
                            &SubKeys,
                            &MaxSubKeyLength,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);

    SubKeyName = malloc(SubKeyLength);
    if (SubKeyName == NULL)
        goto fail3;

    for (Index = 0; Index < SubKeys; Index++) {
        SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);
        memset(SubKeyName, 0, SubKeyLength);

        Error = RegEnumKeyEx(BusKey,
                             Index,
                             (LPTSTR)SubKeyName,
                             &SubKeyLength,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
        if (Error != ERROR_SUCCESS) {
            SetLastError(Error);
            goto fail4;
        }

        if (strncmp(SubKeyName, DeviceKeyPrefix, strlen(DeviceKeyPrefix)) == 0)
            goto found;
    }

    free(SubKeyName);
    SubKeyName = NULL;

found:
    RegCloseKey(BusKey);

    Log("%s", (SubKeyName != NULL) ? SubKeyName : "none found");

    *DeviceKeyName = SubKeyName;
    return TRUE;

fail4:
    Log("fail4");

    free(SubKeyName);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(BusKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

#define PLATFORM_DEVICE_0001_NAME       "VEN_5853&DEV_0001"
#define PLATFORM_DEVICE_0002_NAME       "VEN_5853&DEV_0002"

#define XENSERVER_VENDOR_DEVICE_NAME    "VEN_5853&DEV_C000"

static BOOLEAN
OpenDeviceKey(
    IN  PTCHAR  BusKeyName,
    IN  PTCHAR  DeviceKeyName,
    OUT PHKEY   DeviceKey
    )
{
    BOOLEAN     Success;
    HKEY        BusKey;
    HRESULT     Error;

    Success = OpenBusKey(BusKeyName, &BusKey);
    if (!Success)
        goto fail1;

    Error = RegOpenKeyEx(BusKey,
                         DeviceKeyName,
                         0,
                         KEY_READ,
                         DeviceKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    RegCloseKey(BusKey);

    return TRUE;

fail2:
    Log("fail2");

    RegCloseKey(BusKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
GetDriverKeyName(
    IN  HKEY    DeviceKey,
    OUT PTCHAR  *Name
    )
{
    HRESULT     Error;
    DWORD       SubKeys;
    DWORD       MaxSubKeyLength;
    DWORD       SubKeyLength;
    PTCHAR      SubKeyName;
    DWORD       Index;
    HKEY        SubKey;
    PTCHAR      DriverKeyName;

    Error = RegQueryInfoKey(DeviceKey,
                            NULL,
                            NULL,
                            NULL,
                            &SubKeys,
                            &MaxSubKeyLength,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);

    SubKeyName = malloc(SubKeyLength);
    if (SubKeyName == NULL)
        goto fail2;

    SubKey = NULL;
    DriverKeyName = NULL;

    for (Index = 0; Index < SubKeys; Index++) {
        DWORD       MaxValueLength;
        DWORD       DriverKeyNameLength;
        DWORD       Type;

        SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);
        memset(SubKeyName, 0, SubKeyLength);

        Error = RegEnumKeyEx(DeviceKey,
                             Index,
                             (LPTSTR)SubKeyName,
                             &SubKeyLength,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
        if (Error != ERROR_SUCCESS) {
            SetLastError(Error);
            goto fail3;
        }

        Error = RegOpenKeyEx(DeviceKey,
                             SubKeyName,
                             0,
                             KEY_READ,
                             &SubKey);
        if (Error != ERROR_SUCCESS)
            continue;

        Error = RegQueryInfoKey(SubKey,
                                NULL,
                                NULL,
                                NULL,    
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                NULL,
                                &MaxValueLength,
                                NULL,
                                NULL);
        if (Error != ERROR_SUCCESS) {
            SetLastError(Error);
            goto fail4;
        }

        DriverKeyNameLength = MaxValueLength + sizeof (TCHAR);

        DriverKeyName = calloc(1, DriverKeyNameLength);
        if (DriverKeyName == NULL)
            goto fail5;

        Error = RegQueryValueEx(SubKey,
                                "Driver",
                                NULL,
                                &Type,
                                (LPBYTE)DriverKeyName,
                                &DriverKeyNameLength);
        if (Error == ERROR_SUCCESS &&
            Type == REG_SZ)
            break;

        free(DriverKeyName);
        DriverKeyName = NULL;

        RegCloseKey(SubKey);
        SubKey = NULL;
    }

    Log("%s", (DriverKeyName != NULL) ? DriverKeyName : "none found");

    if (SubKey != NULL)
        RegCloseKey(SubKey);

    free(SubKeyName);

    *Name = DriverKeyName;
    return TRUE;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    if (SubKey != NULL)
        RegCloseKey(SubKey);

fail3:
    Log("fail3");

    free(SubKeyName);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
OpenClassKey(
    OUT PHKEY   ClassKey
    )
{
    HRESULT     Error;

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         CLASS_KEY,
                         0,
                         KEY_READ,
                         ClassKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;
        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
OpenDriverKey(
    IN  PTCHAR  DriverKeyName,
    OUT PHKEY   DriverKey
    )
{
    BOOLEAN     Success;
    HKEY        ClassKey;
    HRESULT     Error;

    Success = OpenClassKey(&ClassKey);
    if (!Success)
        goto fail1;

    Error = RegOpenKeyEx(ClassKey,
                         DriverKeyName,
                         0,
                         KEY_READ,
                         DriverKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    RegCloseKey(ClassKey);

    return TRUE;

fail2:
    Log("fail2");

    RegCloseKey(ClassKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
GetDeviceInstanceID(
    IN  HDEVINFO            DeviceInfoSet,
    IN  PSP_DEVINFO_DATA    DeviceInfoData,
    OUT PTCHAR              *DeviceID,
    OUT PTCHAR              *InstanceID
    )
{
    DWORD                   DeviceInstanceIDLength;
    PTCHAR                  DeviceInstanceID;
    DWORD                   Index;
    PTCHAR                  Prefix;
    DWORD                   InstanceIDLength;
    HRESULT                 Result;
    HRESULT                 Error;

    if (!SetupDiGetDeviceInstanceId(DeviceInfoSet,
                                    DeviceInfoData,
                                    NULL,
                                    0,
                                    &DeviceInstanceIDLength)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            goto fail1;
    }

    DeviceInstanceIDLength += sizeof (TCHAR);

    DeviceInstanceID = calloc(1, DeviceInstanceIDLength);
    if (DeviceInstanceID == NULL)
        goto fail2;

    if (!SetupDiGetDeviceInstanceId(DeviceInfoSet,
                                    DeviceInfoData,
                                    DeviceInstanceID,
                                    DeviceInstanceIDLength,
                                    NULL))
        goto fail3;

    for (Index = 0; Index < strlen(DeviceInstanceID); Index++)
        DeviceInstanceID[Index] = (CHAR)toupper(DeviceInstanceID[Index]);

    *DeviceID = DeviceInstanceID;

    Prefix = strrchr(DeviceInstanceID, '\\');
    assert(Prefix != NULL);
    *Prefix++ = '\0';

    DeviceInstanceID = strrchr(Prefix, '&');
    if (DeviceInstanceID != NULL) {
        *DeviceInstanceID++ = '\0';
    } else {
        DeviceInstanceID = Prefix;
        Prefix = NULL;
    }

    if (Prefix != NULL)
        Log("Parent Prefix = %s", Prefix);

    InstanceIDLength = (ULONG)((strlen(DeviceInstanceID) +
                                1) * sizeof (TCHAR));

    *InstanceID = calloc(1, InstanceIDLength);
    if (*InstanceID == NULL)
        goto fail4;

    Result = StringCbPrintf(*InstanceID,
                            InstanceIDLength,
                            "%s",
                            DeviceInstanceID);
    assert(SUCCEEDED(Result));
    
    Log("DeviceID = %s", *DeviceID);
    Log("InstanceID = %s", *InstanceID);

    return TRUE;

fail4:
    Log("fail4");

    DeviceInstanceID = *DeviceID;
    *DeviceID = NULL;

fail3:
    Log("fail3");

    free(DeviceInstanceID);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
GetActiveDeviceInstanceID(
    OUT PTCHAR  *DeviceID,
    OUT PTCHAR  *InstanceID
    )
{
    HKEY        ParametersKey;
    DWORD       MaxValueLength;
    DWORD       DeviceIDLength;
    DWORD       InstanceIDLength;
    DWORD       Type;
    HRESULT     Error;

    Error = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                           PARAMETERS_KEY(XENBUS),
                           0,
                           NULL,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           NULL,
                           &ParametersKey,
                           NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegQueryInfoKey(ParametersKey,
                            NULL,
                            NULL,
                            NULL,    
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }
       
    DeviceIDLength = MaxValueLength + sizeof (TCHAR);

    *DeviceID = calloc(1, DeviceIDLength);
    if (*DeviceID == NULL)
        goto fail3;

    Error = RegQueryValueEx(ParametersKey,
                            "ActiveDeviceID",
                            NULL,
                            &Type,
                            (LPBYTE)*DeviceID,
                            &DeviceIDLength);
    if (Error != ERROR_SUCCESS || Type != REG_SZ) {
        free(*DeviceID);
        *DeviceID = NULL;
    }

    InstanceIDLength = MaxValueLength + sizeof (TCHAR);

    *InstanceID = calloc(1, InstanceIDLength);
    if (*InstanceID == NULL)
        goto fail4;

    Error = RegQueryValueEx(ParametersKey,
                            "ActiveInstanceID",
                            NULL,
                            &Type,
                            (LPBYTE)*InstanceID,
                            &InstanceIDLength);
    if (Error != ERROR_SUCCESS || Type != REG_SZ) {
        free(*InstanceID);
        *InstanceID = NULL;
    }

    Log("DeviceID = %s", (*DeviceID != NULL) ? *DeviceID : "NOT SET");
    Log("InstanceID = %s", (*InstanceID != NULL) ? *InstanceID : "NOT SET");

    RegCloseKey(ParametersKey);

    return TRUE;

fail4:
    Log("fail4");

    if (*DeviceID != NULL) {
        free(*DeviceID);
        *DeviceID = NULL;
    }

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(ParametersKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
SetActiveDeviceInstanceID(
    IN  PTCHAR  DeviceID,
    IN  PTCHAR  InstanceID
    )
{
    PTCHAR      DeviceName;
    BOOLEAN     Success;
    DWORD       DeviceIDLength;
    DWORD       InstanceIDLength;
    HKEY        ParametersKey;
    HRESULT     Error;

    Log("DeviceID = %s", DeviceID);
    Log("InstanceID = %s", InstanceID);

    DeviceName = strchr(DeviceID, '\\');
    assert(DeviceName != NULL);
    DeviceName++;

    // Check whether we are binding to the XenServer vendor device
    if (strncmp(DeviceName,
                XENSERVER_VENDOR_DEVICE_NAME,
                strlen(XENSERVER_VENDOR_DEVICE_NAME)) != 0) {
        PTCHAR  DeviceKeyName;

        // We are binding to a legacy platform device so only make it
        // active if there is no XenServer vendor device
        Success = GetDeviceKeyName("PCI",
                                   XENSERVER_VENDOR_DEVICE_NAME,
                                   &DeviceKeyName);
        if (!Success)
            goto fail1;

        if (DeviceKeyName != NULL) {
            Log("ignoring");
            free(DeviceKeyName);
            goto done;
        }
    }

    DeviceIDLength = (DWORD)((strlen(DeviceID) +
                              1) * sizeof (TCHAR));
    InstanceIDLength = (DWORD)((strlen(InstanceID) +
                                1) * sizeof (TCHAR));

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         PARAMETERS_KEY(XENBUS),
                         0,
                         KEY_ALL_ACCESS,
                         &ParametersKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    Error = RegSetValueEx(ParametersKey,
                          "ActiveDeviceID",
                          0,
                          REG_SZ,
                          (LPBYTE)DeviceID,
                          DeviceIDLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    Error = RegSetValueEx(ParametersKey,
                          "ActiveInstanceID",
                          0,
                          REG_SZ,
                          (LPBYTE)InstanceID,
                          InstanceIDLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    RegCloseKey(ParametersKey);

done:
    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    RegCloseKey(ParametersKey);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
ClearActiveDeviceInstanceID(
    VOID
    )
{
    HKEY        ParametersKey;
    HRESULT     Error;

    Log("<===>");

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         PARAMETERS_KEY(XENBUS),
                         0,
                         KEY_ALL_ACCESS,
                         &ParametersKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegDeleteValue(ParametersKey,
                           "ActiveDeviceID");
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    Error = RegDeleteValue(ParametersKey,
                           "ActiveInstanceID");
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    RegCloseKey(ParametersKey);

    return TRUE;

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(ParametersKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static PTCHAR
GetProperty(
    IN  HDEVINFO            DeviceInfoSet,
    IN  PSP_DEVINFO_DATA    DeviceInfoData,
    IN  DWORD               Index
    )
{
    DWORD                   Type;
    DWORD                   PropertyLength;
    PTCHAR                  Property;
    HRESULT                 Error;

    if (!SetupDiGetDeviceRegistryProperty(DeviceInfoSet,
                                          DeviceInfoData,
                                          Index,
                                          &Type,
                                          NULL,
                                          0,
                                          &PropertyLength)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            goto fail1;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail2;
    }

    PropertyLength += sizeof (TCHAR);

    Property = calloc(1, PropertyLength);
    if (Property == NULL)
        goto fail3;

    if (!SetupDiGetDeviceRegistryProperty(DeviceInfoSet,
                                          DeviceInfoData,
                                          Index,
                                          NULL,
                                          (PBYTE)Property,
                                          PropertyLength,
                                          NULL))
        goto fail4;

    return Property;

fail4:
    Log("fail4");

    free(Property);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return NULL;
}

static BOOLEAN
MatchExistingDriver(
    VOID
    )
{
    BOOLEAN Success;
    PTCHAR  DeviceKeyName = NULL;
    HKEY    DeviceKey = NULL;
    PTCHAR  DriverKeyName = NULL;
    HKEY    DriverKey = NULL;
    HRESULT Error;
    DWORD   MaxValueLength;
    DWORD   DriverDescLength;
    PTCHAR  DriverDesc = NULL;
    DWORD   ProductNameLength;
    DWORD   Type;

    Log("====>");

    // Look for a legacy platform device
    Success = GetDeviceKeyName("PCI",
                               PLATFORM_DEVICE_0001_NAME,
                               &DeviceKeyName);
    if (!Success)
        goto fail1;

    if (DeviceKeyName != NULL)
        goto found;

    Success = GetDeviceKeyName("PCI",
                               PLATFORM_DEVICE_0002_NAME,
                               &DeviceKeyName);
    if (!Success)
        goto fail2;

    if (DeviceKeyName != NULL)
        goto found;

    // No legacy platform device
    goto done;

found:
    Success = OpenDeviceKey("PCI", DeviceKeyName, &DeviceKey);
    if (!Success)
        goto fail3;

    // Check for a bound driver
    Success = GetDriverKeyName(DeviceKey, &DriverKeyName);
    if (!Success)
        goto fail4;

    if (DriverKeyName == NULL)
        goto done;

    Success = OpenDriverKey(DriverKeyName, &DriverKey);
    if (!Success)
        goto fail5;

    Error = RegQueryInfoKey(DriverKey,
                            NULL,
                            NULL,
                            NULL,    
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail6;
    }

    DriverDescLength = MaxValueLength + sizeof (TCHAR);

    DriverDesc = calloc(1, DriverDescLength);
    if (DriverDesc == NULL)
        goto fail7;

    Error = RegQueryValueEx(DriverKey,
                            "DriverDesc",
                            NULL,
                            &Type,
                            (LPBYTE)DriverDesc,
                            &DriverDescLength);
    if (Error != ERROR_SUCCESS) {
        if (Error == ERROR_FILE_NOT_FOUND)
            goto done;

        SetLastError(Error);
        goto fail8;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail9;
    }

    ProductNameLength = (DWORD)strlen(PRODUCT_NAME_STR);

    if (strncmp(DriverDesc,
                PRODUCT_NAME_STR,
                ProductNameLength) != 0) {
        SetLastError(ERROR_INSTALL_FAILURE);
        goto fail10;
    }

    if (strcmp(DriverDesc + ProductNameLength,
               " PV Bus") != 0) {
        SetLastError(ERROR_INSTALL_FAILURE);
        goto fail11;
    }

done:
    if (DriverDesc != NULL) {
        free(DriverDesc);
        RegCloseKey(DriverKey);
    }

    if (DriverKeyName != NULL) {
        free(DriverKeyName);
        RegCloseKey(DeviceKey);
    }

    if (DeviceKeyName != NULL)
        free(DeviceKeyName);

    Log("<====");

    return TRUE;

fail11:
    Log("fail11");

fail10:
    Log("fail10");

fail9:
    Log("fail9");

fail8:
    Log("fail8");

    free(DriverDesc);

fail7:
    Log("fail7");

fail6:
    Log("fail6");

    RegCloseKey(DriverKey);

fail5:
    Log("fail5");

    free(DriverKeyName);

fail4:
    Log("fail4");

    RegCloseKey(DeviceKey);

fail3:
    Log("fail3");

    free(DeviceKeyName);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

#define DEFINE_REVISION(_N, _S, _SI, _E, _D, _ST, _R, _C, _G, _U, _EM) \
    (_N)

static DWORD    DeviceRevision[] = {
    DEFINE_REVISION_TABLE
};

#undef DEFINE_REVISION

static BOOLEAN
SupportDeviceID(
    IN  PTCHAR      DeviceID
    )
{
    unsigned int    Revision;
    int             Count;
    DWORD           Index;
    HRESULT         Error;

    DeviceID = strrchr(DeviceID, '&');
    assert(DeviceID != NULL);
    DeviceID++;

    Count = sscanf_s(DeviceID,
                     "REV_%8x",
                     &Revision);
    if (Count != 1) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail1;
    }

    for (Index = 0; Index < ARRAYSIZE(DeviceRevision); Index++) {
        if (Revision == DeviceRevision[Index])
            goto found;
    }

    SetLastError(ERROR_FILE_NOT_FOUND);
    goto fail2;

found:
    Log("%x", Revision);

    return TRUE;

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
GetMatchingDeviceID(
    IN  HKEY    DriverKey,
    OUT PTCHAR  *MatchingDeviceID
    )
{
    HRESULT     Error;
    DWORD       MaxValueLength;
    DWORD       MatchingDeviceIDLength;
    DWORD       Type;
    DWORD       Index;

    Error = RegQueryInfoKey(DriverKey,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            &MaxValueLength,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    MatchingDeviceIDLength = MaxValueLength + sizeof (TCHAR);

    *MatchingDeviceID = calloc(1, MatchingDeviceIDLength);
    if (*MatchingDeviceID == NULL)
        goto fail2;

    Error = RegQueryValueEx(DriverKey,
                            "MatchingDeviceId",
                            NULL,
                            &Type,
                            (LPBYTE)*MatchingDeviceID,
                            &MatchingDeviceIDLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    for (Index = 0; Index < strlen(*MatchingDeviceID); Index++)
        (*MatchingDeviceID)[Index] = (CHAR)toupper((*MatchingDeviceID)[Index]);

    Log("%s", *MatchingDeviceID);

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

    free(*MatchingDeviceID);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
SupportChildDrivers(
    VOID
    )
{
    BOOLEAN     Success;
    HKEY        XenbusKey;
    HRESULT     Error;
    DWORD       SubKeys;
    DWORD       MaxSubKeyLength;
    DWORD       SubKeyLength;
    PTCHAR      SubKeyName;
    HKEY        DeviceKey;
    PTCHAR      DriverKeyName;
    HKEY        DriverKey;
    PTCHAR      MatchingDeviceID;
    DWORD       Index;

    Log("====>");

    Success = OpenBusKey("XENBUS", &XenbusKey);
    if (!Success) {
        // If there is no key then this must be a fresh installation
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            goto done;

        goto fail1;
    }

    Error = RegQueryInfoKey(XenbusKey,
                            NULL,
                            NULL,
                            NULL,
                            &SubKeys,
                            &MaxSubKeyLength,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail2;
    }

    SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);

    SubKeyName = malloc(SubKeyLength);
    if (SubKeyName == NULL)
        goto fail3;

    for (Index = 0; Index < SubKeys; Index++) {
        SubKeyLength = MaxSubKeyLength + sizeof (TCHAR);
        memset(SubKeyName, 0, SubKeyLength);

        Error = RegEnumKeyEx(XenbusKey,
                             Index,
                             (LPTSTR)SubKeyName,
                             &SubKeyLength,
                             NULL,
                             NULL,
                             NULL,
                             NULL);
        if (Error != ERROR_SUCCESS) {
            SetLastError(Error);
            goto fail4;
        }

        Success = OpenDeviceKey("XENBUS", SubKeyName, &DeviceKey);
        if (!Success)
            goto fail5;

        Success = GetDriverKeyName(DeviceKey, &DriverKeyName);
        if (!Success)
            goto fail6;

        if (DriverKeyName == NULL)
            goto loop;

        Success = OpenDriverKey(DriverKeyName, &DriverKey);
        if (!Success)
            goto fail7;

        Success = GetMatchingDeviceID(DriverKey, &MatchingDeviceID);
        if (!Success)
            goto fail8;

        Success = SupportDeviceID(MatchingDeviceID);
        if (!Success)
            goto fail9;

        free(MatchingDeviceID);

        RegCloseKey(DriverKey);

        free(DriverKeyName);

    loop:
        RegCloseKey(DeviceKey);
    }

    free(SubKeyName);

    RegCloseKey(XenbusKey);

done:
    Log("<====");

    return TRUE;

fail9:
    Log("fail9");

    free(MatchingDeviceID);

fail8:
    Log("fail8");

    RegCloseKey(DriverKey);

fail7:
    Log("fail7");

    free(DriverKeyName);

fail6:
    Log("fail6");

    RegCloseKey(DeviceKey);

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    free(SubKeyName);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    RegCloseKey(XenbusKey);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
InstallFilter(
    IN  const GUID  *Guid,
    IN  PTCHAR      Filter
    )
{
    HRESULT         Error;
    DWORD           Type;
    DWORD           OldLength;
    DWORD           NewLength;
    PTCHAR          UpperFilters;
    ULONG           Offset;

    if (!SetupDiGetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         &Type,
                                         NULL,
                                         0,
                                         &OldLength,
                                         NULL,
                                         NULL)) {
        Error = GetLastError();

        if (Error == ERROR_INVALID_DATA) {
            Type = REG_MULTI_SZ;
            OldLength = sizeof (TCHAR);
        } else if (Error != ERROR_INSUFFICIENT_BUFFER) {
            goto fail1;
        }
    }

    if (Type != REG_MULTI_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail2;
    }

    NewLength = OldLength + (DWORD)((strlen(Filter) + 1) * sizeof (TCHAR));

    UpperFilters = calloc(1, NewLength);
    if (UpperFilters == NULL)
        goto fail3;

    Offset = 0;
    if (OldLength != sizeof (TCHAR)) {
        if (!SetupDiGetClassRegistryProperty(Guid,
                                             SPCRP_UPPERFILTERS,
                                             &Type,
                                             (PBYTE)UpperFilters,
                                             OldLength,
                                             NULL,
                                             NULL,
                                             NULL))
            goto fail4;

        while (UpperFilters[Offset] != '\0') {
            ULONG   FilterLength;

            FilterLength = (ULONG)strlen(&UpperFilters[Offset]) / sizeof (TCHAR);

            if (_stricmp(&UpperFilters[Offset], Filter) == 0) {
                Log("%s already present", Filter);
                goto done;
            }

            Offset += FilterLength + 1;
        }
    }

    memmove(&UpperFilters[Offset], Filter, strlen(Filter));
    Log("added %s", Filter);

    if (!SetupDiSetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         (PBYTE)UpperFilters,
                                         NewLength,
                                         NULL,
                                         NULL))
        goto fail5;

done:
    free(UpperFilters);

    return TRUE;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    free(UpperFilters);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
RemoveFilter(
    IN  const GUID  *Guid,
    IN  PTCHAR      Filter
    )
{
    HRESULT         Error;
    DWORD           Type;
    DWORD           OldLength;
    DWORD           NewLength;
    PTCHAR          UpperFilters;
    ULONG           Offset;
    ULONG           FilterLength;

    if (!SetupDiGetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         &Type,
                                         NULL,
                                         0,
                                         &OldLength,
                                         NULL,
                                         NULL)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            goto fail1;
    }

    if (Type != REG_MULTI_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail2;
    }

    UpperFilters = calloc(1, OldLength);
    if (UpperFilters == NULL)
        goto fail3;

    if (!SetupDiGetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         &Type,
                                         (PBYTE)UpperFilters,
                                         OldLength,
                                         NULL,
                                         NULL,
                                         NULL))
        goto fail4;

    Offset = 0;
    FilterLength = 0;
    while (UpperFilters[Offset] != '\0') {
        FilterLength = (ULONG)strlen(&UpperFilters[Offset]) / sizeof (TCHAR);

        if (_stricmp(&UpperFilters[Offset], Filter) == 0)
            goto remove;

        Offset += FilterLength + 1;
    }

    goto done;

remove:
    NewLength = OldLength - ((FilterLength + 1) * sizeof (TCHAR));

    memmove(&UpperFilters[Offset],
            &UpperFilters[Offset + FilterLength + 1],
            (NewLength - Offset) * sizeof (TCHAR));

    Log("removed %s", Filter);

    if (!SetupDiSetClassRegistryProperty(Guid,
                                         SPCRP_UPPERFILTERS,
                                         (PBYTE)UpperFilters,
                                         NewLength,
                                         NULL,
                                         NULL))
        goto fail5;

done:
    free(UpperFilters);

    return TRUE;

fail5:
    Log("fail5");

fail4:
    Log("fail4");

    free(UpperFilters);

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
RequestReboot(
    IN  HDEVINFO            DeviceInfoSet,
    IN  PSP_DEVINFO_DATA    DeviceInfoData
    )
{
    SP_DEVINSTALL_PARAMS    DeviceInstallParams;
    HRESULT                 Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    DeviceInstallParams.Flags |= DI_NEEDREBOOT;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    if (!SetupDiSetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail2;

    return TRUE;

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOLEAN
SetFriendlyName(
    IN  HDEVINFO            DeviceInfoSet,
    IN  PSP_DEVINFO_DATA    DeviceInfoData,
    IN  PTCHAR              DeviceID
    )
{
    PTCHAR                  Description;
    unsigned int            Value;
    TCHAR                   FriendlyName[MAX_PATH];
    DWORD                   FriendlyNameLength;
    HRESULT                 Result;
    HRESULT                 Error;

    Description = GetProperty(DeviceInfoSet,
                              DeviceInfoData,
                              SPDRP_DEVICEDESC);
    if (Description == NULL)
        goto fail1;

    if (sscanf_s(DeviceID,
                 "PCI\\VEN_5853&DEV_%x",
                 &Value) != 1) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail2;
    }

    Result = StringCbPrintf(FriendlyName,
                            MAX_PATH,
                            "%s (%04X)",
                            Description,
                            Value);
    if (!SUCCEEDED(Result))
        goto fail3;

    FriendlyNameLength = (DWORD)(strlen(FriendlyName) + sizeof (TCHAR));

    if (!SetupDiSetDeviceRegistryProperty(DeviceInfoSet,
                                          DeviceInfoData,
                                          SPDRP_FRIENDLYNAME,
                                          (PBYTE)FriendlyName,
                                          FriendlyNameLength))
        goto fail4;

    Log("%s", FriendlyName);

    free(Description);

    return TRUE;

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

    free(Description);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static HRESULT
DifInstallPreProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    BOOLEAN                         Success;
    PTCHAR                          DeviceID;
    PTCHAR                          InstanceID;
    HRESULT                         Error;

    UNREFERENCED_PARAMETER(Context);

    Log("====>");

    Success = MatchExistingDriver();
    if (!Success)
        goto fail1;

    Success = SupportChildDrivers();
    if (!Success)
        goto fail2;

    Success = GetActiveDeviceInstanceID(&DeviceID, &InstanceID);
    if (!Success)
        goto fail3;

    if (DeviceID == NULL) {
        assert(InstanceID == NULL);

        Success = GetDeviceInstanceID(DeviceInfoSet, DeviceInfoData,
                                      &DeviceID, &InstanceID);
        if (!Success)
            goto fail4;

        Success = SetActiveDeviceInstanceID(DeviceID, InstanceID);
        if (!Success)
            goto fail5;
    }

    free(DeviceID);
    free(InstanceID);

    Log("<====");
    
    return NO_ERROR;

fail5:
    Log("fail5");

    free(DeviceID);
    free(InstanceID);

fail4:
    Log("fail4");

fail3:
    Log("fail3");

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static HRESULT
DifInstallPostProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    HRESULT                         Error;
    PTCHAR                          DeviceID;
    PTCHAR                          InstanceID;
    PTCHAR                          ActiveDeviceID;
    PTCHAR                          ActiveInstanceID;
    BOOLEAN                         Active;
    BOOLEAN                         Success;

    UNREFERENCED_PARAMETER(Context);

    Log("====>");

    Success = GetDeviceInstanceID(DeviceInfoSet, DeviceInfoData,
                                  &DeviceID, &InstanceID);
    if (!Success)
        goto fail1;

    Success = SetFriendlyName(DeviceInfoSet, DeviceInfoData, DeviceID);
    if (!Success)
        goto fail2;

    Success = GetActiveDeviceInstanceID(&ActiveDeviceID, &ActiveInstanceID);
    if (!Success)
        goto fail3;

    if (ActiveDeviceID != NULL) {
        assert(ActiveInstanceID != NULL);
        Active = (_stricmp(ActiveDeviceID, DeviceID) == 0 &&
                  _stricmp(ActiveInstanceID, InstanceID) == 0) ?
            TRUE :
            FALSE;

        free(ActiveDeviceID);
        free(ActiveInstanceID);
    } else {
        Active = FALSE;
    }

    if (Active) {
        (VOID) InstallFilter(&GUID_DEVCLASS_SYSTEM, "XENFILT");
        (VOID) InstallFilter(&GUID_DEVCLASS_HDC, "XENFILT");
        (VOID) RequestReboot(DeviceInfoSet, DeviceInfoData);
    }

    free(DeviceID);
    free(InstanceID);

    Log("<====");

    return NO_ERROR;

fail3:
    Log("fail3");

    free(DeviceID);
    free(InstanceID);

fail2:
    Log("fail2");

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static HRESULT
DifInstall(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    SP_DEVINSTALL_PARAMS            DeviceInstallParams;
    HRESULT                         Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    if (!Context->PostProcessing) {
        Error = DifInstallPreProcess(DeviceInfoSet, DeviceInfoData, Context);

        if (Error == NO_ERROR)
            Error = ERROR_DI_POSTPROCESSING_REQUIRED; 
    } else {
        Error = Context->InstallResult;
        
        if (Error == NO_ERROR) {
            (VOID) DifInstallPostProcess(DeviceInfoSet, DeviceInfoData, Context);
        } else {
            PTCHAR  Message;

            Message = GetErrorMessage(Error);
            Log("NOT RUNNING (DifInstallPreProcess Error: %s)", Message);
            LocalFree(Message);
        }

        Error = NO_ERROR; 
    }

    return Error;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static HRESULT
DifRemovePreProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    BOOLEAN                         Success;
    PTCHAR                          DeviceID;
    PTCHAR                          InstanceID;
    PTCHAR                          ActiveDeviceID;
    PTCHAR                          ActiveInstanceID;
    BOOLEAN                         Active;
    HRESULT                         Error;

    UNREFERENCED_PARAMETER(Context);

    Log("====>");

    Success = GetDeviceInstanceID(DeviceInfoSet, DeviceInfoData,
                                  &DeviceID, &InstanceID);
    if (!Success)
        goto fail1;

    Success = GetActiveDeviceInstanceID(&ActiveDeviceID, &ActiveInstanceID);
    if (!Success)
        goto fail2;

    if (ActiveDeviceID != NULL) {
        assert(ActiveInstanceID != NULL);
        Active = (_stricmp(ActiveDeviceID, DeviceID) == 0 &&
                  _stricmp(ActiveInstanceID, InstanceID) == 0) ?
            TRUE :
            FALSE;
        
        free(ActiveDeviceID);
        free(ActiveInstanceID);
    } else {
        Active = FALSE;
    }

    if (Active) {
        ClearActiveDeviceInstanceID();

        (VOID) RemoveFilter(&GUID_DEVCLASS_HDC, "XENFILT");
        (VOID) RemoveFilter(&GUID_DEVCLASS_SYSTEM, "XENFILT");
    }

    free(DeviceID);
    free(InstanceID);

    Log("<====");

    return NO_ERROR;

fail2:
    Log("fail2");

    free(DeviceID);
    free(InstanceID);

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

static HRESULT
DifRemovePostProcess(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    UNREFERENCED_PARAMETER(DeviceInfoSet);
    UNREFERENCED_PARAMETER(DeviceInfoData);
    UNREFERENCED_PARAMETER(Context);

    Log("<===>");

    return NO_ERROR;
}

static HRESULT
DifRemove(
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    SP_DEVINSTALL_PARAMS            DeviceInstallParams;
    HRESULT                         Error;

    DeviceInstallParams.cbSize = sizeof (DeviceInstallParams);

    if (!SetupDiGetDeviceInstallParams(DeviceInfoSet,
                                       DeviceInfoData,
                                       &DeviceInstallParams))
        goto fail1;

    Log("Flags = %08x", DeviceInstallParams.Flags);

    if (!Context->PostProcessing) {
        Error = DifRemovePreProcess(DeviceInfoSet, DeviceInfoData, Context);

        if (Error == NO_ERROR)
            Error = ERROR_DI_POSTPROCESSING_REQUIRED; 
    } else {
        Error = Context->InstallResult;
        
        if (Error == NO_ERROR) {
            (VOID) DifRemovePostProcess(DeviceInfoSet, DeviceInfoData, Context);
        } else {
            PTCHAR  Message;

            Message = GetErrorMessage(Error);
            Log("NOT RUNNING (DifRemovePreProcess Error: %s)", Message);
            LocalFree(Message);
        }

        Error = NO_ERROR; 
    }

    return Error;

fail1:
    Error = GetLastError();

    {
        PTCHAR  Message;

        Message = GetErrorMessage(Error);
        Log("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return Error;
}

DWORD CALLBACK
Entry(
    IN  DI_FUNCTION                 Function,
    IN  HDEVINFO                    DeviceInfoSet,
    IN  PSP_DEVINFO_DATA            DeviceInfoData,
    IN  PCOINSTALLER_CONTEXT_DATA   Context
    )
{
    HRESULT                         Error;

    Log("%s (%s) ===>",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    if (!Context->PostProcessing) {
        Log("%s PreProcessing",
            __FunctionName(Function));
    } else {
        Log("%s PostProcessing (%08x)",
            __FunctionName(Function),
            Context->InstallResult);
    }

    switch (Function) {
    case DIF_INSTALLDEVICE: {
        SP_DRVINFO_DATA         DriverInfoData;
        BOOLEAN                 DriverInfoAvailable;

        DriverInfoData.cbSize = sizeof (DriverInfoData);
        DriverInfoAvailable = SetupDiGetSelectedDriver(DeviceInfoSet,
                                                       DeviceInfoData,
                                                       &DriverInfoData) ?
                              TRUE :
                              FALSE;

        // If there is no driver information then the NULL driver is being
        // installed. Treat this as we would a DIF_REMOVE.
        Error = (DriverInfoAvailable) ?
                DifInstall(DeviceInfoSet, DeviceInfoData, Context) :
                DifRemove(DeviceInfoSet, DeviceInfoData, Context);
        break;
    }
    case DIF_REMOVE:
        Error = DifRemove(DeviceInfoSet, DeviceInfoData, Context);
        break;
    default:
        if (!Context->PostProcessing) {
            Error = NO_ERROR;
        } else {
            Error = Context->InstallResult;
        }

        break;
    }

    Log("%s (%s) <===",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return (DWORD)Error;
}

DWORD CALLBACK
Version(
    IN  HWND        Window,
    IN  HINSTANCE   Module,
    IN  PTCHAR      Buffer,
    IN  INT         Reserved
    )
{
    UNREFERENCED_PARAMETER(Window);
    UNREFERENCED_PARAMETER(Module);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Reserved);

    Log("%s (%s)",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR);

    return NO_ERROR;
}

static FORCEINLINE const CHAR *
__ReasonName(
    IN  DWORD       Reason
    )
{
#define _NAME(_Reason)          \
        case DLL_ ## _Reason:   \
            return #_Reason;

    switch (Reason) {
    _NAME(PROCESS_ATTACH);
    _NAME(PROCESS_DETACH);
    _NAME(THREAD_ATTACH);
    _NAME(THREAD_DETACH);
    default:
        break;
    }

    return "UNKNOWN";

#undef  _NAME
}

BOOL WINAPI
DllMain(
    IN  HINSTANCE   Module,
    IN  DWORD       Reason,
    IN  PVOID       Reserved
    )
{
    UNREFERENCED_PARAMETER(Module);
    UNREFERENCED_PARAMETER(Reserved);

    Log("%s (%s): %s",
        MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR,
        DAY_STR "/" MONTH_STR "/" YEAR_STR,
        __ReasonName(Reason));

    return TRUE;
}
