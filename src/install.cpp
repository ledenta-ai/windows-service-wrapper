/*
 * install.cpp
 *
 * SCM management helpers: install, uninstall, start, stop.
 */

#include "service-wrapper.h"
#include <strsafe.h>
#include <cstdio>

#pragma comment(lib, "advapi32.lib")

// -----------------------------------------------------------------------
//  Internal helpers
// -----------------------------------------------------------------------
static bool SetDescription(SC_HANDLE h_svc, const std::wstring& description)
{
    if (description.empty()) {
        return true;
    }

    SERVICE_DESCRIPTIONW sd = { const_cast<LPWSTR>(description.c_str()) };
    return ChangeServiceConfig2W(h_svc, SERVICE_CONFIG_DESCRIPTION, &sd) != FALSE;
}

// -----------------------------------------------------------------------
//  CmdInstall
// -----------------------------------------------------------------------
int CmdInstall(const ServiceConfig& config, const std::wstring& ini_path)
{
    if (config.m_executable.empty()) {
        fwprintf(stderr,
            L"ERROR: 'Executable' key is missing or empty in %s\n",
            ini_path.c_str());
        return 1;
    }

    wchar_t self_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self_path, _countof(self_path));

    wchar_t bin_path[MAX_PATH * 2] = {};
    StringCchPrintfW(bin_path, _countof(bin_path), L"\"%s\" --run", self_path);

    SC_HANDLE h_scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!h_scm) {
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE h_svc = CreateServiceW(
        h_scm,
        config.m_service_name.c_str(),
        config.m_display_name.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        bin_path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    if (!h_svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            fwprintf(stderr,
                L"Service \"%s\" already exists. Uninstall first.\n",
                config.m_service_name.c_str());
        } else {
            fwprintf(stderr, L"CreateService failed: %lu\n", err);
        }
        CloseServiceHandle(h_scm);
        return 1;
    }

    SetDescription(h_svc, config.m_description);

    fwprintf(stdout,
        L"Service \"%s\" installed successfully.\n"
        L"  Binary : %s\n"
        L"  Wraps  : %s\n",
        config.m_service_name.c_str(),
        bin_path,
        config.m_executable.c_str());

    CloseServiceHandle(h_svc);
    CloseServiceHandle(h_scm);
    return 0;
}

// -----------------------------------------------------------------------
//  CmdUninstall
// -----------------------------------------------------------------------
int CmdUninstall(const std::wstring& service_name)
{
    SC_HANDLE h_scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!h_scm) {
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE h_svc = OpenServiceW(
        h_scm,
        service_name.c_str(),
        SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);

    if (!h_svc) {
        fwprintf(stderr,
            L"OpenService(\"%s\") failed: %lu\n",
            service_name.c_str(), GetLastError());
        CloseServiceHandle(h_scm);
        return 1;
    }

    SERVICE_STATUS ss = {};
    bool is_running = QueryServiceStatus(h_svc, &ss)
        && ss.dwCurrentState != SERVICE_STOPPED;

    if (is_running) {
        ControlService(h_svc, SERVICE_CONTROL_STOP, &ss);

        for (int i = 0; i < 30; ++i) {
            Sleep(500);
            if (QueryServiceStatus(h_svc, &ss) && ss.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
        }
    }

    BOOL deleted = DeleteService(h_svc);
    if (deleted) {
        fwprintf(stdout, L"Service \"%s\" removed.\n", service_name.c_str());
    } else {
        fwprintf(stderr, L"DeleteService failed: %lu\n", GetLastError());
    }

    CloseServiceHandle(h_svc);
    CloseServiceHandle(h_scm);
    return deleted ? 0 : 1;
}

// -----------------------------------------------------------------------
//  CmdStart
// -----------------------------------------------------------------------
int CmdStart(const std::wstring& service_name)
{
    SC_HANDLE h_scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!h_scm) {
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE h_svc = OpenServiceW(
        h_scm,
        service_name.c_str(),
        SERVICE_START | SERVICE_QUERY_STATUS);

    if (!h_svc) {
        fwprintf(stderr,
            L"OpenService(\"%s\") failed: %lu\n",
            service_name.c_str(), GetLastError());
        CloseServiceHandle(h_scm);
        return 1;
    }

    BOOL started = StartServiceW(h_svc, 0, nullptr);
    if (!started && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        fwprintf(stderr, L"StartService failed: %lu\n", GetLastError());
        CloseServiceHandle(h_svc);
        CloseServiceHandle(h_scm);
        return 1;
    }

    SERVICE_STATUS ss = {};
    for (int i = 0; i < 60; ++i) {
        Sleep(500);
        QueryServiceStatus(h_svc, &ss);
        if (ss.dwCurrentState == SERVICE_RUNNING) {
            break;
        }
    }

    if (ss.dwCurrentState == SERVICE_RUNNING) {
        fwprintf(stdout, L"Service \"%s\" is running.\n", service_name.c_str());
    } else {
        fwprintf(stderr,
            L"Service did not reach RUNNING state (state=%lu).\n",
            ss.dwCurrentState);
    }

    CloseServiceHandle(h_svc);
    CloseServiceHandle(h_scm);
    return (ss.dwCurrentState == SERVICE_RUNNING) ? 0 : 1;
}

// -----------------------------------------------------------------------
//  CmdStop
// -----------------------------------------------------------------------
int CmdStop(const std::wstring& service_name)
{
    SC_HANDLE h_scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!h_scm) {
        fwprintf(stderr, L"OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    SC_HANDLE h_svc = OpenServiceW(
        h_scm,
        service_name.c_str(),
        SERVICE_STOP | SERVICE_QUERY_STATUS);

    if (!h_svc) {
        fwprintf(stderr,
            L"OpenService(\"%s\") failed: %lu\n",
            service_name.c_str(), GetLastError());
        CloseServiceHandle(h_scm);
        return 1;
    }

    SERVICE_STATUS ss = {};
    BOOL controlled = ControlService(h_svc, SERVICE_CONTROL_STOP, &ss);
    if (!controlled) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_NOT_ACTIVE) {
            fwprintf(stderr, L"ControlService(STOP) failed: %lu\n", err);
            CloseServiceHandle(h_svc);
            CloseServiceHandle(h_scm);
            return 1;
        }
    }

    for (int i = 0; i < 60; ++i) {
        Sleep(500);
        QueryServiceStatus(h_svc, &ss);
        if (ss.dwCurrentState == SERVICE_STOPPED) {
            break;
        }
    }

    if (ss.dwCurrentState == SERVICE_STOPPED) {
        fwprintf(stdout, L"Service \"%s\" stopped.\n", service_name.c_str());
    } else {
        fwprintf(stderr,
            L"Service did not stop (state=%lu).\n",
            ss.dwCurrentState);
    }

    CloseServiceHandle(h_svc);
    CloseServiceHandle(h_scm);
    return (ss.dwCurrentState == SERVICE_STOPPED) ? 0 : 1;
}

// -----------------------------------------------------------------------
//  PrintUsage
// -----------------------------------------------------------------------
void PrintUsage(const wchar_t* exe_name)
{
    fwprintf(stdout,
        L"ServiceWrapper v%s\n\n"
        L"Usage: %s <command>\n\n"
        L"Commands:\n"
        L"  --install     Install the service (reads %s)\n"
        L"  --uninstall   Remove the service\n"
        L"  --start       Start the service\n"
        L"  --stop        Stop the service\n"
        L"  --run         (internal) Run as a service process\n"
        L"  --help        Show this help\n\n"
        L"Configuration file: %s  (same directory as this executable)\n",
        SW_VERSION,
        exe_name,
        SW_CONFIG_FILE,
        SW_CONFIG_FILE);
}
