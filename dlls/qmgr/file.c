/*
 * Queue Manager (BITS) File
 *
 * Copyright 2007, 2008 Google (Roy Shea, Dan Hipschman)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winreg.h"
#include "winhttp.h"
#define COBJMACROS
#include "qmgr.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(qmgr);

static inline BackgroundCopyFileImpl *impl_from_IBackgroundCopyFile2(
    IBackgroundCopyFile2 *iface)
{
    return CONTAINING_RECORD(iface, BackgroundCopyFileImpl, IBackgroundCopyFile2_iface);
}

static HRESULT WINAPI BackgroundCopyFile_QueryInterface(
    IBackgroundCopyFile2 *iface,
    REFIID riid,
    void **obj)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);

    TRACE("(%p)->(%s %p)\n", file, debugstr_guid(riid), obj);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IBackgroundCopyFile) ||
        IsEqualGUID(riid, &IID_IBackgroundCopyFile2))
    {
        *obj = iface;
    }
    else
    {
        *obj = NULL;
        return E_NOINTERFACE;
    }

    IBackgroundCopyFile2_AddRef(iface);
    return S_OK;
}

static ULONG WINAPI BackgroundCopyFile_AddRef(
    IBackgroundCopyFile2 *iface)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);
    ULONG ref = InterlockedIncrement(&file->ref);
    TRACE("(%p)->(%ld)\n", file, ref);
    return ref;
}

static ULONG WINAPI BackgroundCopyFile_Release(
    IBackgroundCopyFile2 *iface)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);
    ULONG ref = InterlockedDecrement(&file->ref);

    TRACE("(%p)->(%ld)\n", file, ref);

    if (ref == 0)
    {
        IBackgroundCopyJob4_Release(&file->owner->IBackgroundCopyJob4_iface);
        free(file->info.LocalName);
        free(file->info.RemoteName);
        free(file->ranges);
        free(file);
    }

    return ref;
}

/* Get the remote name of a background copy file */
static HRESULT WINAPI BackgroundCopyFile_GetRemoteName(
    IBackgroundCopyFile2 *iface,
    LPWSTR *pVal)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);

    TRACE("(%p)->(%p)\n", file, pVal);

    return return_strval(file->info.RemoteName, pVal);
}

static HRESULT WINAPI BackgroundCopyFile_GetLocalName(
    IBackgroundCopyFile2 *iface,
    LPWSTR *pVal)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);

    TRACE("(%p)->(%p)\n", file, pVal);

    return return_strval(file->info.LocalName, pVal);
}

static HRESULT WINAPI BackgroundCopyFile_GetProgress(
    IBackgroundCopyFile2 *iface,
    BG_FILE_PROGRESS *pVal)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);

    TRACE("(%p)->(%p)\n", file, pVal);

    EnterCriticalSection(&file->owner->cs);
    *pVal = file->fileProgress;
    LeaveCriticalSection(&file->owner->cs);

    return S_OK;
}

static HRESULT WINAPI BackgroundCopyFile_GetFileRanges(
    IBackgroundCopyFile2 *iface,
    DWORD *RangeCount,
    BG_FILE_RANGE **Ranges)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);

    TRACE("(%p)->(%p %p)\n", file, RangeCount, Ranges);

    if (!RangeCount || !Ranges) return E_INVALIDARG;
    *RangeCount = file->range_count;
    *Ranges = NULL;
    if (!file->range_count) return S_OK;
    if (!(*Ranges = CoTaskMemAlloc(file->range_count * sizeof(**Ranges)))) return E_OUTOFMEMORY;
    memcpy(*Ranges, file->ranges, file->range_count * sizeof(**Ranges));
    return S_OK;
}

static HRESULT WINAPI BackgroundCopyFile_SetRemoteName(
    IBackgroundCopyFile2 *iface,
    LPCWSTR Val)
{
    BackgroundCopyFileImpl *file = impl_from_IBackgroundCopyFile2(iface);
    FIXME("(%p)->(%s)\n", file, debugstr_w(Val));
    return E_NOTIMPL;
}

static const IBackgroundCopyFile2Vtbl BackgroundCopyFile2Vtbl =
{
    BackgroundCopyFile_QueryInterface,
    BackgroundCopyFile_AddRef,
    BackgroundCopyFile_Release,
    BackgroundCopyFile_GetRemoteName,
    BackgroundCopyFile_GetLocalName,
    BackgroundCopyFile_GetProgress,
    BackgroundCopyFile_GetFileRanges,
    BackgroundCopyFile_SetRemoteName
};

HRESULT BackgroundCopyFileConstructor(BackgroundCopyJobImpl *owner,
                                      LPCWSTR remoteName, LPCWSTR localName,
                                      BackgroundCopyFileImpl **file)
{
    BackgroundCopyFileImpl *This;

    TRACE("(%s, %s, %p)\n", debugstr_w(remoteName), debugstr_w(localName), file);

    This = calloc(1, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->info.RemoteName = wcsdup(remoteName);
    if (!This->info.RemoteName)
    {
        free(This);
        return E_OUTOFMEMORY;
    }

    This->info.LocalName = wcsdup(localName);
    if (!This->info.LocalName)
    {
        free(This->info.RemoteName);
        free(This);
        return E_OUTOFMEMORY;
    }

    This->IBackgroundCopyFile2_iface.lpVtbl = &BackgroundCopyFile2Vtbl;
    This->ref = 1;
    This->fileProgress.BytesTotal = BG_SIZE_UNKNOWN;
    This->owner = owner;
    IBackgroundCopyJob4_AddRef(&owner->IBackgroundCopyJob4_iface);

    *file = This;
    return S_OK;
}

static HRESULT hresult_from_http_response(DWORD code)
{
    switch (code)
    {
    case 200: return S_OK;
    case 206: return S_OK;
    case 400: return BG_E_HTTP_ERROR_400;
    case 401: return BG_E_HTTP_ERROR_401;
    case 404: return BG_E_HTTP_ERROR_404;
    case 407: return BG_E_HTTP_ERROR_407;
    case 414: return BG_E_HTTP_ERROR_414;
    case 501: return BG_E_HTTP_ERROR_501;
    case 503: return BG_E_HTTP_ERROR_503;
    case 504: return BG_E_HTTP_ERROR_504;
    case 505: return BG_E_HTTP_ERROR_505;
    default:
        FIXME("unhandled response code %lu\n", code);
        return S_OK;
    }
}

static void CALLBACK progress_callback_http(HINTERNET handle, DWORD_PTR context, DWORD status,
                                            LPVOID buf, DWORD buflen)
{
    BackgroundCopyFileImpl *file = (BackgroundCopyFileImpl *)context;
    BackgroundCopyJobImpl *job = file->owner;

    TRACE("%p, %p, %lx, %p, %lu\n", handle, file, status, buf, buflen);

    switch (status)
    {
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    {
        DWORD code, len, size;

        size = sizeof(code);
        if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &code, &size, NULL))
        {
            if ((job->error.code = hresult_from_http_response(code)))
            {
                EnterCriticalSection(&job->cs);

                job->error.context = BG_ERROR_CONTEXT_REMOTE_FILE;
                if (job->error.file) IBackgroundCopyFile2_Release(job->error.file);
                job->error.file = &file->IBackgroundCopyFile2_iface;
                IBackgroundCopyFile2_AddRef(job->error.file);

                LeaveCriticalSection(&job->cs);
                transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_ERROR);
            }
            else
            {
                EnterCriticalSection(&job->cs);

                job->error.context = 0;
                if (job->error.file)
                {
                    IBackgroundCopyFile2_Release(job->error.file);
                    job->error.file = NULL;
                }

                LeaveCriticalSection(&job->cs);
            }
        }
        size = sizeof(len);
        /* With ranges, the total comes from the ranges (transfer_file_http). */
        if (!file->range_count &&
            WinHttpQueryHeaders(handle, WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER,
                                NULL, &len, &size, NULL))
        {
            file->fileProgress.BytesTotal = len;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
    {
        file->read_size = buflen;
        break;
    }
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
        WINHTTP_ASYNC_RESULT *result = (WINHTTP_ASYNC_RESULT *)buf;
        job->error.code = HRESULT_FROM_WIN32(result->dwError);
        transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_ERROR);
        break;
    }
    default: break;
    }

    SetEvent(job->wait);
}

static DWORD wait_for_completion(BackgroundCopyJobImpl *job)
{
    HANDLE handles[2] = {job->wait, job->cancel};
    DWORD error = ERROR_SUCCESS;

    switch (WaitForMultipleObjects(2, handles, FALSE, INFINITE))
    {
    case WAIT_OBJECT_0:
        break;

    case WAIT_OBJECT_0 + 1:
        error = ERROR_CANCELLED;
        transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_CANCELLED);
        break;

    default:
        error = GetLastError();
        transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_ERROR);
        break;
    }

    return error;
}

static UINT target_from_index(UINT index)
{
    switch (index)
    {
    case 0: return WINHTTP_AUTH_TARGET_SERVER;
    case 1: return WINHTTP_AUTH_TARGET_PROXY;
    default:
        ERR("unhandled index %u\n", index);
        break;
    }
    return 0;
}

static UINT scheme_from_index(UINT index)
{
    switch (index)
    {
    case 0: return WINHTTP_AUTH_SCHEME_BASIC;
    case 1: return WINHTTP_AUTH_SCHEME_NTLM;
    case 2: return WINHTTP_AUTH_SCHEME_PASSPORT;
    case 3: return WINHTTP_AUTH_SCHEME_DIGEST;
    case 4: return WINHTTP_AUTH_SCHEME_NEGOTIATE;
    default:
        ERR("unhandled index %u\n", index);
        break;
    }
    return 0;
}

static BOOL set_request_credentials(HINTERNET req, BackgroundCopyJobImpl *job)
{
    UINT i, j;

    for (i = 0; i < BG_AUTH_TARGET_PROXY; i++)
    {
        UINT target = target_from_index(i);
        for (j = 0; j < BG_AUTH_SCHEME_PASSPORT; j++)
        {
            UINT scheme = scheme_from_index(j);
            const WCHAR *username = job->http_options.creds[i][j].Credentials.Basic.UserName;
            const WCHAR *password = job->http_options.creds[i][j].Credentials.Basic.Password;

            if (!username) continue;
            if (!WinHttpSetCredentials(req, target, scheme, username, password, NULL)) return FALSE;
        }
    }
    return TRUE;
}

static void set_file_error(BackgroundCopyFileImpl *file, HRESULT code)
{
    BackgroundCopyJobImpl *job = file->owner;

    EnterCriticalSection(&job->cs);
    job->error.code = code;
    job->error.context = BG_ERROR_CONTEXT_REMOTE_FILE;
    if (job->error.file) IBackgroundCopyFile2_Release(job->error.file);
    job->error.file = &file->IBackgroundCopyFile2_iface;
    IBackgroundCopyFile2_AddRef(job->error.file);
    LeaveCriticalSection(&job->cs);
}

/* A "Range: bytes=..." line in front of the job's own headers. */
static WCHAR *range_headers(BackgroundCopyJobImpl *job, const BG_FILE_RANGE *range)
{
    const WCHAR *extra = job->http_options.headers ? job->http_options.headers : L"";
    size_t len = wcslen(extra) + 64;
    WCHAR *headers;

    if (!(headers = malloc(len * sizeof(WCHAR)))) return NULL;
    if (range->Length == BG_LENGTH_TO_EOF)
        swprintf(headers, len, L"Range: bytes=%I64u-\r\n%s", range->InitialOffset, extra);
    else
        swprintf(headers, len, L"Range: bytes=%I64u-%I64u\r\n%s", range->InitialOffset,
                 range->InitialOffset + range->Length - 1, extra);
    return headers;
}

/* The server must answer a range with 206 and that range: a 200 means it ignored the range,
 * a 416 that the range is past the end of the file. */
static HRESULT check_range_response(BackgroundCopyFileImpl *file, HINTERNET req,
                                    const BG_FILE_RANGE *range)
{
    WCHAR content_range[128];
    DWORD code, size = sizeof(code);
    UINT64 first, len;

    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                             NULL, &code, &size, NULL))
        return BG_E_INSUFFICIENT_RANGE_SUPPORT;
    if (code == 416) return BG_E_INVALID_RANGE;
    if (code != 206) return BG_E_INSUFFICIENT_RANGE_SUPPORT;

    size = sizeof(content_range);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_RANGE, NULL, content_range, &size, NULL) ||
        swscanf(content_range, L"bytes %I64u-", &first) != 1 || first != range->InitialOffset)
    {
        WARN("asked for bytes from %I64u, got %s\n", range->InitialOffset, debugstr_w(content_range));
        return BG_E_INSUFFICIENT_RANGE_SUPPORT;
    }

    size = sizeof(len);
    if (range->Length == BG_LENGTH_TO_EOF &&
        WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH|WINHTTP_QUERY_FLAG_NUMBER64,
                            NULL, &len, &size, NULL))
    {
        BackgroundCopyJobImpl *job = file->owner;
        EnterCriticalSection(&job->cs);
        if (file->fileProgress.BytesTotal == BG_SIZE_UNKNOWN) file->fileProgress.BytesTotal = 0;
        file->fileProgress.BytesTotal += len;
        LeaveCriticalSection(&job->cs);
    }
    return S_OK;
}

/* The whole file in one request, or each range in its own request on the same connection,
 * written one after another in the order given (AddFileWithRanges). */
static BOOL transfer_file_http(BackgroundCopyFileImpl *file, URL_COMPONENTSW *uc,
                               const WCHAR *tmpfile)
{
    BackgroundCopyJobImpl *job = file->owner;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HINTERNET ses, con = NULL, req = NULL;
    DWORD flags = (uc->nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    DWORD count = file->range_count ? file->range_count : 1, i;
    WCHAR *headers = NULL;
    char buf[4096];
    BOOL ret = FALSE;
    DWORD written;
    HRESULT hr;

    transitionJobState(job, BG_JOB_STATE_QUEUED, BG_JOB_STATE_CONNECTING);

    if (!(ses = WinHttpOpen(NULL, 0, NULL, NULL, WINHTTP_FLAG_ASYNC))) return FALSE;
    WinHttpSetStatusCallback(ses, progress_callback_http, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 0);
    if (!WinHttpSetOption(ses, WINHTTP_OPTION_CONTEXT_VALUE, &file, sizeof(file))) goto done;

    if (!(con = WinHttpConnect(ses, uc->lpszHostName, uc->nPort, 0))) goto done;

    for (i = 0; i < count; i++)
    {
        const BG_FILE_RANGE *range = file->range_count ? &file->ranges[i] : NULL;
        UINT64 received = 0;

        ret = FALSE;
        if (range && !range->Length) { ret = TRUE; continue; }

        if (!(req = WinHttpOpenRequest(con, NULL, uc->lpszUrlPath, NULL, NULL, NULL, flags))) goto done;
        if (!set_request_credentials(req, job)) goto done;
        if (range && !(headers = range_headers(job, range))) goto done;

        if (!(WinHttpSendRequest(req, range ? headers : job->http_options.headers, ~0u, NULL, 0, 0,
                                 (DWORD_PTR)file))) goto done;
        if (wait_for_completion(job) || FAILED(job->error.code)) goto done;

        if (!(WinHttpReceiveResponse(req, NULL))) goto done;
        if (wait_for_completion(job) || FAILED(job->error.code)) goto done;

        if (range && FAILED(hr = check_range_response(file, req, range)))
        {
            set_file_error(file, hr);
            goto done;
        }

        transitionJobState(job, BG_JOB_STATE_CONNECTING, BG_JOB_STATE_TRANSFERRING);

        if (handle == INVALID_HANDLE_VALUE)
        {
            handle = CreateFileW(tmpfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (handle == INVALID_HANDLE_VALUE) goto done;
        }

        for (;;)
        {
            file->read_size = 0;
            if (!(ret = WinHttpReadData(req, buf, sizeof(buf), NULL))) break;
            if (wait_for_completion(job) || FAILED(job->error.code))
            {
                ret = FALSE;
                break;
            }
            if (!file->read_size) break;
            if (!(ret = WriteFile(handle, buf, file->read_size, &written, NULL))) break;
            received += file->read_size;

            EnterCriticalSection(&job->cs);
            file->fileProgress.BytesTransferred += file->read_size;
            job->jobProgress.BytesTransferred += file->read_size;
            LeaveCriticalSection(&job->cs);
        }
        if (!ret) goto done;

        /* A range that runs past the end of the file comes back short. */
        if (range && range->Length != BG_LENGTH_TO_EOF && received != range->Length)
        {
            WARN("range at %I64u: asked for %I64u bytes, got %I64u\n", range->InitialOffset,
                 range->Length, received);
            set_file_error(file, BG_E_INVALID_RANGE);
            ret = FALSE;
            goto done;
        }

        WinHttpCloseHandle(req);
        req = NULL;
        free(headers);
        headers = NULL;
    }

done:
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    free(headers);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    if (!ret && !transitionJobState(job, BG_JOB_STATE_CONNECTING, BG_JOB_STATE_ERROR))
        transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_ERROR);

    SetEvent(job->done);
    return ret;
}

static DWORD CALLBACK progress_callback_local(LARGE_INTEGER totalSize, LARGE_INTEGER totalTransferred,
                                              LARGE_INTEGER streamSize, LARGE_INTEGER streamTransferred,
                                              DWORD streamNum, DWORD reason, HANDLE srcFile,
                                              HANDLE dstFile, LPVOID obj)
{
    BackgroundCopyFileImpl *file = obj;
    BackgroundCopyJobImpl *job = file->owner;
    ULONG64 diff;

    EnterCriticalSection(&job->cs);
    diff = (file->fileProgress.BytesTotal == BG_SIZE_UNKNOWN
            ? totalTransferred.QuadPart
            : totalTransferred.QuadPart - file->fileProgress.BytesTransferred);
    file->fileProgress.BytesTotal = totalSize.QuadPart;
    file->fileProgress.BytesTransferred = totalTransferred.QuadPart;
    job->jobProgress.BytesTransferred += diff;
    LeaveCriticalSection(&job->cs);

    return (job->state == BG_JOB_STATE_TRANSFERRING
            ? PROGRESS_CONTINUE
            : PROGRESS_CANCEL);
}

static BOOL transfer_file_local(BackgroundCopyFileImpl *file, const WCHAR *tmpname)
{
    BackgroundCopyJobImpl *job = file->owner;
    const WCHAR *ptr;
    BOOL ret;

    transitionJobState(job, BG_JOB_STATE_QUEUED, BG_JOB_STATE_TRANSFERRING);

    if (lstrlenW(file->info.RemoteName) > 7 && !wcsnicmp(file->info.RemoteName, L"file://", 7))
        ptr = file->info.RemoteName + 7;
    else
        ptr = file->info.RemoteName;

    if (!(ret = CopyFileExW(ptr, tmpname, progress_callback_local, file, NULL, 0)))
    {
        WARN("Local file copy failed: error %lu\n", GetLastError());
        transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_ERROR);
    }

    SetEvent(job->done);
    return ret;
}

BOOL processFile(BackgroundCopyFileImpl *file, BackgroundCopyJobImpl *job)
{
    WCHAR tmpDir[MAX_PATH], tmpName[MAX_PATH];
    WCHAR host[MAX_PATH];
    URL_COMPONENTSW uc;
    BOOL ret;

    if (!GetTempPathW(MAX_PATH, tmpDir))
    {
        ERR("Couldn't create temp file name: %ld\n", GetLastError());
        /* Guessing on what state this should give us */
        transitionJobState(job, BG_JOB_STATE_QUEUED, BG_JOB_STATE_TRANSIENT_ERROR);
        return FALSE;
    }

    if (!GetTempFileNameW(tmpDir, L"BIT", 0, tmpName))
    {
        ERR("Couldn't create temp file: %ld\n", GetLastError());
        /* Guessing on what state this should give us */
        transitionJobState(job, BG_JOB_STATE_QUEUED, BG_JOB_STATE_TRANSIENT_ERROR);
        return FALSE;
    }

    EnterCriticalSection(&job->cs);
    file->fileProgress.BytesTotal = BG_SIZE_UNKNOWN;
    file->fileProgress.BytesTransferred = 0;
    file->fileProgress.Completed = FALSE;
    if (file->range_count)
    {
        /* The ranges' lengths; one to the end of the file adds its length when it arrives. */
        UINT64 total = 0;
        BOOL to_eof = FALSE;
        DWORD i;

        for (i = 0; i < file->range_count; i++)
        {
            if (file->ranges[i].Length == BG_LENGTH_TO_EOF) to_eof = TRUE;
            else total += file->ranges[i].Length;
        }
        file->fileProgress.BytesTotal = to_eof && !total ? BG_SIZE_UNKNOWN : total;
    }
    LeaveCriticalSection(&job->cs);

    TRACE("Transferring: %s -> %s -> %s\n",
          debugstr_w(file->info.RemoteName),
          debugstr_w(tmpName),
          debugstr_w(file->info.LocalName));

    uc.dwStructSize      = sizeof(uc);
    uc.nScheme           = 0;
    uc.lpszScheme        = NULL;
    uc.dwSchemeLength    = 0;
    uc.lpszUserName      = NULL;
    uc.dwUserNameLength  = 0;
    uc.lpszPassword      = NULL;
    uc.dwPasswordLength  = 0;
    uc.lpszHostName      = host;
    uc.dwHostNameLength  = ARRAY_SIZE(host);
    uc.nPort             = 0;
    uc.lpszUrlPath       = NULL;
    uc.dwUrlPathLength   = ~0u;
    uc.lpszExtraInfo     = NULL;
    uc.dwExtraInfoLength = 0;
    ret = WinHttpCrackUrl(file->info.RemoteName, 0, 0, &uc);
    if (!ret)
    {
        TRACE("WinHttpCrackUrl failed, trying local file copy\n");
        if (!transfer_file_local(file, tmpName)) WARN("local transfer failed\n");
    }
    else if (!transfer_file_http(file, &uc, tmpName)) WARN("HTTP transfer failed\n");

    if (transitionJobState(job, BG_JOB_STATE_CONNECTING, BG_JOB_STATE_QUEUED) ||
        transitionJobState(job, BG_JOB_STATE_TRANSFERRING, BG_JOB_STATE_QUEUED))
    {
        lstrcpyW(file->tempFileName, tmpName);

        EnterCriticalSection(&job->cs);
        file->fileProgress.Completed = TRUE;
        job->jobProgress.FilesTransferred++;
        LeaveCriticalSection(&job->cs);

        return TRUE;
    }
    else
    {
        DeleteFileW(tmpName);
        return FALSE;
    }
}
