#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <string>
#include <map>
#include <atomic>
#include <thread>
#include <intrin.h>

#include <projectedfslib.h>
#pragma comment(lib, "projectedfslib.lib")

#pragma region helpers

char     gs_fileBuf[513];
uint32_t gs_fileBufCrc32 = 0;

//simplify error management for the purpose of this repro
inline void VERIFY_(bool condition, const char * file, int line)
{
    if (!condition)
    {
        printf("error at %s:%d\n", file, line);
        exit(-1);
    }
}

#define VERIFY(cond) VERIFY_(cond, __FILE__, __LINE__);

struct GUIDComparer
{
    bool operator()(const GUID & Left, const GUID & Right) const
    {
        return memcmp(&Left, &Right, sizeof(Right)) < 0;
    }
};

// for the filter driver to properly merge our list with the ntfs list using a simple array walk O(n)
// we must use the same sorting order as NTFS. PrjFileNameCompare uses wcsicmp which use towlower for 
// comparison ('_' < 'a') while NTFS seems to use upper case comparison ('_' > 'A').
int NtfsFileNameCompare(const wchar_t* cs, const wchar_t * ct)
{
    while (towupper(*cs) == towupper(*ct))
    {
        if (*cs == 0)
            return 0;
        cs++;
        ct++;
    }
    return towupper(*cs) - towupper(*ct);
}

struct FileNameComparer
{
    typedef int (STDAPICALLTYPE * COMPARER)(PCWSTR fileName1, PCWSTR fileName2);
    static COMPARER Comparer;

    bool operator()(const std::wstring& lhs, const std::wstring& rhs) const
    {
        return Comparer(lhs.c_str(), rhs.c_str()) < 0;
    }
};

//rely on core i7 instruction without cpuid for the purpose of this test
uint32_t crc32(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;

    while (len >= 4)
    {
        crc = _mm_crc32_u32(crc, *(uint32_t*)buf);
        buf += 4;
        len -= 4;
    }

    for (uint32_t i = 0; i < (len & 3); ++i)
        crc = _mm_crc32_u8(crc, buf[i]);

    return ~crc;
}

FileNameComparer::COMPARER FileNameComparer::Comparer = PrjFileNameCompare;

typedef std::map<std::wstring, PRJ_PLACEHOLDER_INFO, FileNameComparer> filelist_t;

struct DirEnumSession;
filelist_t                                                     gs_fileList;
std::map<GUID, DirEnumSession*, GUIDComparer>                  gs_activeEnumSessions;
CRITICAL_SECTION                                               gs_cs;
PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT                           gs_virtualizationContext;

struct DirEnumSession {
    std::wstring fileName;
    filelist_t::iterator it;
    bool scanComplete;

    DirEnumSession(PCWSTR fileName)
        : fileName(fileName)
        , scanComplete(false)
        , it(gs_fileList.begin())
    {
    }
};

HRESULT StartDirEnumCallback(
    const PRJ_CALLBACK_DATA*   CallbackData,
    const GUID*                EnumerationId
)
{
    EnterCriticalSection(&gs_cs);
    gs_activeEnumSessions[*EnumerationId] = new DirEnumSession(CallbackData->FilePathName);
    LeaveCriticalSection(&gs_cs);

    return S_OK;
};

HRESULT EndDirEnumCallback(
    const PRJ_CALLBACK_DATA*   CallbackData,
    const GUID*                EnumerationId
)
{
    EnterCriticalSection(&gs_cs);
    auto it = gs_activeEnumSessions.find(*EnumerationId);
    if (it != gs_activeEnumSessions.end())
    {
        delete it->second;
        gs_activeEnumSessions.erase(it);
    }
    LeaveCriticalSection(&gs_cs);

    return S_OK;
};

HRESULT GetDirEnumCallback(
    const PRJ_CALLBACK_DATA*    CallbackData,
    const GUID*                 EnumerationId,
    PCWSTR                      SearchExpression,
    PRJ_DIR_ENTRY_BUFFER_HANDLE DirEntryBufferHandle
)
{
    DirEnumSession * dirEntry;
    EnterCriticalSection(&gs_cs);
    auto it = gs_activeEnumSessions.find(*EnumerationId);
    if (it == gs_activeEnumSessions.end()) {
        LeaveCriticalSection(&gs_cs);
        return E_UNEXPECTED;
    }
    dirEntry = it->second;
    LeaveCriticalSection(&gs_cs);

    if (CallbackData->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN)
        dirEntry->it = gs_fileList.begin();

    bool wildCards = (SearchExpression != nullptr && PrjDoesNameContainWildCards(SearchExpression));
    for (; dirEntry->it != gs_fileList.end(); ++dirEntry->it)
    {
        if (wildCards)
        {
            if (!PrjFileNameMatch(dirEntry->it->first.c_str(), SearchExpression))
                continue;
        }
        else if (SearchExpression != nullptr && SearchExpression[0] != L'\0' && _wcsicmp(dirEntry->it->first.c_str(), SearchExpression))
            continue;

        HRESULT hr = PrjFillDirEntryBuffer(dirEntry->it->first.c_str(), &dirEntry->it->second.FileBasicInfo, DirEntryBufferHandle);
        if (FAILED(hr))
        {
            if (HRESULT_CODE(hr) == ERROR_INSUFFICIENT_BUFFER)
                return S_OK;
            else
                return hr;
        }
    }

    return S_OK;
}

HRESULT GetPlaceholderInfoCallback(
    const PRJ_CALLBACK_DATA*    CallbackData
)
{
    auto it = gs_fileList.find(CallbackData->FilePathName);
    if (it == gs_fileList.end())
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

    HRESULT hr =
        PrjWritePlaceholderInfo(
            gs_virtualizationContext,
            CallbackData->FilePathName,
            &it->second,
            sizeof(it->second)
        );
    VERIFY(SUCCEEDED(hr));
    return S_OK;
}

HRESULT GetFileDataCallbackErrorInjection = S_OK;
HRESULT GetFileDataCallback(
    const PRJ_CALLBACK_DATA*    CallbackData,
    UINT64                      ByteOffset,
    UINT32                      Length
)
{
    if (Length == 0)
        return S_OK;

    char * buf = (char*)PrjAllocateAlignedBuffer(gs_virtualizationContext, sizeof(gs_fileBuf));
    VERIFY(buf);
    memcpy(buf, gs_fileBuf, sizeof(gs_fileBuf));

    VERIFY(ByteOffset == 0);
    VERIFY(Length == sizeof(gs_fileBuf));

    //split the write in 2 calls so we can inject a failure in the middle
    HRESULT hr1 =
        PrjWriteFileData(
            gs_virtualizationContext,
            &CallbackData->DataStreamId,
            buf,
            ByteOffset,
            Length/2
        );
    
    if (GetFileDataCallbackErrorInjection != S_OK)
    {
        PrjFreeAlignedBuffer(buf);
        return GetFileDataCallbackErrorInjection;
    }

    HRESULT hr2 =
        PrjWriteFileData(
            gs_virtualizationContext,
            &CallbackData->DataStreamId,
            buf + Length / 2,
            ByteOffset + Length / 2,
            Length - Length / 2
        );

    PrjFreeAlignedBuffer(buf);

    VERIFY(SUCCEEDED(hr1) && SUCCEEDED(hr2));
    return S_OK;
}

void StartVirtualization(LPCWSTR virtualPath)
{
    PRJ_CALLBACKS cb;
    memset(&cb, 0, sizeof(cb));
    cb.StartDirectoryEnumerationCallback = StartDirEnumCallback;
    cb.EndDirectoryEnumerationCallback   = EndDirEnumCallback;
    cb.GetDirectoryEnumerationCallback   = GetDirEnumCallback;
    cb.GetPlaceholderInfoCallback        = GetPlaceholderInfoCallback;
    cb.GetFileDataCallback               = GetFileDataCallback;

    PRJ_STARTVIRTUALIZING_OPTIONS options;
    memset(&options, 0, sizeof(options));

    GUID instanceID;
    CoCreateGuid(&instanceID);

    HRESULT hr =
        PrjMarkDirectoryAsPlaceholder(
            virtualPath,
            nullptr,
            nullptr,
            &instanceID);

    VERIFY(SUCCEEDED(hr) || HRESULT_CODE(hr) == ERROR_REPARSE_POINT_ENCOUNTERED);
    
    hr =
        PrjStartVirtualizing(
            virtualPath,
            &cb,
            0,
            &options,
            &gs_virtualizationContext
        );
    VERIFY(SUCCEEDED(hr));
}

void StopVirtualization()
{
    PrjStopVirtualizing(gs_virtualizationContext);
}

BOOL MaterializeFile(LPCWSTR fullName, DWORD & totalReadBytes, uint32_t & crc)
{
    HANDLE hFile =
        CreateFile(
            fullName,
            GENERIC_READ,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    char buffer[4096];
    crc = 0;
    totalReadBytes = 0;
    DWORD readBytes = 0;
    while (ReadFile(hFile, buffer, sizeof(buffer), &readBytes, nullptr) && readBytes > 0)
    {
        crc = crc32(crc, (uint8_t*)buffer, readBytes);
        totalReadBytes += readBytes;
    }

    DWORD lastError = GetLastError();
    VERIFY(CloseHandle(hFile));
    SetLastError(lastError);
    return lastError == ERROR_SUCCESS;
}

BOOL VirtualizeFile(LPCWSTR fileName)
{
    PRJ_UPDATE_FAILURE_CAUSES fc;
    HRESULT hr =
        PrjDeleteFile(
            gs_virtualizationContext,
            fileName,
            PRJ_UPDATE_ALLOW_TOMBSTONE | PRJ_UPDATE_ALLOW_READ_ONLY | PRJ_UPDATE_ALLOW_DIRTY_METADATA | PRJ_UPDATE_ALLOW_DIRTY_DATA,
            &fc
        );
    return SUCCEEDED(hr);
}

int CountFiles(LPCWSTR rootPath)
{
    wchar_t findFile[MAX_PATH];
    wsprintf(findFile, L"%s\\*", rootPath);

    //count file in folder
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(findFile, &fd);
    VERIFY(hFind != INVALID_HANDLE_VALUE);

    wchar_t fullName[MAX_PATH];

    int fileCount = 0;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;

        //wprintf(L"%s\n", fd.cFileName);
        wsprintf(fullName, L"%s\\%s", rootPath, fd.cFileName);
        DWORD totalBytes;
        uint32_t crc;
        MaterializeFile(fullName, totalBytes, crc);
        fileCount++;
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
    return fileCount;
}

#pragma endregion

#pragma region tests
//NTFS seems to sort files using toupper instead of tolower
//PrjFlt seems to use tolower in wcsicmp so if any file contains an underscore _ in their name,
//the sorting gets screwed and some files might appear duplicates in the folder once materialized
//because the PrjFlt O(n) algo to merge virtual list with hydrated files won't have the same sorting order.
bool ReproduceSortingBug(LPCWSTR rootPath, FileNameComparer::COMPARER comparer)
{
    FileNameComparer::Comparer = comparer;

    //setup a read-only file in our file list
    PRJ_PLACEHOLDER_INFO pi;
    memset(&pi, 0, sizeof(pi));
    pi.FileBasicInfo.IsDirectory = FALSE;
    pi.FileBasicInfo.FileAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
    pi.FileBasicInfo.FileSize = sizeof(gs_fileBuf);
    gs_fileList.clear();

    //only those files should appear in the folder after being materialized, if not, 
    //there is a sorting bug because of lower-case/upper-case mismatch between NTFS and PrjFileNameCompare
    gs_fileList.insert(std::make_pair(L"FileA", pi));
    gs_fileList.insert(std::make_pair(L"File_", pi));
    gs_fileList.insert(std::make_pair(L"Fileb", pi));

    CreateDirectory(rootPath, 0);
    StartVirtualization(rootPath);

    //the first pass we materialize the files so they are stored in NTFS
    int count1 = CountFiles(rootPath);   

    //second pass it will trigger the merge between NTFS and virtualized files and should still be 3 files
    int count2 = CountFiles(rootPath);   

    StopVirtualization();
    
    return count1 != 3 || count2 != 3;
}

//if a virtual file READONLY attribute is set, it might take 2 tries before being able to effectively remove
//the read-only flag, which cause access denied in any tool trying to remove the read-only flag and then overwrite the file.
bool ReproduceRemoveReadOnlyFlagNotReallyRemoved(LPCWSTR rootPath)
{
    LPCWSTR fileName = L"File_ReadOnly.bat";

    //setup a read-only file in our file list
    PRJ_PLACEHOLDER_INFO pi;
    memset(&pi, 0, sizeof(pi));
    pi.FileBasicInfo.IsDirectory = FALSE;
    pi.FileBasicInfo.FileAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE;
    pi.FileBasicInfo.FileSize = sizeof(gs_fileBuf);
    gs_fileList.clear();
    gs_fileList.insert(std::make_pair(fileName, pi));
    
    CreateDirectory(rootPath, 0);
    StartVirtualization(rootPath);

    wchar_t fullName[MAX_PATH];
    wsprintf(fullName, L"%s\\%s", rootPath, fileName);

    while (1)
    {
        DWORD totalBytes;
        uint32_t crc;
        VERIFY(MaterializeFile(fullName, totalBytes, crc));

        //remove read-only attribute so we can write to it
        DWORD attr;
        int retry = 0;
        for (; (attr = GetFileAttributes(fullName)) != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY); retry++)
        {
            VERIFY(SetFileAttributes(fullName, FILE_ATTRIBUTE_NORMAL));
        }

        if (retry > 1)
        {
            StopVirtualization();
            return true; //bug reproduced
        }

        //revirtualize the file and try again
        VERIFY(VirtualizeFile(fileName));
    }

    StopVirtualization();
    return false;
}

#pragma endregion

int main()
{
    InitializeCriticalSection(&gs_cs);

    char c = 'a';
    for (int i = 0; i < sizeof(gs_fileBuf); ++i)
        gs_fileBuf[i] = c + (i % 25);
    gs_fileBufCrc32 = crc32(0, (uint8_t*)gs_fileBuf, sizeof(gs_fileBuf));
    
    DWORD dwMode = 0;
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &dwMode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const char * PASS = "\x1b[92mPASS\x1b[39m";
    const char * FAIL = "\x1b[91mFAIL\x1b[39m";

    //all those bugs have been reproduced on W10 1809 latest insider build I could find (10.0.18272.1000)
    printf("Sorting Test with NtfsFileNameCompare: ");
    printf("%s\n", ReproduceSortingBug(L"C:\\PRJFLTBUG_SORTING_NTFS", NtfsFileNameCompare) ? FAIL : PASS);

    printf("Sorting Test with PrjFileNameCompare : ");
    printf("%s\n", ReproduceSortingBug(L"C:\\PRJFLTBUG_SORTING_PRJ", PrjFileNameCompare) ? FAIL : PASS);

    printf("Remove Read-Only Attribute Test      : ");
    printf("%s\n", ReproduceRemoveReadOnlyFlagNotReallyRemoved(L"C:\\PRJFLTBUG_READONLY") ? FAIL : PASS);

    return 0;
}

