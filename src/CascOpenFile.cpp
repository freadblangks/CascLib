/*****************************************************************************/
/* CascOpenFile.cpp                       Copyright (c) Ladislav Zezula 2014 */
/*---------------------------------------------------------------------------*/
/* System-dependent directory functions for CascLib                          */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 01.05.14  1.00  Lad  The first version of CascOpenFile.cpp                */
/*****************************************************************************/

#define __CASCLIB_SELF__
#include "CascLib.h"
#include "CascCommon.h"

//-----------------------------------------------------------------------------
// TCascFile class functions

TCascFile::TCascFile(TCascStorage * ahs, PCASC_CKEY_ENTRY apCKeyEntry)
{
    // Reference the storage handle
    ClassName = CASC_MAGIC_FILE;
    hs = ahs->AddRef();

    FilePointer = 0;
    pCKeyEntry = apCKeyEntry;
    SpanCount = (pCKeyEntry->SpanCount != 0) ? pCKeyEntry->SpanCount : 1;
    bVerifyIntegrity = false;
    bDownloadFileIf = false;
    bCloseFileStream = false;

    // Allocate the array of file spans
    if((pFileSpan = CASC_ALLOC_ZERO<CASC_FILE_SPAN>(SpanCount)) != NULL)
    {
        InitFileSpans(pFileSpan, SpanCount);
        InitCacheStrategy();
    }
}

TCascFile::~TCascFile()
{
    // Free all stuff related to file spans
    if (pFileSpan != NULL)
    {
        PCASC_FILE_SPAN pSpanPtr = pFileSpan;

        for(DWORD i = 0; i < SpanCount; i++, pSpanPtr++)
        {
            // Close the span file stream if this is a local file
            if(bCloseFileStream)
                FileStream_Close(pSpanPtr->pStream);
            pSpanPtr->pStream = NULL;

            // Free the span frames
            CASC_FREE(pSpanPtr->pFrames);
        }

        CASC_FREE(pFileSpan);
    }

    // Free the file cache
    CASC_FREE(pbFileCache);

    // Close (dereference) the archive handle
    hs = hs->Release();
    ClassName = 0;
}

void TCascFile::InitFileSpans(PCASC_FILE_SPAN pSpans, DWORD dwSpanCount)
{
    ULONGLONG FileOffsetMask = ((ULONGLONG)1 << hs->FileOffsetBits) - 1;
    ULONGLONG FileOffsetBits = hs->FileOffsetBits;
    ULONGLONG StartOffset = 0;

    // Reset the sizes
    ContentSize = 0;
    EncodedSize = 0;

    // Add all span sizes
    for(DWORD i = 0; i < dwSpanCount; i++, pSpans++)
    {
        // Put the archive index and archive offset
        pSpans->ArchiveIndex = (DWORD)(pCKeyEntry[i].StorageOffset >> FileOffsetBits);
        pSpans->ArchiveOffs = (DWORD)(pCKeyEntry[i].StorageOffset & FileOffsetMask);

        // Add to the total encoded size
        if(EncodedSize != CASC_INVALID_SIZE64 && pCKeyEntry[i].EncodedSize != CASC_INVALID_SIZE)
        {
            EncodedSize = EncodedSize + pCKeyEntry[i].EncodedSize;
        }
        else
        {
            EncodedSize = CASC_INVALID_SIZE64;
        }

        // Add to the total content size
        if(ContentSize != CASC_INVALID_SIZE64 && pCKeyEntry[i].ContentSize != CASC_INVALID_SIZE)
        {
            // Put the start and end ranges
            pSpans->StartOffset = StartOffset;
            pSpans->EndOffset = StartOffset + pCKeyEntry[i].ContentSize;
            StartOffset = pSpans->EndOffset;

            // Increment the total content size
            ContentSize = ContentSize + pCKeyEntry[i].ContentSize;
        }
        else
        {
            ContentSize = CASC_INVALID_SIZE64;
        }
    }
}

void TCascFile::InitCacheStrategy()
{
    CacheStrategy = CascCacheLastFrame;
    FileCacheStart = FileCacheEnd = 0;
    pbFileCache = NULL;
}

//-----------------------------------------------------------------------------
// Local functions

PCASC_CKEY_ENTRY FindCKeyEntry_CKey(TCascStorage * hs, LPBYTE pbCKey, PDWORD PtrIndex)
{
    return (PCASC_CKEY_ENTRY)hs->CKeyMap.FindObject(pbCKey, PtrIndex);
}

PCASC_CKEY_ENTRY FindCKeyEntry_EKey(TCascStorage * hs, LPBYTE pbEKey, PDWORD PtrIndex)
{
    return (PCASC_CKEY_ENTRY)hs->EKeyMap.FindObject(pbEKey, PtrIndex);
}

bool OpenFileByCKeyEntry(TCascStorage * hs, PCASC_CKEY_ENTRY pCKeyEntry, DWORD dwOpenFlags, HANDLE * PtrFileHandle)
{
    TCascFile * hf = NULL;
    DWORD dwErrCode = ERROR_FILE_NOT_FOUND;

    // If the CKey entry is NULL, we consider the file non-existant
    if(pCKeyEntry != NULL)
    {
        // Create the file handle structure
        if((hf = new TCascFile(hs, pCKeyEntry)) != NULL)
        {
            hf->bVerifyIntegrity   = (dwOpenFlags & CASC_STRICT_DATA_CHECK)  ? true : false;
            hf->bDownloadFileIf    = (hs->dwFeatures & CASC_FEATURE_ONLINE)  ? true : false;
            hf->bOvercomeEncrypted = (dwOpenFlags & CASC_OVERCOME_ENCRYPTED) ? true : false;
            dwErrCode = ERROR_SUCCESS;
        }
        else
        {
            dwErrCode = ERROR_NOT_ENOUGH_MEMORY;
        }
    }

    // Give the output parameter, no matter what
    PtrFileHandle[0] = (HANDLE)hf;

    // Handle last error
    if(dwErrCode != ERROR_SUCCESS)
        SetLastError(dwErrCode);
    return (dwErrCode == ERROR_SUCCESS);
}

bool SetCacheStrategy(HANDLE hFile, CSTRTG CacheStrategy)
{
    TCascFile * hf;

    // Validate the file handle
    if((hf = TCascFile::IsValid(hFile)) != NULL)
    {
        // The cache must not be initialized yet
        if(hf->pbFileCache == NULL)
        {
            hf->CacheStrategy = CacheStrategy;
            return true;
        }
    }

    // Failed. This should never happen
    assert(false);
    return false;
}

//-----------------------------------------------------------------------------
// Public functions

bool WINAPI CascOpenFile(HANDLE hStorage, const void * pvFileName, DWORD dwLocaleFlags, DWORD dwOpenFlags, HANDLE * PtrFileHandle)
{
    PCASC_CKEY_ENTRY pCKeyEntry = NULL;
    TCascStorage * hs;
    const char * szFileName;
    DWORD FileDataId = CASC_INVALID_ID;
    BYTE CKeyEKeyBuffer[MD5_HASH_SIZE];
    DWORD dwErrCode = ERROR_SUCCESS;

    // This parameter is not used
    CASCLIB_UNUSED(dwLocaleFlags);

    // Validate the storage handle
    hs = TCascStorage::IsValid(hStorage);
    if(hs == NULL)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    // Validate the other parameters
    if(PtrFileHandle == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    // Retrieve the CKey/EKey from the file name in different modes
    switch(dwOpenFlags & CASC_OPEN_TYPE_MASK)
    {
        case CASC_OPEN_BY_NAME:

            // The 'pvFileName' must be zero terminated ANSI file name
            szFileName = (const char *)pvFileName;
            if(szFileName == NULL || szFileName[0] == 0)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return false;
            }

            // The first chance: Try to find the file by name (using the root handler)
            pCKeyEntry = hs->pRootHandler->GetFile(hs, szFileName);
            if(pCKeyEntry != NULL)
                break;

            // Second chance: If the file name is actually a file data id, we convert it to file data ID
            if(IsFileDataIdName(szFileName, FileDataId))
            {
                pCKeyEntry = hs->pRootHandler->GetFile(hs, FileDataId);
                if(pCKeyEntry != NULL)
                    break;
            }

            // Third chance: If the file name is a string representation of CKey/EKey, we try to query for CKey
            if(IsFileCKeyEKeyName(szFileName, CKeyEKeyBuffer))
            {
                pCKeyEntry = FindCKeyEntry_CKey(hs, CKeyEKeyBuffer);
                if(pCKeyEntry != NULL)
                    break;

                pCKeyEntry = FindCKeyEntry_EKey(hs, CKeyEKeyBuffer);
                if(pCKeyEntry != NULL)
                    break;
            }

            SetLastError(ERROR_FILE_NOT_FOUND);
            return false;

        case CASC_OPEN_BY_CKEY:

            // The 'pvFileName' must be a pointer to 16-byte CKey or EKey
            if(pvFileName == NULL)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return false;
            }

            // Search the CKey map in order to find the CKey entry
            pCKeyEntry = FindCKeyEntry_CKey(hs, (LPBYTE)pvFileName);
            break;

        case CASC_OPEN_BY_EKEY:

            // The 'pvFileName' must be a pointer to 16-byte CKey or EKey
            if(pvFileName == NULL)
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return false;
            }

            // Search the CKey map in order to find the CKey entry
            pCKeyEntry = FindCKeyEntry_EKey(hs, (LPBYTE)pvFileName);
            break;

        case CASC_OPEN_BY_FILEID:

            // Retrieve the file CKey/EKey
            pCKeyEntry = hs->pRootHandler->GetFile(hs, CASC_FILE_DATA_ID_FROM_STRING(pvFileName));
            break;

        default:

            // Unknown open mode
            dwErrCode = ERROR_INVALID_PARAMETER;
            break;
    }

    // Perform the open operation
    return OpenFileByCKeyEntry(hs, pCKeyEntry, dwOpenFlags, PtrFileHandle);
}

bool WINAPI CascCloseFile(HANDLE hFile)
{
    TCascFile * hf;

    hf = TCascFile::IsValid(hFile);
    if (hf != NULL)
    {
        delete hf;
        return true;
    }

    SetLastError(ERROR_INVALID_HANDLE);
    return false;
}

