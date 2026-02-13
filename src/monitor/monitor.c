/* Copyright (c) Xen Project.
 * Copyright (c) Cloud Software Group, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source 1and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the23
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

#include <windows.h>
#include <tchar.h>
#include <stdlib.h>
#include <strsafe.h>
#include <wtsapi32.h>
#include <cfgmgr32.h>
#include <winternl.h>
#include <powrprof.h>
#include <malloc.h>
#include <assert.h>
#include <TraceLoggingProvider.h>
#include <winmeta.h>
#include <setupapi.h>
#include <devguid.h>

#include <version.h>

#include "messages.h"

#define stringify_literal(_text) #_text
#define stringify(_text) stringify_literal(_text)
#define __MODULE__ stringify(PROJECT)

#define MONITOR_NAME        __MODULE__
#define MONITOR_DISPLAYNAME MONITOR_NAME

typedef struct _MONITOR_CONTEXT {
    SERVICE_STATUS          Status;
    SERVICE_STATUS_HANDLE   Service;
    HKEY                    ParametersKey;
    HANDLE                  StopEvent;
    HANDLE                  RequestEvent;
    HANDLE                  Timer;
    HKEY                    RequestKey;
    PTSTR                   Title;
    PTSTR                   Text;
    PTSTR                   Question;
    BOOL                    RebootPrompted;
    PTSTR                   RebootRequestedBy;
    HANDLE                  ResponseEvent;
    DWORD                   Response;
} MONITOR_CONTEXT, *PMONITOR_CONTEXT;

typedef struct _REBOOT_PROMPT {
    PTSTR                   Title;
    PTSTR                   Text;
    HANDLE                  ResponseEvent;
    PDWORD                  PResponse;
} REBOOT_PROMPT, *PREBOOT_PROMPT;

MONITOR_CONTEXT MonitorContext;

#define MAXIMUM_BUFFER_SIZE 1024
#define REBOOT_RETRY_DELAY  60000L // 1 minute

#define SERVICES_KEY "SYSTEM\\CurrentControlSet\\Services"

#define SERVICE_KEY(_Service) \
        SERVICES_KEY ## "\\" ## _Service

#define PARAMETERS_KEY(_Service) \
        SERVICE_KEY(_Service) ## "\\Parameters"

TRACELOGGING_DEFINE_PROVIDER(MonitorTraceLoggingProvider,
                             MONITOR_NAME,
                             //{54F99C5B-76EC-5F84-3F97-4C9F40AA0F1A}
                             (0x54f99c5b, 0x76ec, 0x5f84, 0x3f, 0x97, 0x4c, 0x9f, 0x40, 0xaa, 0x0f, 0x1a));

typedef enum {
    LOG_INFO,
    LOG_ERROR
} LOG_LEVEL;

#ifdef UNICODE
#define TraceLoggingStringT(_buf, _name)    TraceLoggingWideString(_buf, _name)
#else
#define TraceLoggingStringT(_buf, _name)    TraceLoggingString(_buf, _name)
#endif

static VOID
#pragma prefast(suppress:6262) // Function uses '1036' bytes of stack: exceeds /analyze:stacksize'1024'
__Log(
    _In_ LOG_LEVEL      Level,
    _In_ PCTSTR         Format,
    ...
    )
{
    TCHAR               Buffer[MAXIMUM_BUFFER_SIZE];
    va_list             Arguments;
    size_t              Length;
    HRESULT             Result;

    va_start(Arguments, Format);
    Result = StringCchVPrintf(Buffer, MAXIMUM_BUFFER_SIZE, Format, Arguments);
    va_end(Arguments);

    if (Result != S_OK && Result != STRSAFE_E_INSUFFICIENT_BUFFER)
        return;

    Result = StringCchLength(Buffer, MAXIMUM_BUFFER_SIZE, &Length);
    if (Result != S_OK)
        return;

    Length = __min(MAXIMUM_BUFFER_SIZE - 1, Length + 2);

    _Analysis_assume_(Length < MAXIMUM_BUFFER_SIZE);
    _Analysis_assume_(Length >= 2);
    Buffer[Length] = _T('\0');
    Buffer[Length - 1] = _T('\n');
    Buffer[Length - 2] = _T('\r');

    OutputDebugString(Buffer);

    switch (Level) {
    case LOG_INFO:
        TraceLoggingWrite(MonitorTraceLoggingProvider,
                          "Information",
                          TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                          TraceLoggingStringT(Buffer, "Info"));
        break;
    case LOG_ERROR:
        TraceLoggingWrite(MonitorTraceLoggingProvider,
                          "Error",
                          TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                          TraceLoggingStringT(Buffer, "Error"));
        break;
    default:
        break;
    }
}

#define LogInfo(_Format, ...) \
        __Log(LOG_INFO, _T(__MODULE__ "|" __FUNCTION__ ": " _Format), __VA_ARGS__)

#define LogError(_Format, ...) \
        __Log(LOG_ERROR, _T(__MODULE__ "|" __FUNCTION__ ": " _Format), __VA_ARGS__)

static PTSTR
GetErrorMessage(
    _In_  HRESULT   Error
    )
{
    PTSTR           Message;
    ULONG           Index;

    if (!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       Error,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPTSTR)&Message,
                       0,
                       NULL))
        return NULL;

    for (Index = 0; Message[Index] != _T('\0'); Index++) {
        if (Message[Index] == _T('\r') || Message[Index] == _T('\n')) {
            Message[Index] = _T('\0');
            break;
        }
    }

    return Message;
}

static PCTSTR
ServiceStateName(
    _In_ DWORD  State
    )
{
#define _STATE_NAME(_State) \
    case SERVICE_ ## _State: \
        return _T(#_State)

    switch (State) {
    _STATE_NAME(START_PENDING);
    _STATE_NAME(RUNNING);
    _STATE_NAME(STOP_PENDING);
    _STATE_NAME(STOPPED);
    default:
        break;
    }

    return _T("UNKNOWN");

#undef  _STATE_NAME
}

static VOID
ReportStatus(
    _In_ DWORD          CurrentState,
    _In_ DWORD          Win32ExitCode,
    _In_ DWORD          WaitHint)
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    static DWORD        CheckPoint = 1;
    BOOL                Success;
    HRESULT             Error;

    LogInfo("====> (%s)", ServiceStateName(CurrentState));

    Context->Status.dwCurrentState = CurrentState;
    Context->Status.dwWin32ExitCode = Win32ExitCode;
    Context->Status.dwWaitHint = WaitHint;

    if (CurrentState == SERVICE_START_PENDING)
        Context->Status.dwControlsAccepted = 0;
    else
        Context->Status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                             SERVICE_ACCEPT_SHUTDOWN |
                                             SERVICE_ACCEPT_SESSIONCHANGE;

    if (CurrentState == SERVICE_RUNNING ||
        CurrentState == SERVICE_STOPPED )
        Context->Status.dwCheckPoint = 0;
    else
        Context->Status.dwCheckPoint = CheckPoint++;

    Success = SetServiceStatus(Context->Service, &Context->Status);

    if (!Success)
        goto fail1;

    LogInfo("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

DWORD WINAPI
MonitorCtrlHandlerEx(
    _In_ DWORD          Ctrl,
    _In_ DWORD          EventType,
    _In_ LPVOID         EventData,
    _In_ LPVOID         Argument
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;

    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Argument);

    switch (Ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 0);
        SetEvent(Context->StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
        SetEvent(Context->RequestEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
        return NO_ERROR;

    default:
        break;
    }

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static PCTSTR
WTSStateName(
    _In_ DWORD  State
    )
{
#define _STATE_NAME(_State) \
    case WTS ## _State: \
        return _T(#_State)

    switch (State) {
    _STATE_NAME(Active);
    _STATE_NAME(Connected);
    _STATE_NAME(ConnectQuery);
    _STATE_NAME(Shadow);
    _STATE_NAME(Disconnected);
    _STATE_NAME(Idle);
    _STATE_NAME(Listen);
    _STATE_NAME(Reset);
    _STATE_NAME(Down);
    _STATE_NAME(Init);
    default:
        break;
    }

    return _T("UNKNOWN");

#undef  _STATE_NAME
}

static VOID
DoReboot(
    _In_ PTSTR  Message,
    _In_ DWORD  Timeout
    )
{
    LogInfo("waiting for pending install events...");

    (VOID) CM_WaitNoPendingInstallEvents(INFINITE);

    LogInfo("initiating shutdown...");

#pragma prefast(suppress:28159)
    (VOID) InitiateSystemShutdownEx(NULL,
                                    Message,
                                    Timeout,
                                    TRUE,
                                    TRUE,
                                    SHTDN_REASON_MAJOR_OPERATINGSYSTEM |
                                    SHTDN_REASON_MINOR_INSTALLATION |
                                    SHTDN_REASON_FLAG_PLANNED);
}

static DWORD
GetPromptTimeout(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               Type;
    DWORD               Value;
    DWORD               ValueLength;
    HRESULT             Error;

    ValueLength = sizeof (Value);

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("PromptTimeout"),
                            NULL,
                            &Type,
                            (LPBYTE)&Value,
                            &ValueLength);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        Value = 0;

    LogInfo("%u", Value);

    return Value;
}

static PTSTR
GetDisplayName(
    _In_ PTSTR          DriverName
    )
{
    HRESULT             Result;
    TCHAR               ServiceKeyName[MAX_PATH];
    HKEY                ServiceKey;
    DWORD               MaxValueLength;
    DWORD               Type;
    DWORD               DisplayNameLength;
    PTSTR               DisplayName;
    HRESULT             Error;

    Result = StringCbPrintf(ServiceKeyName,
                            MAX_PATH,
                            _T(SERVICES_KEY "\\%s"),
                            DriverName);
    assert(SUCCEEDED(Result));

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         ServiceKeyName,
                         0,
                         KEY_READ,
                         &ServiceKey);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail1;
    }

    Error = RegQueryInfoKey(ServiceKey,
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

    DisplayNameLength = MaxValueLength + sizeof (TCHAR);

    DisplayName = calloc(1, DisplayNameLength);
    if (DisplayName == NULL)
        goto fail3;

    Error = RegQueryValueEx(ServiceKey,
                            _T("DisplayName"),
                            NULL,
                            &Type,
                            (LPBYTE)DisplayName,
                            &DisplayNameLength);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail4;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail5;
    }

    RegCloseKey(ServiceKey);

    return DisplayName;

fail5:
    LogError("fail5");

fail4:
    LogError("fail4");

    free(DisplayName);

fail3:
    LogError("fail3");

fail2:
    LogError("fail2");

    RegCloseKey(ServiceKey);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return NULL;
}

static VOID
RebootPromptFree(
    PREBOOT_PROMPT      Prompt
    )
{
    if (Prompt) {
        free(Prompt->Text);
        free(Prompt->Title);
        free(Prompt);
    }
}

static DWORD WINAPI
DoPromptForReboot(
    LPVOID lpThreadParameter
    )
{
    PREBOOT_PROMPT      Prompt = lpThreadParameter;
    DWORD               TitleLength;
    DWORD               TextLength;
    DWORD               Timeout;
    PWTS_SESSION_INFO   SessionInfo;
    DWORD               Count;
    DWORD               Index;
    BOOL                Success;
    DWORD               Error;

    assert(Prompt);
    assert(Prompt->ResponseEvent && Prompt->PResponse);
    assert(Prompt->Title && Prompt->Text);

    Error = ERROR_SUCCESS;

    TitleLength = (DWORD)(_tcslen(Prompt->Title) + 1);
    TextLength = (DWORD)(_tcslen(Prompt->Text) + 1);

    Success = WTSEnumerateSessions(WTS_CURRENT_SERVER_HANDLE,
                                   0,
                                   1,
                                   &SessionInfo,
                                   &Count);
    if (!Success) {
        Error = GetLastError();
        goto fail1;
    }

    Timeout = GetPromptTimeout();

    *Prompt->PResponse = 0;

    for (Index = 0; Index < Count; Index++) {
        DWORD                   SessionId = SessionInfo[Index].SessionId;
        PTSTR                   Name = SessionInfo[Index].pWinStationName;
        WTS_CONNECTSTATE_CLASS  State = SessionInfo[Index].State;
        DWORD                   Response;

        LogInfo("[%u]: %s [%s]",
                SessionId,
                Name,
                WTSStateName(State));

        if (State != WTSActive)
            continue;

        Success = WTSSendMessage(WTS_CURRENT_SERVER_HANDLE,
                                 SessionId,
                                 Prompt->Title,
                                 TitleLength * sizeof(TCHAR),
                                 Prompt->Text,
                                 TextLength * sizeof(TCHAR),
                                 MB_YESNO | MB_ICONEXCLAMATION,
                                 Timeout,
                                 &Response,
                                 TRUE);

        if (!Success)
            goto fail2;

        *Prompt->PResponse = Response;
        (VOID) SetEvent(Prompt->ResponseEvent);

        break;
    }

    WTSFreeMemory(SessionInfo);
    RebootPromptFree(Prompt);

    return ERROR_SUCCESS;

fail2:
    LogError("fail2");
    *Prompt->PResponse = 0;

fail1:
    RebootPromptFree(Prompt);

    return Error;
}

static VOID
PromptForReboot(
    _In_ PTSTR          DriverName
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Result;
    PREBOOT_PROMPT      Prompt;
    PTSTR               DisplayName;
    PTSTR               Description;
    HANDLE              PromptThread;
    DWORD               TextLength;
    DWORD               Error;

    assert(DriverName);

    /*
     * Can't use Context->Response here since a previous prompt may not have
     * gotten a response.
     */
    if (Context->RebootPrompted)
        return;
    Context->RebootPrompted = TRUE;

    LogInfo("====> (%s)", DriverName);

    Prompt = calloc(1, sizeof (REBOOT_PROMPT));
    if (Prompt == NULL) {
        Error = ERROR_OUTOFMEMORY;
        goto fail1;
    }
    Prompt->ResponseEvent = Context->ResponseEvent;
    Prompt->PResponse = &Context->Response;

    Prompt->Title = _tcsdup(Context->Title);
    if (Prompt->Title == NULL) {
        Error = ERROR_OUTOFMEMORY;
        goto fail2;
    }

    DisplayName = GetDisplayName(DriverName);
    if (DisplayName == NULL) {
        Error = GetLastError();
        goto fail3;
    }

    Description = _tcsrchr(DisplayName, ';');
    if (Description == NULL)
        Description = DisplayName;
    else
        Description++;

    TextLength = (DWORD)(_tcslen(Description) +
                         1 + // ' '
                         _tcslen(Context->Text) +
                         1 + // ' '
                         _tcslen(Context->Question) +
                         1);

    Prompt->Text = calloc(1, TextLength * sizeof(TCHAR));
    if (Prompt->Text == NULL) {
        Error = ERROR_OUTOFMEMORY;
        goto fail4;
    }

    Result = StringCchPrintf(Prompt->Text,
                             TextLength,
                             TEXT("%s %s %s"),
                             Description,
                             Context->Text,
                             Context->Question);
    assert(SUCCEEDED(Result));

    PromptThread = CreateThread(NULL,
                                0,
                                &DoPromptForReboot,
                                Prompt,
                                0,
                                NULL);
    if (PromptThread == NULL) {
        Error = GetLastError();
        goto fail4;
    }

    CloseHandle(PromptThread);
    free(DisplayName);
    // ownership of Prompt handed to prompt thread

    return;

fail4:
    LogError("fail4");
    free(DisplayName);

fail3:
    LogError("fail3");

fail2:
    LogError("fail2");

fail1:
    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    RebootPromptFree(Prompt);
}

static VOID
TryAutoReboot(
    _In_ PTSTR          DriverName
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Result;
    HANDLE              MsiMutex;
    DWORD               Type;
    DWORD               AutoReboot;
    DWORD               RebootCount;
    DWORD               Length;
    DWORD               Timeout;
    PTSTR               DisplayName;
    PTSTR               Description;
    PTSTR               Text;
    DWORD               TextLength;
    ULONG               PowerInfo;
    NTSTATUS            Status;
    DWORD               Error;

    if (!Context->RebootRequestedBy) {
        Context->RebootRequestedBy = _tcsdup(DriverName);
        if (!Context->RebootRequestedBy) {
            Error = ERROR_OUTOFMEMORY;
            goto fail1;
        }
    }

    // We don't want to suddenly reboot if the user's already said no.
    if (Context->Response == IDNO)
        goto done;

    // Check if there's an installation under way.
    MsiMutex = OpenMutex(SYNCHRONIZE,
                         FALSE,
                         TEXT("Global\\_MSIExecute"));
    if (MsiMutex != NULL) {
        Error = WaitForSingleObject(MsiMutex, 0);
        if (Error == WAIT_OBJECT_0 || Error == WAIT_ABANDONED)
            ReleaseMutex(MsiMutex);

        CloseHandle(MsiMutex);

        if (Error == WAIT_TIMEOUT)
            // The only case where an installation is definitely running.
            goto done;
    }

    Status = CallNtPowerInformation(SystemExecutionState,
                                    NULL,
                                    0,
                                    &PowerInfo,
                                    sizeof(PowerInfo));
    if (Status < 0 || (PowerInfo & ES_SYSTEM_REQUIRED))
        goto done;

    Length = sizeof (DWORD);

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("AutoReboot"),
                            NULL,
                            &Type,
                            (LPBYTE)&AutoReboot,
                            &Length);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        AutoReboot = 0;

    if (AutoReboot == 0)
        goto prompt;

    Length = sizeof (DWORD);

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("RebootCount"),
                            NULL,
                            &Type,
                            (LPBYTE)&RebootCount,
                            &Length);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        RebootCount = 0;

    if (RebootCount >= AutoReboot)
        goto prompt;

    LogInfo("AutoRebooting (reboot %u of %u)\n",
            RebootCount,
            AutoReboot);

    ++RebootCount;

    (VOID) RegSetValueEx(Context->ParametersKey,
                         _T("RebootCount"),
                         0,
                         REG_DWORD,
                         (const BYTE*)&RebootCount,
                         (DWORD) sizeof(DWORD));

    (VOID) RegFlushKey(Context->ParametersKey);

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("AutoRebootTimeout"),
                            NULL,
                            &Type,
                            (LPBYTE)&Timeout,
                            &Length);
    if (Error != ERROR_SUCCESS ||
        Type != REG_DWORD)
        Timeout = 60;

    DisplayName = GetDisplayName(Context->RebootRequestedBy);
    if (DisplayName == NULL) {
        Error = GetLastError();
        goto fail2;
    }

    Description = _tcsrchr(DisplayName, ';');
    if (Description == NULL)
        Description = DisplayName;
    else
        Description++;

    TextLength = (DWORD)(_tcslen(Description) +
                         1 + // ' '
                         _tcslen(Context->Text) +
                         1);

    Text = calloc(1, TextLength * sizeof(TCHAR));
    if (Text == NULL) {
        SetLastError(ERROR_OUTOFMEMORY);
        goto fail3;
    }

    Result = StringCchPrintf(Text,
                             TextLength,
                             TEXT("%s %s"),
                             Description,
                             Context->Text);
    assert(SUCCEEDED(Result));

    free(DisplayName);

    DoReboot(Text, Timeout);

    free(Text);

    return;

prompt:
    PromptForReboot(Context->RebootRequestedBy);

    return;

done:
    return;

fail3:
    LogError("fail3");

    free(DisplayName);

fail2:
    LogError("fail2");

fail1:
    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return;
}

static VOID
CheckRequestSubKeys(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               SubKeys;
    DWORD               MaxSubKeyLength;
    DWORD               SubKeyLength;
    PTSTR               SubKeyName;
    DWORD               Index;
    HKEY                SubKey;
    HRESULT             Error;

    LogInfo("====>");

    Error = RegQueryInfoKey(Context->RequestKey,
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

    SubKeyLength = MaxSubKeyLength + 1;

    SubKeyName = calloc(1, SubKeyLength * sizeof(TCHAR));
    if (SubKeyName == NULL)
        goto fail2;

    for (Index = 0; Index < SubKeys; Index++) {
        DWORD   Length;
        DWORD   Type;
        DWORD   Reboot;

        SubKeyLength = MaxSubKeyLength + 1;
        memset(SubKeyName, 0, SubKeyLength * sizeof(TCHAR));

        Error = RegEnumKeyEx(Context->RequestKey,
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

        LogInfo("%s", SubKeyName);

        Error = RegOpenKeyEx(Context->RequestKey,
                             SubKeyName,
                             0,
                             KEY_READ,
                             &SubKey);
        if (Error != ERROR_SUCCESS)
            continue;

        Length = sizeof (DWORD);
        Error = RegQueryValueEx(SubKey,
                                _T("Reboot"),
                                NULL,
                                &Type,
                                (LPBYTE)&Reboot,
                                &Length);
        if (Error != ERROR_SUCCESS ||
            Type != REG_DWORD)
            goto loop;

        if (Reboot != 0)
            goto found;

loop:
        RegCloseKey(SubKey);
    }

    Error = RegDeleteValue(Context->ParametersKey,
                           _T("RebootCount"));
    if (Error == ERROR_SUCCESS)
        (VOID) RegFlushKey(Context->ParametersKey);

    goto done;

found:
    RegCloseKey(SubKey);

    if (!Context->RebootRequestedBy)
        TryAutoReboot(SubKeyName);

done:
    free(SubKeyName);

    LogInfo("<====");

    return;

fail3:
    LogError("fail3");

    free(SubKeyName);

fail2:
    LogError("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static VOID
CheckRequestKey(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    HRESULT             Error;

    LogInfo("====>");

    CheckRequestSubKeys();

    Error = RegNotifyChangeKeyValue(Context->RequestKey,
                                    TRUE,
                                    REG_NOTIFY_CHANGE_LAST_SET,
                                    Context->RequestEvent,
                                    TRUE);

    if (Error != ERROR_SUCCESS)
        goto fail1;

    LogInfo("<====");

    return;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

static BOOL
AcquireShutdownPrivilege(
    VOID
    )
{
    HANDLE              Token;
    TOKEN_PRIVILEGES    New;
    BOOL                Success;
    HRESULT             Error;

    LogInfo("====>");

    New.PrivilegeCount = 1;

    Success = LookupPrivilegeValue(NULL,
                                   SE_SHUTDOWN_NAME,
                                   &New.Privileges[0].Luid);

    if (!Success)
        goto fail1;

    New.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    Success = OpenProcessToken(GetCurrentProcess(),
                               TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                               &Token);

    if (!Success)
        goto fail2;

    Success = AdjustTokenPrivileges(Token,
                                    FALSE,
                                    &New,
                                    0,
                                    NULL,
                                    NULL);

    if (!Success)
        goto fail3;

    CloseHandle(Token);

    LogInfo("<====");

    return TRUE;

fail3:
    LogError("fail3");

    CloseHandle(Token);

fail2:
    LogError("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

_Success_(return)
static BOOL
GetRequestKeyName(
    _Outptr_result_z_ PTSTR     *RequestKeyName
    )
{
    PMONITOR_CONTEXT            Context = &MonitorContext;
    DWORD                       MaxValueLength;
    DWORD                       RequestKeyNameSize;
    DWORD                       Type;
    HRESULT                     Error;

    Error = RegQueryInfoKey(Context->ParametersKey,
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

    RequestKeyNameSize = (MaxValueLength + 1) * sizeof(TCHAR);

    *RequestKeyName = calloc(1, RequestKeyNameSize);
    if (*RequestKeyName == NULL)
        goto fail2;

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("RequestKey"),
                            NULL,
                            &Type,
                            (LPBYTE)(*RequestKeyName),
                            &RequestKeyNameSize);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    LogInfo("%s", *RequestKeyName);

    return TRUE;

fail4:
    LogError("fail4");

fail3:
    LogError("fail3");

    free(*RequestKeyName);

fail2:
    LogError("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
GetDialogParameters(
    VOID
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    DWORD               MaxValueLength;
    DWORD               TitleSize;
    DWORD               TextSize;
    DWORD               QuestionSize;
    DWORD               Type;
    HRESULT             Error;

    Error = RegQueryInfoKey(Context->ParametersKey,
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

    TitleSize = (MaxValueLength + 1) * sizeof(TCHAR);

    Context->Title = calloc(1, TitleSize);
    if (Context == NULL)
        goto fail2;

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("DialogTitle"),
                            NULL,
                            &Type,
                            (LPBYTE)Context->Title,
                            &TitleSize);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail3;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail4;
    }

    TextSize = (MaxValueLength + 1) * sizeof (TCHAR);

    Context->Text = calloc(1, TextSize);
    if (Context == NULL)
        goto fail5;

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("DialogText"),
                            NULL,
                            &Type,
                            (LPBYTE)Context->Text,
                            &TextSize);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail6;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail7;
    }

    QuestionSize = (MaxValueLength + 1) * sizeof (TCHAR);

    Context->Question = calloc(1, QuestionSize);
    if (Context == NULL)
        goto fail8;

    Error = RegQueryValueEx(Context->ParametersKey,
                            _T("DialogQuestion"),
                            NULL,
                            &Type,
                            (LPBYTE)Context->Question,
                            &QuestionSize);
    if (Error != ERROR_SUCCESS) {
        SetLastError(Error);
        goto fail9;
    }

    if (Type != REG_SZ) {
        SetLastError(ERROR_BAD_FORMAT);
        goto fail10;
    }

    return TRUE;

fail10:
    LogError("fail10");

fail9:
    LogError("fail9");

    free(Context->Question);

fail8:
    LogError("fail8");

fail7:
    LogError("fail7");

fail6:
    LogError("fail6");

    free(Context->Text);

fail5:
    LogError("fail5");

fail4:
    LogError("fail4");

fail3:
    LogError("fail3");

    free(Context->Title);

fail2:
    LogError("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static VOID
RemoveStartOverride(
    _In_ PTSTR          DriverName
    )
{
    TCHAR               KeyName[MAX_PATH];
    HRESULT             Error;

    LogInfo("%s", DriverName);

    Error = StringCchPrintf(KeyName,
                            MAX_PATH,
                            _T(SERVICES_KEY "\\%s\\StartOverride"),
                            DriverName);
    assert(SUCCEEDED(Error));

    (VOID) RegDeleteKey(HKEY_LOCAL_MACHINE, KeyName);
}

static VOID
RemoveStartOverrideForClass(
    _In_ const GUID*    Guid
    )
{
    HRESULT             Error;
    HDEVINFO            DevInfo;
    DWORD               Index;
    SP_DEVINFO_DATA     DevInfoData;

    DevInfo = SetupDiGetClassDevs(Guid,
                                  NULL,
                                  NULL,
                                  0);
    if (DevInfo == INVALID_HANDLE_VALUE)
        goto fail1;

    memset(&DevInfoData, 0, sizeof(DevInfoData));
    DevInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (Index = 0;
         SetupDiEnumDeviceInfo(DevInfo, Index, &DevInfoData);
         ++Index) {
        TCHAR           Buffer[MAX_PATH];
        memset(Buffer, 0, sizeof(Buffer));

        if (SetupDiGetDeviceRegistryProperty(DevInfo,
                                             &DevInfoData,
                                             SPDRP_SERVICE,
                                             NULL,
                                             (PBYTE)Buffer,
                                             sizeof(Buffer),
                                             NULL)) {
            Buffer[MAX_PATH - 1] = _T('\0');
            RemoveStartOverride(Buffer);
        }

        memset(&DevInfoData, 0, sizeof(DevInfoData));
        DevInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    }

    SetupDiDestroyDeviceInfoList(DevInfo);

    return;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }
}

VOID WINAPI
MonitorMain(
    _In_    DWORD       argc,
    _In_    LPTSTR      *argv
    )
{
    PMONITOR_CONTEXT    Context = &MonitorContext;
    PTSTR               RequestKeyName;
    BOOL                Success;
    HRESULT             Error;
    LARGE_INTEGER       DueTime;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    if (TraceLoggingRegister(MonitorTraceLoggingProvider) != ERROR_SUCCESS)
        LogInfo("TraceLoggingRegister failed");

    LogInfo("====>");

    RemoveStartOverrideForClass(&GUID_DEVCLASS_SCSIADAPTER);

    Error = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                         _T(PARAMETERS_KEY(__MODULE__)),
                         0,
                         KEY_READ,
                         &Context->ParametersKey);
    if (Error != ERROR_SUCCESS)
        goto fail1;

    Success = AcquireShutdownPrivilege();
    if (!Success)
        goto fail2;

    Context->Service = RegisterServiceCtrlHandlerEx(_T(MONITOR_NAME),
                                                    MonitorCtrlHandlerEx,
                                                    NULL);
    if (Context->Service == NULL)
        goto fail3;

    Context->Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Context->Status.dwServiceSpecificExitCode = 0;

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    Context->StopEvent = CreateEvent(NULL,
                                     TRUE,
                                     FALSE,
                                     NULL);

    if (Context->StopEvent == NULL)
        goto fail4;

    Context->RequestEvent = CreateEvent(NULL,
                                        TRUE,
                                        FALSE,
                                        NULL);
    if (Context->RequestEvent == NULL)
        goto fail5;

    Context->ResponseEvent = CreateEvent(NULL,
                                         FALSE,
                                         FALSE,
                                         NULL);
    if (Context->ResponseEvent == NULL)
        goto fail6;
    Context->Response = 0;

    Success = GetRequestKeyName(&RequestKeyName);
    if (!Success)
        goto fail7;

    Error = RegCreateKeyEx(HKEY_LOCAL_MACHINE,
                           RequestKeyName,
                           0,
                           NULL,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           NULL,
                           &Context->RequestKey,
                           NULL);
    if (Error != ERROR_SUCCESS)
        goto fail8;

    Success = GetDialogParameters();
    if (!Success)
        goto fail9;

    Context->Timer = CreateWaitableTimer(NULL, FALSE, NULL);
    if (Context->Timer == NULL)
        goto fail10;

    DueTime.QuadPart = -10000LL * REBOOT_RETRY_DELAY;

    Success = SetWaitableTimer(Context->Timer,
                               &DueTime,
                               REBOOT_RETRY_DELAY,
                               NULL,
                               NULL,
                               FALSE);
    if (!Success)
        goto fail11;

    SetEvent(Context->RequestEvent);

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    for (;;) {
        HANDLE  Events[4];
        DWORD   Object;

        Events[0] = Context->StopEvent;
        Events[1] = Context->RequestEvent;
        Events[2] = Context->ResponseEvent;
        Events[3] = Context->Timer;

        LogInfo("waiting (%u)...", ARRAYSIZE(Events));
        Object = WaitForMultipleObjects(ARRAYSIZE(Events),
                                        Events,
                                        FALSE,
                                        INFINITE);
        LogInfo("awake");

        switch (Object) {
        case WAIT_OBJECT_0:
            ResetEvent(Events[0]);
            goto done;

        case WAIT_OBJECT_0 + 1:
            ResetEvent(Events[1]);
            CheckRequestKey();
            break;

        case WAIT_OBJECT_0 + 2:
            if (Context->Response == IDYES || Context->Response == IDTIMEOUT)
                DoReboot(NULL, 0);
            break;

        case WAIT_OBJECT_0 + 3:
            if (Context->RebootRequestedBy)
                TryAutoReboot(Context->RebootRequestedBy);
            break;

        default:
            break;
        }
    }

done:
    free(Context->RebootRequestedBy);
    CancelWaitableTimer(Context->Timer);
    CloseHandle(Context->Timer);

    (VOID) RegDeleteTree(Context->RequestKey, NULL);

    free(Context->Question);
    free(Context->Text);
    free(Context->Title);
    RegCloseKey(Context->RequestKey);
    free(RequestKeyName);
    CloseHandle(Context->ResponseEvent);
    CloseHandle(Context->RequestEvent);
    CloseHandle(Context->StopEvent);

    RegCloseKey(Context->ParametersKey);
    RemoveStartOverrideForClass(&GUID_DEVCLASS_SCSIADAPTER);

    ReportStatus(SERVICE_STOPPED, NO_ERROR, 0);

    LogInfo("<====");

    TraceLoggingUnregister(MonitorTraceLoggingProvider);

    return;

fail11:
    LogError("fail11");

    CloseHandle(Context->Timer);

fail10:
    LogError("fail10");

fail9:
    LogError("fail9");

    RegCloseKey(Context->RequestKey);

fail8:
    LogError("fail8");

    free(RequestKeyName);

fail7:
    LogError("fail7");

    CloseHandle(Context->ResponseEvent);

fail6:
    LogError("fail6");

    CloseHandle(Context->RequestEvent);

fail5:
    LogError("fail5");

    CloseHandle(Context->StopEvent);

fail4:
    LogError("fail4");

    ReportStatus(SERVICE_STOPPED, GetLastError(), 0);

fail3:
    LogError("fail3");

fail2:
    LogError("fail2");

    RegCloseKey(Context->ParametersKey);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    TraceLoggingUnregister(MonitorTraceLoggingProvider);
}

static BOOL
MonitorCreate(
    VOID
    )
{
    SC_HANDLE   SCManager;
    SC_HANDLE   Service;
    TCHAR       Path[MAX_PATH];
    HRESULT     Error;

    LogInfo("====>");

    if(!GetModuleFileName(NULL, Path, MAX_PATH))
        goto fail1;

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail2;

    Service = CreateService(SCManager,
                            _T(MONITOR_NAME),
                            _T(MONITOR_DISPLAYNAME),
                            SERVICE_ALL_ACCESS,
                            SERVICE_WIN32_OWN_PROCESS,
                            SERVICE_AUTO_START,
                            SERVICE_ERROR_NORMAL,
                            Path,
                            NULL,
                            NULL,
                            NULL,
                            NULL,
                            NULL);

    if (Service == NULL)
        goto fail3;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    LogInfo("<====");

    return TRUE;

fail3:
    LogError("fail3");

    CloseServiceHandle(SCManager);

fail2:
    LogError("fail2");

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorDelete(
    VOID
    )
{
    SC_HANDLE           SCManager;
    SC_HANDLE           Service;
    BOOL                Success;
    SERVICE_STATUS      Status;
    HRESULT             Error;

    LogInfo("====>");

    SCManager = OpenSCManager(NULL,
                              NULL,
                              SC_MANAGER_ALL_ACCESS);

    if (SCManager == NULL)
        goto fail1;

    Service = OpenService(SCManager,
                          _T(MONITOR_NAME),
                          SERVICE_ALL_ACCESS);

    if (Service == NULL)
        goto fail2;

    Success = ControlService(Service,
                             SERVICE_CONTROL_STOP,
                             &Status);

    if (!Success)
        goto fail3;

    Success = DeleteService(Service);

    if (!Success)
        goto fail4;

    CloseServiceHandle(Service);
    CloseServiceHandle(SCManager);

    LogInfo("<====");

    return TRUE;

fail4:
    LogError("fail4");

fail3:
    LogError("fail3");

    CloseServiceHandle(Service);

fail2:
    LogError("fail2");

    CloseServiceHandle(SCManager);

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

static BOOL
MonitorEntry(
    VOID
    )
{
    SERVICE_TABLE_ENTRY Table[] = {
        { _T(MONITOR_NAME), MonitorMain },
        { NULL, NULL }
    };
    HRESULT             Error;

    LogInfo("%s (%s) ====>",
            _T(MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR),
            _T(DAY_STR "/" MONTH_STR "/" YEAR_STR));

    if (!StartServiceCtrlDispatcher(Table))
        goto fail1;

    LogInfo("%s (%s) <====",
            _T(MAJOR_VERSION_STR "." MINOR_VERSION_STR "." MICRO_VERSION_STR "." BUILD_NUMBER_STR),
            _T(DAY_STR "/" MONTH_STR "/" YEAR_STR));

    return TRUE;

fail1:
    Error = GetLastError();

    {
        PTSTR   Message;
        Message = GetErrorMessage(Error);
        LogError("fail1 (%s)", Message);
        LocalFree(Message);
    }

    return FALSE;
}

int CALLBACK
_tWinMain(
    _In_        HINSTANCE   Current,
    _In_opt_    HINSTANCE   Previous,
    _In_        LPTSTR      CmdLine,
    _In_        int         CmdShow
    )
{
    BOOL                    Success;

    UNREFERENCED_PARAMETER(Current);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CmdShow);

    if (_tcslen(CmdLine) != 0) {
         if (_tcsicmp(CmdLine, TEXT("create")) == 0)
             Success = MonitorCreate();
         else if (_tcsicmp(CmdLine, TEXT("delete")) == 0)
             Success = MonitorDelete();
         else
             Success = FALSE;
    } else
        Success = MonitorEntry();

    return Success ? 0 : 1;
}
