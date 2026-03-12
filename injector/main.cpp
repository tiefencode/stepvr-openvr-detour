#include <Windows.h>
#include <psapi.h>
#include <iostream>
#include <algorithm>

#define DEFAULT_TARGET_PROCESS_NAME "Monster Titans Playground"

char target_process[MAX_PATH];

DWORD find_process_id()
{
	DWORD processes[1024]{};
	DWORD bytesReturned;
	EnumProcesses(processes, sizeof(processes), &bytesReturned);

	for (DWORD pid : processes)
	{
		std::cout << "Testing " << pid << std::endl;

		HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
			PROCESS_VM_READ,
			FALSE, pid);


		if (hProcess != 0)
		{
			char name[MAX_PATH]{ 0 };
			GetModuleBaseNameA(hProcess, nullptr, name, MAX_PATH);
			CloseHandle(hProcess);

			if (name[0] == 0)
				continue;

			std::cout << "Found name " << name << "\n";

			if (nullptr != strstr(name, target_process))
			{
				return pid;
			}
		}
	}
	return 0;
}

void inject_into(DWORD pid, LPCSTR dll)
{
	const auto kernel32 = GetModuleHandleA("kernel32");
	if (!kernel32)
		throw - 1337; //impossible!

	void* loadlibrarya_ptr = GetProcAddress(kernel32, "LoadLibraryA");

	HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
		PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD,
		FALSE, pid);

	if (hProcess != 0)
	{
		const auto len = strlen(dll) + 1;
		const auto buffer = VirtualAllocEx(hProcess, nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);

		if (buffer)
		{
			SIZE_T wroteBytes = 0;
			if (WriteProcessMemory(hProcess, buffer, dll, len, &wroteBytes))
			{
				CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)loadlibrarya_ptr, buffer, 0, nullptr);
			}

		}

		CloseHandle(hProcess);
	}
}

int main(int argc, char* argv[])
{
	if (argc > 1)
	{
		strcpy(target_process, argv[1]);
	}
	else
	{
		//fallback to the monster game for testing
		strcpy(target_process, DEFAULT_TARGET_PROCESS_NAME);
	}


	char current_directory[MAX_PATH] = {};
	GetCurrentDirectoryA(MAX_PATH, current_directory);

	char dll_path[MAX_PATH] = {};
	GetFullPathNameA(".\\stepvr_detour.dll", MAX_PATH, dll_path, nullptr);

	const auto target_pid = find_process_id();

	inject_into(target_pid, dll_path);

	return 0;
}