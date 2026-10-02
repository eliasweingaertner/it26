/*
 * it_dialog_win32.c
 * -----------------
 * Feature 016 (issue #26): the Windows system file dialogs for the Win32
 * pixel backend -- IFileOpenDialog / IFileSaveDialog, driven from C
 * through COM's lpVtbl macros.
 *
 * The dialogs need a single-threaded COM apartment, but miniaudio has
 * already initialised the UI thread as multithreaded (MA_COINIT_VALUE 0),
 * so each dialog runs on its own short-lived STA thread. The UI thread
 * waits for it while still dispatching window messages, so the owner
 * window keeps painting and cross-thread owner calls cannot deadlock.
 *
 * Paths (research R5): the dialog answers in UTF-16; the port's file
 * calls take ANSI. A lossless ANSI conversion is used as is; otherwise
 * the 8.3 short path (pure ASCII). The CP437 display string comes from
 * the UTF-16 with '?' for anything CP437 cannot show.
 */

#ifdef _WIN32

#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
#include <string.h>
#include "it_screen.h"

typedef struct {
    HWND                   owner;
    const it_dialog_req_t *req;
    it_dialog_res_t       *res;
} dlg_job_t;

static void to_wide(const char *s, WCHAR *w, int cap)
{
    w[0] = 0;
    if (s && *s && !MultiByteToWideChar(CP_ACP, 0, s, -1, w, cap))
        w[0] = 0;
}

/* UTF-16 -> CP437 display, '?' per unmappable character */
static int wide_display(const WCHAR *w, char *out, size_t cap)
{
    size_t n = 0;
    int lossy = 0;

    for (; *w && n + 1 < cap; w++) {
        uint32_t u = *w;
        uint16_t c;
        if (u >= 0xD800 && u <= 0xDBFF && w[1] >= 0xDC00 && w[1] <= 0xDFFF) {
            w++;                        /* outside the BMP: never CP437 */
            u = 0;
        }
        c = (u >= 32) ? Screen_UnicodeToCP437(u) : 0;
        if (!c) {
            c = '?';
            lossy = 1;
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
    return lossy;
}

/* UTF-16 -> ANSI; 1 only if every character survived */
static int wide_to_ansi(const WCHAR *w, char *out, int cap)
{
    BOOL used = FALSE;
    int n;

    if (GetACP() == CP_UTF8)            /* "Beta: UTF-8" system setting */
        return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap,
                                   NULL, NULL) > 0;
    n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, cap,
                            NULL, &used);
    return n > 0 && !used;
}

static void fill_result(const WCHAR *w, it_dialog_res_t *res, int creating)
{
    WCHAR shortp[IT_DLG_PATH_MAX];
    DWORD k;
    int created = 0;

    res->lossy = wide_display(w, res->display, sizeof(res->display));
    if (wcslen(w) >= IT_DLG_PATH_MAX - 1) {
        res->status = IT_DLG_REJECTED;
        res->reason = "Path too long";
        return;
    }
    if (wide_to_ansi(w, res->path, (int)sizeof(res->path))) {
        res->status = IT_DLG_CHOSEN;
        return;
    }
    /* the ANSI code page cannot spell this name: use the 8.3 alias,
     * creating the (empty) file first when saving under a new name so
     * an alias exists */
    if (creating) {
        HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            created = GetLastError() != ERROR_ALREADY_EXISTS;
            CloseHandle(h);
        }
    }
    k = GetShortPathNameW(w, shortp, IT_DLG_PATH_MAX);
    if (k > 0 && k < IT_DLG_PATH_MAX && wcscmp(shortp, w) != 0 &&
        wide_to_ansi(shortp, res->path, (int)sizeof(res->path))) {
        res->status = IT_DLG_CHOSEN;
        return;
    }
    if (created)
        DeleteFileW(w);
    res->path[0] = 0;
    res->status = IT_DLG_REJECTED;
    res->reason = creating ? "Can't save under this name on this drive"
                           : "Can't use this file name on this drive";
}

/* "it;s3m" -> L"*.it;*.s3m" */
static void ext_spec(const char *exts, WCHAR *out, int cap)
{
    int n = 0;
    const char *p = exts;

    out[0] = 0;
    while (*p && n < cap - 8) {
        if (n) out[n++] = L';';
        out[n++] = L'*';
        out[n++] = L'.';
        while (*p && *p != ';' && n < cap - 2)
            out[n++] = (WCHAR)*p++;
        if (*p == ';')
            p++;
    }
    out[n] = 0;
}

static void set_start_folder(IFileDialog *d, const char *dir)
{
    WCHAR w[MAX_PATH];
    IShellItem *item = NULL;

    to_wide(dir, w, MAX_PATH);
    if (!w[0])
        GetCurrentDirectoryW(MAX_PATH, w);
    if (SUCCEEDED(SHCreateItemFromParsingName(w, NULL, &IID_IShellItem,
                                              (void **)&item))) {
        IFileDialog_SetFolder(d, item);
        IShellItem_Release(item);
    }
}

static DWORD WINAPI dialog_thread(LPVOID arg)
{
    dlg_job_t *job = (dlg_job_t *)arg;
    const it_dialog_req_t *req = job->req;
    it_dialog_res_t *res = job->res;
    int save = req->kind == IT_DLG_SAVE_MODULE;
    int folder = req->kind == IT_DLG_PICK_FOLDER;
    IFileDialog *d = NULL;
    IShellItem *item = NULL;
    HRESULT hr;
    DWORD opts = 0;
    WCHAR title[64], spec[256], name[MAX_PATH];
    COMDLG_FILTERSPEC types[2];

    res->status = IT_DLG_UNAVAILABLE;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                    COINIT_DISABLE_OLE1DDE)))
        return 0;
    hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog,
                          NULL, CLSCTX_INPROC_SERVER,
                          save ? &IID_IFileSaveDialog : &IID_IFileOpenDialog,
                          (void **)&d);
    if (FAILED(hr) || !d)
        goto out;

    to_wide(Screen_DialogTitle(req->kind), title, 64);
    IFileDialog_SetTitle(d, title);
    IFileDialog_GetOptions(d, &opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (folder)
        opts |= FOS_PICKFOLDERS;
    else if (save)
        opts &= ~(DWORD)FOS_OVERWRITEPROMPT;    /* the tracker asks, as F10 */
    else
        opts |= FOS_FILEMUSTEXIST;
    IFileDialog_SetOptions(d, opts);

    if (save) {
        types[0].pszName = L"Impulse Tracker (*.it)";
        types[0].pszSpec = L"*.it";
        types[1].pszName = L"Scream Tracker 3 (*.s3m)";
        types[1].pszSpec = L"*.s3m";
        IFileDialog_SetFileTypes(d, 2, types);
        IFileDialog_SetFileTypeIndex(d, req->save_format == 1 ? 2 : 1);
        IFileDialog_SetDefaultExtension(d, req->save_format == 1 ? L"s3m"
                                                                 : L"it");
        to_wide(req->suggest_name, name, MAX_PATH);
        if (name[0])
            IFileDialog_SetFileName(d, name);
    } else if (!folder) {
        ext_spec(Screen_DialogExts(req->kind), spec, 256);
        types[0].pszName = title;               /* e.g. "Load Sample" */
        types[0].pszSpec = spec;
        types[1].pszName = L"All files (*.*)";
        types[1].pszSpec = L"*.*";
        IFileDialog_SetFileTypes(d, 2, types);
        IFileDialog_SetFileTypeIndex(d, 1);
    }
    set_start_folder(d, req->start_dir);

    hr = IFileDialog_Show(d, job->owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        res->status = IT_DLG_CANCELLED;
    } else if (SUCCEEDED(hr) &&
               SUCCEEDED(IFileDialog_GetResult(d, &item)) && item) {
        PWSTR w = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH,
                                                &w)) && w) {
            fill_result(w, res, save);
            if (save && res->status == IT_DLG_CHOSEN) {
                UINT idx = 1;
                /* a typed extension wins; otherwise the chosen type */
                if (strrchr(res->display, '.') &&
                    strrchr(res->display, '.') > strrchr(res->display, '\\'))
                    res->save_format = Screen_SaveFormatFromName(res->display);
                else if (SUCCEEDED(IFileDialog_GetFileTypeIndex(d, &idx)))
                    res->save_format = idx == 2 ? 1 : 0;
            }
            CoTaskMemFree(w);
        }
        IShellItem_Release(item);
    } else {
        res->status = IT_DLG_CANCELLED;
    }

out:
    if (d)
        IFileDialog_Release(d);
    CoUninitialize();
    return 0;
}

int Dialog_Win32(HWND owner, const it_dialog_req_t *req, it_dialog_res_t *res)
{
    dlg_job_t job;
    HANDLE th;

    job.owner = owner;
    job.req = req;
    job.res = res;
    th = CreateThread(NULL, 0, dialog_thread, &job, 0, NULL);
    if (!th) {
        res->status = IT_DLG_UNAVAILABLE;
        return res->status;
    }
    /* keep the owner window alive (paint, cross-thread owner calls);
     * its key messages are dropped by the backend afterwards */
    for (;;) {
        DWORD w = MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE,
                                            QS_ALLINPUT);
        MSG msg;
        if (w == WAIT_OBJECT_0)
            break;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    CloseHandle(th);
    return res->status;
}

#else
typedef int it_dialog_win32_unused;     /* ISO C: no empty translation unit */
#endif /* _WIN32 */
