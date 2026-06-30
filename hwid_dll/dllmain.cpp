#include <windows.h>
#include "hooks.hpp"
#include "ipc_reader.hpp"

char g_shared_mem_name[64] = "SAM_IPC_PLACEHOLDER";

static DWORD WINAPI attach_thread(LPVOID param) {
    if (ipc::read_profile(g_shared_mem_name))
        hooks::init();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, attach_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
