//
// author: Lasha Khasaia
// contact: @_qaz_qaz
// license: MIT License
//

#include "stdafx.h"

#pragma comment(lib, "Shlwapi.lib")

#define HOOK_COUNT 50 // ??
#ifndef _WIN64
#define DWORD64 DWORD32 // maybe there is a better way...
#endif

DWORD64 hooks[HOOK_COUNT]{};
HMODULE copyNtdll{};
HMODULE copyKernelBase{};
TCHAR tmp[MAX_PATH + 2]{};
TCHAR sys[MAX_PATH + 2]{};
typedef unsigned char byte;



VOID hookFunction(const CHAR func[], const DWORD index, const TCHAR* libModule) {
	HMODULE lib{};
	if (CompareStringOrdinal(libModule, -1, L"ntdll", -1, FALSE) == CSTR_EQUAL)
	{
		lib = LoadLibrary(L"ntdll");;
	}
	else
	{
		lib = LoadLibrary(L"kernelbase");
	}
	if (!lib) {
		return;
	}
	auto f_addr = static_cast<LPVOID>(GetProcAddress(lib, func));

	csh handle;
	cs_insn *insn;

#if defined (_WIN64)
	const cs_mode mode = CS_MODE_64;
#else
	const cs_mode mode = CS_MODE_32;
#endif

	if (cs_open(CS_ARCH_X86, mode, &handle) == CS_ERR_OK)
	{
		const auto count = cs_disasm(handle, static_cast<byte*>(f_addr), 0x50 /* ? */, uint64_t(f_addr), 0, &insn);
		if (count > 1)
		{
			f_addr = LPVOID(insn[1].address);
			cs_free(insn, count);
		}
		cs_close(&handle);
	}
#if defined(_WIN64)
	byte jmp[] = { 0x48, 0xB8, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFF, 0xE0 };
	DWORD old;
	VirtualProtectEx(GetCurrentProcess(), f_addr, 100, PAGE_EXECUTE_READWRITE, &old);
	memcpy_s(f_addr, 2, jmp, 2);
	*reinterpret_cast<DWORD64*>(static_cast<byte*>(f_addr) + 2) = static_cast<DWORD64>(hooks[index]);
	memcpy_s(static_cast<byte*>(f_addr) + 10, 2, jmp + 10, 2);
	VirtualProtectEx(GetCurrentProcess(), f_addr, 100, old, &old);
#else
	byte jmp[] = { 0x68, 0xCC, 0xCC, 0xCC, 0xCC, 0xC3 };
	DWORD old;
	VirtualProtectEx(GetCurrentProcess(), f_addr, 100, PAGE_EXECUTE_READWRITE, &old);
	memcpy_s(f_addr, 1, jmp, 1);
	*reinterpret_cast<DWORD32*>(static_cast<byte*>(f_addr) + 1) = static_cast<DWORD32>(hooks[index]);
	memcpy_s(static_cast<byte*>(f_addr) + 5, 1, jmp + 5, 1);
	VirtualProtectEx(GetCurrentProcess(), f_addr, 100, old, &old);
#endif
}


// hook functions
LONG WINAPI hookNtClose(_In_ HANDLE Handle)
{
	typedef ULONG WINAPI xNtQueryObject(
		_In_opt_  HANDLE xHandle,
		_In_      DWORD ObjectInformationClass, // ObjectTypeInformation = 0x2
		_Out_opt_ PVOID ObjectInformation,
		_In_      ULONG ObjectInformationLength,
		_Out_opt_ PULONG ReturnLength
	);
	typedef LONG WINAPI NtClose(
		_In_ HANDLE _Handle
	);
	
	// https://web.archive.org/web/20171214081815/http://winapi.freetechsecrets.com/win32/WIN32GetHandleInformation.htm

	DWORD flags{};
	const auto ret = GetHandleInformation(Handle, &flags);

	if (!ret) // invalid handle
	{
		TCHAR msg[0x100]{};
#ifdef _WIN64
		swprintf_s(msg, L"[NtClose] Invalid HANDLE specified by the debuggee - 0x%llx\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.ii\n", DWORD64(Handle));
#else
		swprintf_s(msg, L"[NtClose] Invalid HANDLE specified by the debuggee - 0x%x\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.ii\n", DWORD32(Handle));
#endif
		OutputDebugStringW(msg);
		return STATUS_INVALID_HANDLE; // return success
	}
	const auto close = reinterpret_cast<NtClose*>(GetProcAddress(copyNtdll, "NtClose"));
	return close(Handle);

}


LONG WINAPI hookNtOpenProcess(_Out_ PHANDLE ProcessHandle, _In_ ACCESS_MASK DesiredAccess, _In_ LPVOID ObjectAttributes, _In_opt_ PCLIENT_ID ClientId)
{
	typedef LONG WINAPI xZwOpenProcess(
		_Out_    PHANDLE            xProcessHandle,
		_In_     ACCESS_MASK        xDesiredAccess,
		_In_     LPVOID xObjectAttributes,
		_In_opt_ LPVOID         xClientId
	);

	const auto xOP = reinterpret_cast<xZwOpenProcess*>(GetProcAddress(copyNtdll, "NtOpenProcess"));

	TCHAR fileName[0x100]{};
	// ProcID
	if (!ClientId)
		return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
	const auto procID = DWORD64(ClientId->UniqueProcess);
	const auto hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE)
		return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);

	PROCESSENTRY32 pe = { sizeof(pe) };
	if (Process32First(hSnapshot, &pe)) {
		do {
			if (pe.th32ProcessID == procID) {
				_tcscpy_s(fileName, 0x100, pe.szExeFile);
				break;
			}
		} while (Process32Next(hSnapshot, &pe));
	}
	CloseHandle(hSnapshot);

	if (!_tcscmp(fileName, L"csrss.exe"))
	{
		OutputDebugString(L"[NtOpenProcess] The debuggee attempts to open csrss.exe\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.i\n");
		// return diff value??
	}

	return xOP(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}

LONG WINAPI hookNtCreateFile(
	_Out_ PHANDLE FileHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ POBJECT_ATTRIBUTES ObjectAttributes,
	_Out_ LPVOID IoStatusBlock,
	_In_opt_ PLARGE_INTEGER AllocationSize,
	_In_ ULONG FileAttributes,
	_In_ ULONG ShareAccess,
	_In_ ULONG CreateDisposition,
	_In_ ULONG CreateOptions,
	_In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
	_In_ ULONG EaLength
)
{
	typedef  LONG WINAPI xZwCreateFile(
		_Out_ PHANDLE xFileHandle,
		_In_ ACCESS_MASK xDesiredAccess,
		_In_ POBJECT_ATTRIBUTES xObjectAttributes,
		_Out_ LPVOID xIoStatusBlock,
		_In_opt_ PLARGE_INTEGER xAllocationSize,
		_In_ ULONG xFileAttributes,
		_In_ ULONG xShareAccess,
		_In_ ULONG xCreateDisposition,
		_In_ ULONG xCreateOptions,
		_In_reads_bytes_opt_(xEaLength) PVOID xEaBuffer,
		_In_ ULONG xEaLength
	);

	// is there a better way to remove \??\ ?
	const DWORD64 size = ObjectAttributes->ObjectName->MaximumLength - (4 * sizeof(TCHAR));
	const auto fileName = new TCHAR[size]{};
	_tcscpy_s(fileName, size, ObjectAttributes->ObjectName->Buffer + 4);
	TCHAR cName[MAX_PATH + 2]{};
	DWORD retSize = MAX_PATH + 2;
	QueryFullProcessImageName(GetCurrentProcess(), 0, cName,  &retSize); // GetModuleFileNameEx(GetCurrentProcess(), nullptr, cName, MAX_PATH + 2);

	if (!_tcscmp(fileName, cName))
	{
		OutputDebugString(L"[NtCreateFile] The debuggee attempts to open itself exclusively\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.iii\n");
	}

	const auto devName = new TCHAR[ObjectAttributes->ObjectName->MaximumLength]{};
	_tcscpy_s(devName, ObjectAttributes->ObjectName->MaximumLength, ObjectAttributes->ObjectName->Buffer);
	// TODO: add more checks like this
	if (!_tcscmp(devName, L"\\??\\PROCEXP152"))
	{
		OutputDebugString(L"[NtCreateFile] The debuggee attempts to open PROCESS EXPLORER driver [\\??\\PROCEXP152]\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.B.iii\n");
	}

	// Some debuggers open handle when debuggee loads dll and forget to close handle, we can use this behavior to detect a debugger (not all, but IDA Pro at least)
	// Anti-debugger trick: load a dll via LoadLibrary and try to open the same file for opened for exclusive access.
	TCHAR filePath[0x1000]{};
	_tcscpy_s(filePath, 0x1000, L"[_]");
	_tcscpy_s(filePath + 3, 0x1000 - 3, fileName);

	TCHAR fulltmp[MAX_PATH + 2]{};
	GetFullPathName(tmp, MAX_PATH + 2, fulltmp, nullptr);

	if (_tcscmp(fileName, fulltmp) && _tcscmp(fileName, sys))
		OutputDebugString(filePath);

	delete[] fileName;
	delete[] devName;
	const auto oCf = reinterpret_cast<xZwCreateFile*>(GetProcAddress(copyNtdll, "NtCreateFile"));
	return oCf(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

ULONG WINAPI hookNtSetDebugFilterState(ULONG ComponentId, ULONG Level, BOOLEAN State)
{
	UNREFERENCED_PARAMETER(ComponentId);
	UNREFERENCED_PARAMETER(Level);
	UNREFERENCED_PARAMETER(State);
	// typedef ULONG WINAPI NtSetDebugFilterState(ULONG ComponentId, ULONG Level, BOOLEAN State);
	// auto xNtSetDebugFilterState = reinterpret_cast<NtSetDebugFilterState*>(GetProcAddress(copyNtdll, "NtSetDebugFilterState"));

	OutputDebugString(L"[NtSetDebugFilterState] The debuggee attempts to use NtSetDebugFilterState trick\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.vi\n");

	// return xNtSetDebugFilterState(ComponentId, Level, State);
	return 1; // fake it ;)
}

LONG WINAPI hookNtQueryInformationProcess(
	_In_      HANDLE           ProcessHandle,
	_In_      PROCESSINFOCLASS ProcessInformationClass,
	_Out_     PVOID            ProcessInformation,
	_In_      ULONG            ProcessInformationLength,
	_Out_opt_ PULONG           ReturnLength
)
{
	typedef LONG WINAPI NtQueryInformationProcess(
		_In_      HANDLE           xProcessHandle,
		_In_      PROCESSINFOCLASS xProcessInformationClass,
		_Out_     PVOID            xProcessInformation,
		_In_      ULONG            xProcessInformationLength,
		_Out_opt_ PULONG           xReturnLength
	);

	const auto oNQi = reinterpret_cast<NtQueryInformationProcess*>(GetProcAddress(copyNtdll, "NtQueryInformationProcess"));

	const auto status = oNQi(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
	if (ProcessInformationClass == ProcessDebugPort)
	{
		OutputDebugString(L"[ProcessDebugPort] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.a\n");
		*static_cast<DWORD64*>(ProcessInformation) = 0; // fake it ;)
	}
	if (ProcessInformationClass == ProcessDebugObjectHandle)
	{
		OutputDebugString(L"[ProcessDebugObjectHandle] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.b\n");
		*static_cast<DWORD64*>(ProcessInformation) = 0; // fake it ;)
	}

	if (ProcessInformationClass == ProcessDebugFlags)
	{
		OutputDebugString(L"[ProcessDebugFlags] The debuggee attempts to detect a debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.D.viii.c\n");
		*static_cast<DWORD*>(ProcessInformation) = 1; // fake it ;)
	}

	return status;
}

LONG WINAPI hookNtQuerySystemInformation(
	ULONG SystemInformationClass,
	PVOID SystemInformation,
	ULONG SystemInformationLength,
	PULONG ReturnLength)
{
	typedef LONG WINAPI pNtQuerySystemInformation(
		ULONG xSystemInformationClass,
		PVOID xSystemInformation,
		ULONG xSystemInformationLength,
		PULONG xReturnLength);

	const auto querySysInfo = reinterpret_cast<pNtQuerySystemInformation*>(GetProcAddress(copyNtdll, "NtQuerySystemInformation"));
	const ULONG SystemKernelDebuggerInformation = 0x23;

	const auto status = querySysInfo(SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);

	if (SystemInformationClass == SystemKernelDebuggerInformation)
	{
		OutputDebugString(L"[SystemKernelDebuggerInformation] The debuggee attempts to detect a kernel debugger\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.E.iii\n");

		SYSTEM_KERNEL_DEBUGGER_INFORMATION* sysKernelInfo = static_cast<SYSTEM_KERNEL_DEBUGGER_INFORMATION*>(SystemInformation);
		sysKernelInfo->KernelDebuggerEnabled = FALSE; // fake it ;)
		sysKernelInfo->KernelDebuggerNotPresent = TRUE;
	}

	return status;
}

LONG WINAPI hookNtSetInformationThread(
	_In_ HANDLE          ThreadHandle,
	_In_ THREADINFOCLASS ThreadInformationClass,
	_In_ PVOID           ThreadInformation,
	_In_ ULONG           ThreadInformationLength
)
{
	typedef LONG WINAPI NtSetInformationThread(
		_In_ HANDLE          xThreadHandle,
		_In_ THREADINFOCLASS xThreadInformationClass,
		_In_ PVOID           xThreadInformation,
		_In_ ULONG           xThreadInformationLength
	);
	const auto setInfoThread = reinterpret_cast<NtSetInformationThread*>(GetProcAddress(copyNtdll, "NtSetInformationThread"));

	if (ThreadInformationClass == ThreadHideFromDebugger)
	{
		OutputDebugString(L"[ThreadHideFromDebugger] The debuggee attempts to hide/escape\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.F.iii\n");
		return 1;
	}
	else // .... 
	{
		return setInfoThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);
	}

}

LONG WINAPI hookNtCreateUserProcess(PHANDLE ProcessHandle, PHANDLE ThreadHandle, ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes, POBJECT_ATTRIBUTES ThreadObjectAttributes, ULONG ulProcessFlags, ULONG ulThreadFlags, PRTL_USER_PROCESS_PARAMETERS RtlUserProcessParameters, LPVOID PsCreateInfo, LPVOID PsAttributeList)
{
	typedef LONG WINAPI xNtCreateUserProcess(PHANDLE xProcessHandle, PHANDLE xThreadHandle, ACCESS_MASK xProcessDesiredAccess, ACCESS_MASK xThreadDesiredAccess, POBJECT_ATTRIBUTES xProcessObjectAttributes, POBJECT_ATTRIBUTES xThreadObjectAttributes, ULONG xulProcessFlags, ULONG xulThreadFlags, PRTL_USER_PROCESS_PARAMETERS xRtlUserProcessParameters, LPVOID xPsCreateInfo, LPVOID xPsAttributeList);

	const auto createUserProc = reinterpret_cast<xNtCreateUserProcess*>(GetProcAddress(copyNtdll, "NtCreateUserProcess"));

	TCHAR msg[0x1000]{};
	_tcscat_s(msg, L"[NtCreateUserprocess] The debuggee attempts to create a new process: ");

	const auto tmpvar = new TCHAR[RtlUserProcessParameters->ImagePathName.MaximumLength]{};
	_tcscpy_s(tmpvar, RtlUserProcessParameters->ImagePathName.Length, RtlUserProcessParameters->ImagePathName.Buffer);

	_tcscat_s(msg, tmpvar);
	_tcscat_s(msg, L"\n\tref: The \"Ultimate\" Anti-Debugging Reference: 7.G.i\n");

	OutputDebugString(msg);

	// return STATUS_ACCESS_DENIED; // TODO: better way to deal with a child process
	
	// Job blocks it
	return createUserProc(ProcessHandle, ThreadHandle, ProcessDesiredAccess, ThreadDesiredAccess, ProcessObjectAttributes, ThreadObjectAttributes, ulProcessFlags, ulThreadFlags, RtlUserProcessParameters, PsCreateInfo, PsAttributeList);
}

LONG WINAPI hookNtCreateThreadEx(
	_Out_ PHANDLE ThreadHandle,
	_In_ ACCESS_MASK DesiredAccess,
	_In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
	_In_ HANDLE ProcessHandle,
	_In_ PVOID StartRoutine, // PUSER_THREAD_START_ROUTINE
	_In_opt_ PVOID Argument,
	_In_ ULONG CreateFlags, // THREAD_CREATE_FLAGS_*
	_In_opt_ ULONG_PTR ZeroBits,
	_In_opt_ SIZE_T StackSize,
	_In_opt_ SIZE_T MaximumStackSize,
	_In_opt_ PVOID AttributeList
)
{
	typedef LONG WINAPI xNtCreateThreadEx(
		_Out_ PHANDLE xThreadHandle,
		_In_ ACCESS_MASK xDesiredAccess,
		_In_opt_ POBJECT_ATTRIBUTES xObjectAttributes,
		_In_ HANDLE xProcessHandle,
		_In_ PVOID xStartRoutine, // PUSER_THREAD_START_ROUTINE
		_In_opt_ PVOID xArgument,
		_In_ ULONG xCreateFlags, // THREAD_CREATE_FLAGS_*
		_In_opt_ ULONG_PTR xZeroBits,
		_In_opt_ SIZE_T xStackSize,
		_In_opt_ SIZE_T xMaximumStackSize,
		_In_opt_ PVOID xAttributeList
	);
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER 0x00000004

	const auto exThread = reinterpret_cast<xNtCreateThreadEx*>(GetProcAddress(copyNtdll, "NtCreateThreadEx"));

	if (CreateFlags == THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
	{
		OutputDebugString(L"[NtCreateThreadEx] The debuggee attempts to use THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER flag to hide from us. [FLAG REMOVED]\n\tref: https://goo.gl/4auRMZ \n");

		CreateFlags ^= THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER;
	}

	return exThread(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList);
}

LONG WINAPI hookNtSystemDebugControl(
    IN DEBUG_CONTROL_CODE Command,
    IN PVOID InputBuffer,
    IN ULONG InputBufferLength,
    OUT PVOID OutputBuffer,
    IN ULONG OutputBufferLength,
    OUT PULONG ReturnLength
)
{
	typedef LONG WINAPI
		xNtSystemDebugControl(
			IN DEBUG_CONTROL_CODE xCommand,
			IN PVOID xInputBuffer,
			IN ULONG xInputBufferLength,
			OUT PVOID xOutputBuffer,
			IN ULONG xOutputBufferLength,
			OUT PULONG xReturnLength
		);

	const auto ntSysDbgCtrl = reinterpret_cast<xNtSystemDebugControl*>(GetProcAddress(copyNtdll, "NtSystemDebugControl"));

	if (Command != 0x1d)
	{
		OutputDebugString(L"[NtSystemDebugControl] The debuggee attempts to detect a debugger\n\tref: https://goo.gl/j4g5pV \n");
	}

	return ntSysDbgCtrl(Command, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength, ReturnLength);
}

BOOL hookNtYieldExecutionAPI()
{
	typedef BOOL xNtYieldExecutionAPI();
	const auto nYExec = reinterpret_cast<xNtYieldExecutionAPI*>(GetProcAddress(copyNtdll, "NtYieldExecutionAPI"));

	OutputDebugStringA("[NtYieldExecutionAPI] Unreliable method for detecting a debugger\n\tref: ref: The \"Ultimate\" Anti-Debugging Reference: 7.D.xiii\n");

	return nYExec();
}

LONG WINAPI hookNtSetLdtEntries ( ULONG Selector1, LDT_ENTRY Entry1, ULONG Selector2, LDT_ENTRY Entry2)
{
	typedef LONG WINAPI xNtSetLdtEntries (
		ULONG xSelector1,
		LDT_ENTRY xEntry1,
		ULONG xSelector2,
		LDT_ENTRY xEntry2
		);
	const auto ntSetLdt = reinterpret_cast<xNtSetLdtEntries*>(GetProcAddress(copyNtdll, "NtSetLdtEntries"));

	OutputDebugString(L"[NtSetLdtEntries] The debugee uses NtSetLdtEntries API\n\tref: https://goo.gl/HaKCfH - 2.1.2\n");

	return ntSetLdt(Selector1, Entry1, Selector2, Entry2);
}

ULONG NTAPI hookNtQueryInformationThread(
	_In_ HANDLE ThreadHandle,
	_In_ THREADINFOCLASS ThreadInformationClass,
	_Out_writes_bytes_(ThreadInformationLength) PVOID ThreadInformation,
	_In_ ULONG ThreadInformationLength,
	_Out_opt_ PULONG ReturnLength
)
{
	typedef ULONG
		NTAPI
		NtQueryInformationThread(
			_In_ HANDLE xThreadHandle,
			_In_ THREADINFOCLASS xThreadInformationClass,
			_Out_writes_bytes_(xThreadInformationLength) PVOID xThreadInformation,
			_In_ ULONG xThreadInformationLength,
			_Out_opt_ PULONG xReturnLength
		);
	const auto NtQrInfThr = reinterpret_cast<NtQueryInformationThread*>(GetProcAddress(copyNtdll, "NtQueryInformationThread"));


	if (ThreadInformationClass == ThreadHideFromDebugger)
	{
		OutputDebugString(L"[NtQueryInformationThread] The debugee tries to detect anti-anti-debug tool\n\tref: https://goo.gl/k4P2a3 \n");
	}

	return NtQrInfThr(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength, ReturnLength);
}

BOOL WINAPI hookIsDebuggerPresent(void)
{
	OutputDebugString(L"[IsDebuggerPresent] The debugee tries to detect a debugger\n\tref: https://goo.gl/cg7Fkm \n");

	return FALSE; // :)
}

BOOL WINAPI hookCheckRemoteDebuggerPresent(
	_In_    HANDLE hProcess,
	_Inout_ PBOOL  pbDebuggerPresent
)
{
	OutputDebugString(L"[CheckRemoteDebuggerPresent] The debugee tries to detect a debugger\n\tref: https://goo.gl/LrUdaG \n");

	typedef BOOL WINAPI xCheckRemoteDebuggerPresent(
		_In_    HANDLE xhProcess,
		_Inout_ PBOOL  xpbDebuggerPresent
	);

	const auto chkRemDbg = reinterpret_cast<xCheckRemoteDebuggerPresent*>(GetProcAddress(copyKernelBase, "CheckRemoteDebuggerPresent"));
	return chkRemDbg(hProcess, pbDebuggerPresent);
}


VOID doWork() {

#ifdef _DEBUG
	MessageBox(nullptr, L"DLL injected successfully", L"Debug Mode", MB_ICONINFORMATION);
#endif

	GetTempPath(MAX_PATH + 2, tmp);
	_tcscat_s(tmp, L"\\ntdllCopy.dll");
	GetSystemDirectory(sys, MAX_PATH + 2);
	_tcscat_s(sys, L"\\ntdll.dll");
	CopyFile(sys, tmp, FALSE);
	copyNtdll = LoadLibrary(tmp); // for us ;)
	if (!copyNtdll)
		return;

	TCHAR tmp2[MAX_PATH + 2]{};
	TCHAR sys2[MAX_PATH + 2]{};
	GetTempPath(MAX_PATH + 2, tmp2);
	_tcscat_s(tmp2, L"\\kernelBaseCopy.dll");
	GetSystemDirectory(sys2, MAX_PATH + 2);
	_tcscat_s(sys2, L"\\kernelbase.dll");
	CopyFile(sys2, tmp2, FALSE);
	copyKernelBase = LoadLibrary(tmp2); // for us ;)
	if (!copyKernelBase)
		return;

	// prepare hook functions
	hooks[0] = DWORD64(hookNtClose); // CloseHandle, NtClose
	hooks[1] = DWORD64(hookNtOpenProcess); // NtOpenProcess for csrss.exe
	hooks[2] = DWORD64(hookNtCreateFile); // ...
	hooks[3] = DWORD64(hookNtSetDebugFilterState);
	hooks[4] = DWORD64(hookNtQueryInformationProcess);
	hooks[5] = DWORD64(hookNtQuerySystemInformation);
	hooks[6] = DWORD64(hookNtSetInformationThread);
	hooks[7] = DWORD64(hookNtCreateUserProcess);
	hooks[8] = DWORD64(hookNtCreateThreadEx);
	hooks[9] = DWORD64(hookNtSystemDebugControl);
	hooks[10] = DWORD64(hookNtYieldExecutionAPI);
	hooks[11] = DWORD64(hookNtSetLdtEntries);
	hooks[12] = DWORD64(hookNtQueryInformationThread);

    hooks[13] = DWORD64(hookIsDebuggerPresent);
	hooks[14] = DWORD64(hookCheckRemoteDebuggerPresent);

	hookFunction("NtClose", 0, L"ntdll");
	hookFunction("NtOpenProcess", 1, L"ntdll");
	hookFunction("NtCreateFile", 2, L"ntdll");
	hookFunction("NtSetDebugFilterState", 3, L"ntdll");
	hookFunction("NtQueryInformationProcess", 4, L"ntdll");
	hookFunction("NtQuerySystemInformation", 5, L"ntdll");
	hookFunction("NtSetInformationThread", 6, L"ntdll");
	hookFunction("NtCreateUserProcess", 7, L"ntdll");
	hookFunction("NtCreateThreadEx", 8, L"ntdll");
	hookFunction("NtSystemDebugControl", 9, L"ntdll");
	hookFunction("NtYieldExecution", 10, L"ntdll");
	hookFunction("NtSetLdtEntries", 11, L"ntdll");
	hookFunction("NtQueryInformationThread", 12, L"ntdll");

	hookFunction("IsDebuggerPresent", 13, L"kernelbase");
	hookFunction("CheckRemoteDebuggerPresent", 14, L"kernelbase");

	//// hey...
	OutputDebugString(L"ImDoneHere");
}



BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved
)
{
	UNREFERENCED_PARAMETER(hModule);
	UNREFERENCED_PARAMETER(lpReserved);

	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
		doWork();

	return TRUE;
}