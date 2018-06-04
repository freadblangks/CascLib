/*****************************************************************************/
/* CascOpenStorage.cpp                    Copyright (c) Ladislav Zezula 2014 */
/*---------------------------------------------------------------------------*/
/* Storage functions for CASC                                                */
/* Note: WoW6 offsets refer to WoW.exe 6.0.3.19116 (32-bit)                  */
/* SHA1: c10e9ffb7d040a37a356b96042657e1a0c95c0dd                            */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 29.04.14  1.00  Lad  The first version of CascOpenStorage.cpp             */
/*****************************************************************************/

#define __CASCLIB_SELF__
#include "CascLib.h"
#include "CascCommon.h"

//-----------------------------------------------------------------------------
// Local structures

#define ROOT_SEARCH_PHASE_INITIALIZING  0
#define ROOT_SEARCH_PHASE_LISTFILE      1
#define ROOT_SEARCH_PHASE_NAMELESS      2
#define ROOT_SEARCH_PHASE_FINISHED      2

// On-disk version of locale block
typedef struct _FILE_LOCALE_BLOCK
{
    DWORD NumberOfFiles;                        // Number of entries
    DWORD Flags;
    DWORD Locales;                              // File locale mask (CASC_LOCALE_XXX)

    // Followed by a block of 32-bit integers (count: NumberOfFiles)
    // Followed by the MD5 and file name hash (count: NumberOfFiles)

} FILE_LOCALE_BLOCK, *PFILE_LOCALE_BLOCK;

// On-disk version of root entry
typedef struct _FILE_ROOT_ENTRY
{
    CONTENT_KEY CKey;                           // MD5 of the file
    ULONGLONG FileNameHash;                     // Jenkins hash of the file name

} FILE_ROOT_ENTRY, *PFILE_ROOT_ENTRY;


typedef struct _FILE_ROOT_BLOCK
{
    PFILE_LOCALE_BLOCK pLocaleBlockHdr;         // Pointer to the locale block
    PDWORD FileDataIds;                         // Pointer to the array of File Data IDs
    PFILE_ROOT_ENTRY pRootEntries;

} FILE_ROOT_BLOCK, *PFILE_ROOT_BLOCK;

// Root file entry for CASC storages (World of Warcraft 6.0+)
// Does not match to the in-file structure of the root entry
typedef struct _WOW_FILE_ENTRY
{
    CONTENT_KEY CKey;                           // File content key (MD5)
    ULONGLONG FileNameHash;                     // Jenkins hash of the file name
    DWORD FileDataId;                           // File Data ID
    DWORD Locales;                              // Locale flags of the file

} WOW_FILE_ENTRY, *PWOW_FILE_ENTRY;

struct TRootHandler_WoW6 : public TRootHandler
{
    // Linear global list of file entries
    CASC_ARRAY FileTable;

    // Lookup map of FileDataId->FileEntry and FileNameHash -> FileEntry
    PCASC_MAP pDataIdMap;
    PCASC_MAP pNameMap;

    // For counting files
    DWORD dwTotalFileCount;
};

// Prototype for root file parsing routine
typedef int (*PARSE_ROOT)(TRootHandler_WoW6 * pRootHandler, PFILE_ROOT_BLOCK pBlockInfo);

//-----------------------------------------------------------------------------
// Local functions

static bool IsFileDataIdName(const char * szFileName)
{
    BYTE BinaryValue[4];

    // File name must begin with "File", case insensitive
    if(AsciiToUpperTable_BkSlash[szFileName[0]] == 'F' &&
       AsciiToUpperTable_BkSlash[szFileName[1]] == 'I' &&
       AsciiToUpperTable_BkSlash[szFileName[2]] == 'L' &&
       AsciiToUpperTable_BkSlash[szFileName[3]] == 'E')
    {
        // Then, 8 hexadecimal digits must follow
        if(ConvertStringToBinary(szFileName + 4, 8, BinaryValue) == ERROR_SUCCESS)
        {
            // Must be followed by an extension or end-of-string
            return (szFileName[0x0C] == 0 || szFileName[0x0C] == '.');
        }
    }

    return false;
}

LPBYTE VerifyLocaleBlock(PFILE_ROOT_BLOCK pBlockInfo, LPBYTE pbFilePointer, LPBYTE pbFileEnd)
{
    // Validate the file locale block
    pBlockInfo->pLocaleBlockHdr = (PFILE_LOCALE_BLOCK)pbFilePointer;
    pbFilePointer = (LPBYTE)(pBlockInfo->pLocaleBlockHdr + 1);
    if(pbFilePointer > pbFileEnd)
        return NULL;

    // Validate the array of File Data IDs
    pBlockInfo->FileDataIds = (PDWORD)pbFilePointer;
    pbFilePointer = (LPBYTE)(pBlockInfo->FileDataIds + pBlockInfo->pLocaleBlockHdr->NumberOfFiles);
    if(pbFilePointer > pbFileEnd)
        return NULL;

    // Validate the array of root entries
    pBlockInfo->pRootEntries = (PFILE_ROOT_ENTRY)pbFilePointer;
    pbFilePointer = (LPBYTE)(pBlockInfo->pRootEntries + pBlockInfo->pLocaleBlockHdr->NumberOfFiles);
    if(pbFilePointer > pbFileEnd)
        return NULL;

    // Return the position of the next block
    return pbFilePointer;
}

// Verify whether this might be a WOW root file
static bool IsRootFile_WoW6(LPBYTE pbRootFile, DWORD cbRootFile)
{
    FILE_ROOT_BLOCK RootBlock;

    // Validate the file locale block
    pbRootFile = VerifyLocaleBlock(&RootBlock, pbRootFile, pbRootFile + cbRootFile);
    return (pbRootFile != NULL);
}

static int ParseRoot_CountFiles(
    TRootHandler_WoW6 * pRootHandler,
    PFILE_ROOT_BLOCK pRootBlock)
{
    // Add the file count to the total file count
    pRootHandler->dwTotalFileCount += pRootBlock->pLocaleBlockHdr->NumberOfFiles;
    return ERROR_SUCCESS;
}

static int ParseRoot_AddRootEntries(
    TRootHandler_WoW6 * pRootHandler,
    PFILE_ROOT_BLOCK pRootBlock)
{
    PWOW_FILE_ENTRY pFileEntry;
    DWORD dwFileDataId = 0;

    // Sanity checks
    assert(pRootHandler->FileTable.ItemArray != NULL);
    assert(pRootHandler->FileTable.ItemCountMax != 0);

    // WoW.exe (build 19116): Blocks with zero files are skipped
    for(DWORD i = 0; i < pRootBlock->pLocaleBlockHdr->NumberOfFiles; i++)
    {
        // Create new entry, with overflow check
        if(pRootHandler->FileTable.ItemCount >= pRootHandler->FileTable.ItemCountMax)
            return ERROR_INSUFFICIENT_BUFFER;
        pFileEntry = (PWOW_FILE_ENTRY)Array_Insert(&pRootHandler->FileTable, NULL, 1);

        // (004147A3) Prepare the WOW_FILE_ENTRY structure
        pFileEntry->FileNameHash = pRootBlock->pRootEntries[i].FileNameHash;
        pFileEntry->FileDataId = dwFileDataId + pRootBlock->FileDataIds[i];
        pFileEntry->Locales = pRootBlock->pLocaleBlockHdr->Locales;
        pFileEntry->CKey = pRootBlock->pRootEntries[i].CKey;

        // Also, insert the entry to the map
        Map_InsertObject(pRootHandler->pDataIdMap, pFileEntry, &pFileEntry->FileDataId);
        Map_InsertObject(pRootHandler->pNameMap, pFileEntry, &pFileEntry->FileNameHash);

        // Update the local File Data Id
        assert((pFileEntry->FileDataId + 1) > pFileEntry->FileDataId);
        dwFileDataId = pFileEntry->FileDataId + 1;

        // Move to the next root entry
        pFileEntry++;
    }

    return ERROR_SUCCESS;
}

static int ParseWowRootFileInternal(
    TRootHandler_WoW6 * pRootHandler,
    PARSE_ROOT pfnParseRoot,
    LPBYTE pbRootFile,
    LPBYTE pbRootFileEnd,
    DWORD dwLocaleMask,
    BYTE bOverrideArchive,
    BYTE bAudioLocale)
{
    FILE_ROOT_BLOCK RootBlock;

    // Now parse the root file
    while(pbRootFile < pbRootFileEnd)
    {
        // Validate the file locale block
        pbRootFile = VerifyLocaleBlock(&RootBlock, pbRootFile, pbRootFileEnd);
        if(pbRootFile == NULL)
            break;

        // WoW.exe (build 19116): Entries with flag 0x100 set are skipped
        if(RootBlock.pLocaleBlockHdr->Flags & 0x100)
            continue;

        // WoW.exe (build 19116): Entries with flag 0x80 set are skipped if overrideArchive CVAR is set to FALSE (which is by default in non-chinese clients)
        if((RootBlock.pLocaleBlockHdr->Flags & 0x80) && bOverrideArchive == 0)
            continue;

        // WoW.exe (build 19116): Entries with (flags >> 0x1F) not equal to bAudioLocale are skipped
        if((RootBlock.pLocaleBlockHdr->Flags >> 0x1F) != bAudioLocale)
            continue;

        // WoW.exe (build 19116): Locales other than defined mask are skipped too
        if((RootBlock.pLocaleBlockHdr->Locales & dwLocaleMask) == 0)
            continue;

        // Now call the custom function
        pfnParseRoot(pRootHandler, &RootBlock);
    }

    return ERROR_SUCCESS;
}

/*
// known dwRegion values returned from sub_661316 (7.0.3.22210 x86 win), also referred by lua GetCurrentRegion
#define WOW_REGION_US              0x01
#define WOW_REGION_KR              0x02
#define WOW_REGION_EU              0x03
#define WOW_REGION_TW              0x04
#define WOW_REGION_CN              0x05

#define WOW_LOCALE_ENUS            0x00
#define WOW_LOCALE_KOKR            0x01
#define WOW_LOCALE_FRFR            0x02
#define WOW_LOCALE_DEDE            0x03
#define WOW_LOCALE_ZHCN            0x04
#define WOW_LOCALE_ZHTW            0x05
#define WOW_LOCALE_ESES            0x06
#define WOW_LOCALE_ESMX            0x07
#define WOW_LOCALE_RURU            0x08
#define WOW_LOCALE_PTBR            0x0A
#define WOW_LOCALE_ITIT            0x0B

    // dwLocale is obtained from a WOW_LOCALE_* to CASC_LOCALE_BIT_* mapping (sub_6615D0 in 7.0.3.22210 x86 win)
    // because (ENUS, ENGB) and (PTBR, PTPT) pairs share the same value on WOW_LOCALE_* enum
    // dwRegion is used to distinguish them
    if(dwRegion == WOW_REGION_EU)
    {
        // Is this english version of WoW?
        if(dwLocale == CASC_LOCALE_BIT_ENUS)
        {
            LoadWowRootFileLocales(hs, pbRootFile, cbRootFile, CASC_LOCALE_ENGB, bOverrideArchive, bAudioLocale);
            LoadWowRootFileLocales(hs, pbRootFile, cbRootFile, CASC_LOCALE_ENUS, bOverrideArchive, bAudioLocale);
            return ERROR_SUCCESS;
        }

        // Is this portuguese version of WoW?
        if(dwLocale == CASC_LOCALE_BIT_PTBR)
        {
            LoadWowRootFileLocales(hs, pbRootFile, cbRootFile, CASC_LOCALE_PTPT, bOverrideArchive, bAudioLocale);
            LoadWowRootFileLocales(hs, pbRootFile, cbRootFile, CASC_LOCALE_PTBR, bOverrideArchive, bAudioLocale);
        }
    }
    else
        LoadWowRootFileLocales(hs, pbRootFile, cbRootFile, (1 << dwLocale), bOverrideArchive, bAudioLocale);
*/

static int ParseWowRootFile2(
    TRootHandler_WoW6 * pRootHandler,
    PARSE_ROOT pfnParseRoot,
    LPBYTE pbRootFile,
    LPBYTE pbRootFileEnd,
    DWORD dwLocaleMask,
    BYTE bAudioLocale)
{
    // Load the locale as-is
    ParseWowRootFileInternal(pRootHandler, pfnParseRoot, pbRootFile, pbRootFileEnd, dwLocaleMask, false, bAudioLocale);

    // If we wanted enGB, we also load enUS for the missing files
    if(dwLocaleMask == CASC_LOCALE_ENGB)
        ParseWowRootFileInternal(pRootHandler, pfnParseRoot, pbRootFile, pbRootFileEnd, CASC_LOCALE_ENUS, false, bAudioLocale);

    if(dwLocaleMask == CASC_LOCALE_PTPT)
        ParseWowRootFileInternal(pRootHandler, pfnParseRoot, pbRootFile, pbRootFileEnd, CASC_LOCALE_PTBR, false, bAudioLocale);

    return ERROR_SUCCESS;
}

// WoW.exe: 004146C7 (BuildManifest::Load)
static int ParseWowRootFile(
    TRootHandler_WoW6 * pRootHandler,
    PARSE_ROOT pfnParseRoot,
    LPBYTE pbRootFile,
    LPBYTE pbRootFileEnd,
    DWORD dwLocaleMask)
{
    ParseWowRootFile2(pRootHandler, pfnParseRoot, pbRootFile, pbRootFileEnd, dwLocaleMask, 0);
    ParseWowRootFile2(pRootHandler, pfnParseRoot, pbRootFile, pbRootFileEnd, dwLocaleMask, 1);
    return ERROR_SUCCESS;
}

static int RebuildFileMaps(TRootHandler_WoW6 * pRootHandler)
{
    PWOW_FILE_ENTRY pFileEntry;
    size_t dwTotalFileCount = pRootHandler->FileTable.ItemCount;
    int nError = ERROR_SUCCESS;

    // Free the current NameMap, if exists
    if(pRootHandler->pNameMap != NULL)
        Map_Free(pRootHandler->pNameMap);
    pRootHandler->pNameMap = NULL;

    // Free the current DataIdMap, if exists
    if(pRootHandler->pDataIdMap != NULL)
        Map_Free(pRootHandler->pDataIdMap);
    pRootHandler->pDataIdMap = NULL;

    // Create new NameMap (FileNameHash -> WOW_FILE_ENTRY)
    pRootHandler->pDataIdMap = Map_Create(dwTotalFileCount + CASC_EXTRA_FILES, sizeof(DWORD), FIELD_OFFSET(WOW_FILE_ENTRY, FileDataId));
    pRootHandler->pNameMap = Map_Create(dwTotalFileCount + CASC_EXTRA_FILES, sizeof(ULONGLONG), FIELD_OFFSET(WOW_FILE_ENTRY, FileNameHash));
    if(pRootHandler->pDataIdMap && pRootHandler->pNameMap)
    {
        // Parse the entire file table and put items to the map
        for(DWORD i = 0; i < dwTotalFileCount; i++)
        {
            pFileEntry = (PWOW_FILE_ENTRY)Array_ItemAt(&pRootHandler->FileTable, i);
            if(pFileEntry != NULL && pFileEntry->FileNameHash != 0)
            {
                Map_InsertObject(pRootHandler->pDataIdMap, pFileEntry, &pFileEntry->FileDataId);
                Map_InsertObject(pRootHandler->pNameMap, pFileEntry, &pFileEntry->FileNameHash);
            }
        }
    }
    else
    {
        nError = ERROR_NOT_ENOUGH_MEMORY;
    }

    return nError;
}

//-----------------------------------------------------------------------------
// Implementation of WoW6 root file

static int WowHandler_Insert(
    TRootHandler_WoW6 * pRootHandler,
    const char * szFileName,
    PCASC_CKEY_ENTRY pCKeyEntry)
{
    PWOW_FILE_ENTRY pFileEntry;
    char * SaveItemArray = pRootHandler->FileTable.ItemArray;
    DWORD FileDataId = 0;

    // Don't let the number of items to overflow
    if(pRootHandler->FileTable.ItemCount >= pRootHandler->FileTable.ItemCountMax)
        return ERROR_NOT_ENOUGH_MEMORY;

    // Insert the item to the linear file list
    pFileEntry = (PWOW_FILE_ENTRY)Array_Insert(&pRootHandler->FileTable, NULL, 1);
    if(pFileEntry != NULL)
    {
        // Get the file data ID of the previous item (0 if this is the first one)
        if(pRootHandler->FileTable.ItemCount > 1)
            FileDataId = pFileEntry[-1].FileDataId;

        // Fill-in the new entry
        memcpy(pFileEntry->CKey.Value, pCKeyEntry->CKey, CASC_CKEY_SIZE);
        pFileEntry->FileNameHash = CalcFileNameHash(szFileName);
        pFileEntry->FileDataId   = FileDataId + 1;
        pFileEntry->Locales      = CASC_LOCALE_ALL;

        // Verify collisions (debug version only)
        assert(Map_FindObject(pRootHandler->pNameMap, &pFileEntry->FileNameHash, NULL) == NULL);

        // Insert the entry to the maps
        Map_InsertObject(pRootHandler->pDataIdMap, pFileEntry, &pFileEntry->FileDataId);
        Map_InsertObject(pRootHandler->pNameMap, pFileEntry, &pFileEntry->FileNameHash);

        // If the file map had to reallocate itself, the pointers in the file maps
        // are no longer valid. We need to rebuild both maps
        if(pRootHandler->FileTable.ItemArray != SaveItemArray)
        {
            RebuildFileMaps(pRootHandler);
        }
    }

    return ERROR_SUCCESS;
}

static LPBYTE WowHandler_Search(TRootHandler_WoW6 * pRootHandler, TCascSearch * pSearch)
{
    PWOW_FILE_ENTRY pFileEntry;
    ULONGLONG FileNameHash;    

    // Only if we have a listfile
    if(pSearch->pCache != NULL)
    {
        // Keep going through the listfile
        while(ListFile_GetNext(pSearch->pCache, pSearch->szMask, pSearch->szFileName, MAX_PATH))
        {
            // Calculate the hash of the file name
            FileNameHash = CalcFileNameHash(pSearch->szFileName);

            // Find the root entry by name hash
            pFileEntry = (PWOW_FILE_ENTRY)Map_FindObject(pRootHandler->pNameMap, &FileNameHash, NULL);
            if(pFileEntry != NULL)
            {
                // Give the caller the locale mask and file data ID
                pSearch->dwLocaleFlags = pFileEntry->Locales;
                pSearch->dwFileDataId = pFileEntry->FileDataId;
                return pFileEntry->CKey.Value;
            }
        }
    }

    // No more files
    return NULL;
}

static LPBYTE WowHandler_GetKey(TRootHandler_WoW6 * pRootHandler, const char * szFileName, PDWORD /* PtrFileSize */)
{
    PWOW_FILE_ENTRY pFileEntry;
    DWORD FileDataId;
    BYTE FileDataIdLE[4];

    // Open by FileDataId. The file name must be as following:
    // File########.unk, where '#' are hexa-decimal numbers (case insensitive).
    // Extension is ignored in that case
    if(IsFileDataIdName(szFileName))
    {
        ConvertStringToBinary(szFileName + 4, 8, FileDataIdLE);
        FileDataId = ConvertBytesToInteger_4(FileDataIdLE);

        pFileEntry = (PWOW_FILE_ENTRY)Map_FindObject(pRootHandler->pDataIdMap, &FileDataId, NULL);
    }
    else
    {
        // Calculate the HASH value of the normalized file name
        ULONGLONG FileNameHash = CalcFileNameHash(szFileName);

        // Perform the hash search
        pFileEntry = (PWOW_FILE_ENTRY)Map_FindObject(pRootHandler->pNameMap, &FileNameHash, NULL);
    }

    return (pFileEntry != NULL) ? pFileEntry->CKey.Value : NULL;
}

static void WowHandler_EndSearch(TRootHandler_WoW6 * /* pRootHandler */, TCascSearch * pSearch)
{
    if(pSearch->pRootContext != NULL)
        CASC_FREE(pSearch->pRootContext);
    pSearch->pRootContext = NULL;
}

static DWORD WowHandler_GetFileId(TRootHandler_WoW6 * pRootHandler, const char * szFileName)
{
    PWOW_FILE_ENTRY pFileEntry;
    ULONGLONG FileNameHash = CalcFileNameHash(szFileName);

    // Find by the file name hash
    pFileEntry = (PWOW_FILE_ENTRY)Map_FindObject(pRootHandler->pNameMap, &FileNameHash, NULL);
    return (pFileEntry != NULL) ? pFileEntry->FileDataId : 0;
}

static void WowHandler_Close(TRootHandler_WoW6 * pRootHandler)
{
    if(pRootHandler != NULL)
    {
        Array_Free(&pRootHandler->FileTable);
        Map_Free(pRootHandler->pDataIdMap);
        Map_Free(pRootHandler->pNameMap);
        CASC_FREE(pRootHandler);
    }
}

#ifdef _DEBUG
static void WowHandler_Dump(
    TCascStorage * hs,
    TDumpContext * dc,                                      // Pointer to an opened file
    LPBYTE pbRootFile,
    DWORD cbRootFile,
    const TCHAR * szListFile,
    int nDumpLevel)
{
    PCASC_CKEY_ENTRY pCKeyEntry;
    FILE_ROOT_BLOCK BlockInfo;
    PLISTFILE_MAP pListMap;
    QUERY_KEY CKey;
    LPBYTE pbRootFileEnd = pbRootFile + cbRootFile;
    LPBYTE pbFilePointer;
    char szOneLine[0x100];
    DWORD i;

    // Create the listfile map
    pListMap = ListMap_Create(szListFile);

    // Dump the root entries
    for(pbFilePointer = pbRootFile; pbFilePointer <= pbRootFileEnd; )
    {
        // Validate the root block
        pbFilePointer = VerifyLocaleBlock(&BlockInfo, pbFilePointer, pbRootFileEnd);
        if(pbFilePointer == NULL)
            break;

        // Dump the locale block
        dump_print(dc, "Flags: %08X  Locales: %08X  NumberOfFiles: %u\n"
                       "=========================================================\n",
                       BlockInfo.pLocaleBlockHdr->Flags,
                       BlockInfo.pLocaleBlockHdr->Locales,
                       BlockInfo.pLocaleBlockHdr->NumberOfFiles);

        // Dump the hashes and CKeys
        for(i = 0; i < BlockInfo.pLocaleBlockHdr->NumberOfFiles; i++)
        {
            // Dump the entry
            dump_print(dc, "%08X %08X-%08X %s %s\n",
                           (DWORD)(BlockInfo.FileDataIds[i]),
                           (DWORD)(BlockInfo.pRootEntries[i].FileNameHash >> 0x20),
                           (DWORD)(BlockInfo.pRootEntries[i].FileNameHash),
                           StringFromMD5(BlockInfo.pRootEntries[i].CKey.Value, szOneLine),
                           ListMap_FindName(pListMap, BlockInfo.pRootEntries[i].FileNameHash));

            // Find the encoding entry in the encoding table
            if(nDumpLevel >= DUMP_LEVEL_ENCODING_FILE)
            {
                CKey.pbData = BlockInfo.pRootEntries[i].CKey.Value;
                CKey.cbData = MD5_HASH_SIZE;
                pCKeyEntry = FindCKeyEntry(hs, &CKey, NULL);
//              CascDumpCKeyEntry(hs, dc, pCKeyEntry, nDumpLevel);
            }
        }

        // Put extra newline
        dump_print(dc, "\n");
    }

    ListMap_Free(pListMap);
}
#endif

//-----------------------------------------------------------------------------
// Public functions

int RootHandler_CreateWoW6(TCascStorage * hs, LPBYTE pbRootFile, DWORD cbRootFile, DWORD dwLocaleMask)
{
    TRootHandler_WoW6 * pRootHandler;
    LPBYTE pbRootFileEnd = pbRootFile + cbRootFile;
    int nError = ERROR_BAD_FORMAT;

    // Verify whether this might be a WOW root file
    if(IsRootFile_WoW6(pbRootFile, cbRootFile))
    {
        // Allocate the root handler object
        hs->pRootHandler = pRootHandler = CASC_ALLOC(TRootHandler_WoW6, 1);
        if(pRootHandler == NULL)
            return ERROR_NOT_ENOUGH_MEMORY;

        // Fill-in the handler functions
        memset(pRootHandler, 0, sizeof(TRootHandler_WoW6));
        pRootHandler->Insert      = (ROOT_INSERT)WowHandler_Insert;
        pRootHandler->Search      = (ROOT_SEARCH)WowHandler_Search;
        pRootHandler->EndSearch   = (ROOT_ENDSEARCH)WowHandler_EndSearch;
        pRootHandler->GetKey      = (ROOT_GETKEY)WowHandler_GetKey;
        pRootHandler->Close       = (ROOT_CLOSE)WowHandler_Close;
        pRootHandler->GetFileId   = (ROOT_GETFILEID)WowHandler_GetFileId;

#ifdef _DEBUG
        pRootHandler->Dump = WowHandler_Dump;    // Support for ROOT file dump
#endif  // _DEBUG

        // Count the files that are going to be loaded
        ParseWowRootFile(pRootHandler, ParseRoot_CountFiles, pbRootFile, pbRootFileEnd, dwLocaleMask);
        pRootHandler->dwTotalFileCount += CASC_EXTRA_FILES;

        // Create linear table that will contain all file items
        nError = Array_Create(&pRootHandler->FileTable, WOW_FILE_ENTRY, pRootHandler->dwTotalFileCount + CASC_EXTRA_FILES);
        if(nError != ERROR_SUCCESS)
            return nError;

        // Parse the root file again and insert all files to the map
        nError = ParseWowRootFile(pRootHandler, ParseRoot_AddRootEntries, pbRootFile, pbRootFileEnd, dwLocaleMask);
        if(nError != ERROR_SUCCESS)
            return nError;

        // Rebuild the maps
        nError = RebuildFileMaps(pRootHandler);
    }

    return nError;
}
