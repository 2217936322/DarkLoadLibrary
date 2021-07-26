#include "pebutils.h"

ULONG LdrHashEntry(UNICODE_STRING UniName, BOOL XorHash) {
	ULONG ulRes = 0;
	
	RtlHashUnicodeString(
		&UniName,
		TRUE,
		0,
		&ulRes
	);
	
	if (XorHash)
	{
		ulRes &= (LDR_HASH_TABLE_ENTRIES - 1);
	}

	return ulRes;
}

PLDR_DATA_TABLE_ENTRY2 FindLdrTableEntry(
	PCWSTR BaseName
)
{
	PPEB2 pPeb;
	PLDR_DATA_TABLE_ENTRY2 pCurEntry;
	PLIST_ENTRY pListHead, pListEntry;
	
	pPeb = (PPEB2)READ_MEMLOC(PEB_OFFSET);

	if (pPeb == NULL)
	{
		return NULL;
	}

	pListHead = &pPeb->Ldr->InLoadOrderModuleList;
	pListEntry = pListHead->Flink;

	do
	{

		pCurEntry = CONTAINING_RECORD(pListEntry, LDR_DATA_TABLE_ENTRY2, InLoadOrderLinks);
		pListEntry = pListEntry->Flink;

		INT BaseName1 = _wcsnicmp(BaseName, pCurEntry->BaseDllName.Buffer, (pCurEntry->BaseDllName.Length / sizeof(wchar_t)) - 4);
		INT BaseName2 = _wcsnicmp(BaseName, pCurEntry->BaseDllName.Buffer, pCurEntry->BaseDllName.Length / sizeof(wchar_t));

		if (!BaseName1 || !BaseName2)
		{
			return pCurEntry;
		}

	} while (pListEntry != pListHead);

	return NULL;

}

PRTL_RB_TREE FindModuleBaseAddressIndex()
{
	SIZE_T stEnd = 0;
	PRTL_BALANCED_NODE pNode = NULL;
	PRTL_RB_TREE pModBaseAddrIndex = NULL;

	PLDR_DATA_TABLE_ENTRY2 pLdrEntry = FindLdrTableEntry(L"ntdll.dll");

	pNode = &pLdrEntry->BaseAddressIndexNode;

	do
	{
		pNode = (PRTL_BALANCED_NODE)(pNode->ParentValue & (~7));
	} while (pNode->ParentValue & (~7));

	if (!pNode->Red)
	{
		DWORD dwLen = 0;
		SIZE_T stBegin = 0;

		PIMAGE_NT_HEADERS pNtHeaders = RVA(
			PIMAGE_NT_HEADERS, 
			pLdrEntry->DllBase, 
			((PIMAGE_DOS_HEADER)pLdrEntry->DllBase)->e_lfanew
		);

		PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNtHeaders);

		for (INT i = 0; i < pNtHeaders->FileHeader.NumberOfSections; i++)
		{
			if (!strcmp(".data", (LPCSTR)pSection->Name))
			{
				stBegin = (SIZE_T)pLdrEntry->DllBase + pSection->VirtualAddress;
				dwLen = pSection->Misc.VirtualSize;

				break;
			}

			++pSection;
		}

		for (DWORD i = 0; i < dwLen - sizeof(SIZE_T); ++stBegin, ++i) 
		{

			SIZE_T stRet = RtlCompareMemory(
				(PVOID)stBegin, 
				(PVOID)&pNode, 
				sizeof(SIZE_T)
			);

			if (stRet == sizeof(SIZE_T)) 
			{
				stEnd = stBegin;
				break;
			}
		}

		if (stEnd == 0)
		{
			return NULL;
		}

		PRTL_RB_TREE pTree = (PRTL_RB_TREE)stEnd;
		
		if (pTree && pTree->Root && pTree->Min)
		{
			pModBaseAddrIndex = pTree;
		}
	}
	
	return pModBaseAddrIndex;
}

BOOL AddBaseAddressEntry(
	PLDR_DATA_TABLE_ENTRY2 pLdrEntry,
	PVOID lpBaseAddr
)
{

	PRTL_RB_TREE pModBaseAddrIndex = FindModuleBaseAddressIndex();

	if (!pModBaseAddrIndex)
	{
		return FALSE;
	}

	BOOL bRight = FALSE;
	PLDR_DATA_TABLE_ENTRY2 pLdrNode = (PLDR_DATA_TABLE_ENTRY2)((size_t)pModBaseAddrIndex - offsetof(LDR_DATA_TABLE_ENTRY2, BaseAddressIndexNode));

	do
	{

		if (lpBaseAddr < pLdrNode->DllBase)
		{
			if (!pLdrNode->BaseAddressIndexNode.Left)
			{
				break;
			}

			pLdrNode = (PLDR_DATA_TABLE_ENTRY2)((size_t)pLdrNode->BaseAddressIndexNode.Left - offsetof(LDR_DATA_TABLE_ENTRY2, BaseAddressIndexNode));
		}

		else if (lpBaseAddr > pLdrNode->DllBase)
		{
			if (!pLdrNode->BaseAddressIndexNode.Right)
			{
				bRight = TRUE;
				break;
			}

			pLdrNode = (PLDR_DATA_TABLE_ENTRY2)((size_t)pLdrNode->BaseAddressIndexNode.Right - offsetof(LDR_DATA_TABLE_ENTRY2, BaseAddressIndexNode));
		}

		else
		{
			pLdrNode->DdagNode->LoadCount++;
		}

	} while (TRUE);

	RtlRbInsertNodeEx(pModBaseAddrIndex, &pLdrNode->BaseAddressIndexNode, bRight, &pLdrEntry->BaseAddressIndexNode);

	return TRUE;
}

PLIST_ENTRY FindHashTable() {
	PLIST_ENTRY pList = NULL;
	PLIST_ENTRY pHead = NULL;
	PLIST_ENTRY pEntry = NULL;
	PLDR_DATA_TABLE_ENTRY2 pCurrentEntry = NULL;

	PPEB2 pPeb = (PPEB2)READ_MEMLOC(PEB_OFFSET);

	pHead = &pPeb->Ldr->InInitializationOrderModuleList;
	pEntry = pHead->Flink;

	do
	{
		pCurrentEntry = CONTAINING_RECORD(
			pEntry,
			LDR_DATA_TABLE_ENTRY2,
			InInitializationOrderLinks
		);

		pEntry = pEntry->Flink;

		if (pCurrentEntry->HashLinks.Flink == &pCurrentEntry->HashLinks)
		{
			continue;
		}

		pList = pCurrentEntry->HashLinks.Flink;

		if (pList->Flink == &pCurrentEntry->HashLinks)
		{
			ULONG ulHash = LdrHashEntry(
				pCurrentEntry->BaseDllName,
				TRUE
			);

			pList = (PLIST_ENTRY)(
				(size_t)pCurrentEntry->HashLinks.Flink -
				ulHash *
				sizeof(LIST_ENTRY)
			);

			break;
		}

		pList = NULL;
	} while (pHead != pEntry);

	return pList;
}

VOID InsertTailList(
	PLIST_ENTRY ListHead,
	PLIST_ENTRY Entry
)
{
	PLIST_ENTRY Blink;

	Blink = ListHead->Blink;
	Entry->Flink = ListHead;
	Entry->Blink = Blink;
	Blink->Flink = Entry;
	ListHead->Blink = Entry;

	return;
}

BOOL AddHashTableEntry(
	PLDR_DATA_TABLE_ENTRY2 pLdrEntry
)
{
	PPEB pPeb;
	PPEB_LDR_DATA2 pPebData;
	PLIST_ENTRY LdrpHashTable;

	pPeb = (PPEB)READ_MEMLOC(PEB_OFFSET);

	RtlInitializeListEntry(
		&pLdrEntry->HashLinks
	);

	LdrpHashTable = FindHashTable();
	if (!LdrpHashTable)
	{
		return FALSE;
	}

	pPebData = (PPEB_LDR_DATA2)pPeb->Ldr;

	// insert into hash table
	ULONG ulHash = LdrHashEntry(
		pLdrEntry->BaseDllName,
		TRUE
	);
	
	InsertTailList(
		&LdrpHashTable[ulHash],
		&pLdrEntry->HashLinks
	);

	// insert into other lists
	InsertTailList(
		&pPebData->InLoadOrderModuleList,
		&pLdrEntry->InLoadOrderLinks
	);

	InsertTailList(
		&pPebData->InMemoryOrderModuleList,
		&pLdrEntry->InMemoryOrderLinks
	);

	InsertTailList(
		&pPebData->InInitializationOrderModuleList,
		&pLdrEntry->InInitializationOrderLinks
	);

	return TRUE;
}

HMODULE IsModulePresentA(
	char* Name
)
{
	wchar_t wtext[500];
	mbstowcs(wtext, Name, strlen(Name) + 1);
	return IsModulePresent(wtext);
}

HMODULE IsModulePresent(
	LPCWSTR lpwName
)
{
	if (lpwName == NULL)
		return (HMODULE)NULL;

	PPEB pPeb;
	PUCHAR ucModPtrOff;
	PLDR_DATA_TABLE_ENTRY2 pLdrTbl;
	
	pPeb = (PPEB)READ_MEMLOC(PEB_OFFSET);

	PLIST_ENTRY pModListEnd = &pPeb->Ldr->InMemoryOrderModuleList;
	PLIST_ENTRY pModList = pModListEnd->Flink;

	do
	{
		ucModPtrOff = (PUCHAR)pModList - (sizeof(LIST_ENTRY));

		pLdrTbl = (PLDR_DATA_TABLE_ENTRY2)ucModPtrOff;

		if (!_wcsicmp(
			pLdrTbl->BaseDllName.Buffer, 
			(PWSTR)lpwName)
		)
		{
			// already loaded, so return the base address
			return (HMODULE)pLdrTbl->DllBase;
		}

		pModList = pModList->Flink;
	} while (pModList != pModListEnd);

	return (HMODULE)NULL;
}

FARPROC GetFunctionAddress(
	HMODULE hModule,
	char*  ProcName
)
{
	STRING aString = { 0 };
	FILL_STRING(
		aString,
		ProcName
	);

	PVOID FunctionAddress = NULL;
	BOOL ok = LocalLdrGetProcedureAddress(
		hModule,
		&aString,
		0,
		&FunctionAddress
	);
	if (!ok)
		return NULL;
	return FunctionAddress;
}

BOOL LocalLdrGetProcedureAddress(
	HMODULE hLibrary,
	PANSI_STRING ProcName,
	WORD Ordinal,
	PVOID* FunctionAddress
)
{
	if (ProcName == NULL && Ordinal == 0)
	{
		printf("LocalLdrGetProcedureAddress: provide either a Function name or Ordinal\n");
		return FALSE;
	}

	if (ProcName != NULL && Ordinal != 0)
	{
		printf("LocalLdrGetProcedureAddress: provide Function name or Ordinal, not both\n");
		return FALSE;
	}

	BOOL ok = FALSE;
	if (hLibrary != NULL)
	{
		ok = _LocalLdrGetProcedureAddress(
			hLibrary,
			ProcName,
			Ordinal,
			FunctionAddress
		);
		if (ok)
			return TRUE;
	}

	// some deprecated DLLs have their functions implemented in KERNEL32 and KERNELBASE
	PVOID kernel32_addr = IsModulePresent(L"KERNEL32.dll");
	if (kernel32_addr != hLibrary)
	{
		ok = _LocalLdrGetProcedureAddress(
			kernel32_addr,
			ProcName,
			Ordinal,
			FunctionAddress
		);
	}
	if (ok)
		return TRUE;

	PVOID kernelbase_addr = IsModulePresent(L"KERNELBASE.dll");
	if (kernelbase_addr != hLibrary)
	{
		ok = _LocalLdrGetProcedureAddress(
			kernelbase_addr,
			ProcName,
			Ordinal,
			FunctionAddress
		);
	}
	if (ok)
		return TRUE;

	if (ProcName != NULL)
		printf("LocalLdrGetProcedureAddress: unable to resolve address of function: %s\n", ProcName->Buffer);
	else
		printf("LocalLdrGetProcedureAddress: unable to resolve address of function ordinal: %d\n", Ordinal);
	return FALSE;
}

BOOL _LocalLdrGetProcedureAddress(
	HMODULE hLibrary,
	PANSI_STRING ProcName,
	WORD Ordinal,
	PVOID* FunctionAddress
)
{
	PIMAGE_NT_HEADERS pNtHeaders;
	PIMAGE_DATA_DIRECTORY pDataDir;
	PIMAGE_EXPORT_DIRECTORY pExpDir;
	PIMAGE_SECTION_HEADER pSecHeader;

	if (hLibrary == NULL)
		return FALSE;

	pNtHeaders = RVA(
		PIMAGE_NT_HEADERS,
		hLibrary,
		((PIMAGE_DOS_HEADER)hLibrary)->e_lfanew
	);

	if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE)
	{
		printf("LocalLdrGetProcedureAddress: invalid IMAGE_NT_SIGNATURE\n");
		return FALSE;
	}

	// find the address range for the .text section
	PVOID startValidSection = NULL;
	PVOID endValidSection = NULL;

	for (int i = 0; i < pNtHeaders->FileHeader.NumberOfSections; i++)
	{
		pSecHeader = RVA(
			PIMAGE_SECTION_HEADER,
			&pNtHeaders->OptionalHeader,
			pNtHeaders->FileHeader.SizeOfOptionalHeader + i * IMAGE_SIZEOF_SECTION_HEADER
		);
		if (strncmp(".text", pSecHeader->Name, 6) == 0)
		{
			startValidSection = RVA(
				PVOID,
				hLibrary,
				pSecHeader->VirtualAddress
			);
			endValidSection = RVA(
				PVOID,
				startValidSection,
				pSecHeader->SizeOfRawData
			);
			break;
		}
	}
	if (startValidSection == NULL || endValidSection == NULL)
		return FALSE;

	pDataDir = &pNtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (pDataDir->Size)
	{
		pExpDir = RVA(
			PIMAGE_EXPORT_DIRECTORY,
			hLibrary,
			pDataDir->VirtualAddress
		);

		int numberOfEntries = ProcName != NULL ? pExpDir->NumberOfNames : pExpDir->NumberOfFunctions;
		// iterate over all the exports
		for (int i = 0; i < numberOfEntries; i++)
		{
			BOOL found = FALSE;
			ULONG32 FunctionOrdinal;
			if (ProcName != NULL)
			{
				// searching by name
				ULONG32* pRVA = RVA(
					ULONG32*,
					hLibrary,
					pExpDir->AddressOfNames + i * 4
				);
				LPCSTR functionName = RVA(
					LPCSTR,
					hLibrary,
					*pRVA
				);
				if (strlen(functionName) != ProcName->Length)
					continue;
				if (strncmp(functionName, ProcName->Buffer, ProcName->Length) == 0)
				{
					// found it
					found = TRUE;
					short* pRVA2 = RVA(
						short*,
						hLibrary,
						pExpDir->AddressOfNameOrdinals + i * 2
					);
					FunctionOrdinal = pExpDir->Base + *pRVA2;
				}
			}
			else
			{
				// searching by ordinal
				short* pRVA2 = RVA(
					short*,
					hLibrary,
					pExpDir->AddressOfNameOrdinals + i * 2
				);
				FunctionOrdinal = pExpDir->Base + *pRVA2;
				if (FunctionOrdinal == Ordinal)
				{
					// found it
					found = TRUE;
				}
			}
			if (found)
			{
				ULONG32* pFunctionRVA = RVA(
					ULONG32*,
					hLibrary,
					pExpDir->AddressOfFunctions + 4 * (FunctionOrdinal - pExpDir->Base)
				);
				PVOID FunctionPtr = RVA(
					PVOID,
					hLibrary,
					*pFunctionRVA
				);

				if (startValidSection > FunctionPtr || FunctionPtr > endValidSection)
				{
					// this is not a pointer to a function, but a reference to another library with the real address
					size_t full_length = strlen((char*)FunctionPtr);
					int lib_length = 0;
					for (int j = 0; j < full_length; j++)
					{
						if (((char*)FunctionPtr)[j] == '.')
						{
							lib_length = j;
							break;
						}
					}
					if (lib_length != 0)
					{

						size_t func_length = full_length - lib_length - 1;
						char* libname = HeapAlloc(
							GetProcessHeap(),
							HEAP_ZERO_MEMORY,
							lib_length + 5
						);
						if (!libname)
							return FALSE;
						strncpy(libname, (char*)FunctionPtr, lib_length);
						strncpy(libname + lib_length, ".dll", 5);
						char* funcname = (char*)FunctionPtr + lib_length + 1;
						STRING funcname_s = { 0 };
						FILL_STRING(
							funcname_s,
							funcname
						);
						PVOID lib_addr = IsModulePresentA(libname);
						if (lib_addr == NULL || lib_addr == hLibrary)
						{
							HeapFree(GetProcessHeap(), 0, libname); libname = NULL;
							return FALSE;
						}

						// call ourselves recursively
						BOOL ok = FALSE;
						ok = LocalLdrGetProcedureAddress(
							lib_addr,
							&funcname_s,
							0,
							&FunctionPtr
						);
						if (!ok)
						{
							printf("LocalLdrGetProcedureAddress: failed to resolve address of: %s!%s\n", libname, funcname);
							HeapFree(GetProcessHeap(), 0, libname); libname = NULL;
							return FALSE;
						}
						HeapFree(GetProcessHeap(), 0, libname); libname = NULL;
					}
				}
				*FunctionAddress = FunctionPtr;
				return TRUE;
			}
		}
	}
	return FALSE;
}

BOOL LinkModuleToPEB(
	PDARKMODULE pdModule
)
{
	PIMAGE_NT_HEADERS pNtHeaders;
	UNICODE_STRING FullDllName, BaseDllName;
	PLDR_DATA_TABLE_ENTRY2 pLdrEntry = NULL;

	pNtHeaders = RVA(
		PIMAGE_NT_HEADERS, 
		pdModule->pbDllData, 
		((PIMAGE_DOS_HEADER)pdModule->pbDllData)->e_lfanew
	);

	// convert the names to unicode
	RtlInitUnicodeString(
		&FullDllName, 
		pdModule->LocalDLLName
	);

	RtlInitUnicodeString(
		&BaseDllName, 
		pdModule->CrackedDLLName
	);

	// link the entry to the PEB
	pLdrEntry = (PLDR_DATA_TABLE_ENTRY2)HeapAlloc(
		GetProcessHeap(),
		HEAP_ZERO_MEMORY,
		sizeof(LDR_DATA_TABLE_ENTRY2)
	);

	if (!pLdrEntry)
	{
		return FALSE;
	}

	// start setting the values in the entry
	NtQuerySystemTime(&pLdrEntry->LoadTime);

	// do the obvious ones
	pLdrEntry->ReferenceCount        = 1;
	pLdrEntry->LoadReason            = LoadReasonDynamicLoad;
	pLdrEntry->OriginalBase          = pNtHeaders->OptionalHeader.ImageBase;

	// set the hash value
	pLdrEntry->BaseNameHashValue = LdrHashEntry(
		BaseDllName,
		FALSE
	);

	// correctly add the base address to the entry
	AddBaseAddressEntry(
		pLdrEntry,
		(PVOID)pdModule->ModuleBase
	);

	// an the rest
	pLdrEntry->ImageDll              = TRUE;
	pLdrEntry->LoadNotificationsSent = TRUE; // lol
	pLdrEntry->EntryProcessed        = TRUE;
	pLdrEntry->InLegacyLists         = TRUE;
	pLdrEntry->InIndexes             = TRUE;
	pLdrEntry->ProcessAttachCalled   = TRUE;
	pLdrEntry->InExceptionTable      = FALSE;
	pLdrEntry->DllBase               = (PVOID)pdModule->ModuleBase;
	pLdrEntry->SizeOfImage           = pNtHeaders->OptionalHeader.SizeOfImage;
	pLdrEntry->TimeDateStamp         = pNtHeaders->FileHeader.TimeDateStamp;
	pLdrEntry->BaseDllName           = BaseDllName;
	pLdrEntry->FullDllName           = FullDllName;
	pLdrEntry->ObsoleteLoadCount     = 1;
	pLdrEntry->Flags                 = LDRP_IMAGE_DLL | LDRP_ENTRY_INSERTED | LDRP_ENTRY_PROCESSED | LDRP_PROCESS_ATTACH_CALLED;

	// set the correct values in the Ddag node struct
	pLdrEntry->DdagNode = (PLDR_DDAG_NODE)HeapAlloc(
		GetProcessHeap(),
		HEAP_ZERO_MEMORY,
		sizeof(LDR_DDAG_NODE)
	);

	if (!pLdrEntry->DdagNode)
	{
		return 0;
	}

	pLdrEntry->NodeModuleLink.Flink    = &pLdrEntry->DdagNode->Modules;
	pLdrEntry->NodeModuleLink.Blink    = &pLdrEntry->DdagNode->Modules;
	pLdrEntry->DdagNode->Modules.Flink = &pLdrEntry->NodeModuleLink;
	pLdrEntry->DdagNode->Modules.Blink = &pLdrEntry->NodeModuleLink;
	pLdrEntry->DdagNode->State         = LdrModulesReadyToRun;
	pLdrEntry->DdagNode->LoadCount     = 1;

	// add the hash to the LdrpHashTable
	AddHashTableEntry(
		pLdrEntry
	);

	// set the entry point
	pLdrEntry->EntryPoint = RVA(
		PVOID,
		pdModule->ModuleBase,
		pNtHeaders->OptionalHeader.AddressOfEntryPoint
	);

	return TRUE;
}