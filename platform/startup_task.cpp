#include "platform/startup_task.hpp"

#define SECURITY_WIN32

#include <string>

#include <windows.h>
#include <taskschd.h>
#include <security.h>
#include <comdef.h>
#include <comutil.h>
#include <wrl/client.h>

#include "core/log.hpp"

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "secur32.lib")

namespace sam::platform::startup_task {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kTaskName[] = L"Luminary Steam Account Manager";

// Tolerate an existing apartment; we usually own COM on this thread.
struct ComInit {
    HRESULT hr;
    bool owned = false;
    ComInit() {
        hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) {
            owned = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            hr = S_OK;
        }
    }
    ~ComInit() { if (owned) ::CoUninitialize(); }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
};

std::wstring current_exe_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(buf, n);
}

// SAM-compatible name (DOMAIN\user or MicrosoftAccount\...); Task Scheduler
// accepts it for local, domain and Microsoft accounts alike.
std::wstring current_user_sam() {
    wchar_t buf[512];
    ULONG n = ARRAYSIZE(buf);
    if (::GetUserNameExW(NameSamCompatible, buf, &n)) {
        return std::wstring(buf, n);
    }
    wchar_t user[256];
    DWORD un = ARRAYSIZE(user);
    if (::GetUserNameW(user, &un) && un > 0) {
        return std::wstring(user, un - 1);
    }
    return {};
}

ComPtr<ITaskFolder> connect_root(ComPtr<ITaskService>& svc) {
    if (FAILED(::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(svc.GetAddressOf())))) {
        return nullptr;
    }
    if (FAILED(svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) {
        return nullptr;
    }
    ComPtr<ITaskFolder> folder;
    if (FAILED(svc->GetFolder(_bstr_t(L"\\"), folder.GetAddressOf()))) {
        return nullptr;
    }
    return folder;
}

bool create_task(const std::wstring& args, bool persistent) {
    const std::wstring exe = current_exe_path();
    if (exe.empty()) return false;

    ComInit com;
    if (FAILED(com.hr)) return false;

    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> folder = connect_root(svc);
    if (!folder) return false;

    ComPtr<ITaskDefinition> task;
    if (FAILED(svc->NewTask(0, task.GetAddressOf()))) return false;

    if (ComPtr<IRegistrationInfo> reg; SUCCEEDED(task->get_RegistrationInfo(reg.GetAddressOf())) && reg) {
        reg->put_Author(_bstr_t(L"Luminary"));
        reg->put_Description(_bstr_t(
            persistent ? L"Opens the Steam Account Manager at logon."
                       : L"Refreshes Steam account data in the background at logon."));
    }

    if (ComPtr<IPrincipal> principal; SUCCEEDED(task->get_Principal(principal.GetAddressOf())) && principal) {
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }

    if (ComPtr<ITaskSettings> settings; SUCCEEDED(task->get_Settings(settings.GetAddressOf())) && settings) {
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        // The GUI runs indefinitely; a time limit would kill it. PT0S = no limit.
        settings->put_ExecutionTimeLimit(_bstr_t(persistent ? L"PT0S" : L"PT5M"));
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_Enabled(VARIANT_TRUE);
    }

    const std::wstring user = current_user_sam();

    {
        ComPtr<ITriggerCollection> triggers;
        if (FAILED(task->get_Triggers(triggers.GetAddressOf())) || !triggers) return false;
        ComPtr<ITrigger> trigger;
        if (FAILED(triggers->Create(TASK_TRIGGER_LOGON, trigger.GetAddressOf())) || !trigger) return false;
        ComPtr<ILogonTrigger> logon;
        if (SUCCEEDED(trigger.As(&logon)) && logon) {
            if (!user.empty()) logon->put_UserId(_bstr_t(user.c_str()));
            logon->put_Delay(_bstr_t(L"PT15S"));
            logon->put_Id(_bstr_t(L"LogonTrigger"));
        }
    }

    {
        ComPtr<IActionCollection> actions;
        if (FAILED(task->get_Actions(actions.GetAddressOf())) || !actions) return false;
        ComPtr<IAction> action;
        if (FAILED(actions->Create(TASK_ACTION_EXEC, action.GetAddressOf())) || !action) return false;
        ComPtr<IExecAction> exec;
        if (FAILED(action.As(&exec)) || !exec) return false;
        exec->put_Path(_bstr_t(exe.c_str()));
        if (!args.empty()) exec->put_Arguments(_bstr_t(args.c_str()));
    }

    ComPtr<IRegisteredTask> registered;
    const HRESULT hr = folder->RegisterTaskDefinition(
        _bstr_t(kTaskName), task.Get(), TASK_CREATE_OR_UPDATE,
        user.empty() ? _variant_t() : _variant_t(user.c_str()),
        _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""),
        registered.GetAddressOf());
    if (FAILED(hr)) {
        SAM_LOG_ERROR("startup_task: RegisterTaskDefinition failed: 0x{:08x}",
                      static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

bool delete_task() {
    ComInit com;
    if (FAILED(com.hr)) return false;

    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> folder = connect_root(svc);
    if (!folder) return false;

    const HRESULT hr = folder->DeleteTask(_bstr_t(kTaskName), 0);
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        SAM_LOG_ERROR("startup_task: DeleteTask failed: 0x{:08x}",
                      static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

}  // namespace

bool set_run_at_logon(bool enable, const std::wstring& args, bool persistent) {
    return enable ? create_task(args, persistent) : delete_task();
}

bool is_run_at_logon_enabled() {
    ComInit com;
    if (FAILED(com.hr)) return false;

    ComPtr<ITaskService> svc;
    ComPtr<ITaskFolder> folder = connect_root(svc);
    if (!folder) return false;

    ComPtr<IRegisteredTask> task;
    const HRESULT hr = folder->GetTask(_bstr_t(kTaskName), task.GetAddressOf());
    return SUCCEEDED(hr) && task;
}

}  // namespace sam::platform::startup_task
