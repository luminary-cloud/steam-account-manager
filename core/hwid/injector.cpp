#include "core/hwid/injector.hpp"

#include <cstring>
#include <format>
#include <vector>

#include <windows.h>
#include <winternl.h>

#include "core/hwid/gen/hwid_dll_bytes.hpp"

namespace sam::core::hwid {

namespace {

using FnLoadLibraryA = HINSTANCE(WINAPI*)(const char*);
using FnGetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using FnRtlAddFunctionTable = BOOL(WINAPIV*)(PRUNTIME_FUNCTION, DWORD, DWORD64);
using FnDllEntryPoint = BOOL(WINAPI*)(void*, DWORD, void*);
using FnNtCreateThreadEx = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, void*, HANDLE,
    LPTHREAD_START_ROUTINE, LPVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, void*);

struct MappingData {
    FnLoadLibraryA pLoadLibraryA;
    FnGetProcAddress pGetProcAddress;
    FnRtlAddFunctionTable pRtlAddFunctionTable;
    BYTE* pbase;
    HINSTANCE hMod;
    DWORD fdwReasonParam;
    LPVOID reservedParam;
    BOOL SEHSupport;
};

#define RELOC_FLAG64(RelInfo) ((RelInfo >> 0x0C) == IMAGE_REL_BASED_DIR64)

#pragma runtime_checks("", off)
#pragma optimize("", off)
__declspec(noinline) void __stdcall Shellcode(MappingData* pData) {
    if (!pData)
        return;

    BYTE* pBase = pData->pbase;
    auto* pOpt = &reinterpret_cast<IMAGE_NT_HEADERS*>(
        pBase + reinterpret_cast<IMAGE_DOS_HEADER*>((uintptr_t)pBase)->e_lfanew)->OptionalHeader;

    auto _LoadLibraryA = pData->pLoadLibraryA;
    auto _GetProcAddress = pData->pGetProcAddress;
    auto _RtlAddFunctionTable = pData->pRtlAddFunctionTable;
    auto _DllMain = reinterpret_cast<FnDllEntryPoint>(pBase + pOpt->AddressOfEntryPoint);

    BYTE* LocationDelta = pBase - pOpt->ImageBase;
    if (LocationDelta) {
        if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
            auto* pRelocData = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
            const auto* pRelocEnd = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                reinterpret_cast<uintptr_t>(pRelocData) +
                pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);
            while (pRelocData < pRelocEnd && pRelocData->SizeOfBlock) {
                UINT count = (pRelocData->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                WORD* pRelInfo = reinterpret_cast<WORD*>(pRelocData + 1);
                for (UINT i = 0; i != count; ++i, ++pRelInfo) {
                    if (RELOC_FLAG64(*pRelInfo)) {
                        UINT_PTR* pPatch = reinterpret_cast<UINT_PTR*>(
                            pBase + pRelocData->VirtualAddress + ((*pRelInfo) & 0xFFF));
                        *pPatch += reinterpret_cast<UINT_PTR>(LocationDelta);
                    }
                }
                pRelocData = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<BYTE*>(pRelocData) + pRelocData->SizeOfBlock);
            }
        }
    }

    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        auto* pImportDescr = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        while (pImportDescr->Name) {
            char* szMod = reinterpret_cast<char*>(pBase + pImportDescr->Name);
            HINSTANCE hDll = _LoadLibraryA(szMod);

            ULONG_PTR* pThunkRef = reinterpret_cast<ULONG_PTR*>(
                pBase + pImportDescr->OriginalFirstThunk);
            ULONG_PTR* pFuncRef = reinterpret_cast<ULONG_PTR*>(
                pBase + pImportDescr->FirstThunk);

            if (!pImportDescr->OriginalFirstThunk)
                pThunkRef = pFuncRef;

            for (; *pThunkRef; ++pThunkRef, ++pFuncRef) {
                if (IMAGE_SNAP_BY_ORDINAL(*pThunkRef)) {
                    *pFuncRef = (ULONG_PTR)_GetProcAddress(
                        hDll, reinterpret_cast<char*>(*pThunkRef & 0xFFFF));
                } else {
                    auto* pImport = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                        pBase + (*pThunkRef));
                    *pFuncRef = (ULONG_PTR)_GetProcAddress(hDll, pImport->Name);
                }
            }
            ++pImportDescr;
        }
    }

    if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto* pTLS = reinterpret_cast<IMAGE_TLS_DIRECTORY*>(
            pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        auto* pCallback = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(pTLS->AddressOfCallBacks);
        for (; pCallback && *pCallback; ++pCallback)
            (*pCallback)(pBase, DLL_PROCESS_ATTACH, nullptr);
    }

    bool exceptionFailed = false;
    if (pData->SEHSupport) {
        auto excep = pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (excep.Size) {
            if (!_RtlAddFunctionTable(
                    reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY*>(pBase + excep.VirtualAddress),
                    excep.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY),
                    (DWORD64)pBase)) {
                exceptionFailed = true;
            }
        }
    }

    _DllMain(pBase, pData->fdwReasonParam, pData->reservedParam);

    if (exceptionFailed)
        pData->hMod = reinterpret_cast<HINSTANCE>(0x505050);
    else
        pData->hMod = reinterpret_cast<HINSTANCE>(pBase);
}

__declspec(noinline) void __stdcall ShellcodeEnd() { }
#pragma optimize("", on)
#pragma runtime_checks("", restore)

InjectResult map_into(HANDLE hProc, const BYTE* dll_data, SIZE_T dll_size) {
    if (reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_data)->e_magic != 0x5A4D)
        return {false, "Invalid DLL magic"};

    auto* pNtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        dll_data + reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_data)->e_lfanew);
    auto* pOptHeader = &pNtHeaders->OptionalHeader;
    auto* pFileHeader = &pNtHeaders->FileHeader;

    if (pFileHeader->Machine != IMAGE_FILE_MACHINE_AMD64)
        return {false, "DLL is not x64"};

    BYTE* pTargetBase = static_cast<BYTE*>(VirtualAllocEx(
        hProc, nullptr, pOptHeader->SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!pTargetBase)
        return {false, std::format("VirtualAllocEx failed: {}", GetLastError())};

    if (!WriteProcessMemory(hProc, pTargetBase, dll_data, 0x1000, nullptr)) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        return {false, "Failed to write PE header"};
    }

    auto* pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
    for (UINT i = 0; i < pFileHeader->NumberOfSections; ++i, ++pSectionHeader) {
        if (pSectionHeader->SizeOfRawData) {
            if (!WriteProcessMemory(hProc,
                    pTargetBase + pSectionHeader->VirtualAddress,
                    dll_data + pSectionHeader->PointerToRawData,
                    pSectionHeader->SizeOfRawData, nullptr)) {
                VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
                return {false, "Failed to write section"};
            }
        }
    }

    MappingData data{};
    data.pLoadLibraryA = LoadLibraryA;
    data.pGetProcAddress = GetProcAddress;
    data.pRtlAddFunctionTable = reinterpret_cast<FnRtlAddFunctionTable>(RtlAddFunctionTable);
    data.pbase = pTargetBase;
    data.fdwReasonParam = DLL_PROCESS_ATTACH;
    data.reservedParam = nullptr;
    data.SEHSupport = TRUE;

    BYTE* pMappingData = static_cast<BYTE*>(VirtualAllocEx(
        hProc, nullptr, sizeof(MappingData), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pMappingData) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        return {false, "Failed to allocate mapping data"};
    }

    if (!WriteProcessMemory(hProc, pMappingData, &data, sizeof(MappingData), nullptr)) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);
        return {false, "Failed to write mapping data"};
    }

    auto shellcode_size = reinterpret_cast<uintptr_t>(ShellcodeEnd) -
                          reinterpret_cast<uintptr_t>(Shellcode);
    if (shellcode_size < 16 || shellcode_size > 0x10000)
        shellcode_size = 0x1000;

    void* pShellcode = VirtualAllocEx(
        hProc, nullptr, shellcode_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pShellcode) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);
        return {false, "Failed to allocate shellcode"};
    }

    if (!WriteProcessMemory(hProc, pShellcode, Shellcode, shellcode_size, nullptr)) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pShellcode, 0, MEM_RELEASE);
        return {false, "Failed to write shellcode"};
    }

    auto NtCreateThreadEx = reinterpret_cast<FnNtCreateThreadEx>(
        ::GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateThreadEx"));

    HANDLE hThread = nullptr;
    if (NtCreateThreadEx) {
        NtCreateThreadEx(&hThread, THREAD_ALL_ACCESS, nullptr, hProc,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(pShellcode),
            pMappingData, 0, 0, 0, 0, nullptr);
    }
    if (!hThread) {
        hThread = CreateRemoteThread(hProc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(pShellcode),
            pMappingData, 0, nullptr);
    }
    if (!hThread) {
        VirtualFreeEx(hProc, pTargetBase, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pShellcode, 0, MEM_RELEASE);
        return {false, std::format("Thread creation failed: {}", GetLastError())};
    }
    CloseHandle(hThread);

    HINSTANCE hCheck = nullptr;
    for (int tries = 0; tries < 500; ++tries) {
        DWORD exitcode = 0;
        GetExitCodeProcess(hProc, &exitcode);
        if (exitcode != STILL_ACTIVE)
            return {false, std::format("Target process exited: {}", exitcode)};

        MappingData check{};
        ReadProcessMemory(hProc, pMappingData, &check, sizeof(check), nullptr);
        hCheck = check.hMod;

        if (hCheck) break;
        Sleep(10);
    }

    if (!hCheck) {
        VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);
        VirtualFreeEx(hProc, pShellcode, 0, MEM_RELEASE);
        return {false, "Shellcode timed out"};
    }

    BYTE zero_page[0x1000]{};

    WriteProcessMemory(hProc, pTargetBase, zero_page, 0x1000, nullptr);

    pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
    for (UINT i = 0; i < pFileHeader->NumberOfSections; ++i, ++pSectionHeader) {
        if (pSectionHeader->Misc.VirtualSize) {
            if (strcmp(reinterpret_cast<const char*>(pSectionHeader->Name), ".rsrc") == 0 ||
                strcmp(reinterpret_cast<const char*>(pSectionHeader->Name), ".reloc") == 0) {
                WriteProcessMemory(hProc,
                    pTargetBase + pSectionHeader->VirtualAddress,
                    zero_page,
                    (std::min)(static_cast<DWORD>(sizeof(zero_page)),
                               pSectionHeader->Misc.VirtualSize),
                    nullptr);
            }
        }
    }

    pSectionHeader = IMAGE_FIRST_SECTION(pNtHeaders);
    for (UINT i = 0; i < pFileHeader->NumberOfSections; ++i, ++pSectionHeader) {
        if (pSectionHeader->Misc.VirtualSize) {
            DWORD oldp = 0;
            DWORD prot = PAGE_READONLY;
            if (pSectionHeader->Characteristics & IMAGE_SCN_MEM_WRITE)
                prot = PAGE_READWRITE;
            else if (pSectionHeader->Characteristics & IMAGE_SCN_MEM_EXECUTE)
                prot = PAGE_EXECUTE_READ;
            VirtualProtectEx(hProc,
                pTargetBase + pSectionHeader->VirtualAddress,
                pSectionHeader->Misc.VirtualSize, prot, &oldp);
        }
    }

    DWORD oldp = 0;
    VirtualProtectEx(hProc, pTargetBase,
        IMAGE_FIRST_SECTION(pNtHeaders)->VirtualAddress, PAGE_READONLY, &oldp);

    VirtualFreeEx(hProc, pShellcode, 0, MEM_RELEASE);
    VirtualFreeEx(hProc, pMappingData, 0, MEM_RELEASE);

    return {true, {}};
}

}  // namespace

InjectResult inject_hwid_dll(std::uint32_t pid, const std::string& shared_mem_name) {
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc)
        return {false, std::format("OpenProcess failed: {}", GetLastError())};

    std::vector<BYTE> dll_copy(kHwidDll, kHwidDll + kHwidDllSize);

    // Patch the shared memory name placeholder in .data before mapping.
    // The DLL has a char[64] at a known symbol initialized with "SAM_IPC_PLACEHOLDER".
    static constexpr char kPlaceholder[] = "SAM_IPC_PLACEHOLDER";
    for (std::size_t i = 0; i + sizeof(kPlaceholder) <= dll_copy.size(); ++i) {
        if (std::memcmp(dll_copy.data() + i, kPlaceholder, sizeof(kPlaceholder)) == 0) {
            std::memset(dll_copy.data() + i, 0, 64);
            std::size_t n = (std::min)(shared_mem_name.size(), std::size_t{63});
            std::memcpy(dll_copy.data() + i, shared_mem_name.data(), n);
            break;
        }
    }

    auto result = map_into(hProc, dll_copy.data(), dll_copy.size());
    CloseHandle(hProc);
    return result;
}

}  // namespace sam::core::hwid
