#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <objbase.h>
#include <gdiplus.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <strsafe.h>

static const wchar_t *TITLE = L"Съёмка ГЖВ · 1.2";
static const wchar_t *MAIN_CLASS = L"LinkamCaptureMain";
static const wchar_t *SELECT_CLASS = L"LinkamCaptureSelection";
static const wchar_t *REVIEW_CLASS = L"LinkamCaptureReview";
static const wchar_t *EXPERIMENT_CLASS = L"LinkamCaptureExperiment";
static const CLSID PNG_ENCODER = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
static const CLSID JPEG_ENCODER = {0x557cf401, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
static const GUID QUALITY_PARAMETER = {0x1d5be4b5, 0xfa4a, 0x452d, {0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb}};
static const UINT_PTR START_TIMER = 1, CAPTURE_TIMER = 2, SELECT_TIMER = 3;
static const int STOP_HOTKEY = 1;
static const unsigned SAMPLE_LIMIT = 60, INCLUSION_LIMIT = 20, MAX_RATE = 30;
enum Control { REGION = 100, FOLDER, START, STOP, REVIEW, LAST, INTERVAL, RATE, MODE_FRAMES, MODE_VIDEO,
               PREV, NEXT, FRAME_SLIDER, EXPORT_FRAME, CLOSE_REVIEW, SAMPLE_BOX, INCLUSION_BOX };

struct Frame {
    unsigned number;            // номер кадра в журнале или в AVI, с единицы
    DWORD elapsed;              // мс от первого кадра
    wchar_t *file;              // серия PNG: имя файла в папке опыта
    unsigned long long offset;  // видео: положение данных JPEG в AVI
    DWORD size;
    bool repeated;              // видео: кадр повторяет предыдущий, новый снимок не успел
};

// AVI с MJPEG в формате OpenDML: первый RIFF совместим с AVI 1.0 (idx1),
// следующие RIFF AVIX позволяют писать файлы больше 1 ГБ.
enum { SUPER_SLOTS = 256, AVIH = 32, STRH = 108, STRF = 172, INDX = 220,
       DMLH = INDX + 24 + 16 * SUPER_SLOTS + 20, MOVI = DMLH + 248 + 8, AVI_HEADER = MOVI + 4 };
static const unsigned long long RIFF_LIMIT = 1000ULL << 20;

struct Avi {
    HANDLE file;
    unsigned long long end, riff, movi, limit;
    DWORD *index;  // пары: смещение данных от 'movi', размер
    unsigned entries, capacity, super, first_frames, total, largest;
};

static HINSTANCE instance;
static HWND main_window, status_label, folder_label, region_button, folder_button;
static HWND start_button, stop_button, interval_box, rate_box, frames_radio, video_radio;
static HWND review_button, last_button;
static HWND review_window, frame_slider, frame_label, previous_button, next_button, export_button;
static HWND sample_box, inclusion_box, preview_label;
static RECT region;
static wchar_t root[MAX_PATH], last_folder[MAX_PATH], series_folder[MAX_PATH], ini[MAX_PATH];
static wchar_t series_name[MAX_PATH], file_prefix[MAX_PATH], journal_path[MAX_PATH], video_path[MAX_PATH];
static wchar_t sample[SAMPLE_LIMIT + 1], inclusion[INCLUSION_LIMIT + 1];
static FILE *journal;
static bool recording, hotkey_registered, dragging, changing_selection, video_mode, capture_video, fine_timer;
static DWORD first_ms, delay_ms, shown_second;
static unsigned written, rate, last_slot, repeats;
static int experiment_answer;
static ULONG jpeg_quality = 90;
static IStream *jpeg_stream;
static BYTE *jpeg_now, *jpeg_last;
static DWORD jpeg_now_size, jpeg_last_size, jpeg_now_capacity, jpeg_last_capacity;
static Avi avi;
static HBITMAP selection_image;
static POINT drag_start, drag_end;
static Frame *frames;
static unsigned frame_count, frame_index, review_rate, review_scale;
static Gdiplus::GpImage *review_image;
static IStream *review_stream;
static HANDLE review_file;
static bool review_video;
static wchar_t review_prefix[MAX_PATH];

static void alert(HWND owner, const wchar_t *message) {
    MessageBoxW(owner, message, TITLE, MB_OK | MB_ICONWARNING);
}

static HWND control(HWND owner, const wchar_t *cls, const wchar_t *label, DWORD style,
                    int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
    HWND result = CreateWindowExW(ex, cls, label, WS_CHILD | WS_VISIBLE | style,
                                  x, y, w, h, owner, (HMENU)(INT_PTR)id, instance, NULL);
    SendMessageW(result, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return result;
}

static bool path_in(wchar_t (&path)[MAX_PATH], const wchar_t *folder, const wchar_t *name) {
    const size_t n = wcslen(folder);
    const wchar_t *separator = n && folder[n - 1] == L'\\' ? L"" : L"\\";
    return SUCCEEDED(StringCchPrintfW(path, MAX_PATH, L"%s%s%s", folder, separator, name));
}

static bool folder_exists(const wchar_t *folder) {
    DWORD attrs = GetFileAttributesW(folder);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool file_exists(const wchar_t *path) {
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static const wchar_t *last_component(const wchar_t *path) {
    const wchar_t *slash = wcsrchr(path, L'\\');
    return slash && slash[1] ? slash + 1 : path;
}

// Время от первого кадра: «чч-мм-сс.мс» для имён файлов, «чч:мм:сс.мс» для надписей.
static void format_time(wchar_t *out, size_t size, DWORD ms, bool for_file) {
    StringCchPrintfW(out, size, for_file ? L"%02lu-%02lu-%02lu.%03lu" : L"%02lu:%02lu:%02lu.%03lu",
        (unsigned long)(ms / 3600000), (unsigned long)(ms / 60000 % 60),
        (unsigned long)(ms / 1000 % 60), (unsigned long)(ms % 1000));
}

static int CALLBACK browse_init(HWND window, UINT message, LPARAM, LPARAM param) {
    if (message == BFFM_INITIALIZED && param) SendMessageW(window, BFFM_SETSELECTIONW, TRUE, param);
    return 0;
}

static bool browse_folder(HWND owner, const wchar_t *title, const wchar_t *initial,
                          wchar_t (&result)[MAX_PATH]) {
    BROWSEINFOW dialog = {};
    dialog.hwndOwner = owner;
    dialog.lpszTitle = title;
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    dialog.lpfn = browse_init;
    dialog.lParam = (LPARAM)initial;
    LPITEMIDLIST item = SHBrowseForFolderW(&dialog);
    if (!item) return false;
    bool ok = SHGetPathFromIDListW(item, result) != FALSE;
    CoTaskMemFree(item);
    if (!ok) alert(owner, L"Выберите обычную папку на диске.");
    return ok;
}

static bool region_valid(const RECT &r) {
    return r.left >= 0 && r.top >= 0 && r.right <= GetSystemMetrics(SM_CXSCREEN)
        && r.bottom <= GetSystemMetrics(SM_CYSCREEN) && r.right - r.left >= 32
        && r.bottom - r.top >= 32;
}

static void save_settings() {
    if (!ini[0]) return;
    HANDLE fresh = CreateFileW(ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fresh != INVALID_HANDLE_VALUE) {
        WORD bom = 0xfeff;
        DWORD bytes = 0;
        WriteFile(fresh, &bom, sizeof(bom), &bytes, NULL);
        CloseHandle(fresh);
    }
    wchar_t value[100];
    StringCchPrintfW(value, 100, L"%ld,%ld,%ld,%ld", region.left, region.top, region.right, region.bottom);
    WritePrivateProfileStringW(L"capture", L"region", value, ini);
    GetWindowTextW(interval_box, value, 100);
    WritePrivateProfileStringW(L"capture", L"interval", value, ini);
    GetWindowTextW(rate_box, value, 100);
    WritePrivateProfileStringW(L"capture", L"fps", value, ini);
    WritePrivateProfileStringW(L"capture", L"mode", video_mode ? L"video" : L"frames", ini);
    WritePrivateProfileStringW(L"capture", L"root", root, ini);
    WritePrivateProfileStringW(L"capture", L"last", last_folder, ini);
    WritePrivateProfileStringW(L"capture", L"sample", sample, ini);
    WritePrivateProfileStringW(L"capture", L"inclusion", inclusion, ini);
}

static void load_settings() {
    wchar_t base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, base)))
        path_in(root, base, L"LinkamCapture");
    if (!root[0]) GetCurrentDirectoryW(MAX_PATH, root);
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, base))
            || !path_in(ini, base, L"LinkamCapture.ini")) {
        ini[0] = 0;
        SendMessageW(frames_radio, BM_SETCHECK, BST_CHECKED, 0);
        return;
    }

    wchar_t value[MAX_PATH];
    GetPrivateProfileStringW(L"capture", L"root", root, value, MAX_PATH, ini);
    if (value[0]) StringCchCopyW(root, MAX_PATH, value);
    GetPrivateProfileStringW(L"capture", L"last", L"", last_folder, MAX_PATH, ini);
    GetPrivateProfileStringW(L"capture", L"region", L"", value, MAX_PATH, ini);
    if (swscanf(value, L"%ld,%ld,%ld,%ld", &region.left, &region.top, &region.right, &region.bottom) != 4
            || !region_valid(region)) SetRectEmpty(&region);
    GetPrivateProfileStringW(L"capture", L"interval", L"1", value, MAX_PATH, ini);
    SetWindowTextW(interval_box, value);
    GetPrivateProfileStringW(L"capture", L"fps", L"5", value, MAX_PATH, ini);
    SetWindowTextW(rate_box, value);
    GetPrivateProfileStringW(L"capture", L"mode", L"frames", value, MAX_PATH, ini);
    video_mode = !wcscmp(value, L"video");
    SendMessageW(video_mode ? video_radio : frames_radio, BM_SETCHECK, BST_CHECKED, 0);
    GetPrivateProfileStringW(L"capture", L"sample", L"", sample, SAMPLE_LIMIT + 1, ini);
    GetPrivateProfileStringW(L"capture", L"inclusion", L"", inclusion, INCLUSION_LIMIT + 1, ini);
    UINT quality = GetPrivateProfileIntW(L"capture", L"jpeg_quality", 90, ini);
    if (quality >= 50 && quality <= 100) jpeg_quality = quality;
}

static void update_controls() {
    SetWindowTextW(folder_label, root);
    wchar_t label[96];
    if (region_valid(region))
        StringCchPrintfW(label, 96, L"Выбрать область заново · %ld × %ld пикселей", region.right - region.left, region.bottom - region.top);
    else
        StringCchCopyW(label, 96, L"Выделите изображение камеры и текущую температуру.");
    SetWindowTextW(region_button, label);
    EnableWindow(region_button, !recording);
    EnableWindow(folder_button, !recording);
    EnableWindow(frames_radio, !recording);
    EnableWindow(video_radio, !recording);
    EnableWindow(interval_box, !recording && !video_mode);
    EnableWindow(rate_box, !recording && video_mode);
    EnableWindow(start_button, !recording);
    EnableWindow(stop_button, recording);
    EnableWindow(review_button, !recording);
    EnableWindow(last_button, !recording && last_folder[0] && folder_exists(last_folder));
}

static HBITMAP screen_bitmap(const RECT &r, BYTE **bits = NULL) {
    int width = r.right - r.left, height = r.bottom - r.top;
    HDC screen = GetDC(NULL);
    if (!screen) return NULL;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *pixels = NULL;
    HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, NULL, 0) : NULL;
    if (bitmap) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        bool copied = BitBlt(memory, 0, 0, width, height, screen, r.left, r.top, SRCCOPY | CAPTUREBLT)
                   || BitBlt(memory, 0, 0, width, height, screen, r.left, r.top, SRCCOPY);
        SelectObject(memory, previous);
        GdiFlush();
        if (!copied) {
            DeleteObject(bitmap);
            bitmap = NULL;
        }
    }
    if (memory) DeleteDC(memory);
    ReleaseDC(NULL, screen);
    if (bits) *bits = bitmap ? static_cast<BYTE *>(pixels) : NULL;
    return bitmap;
}

static void drag_rectangle(RECT &r) {
    r.left = std::min(drag_start.x, drag_end.x);
    r.top = std::min(drag_start.y, drag_end.y);
    r.right = std::max(drag_start.x, drag_end.x);
    r.bottom = std::max(drag_start.y, drag_end.y);
    RECT screen = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    IntersectRect(&r, &r, &screen);
}

static POINT mouse_point(LPARAM value) {
    POINT p = { (short)LOWORD(value), (short)HIWORD(value) };
    return p;
}

static LRESULT CALLBACK select_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT p;
        HDC dc = BeginPaint(window, &p);
        RECT client;
        GetClientRect(window, &client);
        HDC memory = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(memory, selection_image);
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old);
        DeleteDC(memory);
        RECT banner = {12, 12, std::min(client.right - 12, 610L), 54};
        FillRect(dc, &banner, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc, L"Выделите мышью включение и показание температуры. Esc — отмена.",
                  -1, &banner, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (dragging) {
            RECT r;
            drag_rectangle(r);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
            InflateRect(&r, -1, -1);
            FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        EndPaint(window, &p);
        return 0;
    }
    case WM_LBUTTONDOWN:
        dragging = true;
        drag_start = drag_end = mouse_point(l);
        SetCapture(window);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging) {
            drag_end = mouse_point(l);
            InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging) {
            drag_end = mouse_point(l);
            RECT selected;
            drag_rectangle(selected);
            dragging = false;
            ReleaseCapture();
            if (region_valid(selected)) {
                region = selected;
                DestroyWindow(window);
            } else InvalidateRect(window, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        DeleteObject(selection_image);
        selection_image = NULL;
        dragging = changing_selection = false;
        ShowWindow(main_window, SW_RESTORE);
        SetForegroundWindow(main_window);
        update_controls();
        save_settings();
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static void show_selection() {
    RECT screen = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    selection_image = screen_bitmap(screen);
    HWND overlay = selection_image ? CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SELECT_CLASS,
        L"Выбор области", WS_POPUP, 0, 0, screen.right, screen.bottom, NULL, NULL, instance, NULL) : NULL;
    if (overlay) {
        ShowWindow(overlay, SW_SHOW);
        SetForegroundWindow(overlay);
    } else {
        if (selection_image) DeleteObject(selection_image);
        selection_image = NULL;
        changing_selection = false;
        ShowWindow(main_window, SW_RESTORE);
        alert(main_window, L"Не удалось получить изображение экрана.");
    }
}

// ---------------------------------------------------------------- запись AVI

static void put16(BYTE *p, unsigned v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void put32(BYTE *p, DWORD v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
static void put64(BYTE *p, unsigned long long v) { put32(p, (DWORD)v); put32(p + 4, (DWORD)(v >> 32)); }
static void tag(BYTE *p, const char *id) { memcpy(p, id, 4); }
static DWORD get32(const BYTE *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }

static bool file_seek(HANDLE file, unsigned long long position) {
    LONG high = (LONG)(position >> 32);
    SetLastError(NO_ERROR);
    DWORD low = SetFilePointer(file, (LONG)(DWORD)position, &high, FILE_BEGIN);
    return low != INVALID_SET_FILE_POINTER || GetLastError() == NO_ERROR;
}

static bool file_write(HANDLE file, const void *data, DWORD size) {
    DWORD done = 0;
    return WriteFile(file, data, size, &done, NULL) && done == size;
}

static bool file_read(HANDLE file, unsigned long long position, void *data, DWORD size) {
    DWORD done = 0;
    return file_seek(file, position) && ReadFile(file, data, size, &done, NULL) && done == size;
}

static bool avi_patch(unsigned long long position, DWORD value) {
    BYTE bytes[4];
    put32(bytes, value);
    return file_seek(avi.file, position) && file_write(avi.file, bytes, 4);
}

static bool avi_append(const void *data, DWORD size) {
    if (!file_seek(avi.file, avi.end) || !file_write(avi.file, data, size)) return false;
    avi.end += size;
    return true;
}

static unsigned long long volume_limit(const wchar_t *folder) {
    wchar_t volume[MAX_PATH], system[32];
    if (GetVolumePathNameW(folder, volume, MAX_PATH)
            && GetVolumeInformationW(volume, NULL, 0, NULL, NULL, NULL, system, 32)
            && !wcsncmp(system, L"FAT", 3))
        return 0xFFFFFFFFULL - (64ULL << 20);  // на FAT32 файл не может быть больше 4 ГБ
    return ~0ULL;
}

static bool avi_open(const wchar_t *path, int width, int height, unsigned fps) {
    avi.file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (avi.file == INVALID_HANDLE_VALUE) {
        avi.file = NULL;
        return false;
    }
    avi.limit = volume_limit(path);
    static BYTE h[AVI_HEADER];
    memset(h, 0, sizeof(h));
    tag(h, "RIFF");
    tag(h + 8, "AVI ");
    tag(h + 12, "LIST");
    put32(h + 16, MOVI - 8 - 20);
    tag(h + 20, "hdrl");
    tag(h + 24, "avih");
    put32(h + 28, 56);
    put32(h + AVIH, 1000000 / fps);
    put32(h + AVIH + 12, 0x10 | 0x100);  // AVIF_HASINDEX | AVIF_ISINTERLEAVED
    put32(h + AVIH + 24, 1);
    put32(h + AVIH + 32, width);
    put32(h + AVIH + 36, height);
    tag(h + 88, "LIST");
    put32(h + 92, INDX + 24 + 16 * SUPER_SLOTS - 96);
    tag(h + 96, "strl");
    tag(h + 100, "strh");
    put32(h + 104, 56);
    tag(h + STRH, "vids");
    tag(h + STRH + 4, "MJPG");
    put32(h + STRH + 20, 1);
    put32(h + STRH + 24, fps);
    put32(h + STRH + 40, 0xffffffff);
    put16(h + STRH + 52, width);
    put16(h + STRH + 54, height);
    tag(h + 164, "strf");
    put32(h + 168, 40);
    put32(h + STRF, 40);
    put32(h + STRF + 4, width);
    put32(h + STRF + 8, height);
    put16(h + STRF + 12, 1);
    put16(h + STRF + 14, 24);
    tag(h + STRF + 16, "MJPG");
    put32(h + STRF + 20, width * height * 3);
    tag(h + 212, "indx");
    put32(h + 216, 24 + 16 * SUPER_SLOTS);
    put16(h + INDX, 4);
    tag(h + INDX + 8, "00dc");
    const int odml = INDX + 24 + 16 * SUPER_SLOTS;
    tag(h + odml, "LIST");
    put32(h + odml + 4, 4 + 8 + 248);
    tag(h + odml + 8, "odml");
    tag(h + odml + 12, "dmlh");
    put32(h + odml + 16, 248);
    tag(h + MOVI - 8, "LIST");
    tag(h + MOVI, "movi");
    avi.end = avi.riff = 0;
    avi.movi = MOVI;
    avi.entries = avi.super = avi.first_frames = avi.total = avi.largest = 0;
    if (avi_append(h, AVI_HEADER)) return true;
    CloseHandle(avi.file);
    avi.file = NULL;
    DeleteFileW(path);
    return false;
}

// Закрывает текущий RIFF: стандартный индекс ix00, для первого RIFF ещё idx1, размеры списков.
// Если индекс не помещается на диск, размеры всё равно исправляются, кадры остаются читаемыми.
static bool avi_close_segment() {
    const unsigned n = avi.entries;
    bool ok = true;
    if (n) {
        const DWORD body = 24 + 8 * n;
        BYTE *chunk = (BYTE *)malloc(8 + body);
        const unsigned long long at = avi.end;
        ok = chunk != NULL && avi.super < SUPER_SLOTS;
        if (ok) {
            tag(chunk, "ix00");
            put32(chunk + 4, body);
            put16(chunk + 8, 2);
            chunk[10] = 0;
            chunk[11] = 1;  // AVI_INDEX_OF_CHUNKS
            put32(chunk + 12, n);
            tag(chunk + 16, "00dc");
            put64(chunk + 20, avi.movi);
            put32(chunk + 28, 0);
            for (unsigned i = 0; i < n; ++i) {
                put32(chunk + 32 + 8 * i, avi.index[2 * i]);
                put32(chunk + 36 + 8 * i, avi.index[2 * i + 1]);
            }
            ok = avi_append(chunk, 8 + body);
        }
        if (ok) {
            BYTE entry[16];
            put64(entry, at);
            put32(entry + 8, 8 + body);
            put32(entry + 12, n);
            ok = file_seek(avi.file, INDX + 24 + 16 * avi.super) && file_write(avi.file, entry, 16)
                && avi_patch(INDX + 4, avi.super + 1);
            if (ok) ++avi.super;
        }
        if (!ok) avi.end = at;
        free(chunk);
    }
    bool sized = avi_patch(avi.movi - 4, (DWORD)(avi.end - avi.movi));
    if (avi.riff == 0) {
        avi.first_frames = n;
        BYTE *chunk = (BYTE *)malloc(8 + 16 * n);
        const unsigned long long at = avi.end;
        bool indexed = chunk != NULL;
        if (indexed) {
            tag(chunk, "idx1");
            put32(chunk + 4, 16 * n);
            for (unsigned i = 0; i < n; ++i) {
                BYTE *e = chunk + 8 + 16 * i;
                tag(e, "00dc");
                put32(e + 4, 0x10);  // AVIIF_KEYFRAME
                put32(e + 8, avi.index[2 * i] - 8);
                put32(e + 12, avi.index[2 * i + 1]);
            }
            indexed = avi_append(chunk, 8 + 16 * n);
        }
        if (!indexed) avi.end = at;
        free(chunk);
        ok = ok && indexed;
    }
    sized = avi_patch(avi.riff + 4, (DWORD)(avi.end - avi.riff - 8)) && sized;
    avi.entries = 0;
    return ok && sized;
}

static bool avi_start_segment() {
    BYTE h[24] = {};
    tag(h, "RIFF");
    tag(h + 8, "AVIX");
    tag(h + 12, "LIST");
    tag(h + 20, "movi");
    const unsigned long long at = avi.end;
    if (!avi_append(h, 24)) return false;
    avi.riff = at;
    avi.movi = at + 20;
    return true;
}

static bool avi_frame(const BYTE *data, DWORD size, wchar_t (&error)[192]) {
    const DWORD padded = size + (size & 1);
    const unsigned long long need = 8ULL + padded;
    unsigned long long index = 32 + 8ULL * (avi.entries + 1) + (avi.riff == 0 ? 8 + 16ULL * (avi.entries + 1) : 0);
    if (avi.entries && avi.end + need + index - avi.riff > RIFF_LIMIT) {
        if (!avi_close_segment() || !avi_start_segment()) {
            StringCchPrintfW(error, 192, L"Не удалось продолжить видеофайл. Код Windows: %lu.", GetLastError());
            return false;
        }
        index = 32 + 8;
    }
    if (avi.super >= SUPER_SLOTS || avi.end + need + index > avi.limit) {
        StringCchCopyW(error, 192, avi.limit == ~0ULL ? L"Видеофайл достиг предельного размера. Запись остановлена, файл сохранён."
            : L"Видео достигло предела 4 ГБ для диска FAT32. Запись остановлена, файл сохранён. "
              L"Для долгих опытов выберите папку на диске NTFS.");
        return false;
    }
    if (avi.entries == avi.capacity) {
        unsigned grown = avi.capacity ? avi.capacity * 2 : 4096;
        DWORD *expanded = (DWORD *)realloc(avi.index, grown * 2 * sizeof(DWORD));
        if (!expanded) {
            StringCchCopyW(error, 192, L"Недостаточно памяти для индекса видео.");
            return false;
        }
        avi.index = expanded;
        avi.capacity = grown;
    }
    BYTE head[8];
    static const BYTE zero = 0;
    tag(head, "00dc");
    put32(head + 4, size);
    const unsigned long long at = avi.end;
    if (!avi_append(head, 8) || !avi_append(data, size) || ((size & 1) && !avi_append(&zero, 1))) {
        DWORD code = GetLastError();
        avi.end = at;
        StringCchPrintfW(error, 192, code == ERROR_DISK_FULL ? L"Диск заполнен. Запись остановлена, видео сохранено до этого места."
            : L"Не удалось записать кадр видео. Код Windows: %lu.", code);
        return false;
    }
    avi.index[2 * avi.entries] = (DWORD)(at + 8 - avi.movi);
    avi.index[2 * avi.entries + 1] = size;
    ++avi.entries;
    ++avi.total;
    avi.largest = std::max(avi.largest, (unsigned)size);
    return true;
}

static bool avi_finish(unsigned fps, wchar_t (&error)[192]) {
    bool ok = avi_close_segment();
    const DWORD buffer = (avi.largest + 8 + 1023) & ~1023u;
    ok = avi_patch(AVIH + 4, avi.largest * fps) && avi_patch(AVIH + 16, avi.first_frames)
        && avi_patch(AVIH + 28, buffer) && avi_patch(STRH + 32, avi.total)
        && avi_patch(STRH + 36, buffer) && avi_patch(DMLH, avi.total) && ok;
    ok = file_seek(avi.file, avi.end) && SetEndOfFile(avi.file) && ok;
    DWORD code = GetLastError();
    ok = CloseHandle(avi.file) && ok;
    avi.file = NULL;
    free(avi.index);
    avi.index = NULL;
    avi.capacity = avi.entries = 0;
    if (!ok) StringCchPrintfW(error, 192, L"Не удалось дописать индекс видео (код Windows: %lu). "
        L"Кадры сохранены, их можно просмотреть в этой программе.", code);
    return ok;
}

// ---------------------------------------------------------------- съёмка

static bool name_problem(const wchar_t *value, const wchar_t *field, wchar_t (&problem)[256]) {
    if (!value[0]) {
        StringCchPrintfW(problem, 256, L"Заполните поле «%s».", field);
        return true;
    }
    for (const wchar_t *p = value; *p; ++p) {
        if (*p < 32) {
            StringCchPrintfW(problem, 256, L"Поле «%s» содержит управляющий символ.", field);
            return true;
        }
        if (wcschr(L"\\/:*?\"<>|", *p)) {
            wchar_t bad[2] = {*p, 0};
            StringCchPrintfW(problem, 256, L"Поле «%s» содержит символ «%s», недопустимый в именах "
                L"файлов Windows. Нельзя использовать: \\ / : * ? \" < > |", field, bad);
            return true;
        }
    }
    return false;
}

// Имя папки опыта: «образец_номер_ГГГГ-ММ-ДД», повторные серии того же дня — «…_02», «…_03».
static bool plan_series(const wchar_t *s, const wchar_t *n, bool create, wchar_t (&name)[MAX_PATH],
                        wchar_t (&folder)[MAX_PATH], wchar_t (&problem)[256]) {
    if (name_problem(s, L"Название образца", problem) || name_problem(n, L"Номер ГЖВ", problem)) return false;
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t prefix[MAX_PATH], base[MAX_PATH];
    StringCchPrintfW(prefix, MAX_PATH, L"%s_%s", s, n);
    StringCchPrintfW(base, MAX_PATH, L"%s_%04u-%02u-%02u", prefix, (unsigned)now.wYear,
                     (unsigned)now.wMonth, (unsigned)now.wDay);
    for (unsigned copy = 1; copy < 100; ++copy) {
        if (copy == 1) StringCchCopyW(name, MAX_PATH, base);
        else StringCchPrintfW(name, MAX_PATH, L"%s_%02u", base, copy);
        const size_t longest = std::max(wcslen(prefix) + 18, wcslen(name) + 4);
        if (!path_in(folder, root, name) || wcslen(folder) + 1 + longest >= MAX_PATH) {
            StringCchCopyW(problem, 256, L"Путь к файлам опыта слишком длинный. Сократите название "
                L"или выберите папку сохранения ближе к корню диска.");
            return false;
        }
        if (create ? CreateDirectoryW(folder, NULL) != FALSE : GetFileAttributesW(folder) == INVALID_FILE_ATTRIBUTES)
            return true;
        if (create && GetLastError() != ERROR_ALREADY_EXISTS) {
            StringCchPrintfW(problem, 256, L"Не удалось создать папку опыта. Код Windows: %lu.", GetLastError());
            return false;
        }
    }
    StringCchCopyW(problem, 256, L"За сегодня уже есть 99 серий этого включения. Измените номер ГЖВ или папку сохранения.");
    return false;
}

static void read_field(HWND box, wchar_t *out, size_t size) {
    wchar_t text[128];
    GetWindowTextW(box, text, 128);
    wchar_t *start = text;
    while (*start && iswspace(*start)) ++start;
    size_t n = wcslen(start);
    while (n && iswspace(start[n - 1])) start[--n] = 0;
    StringCchCopyW(out, size, start);
}

static void update_preview() {
    wchar_t s[SAMPLE_LIMIT + 1], n[INCLUSION_LIMIT + 1], name[MAX_PATH], folder[MAX_PATH];
    wchar_t problem[256], text[700], time[20];
    read_field(sample_box, s, SAMPLE_LIMIT + 1);
    read_field(inclusion_box, n, INCLUSION_LIMIT + 1);
    if (!s[0] && !n[0]) StringCchCopyW(text, 700, L"Заполните оба поля. Кириллица и ведущие нули сохраняются.");
    else if (!plan_series(s, n, false, name, folder, problem)) StringCchCopyW(text, 700, problem);
    else if (video_mode) StringCchPrintfW(text, 700, L"Папка опыта: %s\nВидео: %s.avi", name, name);
    else {
        format_time(time, 20, 0, true);
        StringCchPrintfW(text, 700, L"Папка опыта: %s\nПервый кадр: %s_%s_%s.png", name, s, n, time);
    }
    SetWindowTextW(preview_label, text);
}

static LRESULT CALLBACK experiment_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        control(window, L"STATIC", L"Название образца", 0, 15, 18, 130, 20);
        sample_box = control(window, L"EDIT", sample, ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP,
                             150, 14, 255, 23, SAMPLE_BOX, WS_EX_CLIENTEDGE);
        control(window, L"STATIC", L"Номер ГЖВ", 0, 15, 51, 130, 20);
        inclusion_box = control(window, L"EDIT", inclusion, ES_AUTOHSCROLL | WS_TABSTOP,
                                150, 47, 90, 23, INCLUSION_BOX, WS_EX_CLIENTEDGE);
        SendMessageW(sample_box, EM_SETLIMITTEXT, SAMPLE_LIMIT, 0);
        SendMessageW(inclusion_box, EM_SETLIMITTEXT, INCLUSION_LIMIT, 0);
        preview_label = control(window, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 15, 82, 390, 44);
        control(window, L"BUTTON", L"Начать", BS_DEFPUSHBUTTON | WS_TABSTOP, 215, 136, 92, 28, IDOK);
        control(window, L"BUTTON", L"Отмена", WS_TABSTOP, 313, 136, 92, 28, IDCANCEL);
        update_preview();
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE) update_preview();
        else if (LOWORD(w) == IDCANCEL) experiment_answer = -1;
        else if (LOWORD(w) == IDOK) {
            wchar_t s[SAMPLE_LIMIT + 1], n[INCLUSION_LIMIT + 1], problem[256], name[MAX_PATH], folder[MAX_PATH];
            read_field(sample_box, s, SAMPLE_LIMIT + 1);
            read_field(inclusion_box, n, INCLUSION_LIMIT + 1);
            if (!plan_series(s, n, false, name, folder, problem)) {
                alert(window, problem);
                wchar_t ignored[256];
                HWND box = !name_problem(s, L"", ignored) && name_problem(n, L"", ignored) ? inclusion_box : sample_box;
                SetFocus(box);
                SendMessageW(box, EM_SETSEL, 0, -1);
            } else {
                StringCchCopyW(sample, SAMPLE_LIMIT + 1, s);
                StringCchCopyW(inclusion, INCLUSION_LIMIT + 1, n);
                experiment_answer = 1;
            }
        }
        return 0;
    case WM_CLOSE:
        experiment_answer = -1;
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static bool ask_experiment() {
    RECT size = {0, 0, 420, 178}, owner;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&size, style, FALSE, WS_EX_DLGMODALFRAME);
    GetWindowRect(main_window, &owner);
    const int width = size.right - size.left, height = size.bottom - size.top;
    experiment_answer = 0;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, EXPERIMENT_CLASS, L"Данные опыта", style,
        std::max(0L, (owner.left + owner.right - width) / 2), std::max(0L, (owner.top + owner.bottom - height) / 2),
        width, height, main_window, NULL, instance, NULL);
    if (!window) {
        alert(main_window, L"Не удалось открыть окно данных опыта.");
        return false;
    }
    EnableWindow(main_window, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(sample_box);
    SendMessageW(sample_box, EM_SETSEL, 0, -1);
    MSG message;
    while (!experiment_answer) {
        BOOL got = GetMessageW(&message, NULL, 0, 0);
        if (got <= 0) {
            if (!got) PostQuitMessage((int)message.wParam);
            experiment_answer = -1;
            break;
        }
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    EnableWindow(main_window, TRUE);
    DestroyWindow(window);
    SetForegroundWindow(main_window);
    return experiment_answer > 0;
}

static bool write_utf8(FILE *file, const wchar_t *text) {
    char buffer[MAX_PATH * 3];
    return WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer), NULL, NULL)
        && fputs(buffer, file) != EOF;
}

static void show_progress(DWORD elapsed) {
    if (elapsed / 1000 == shown_second && written > 1) return;
    shown_second = elapsed / 1000;
    wchar_t time[20], title[120], label[200];
    StringCchPrintfW(time, 20, L"%02lu:%02lu:%02lu", (unsigned long)(elapsed / 3600000),
                     (unsigned long)(elapsed / 60000 % 60), (unsigned long)(elapsed / 1000 % 60));
    StringCchPrintfW(title, 120, L"Запись %s — %s", time, TITLE);
    SetWindowTextW(main_window, title);
    const wchar_t *stop = hotkey_registered ? L"Остановка — Ctrl+Shift+F9." : L"Для остановки откройте окно с панели задач.";
    if (capture_video)
        StringCchPrintfW(label, 200, L"Идёт видеозапись %s. Кадров в файле: %u, из них повторов: %u.\n%s",
                         time, avi.total, repeats, stop);
    else
        StringCchPrintfW(label, 200, L"Идёт съёмка %s. Кадров: %u.\n%s", time, written, stop);
    SetWindowTextW(status_label, label);
}

static void remove_empty_series() {
    if (journal_path[0]) DeleteFileW(journal_path);
    if (video_path[0]) DeleteFileW(video_path);
    wchar_t temp[MAX_PATH];
    if (path_in(temp, series_folder, L"frame.tmp")) DeleteFileW(temp);
    RemoveDirectoryW(series_folder);
}

static bool video_tick(wchar_t (&error)[192]);

static void stop_capture(const wchar_t *error = NULL) {
    if (!recording) return;
    KillTimer(main_window, START_TIMER);
    KillTimer(main_window, CAPTURE_TIMER);
    wchar_t problem[256] = {}, late[192] = {};
    if (error) StringCchCopyW(problem, 256, error);
    if (capture_video) {
        if (!error && written && !video_tick(late)) StringCchCopyW(problem, 256, late);
        late[0] = 0;
        if (avi.file && !avi_finish(rate, late) && written && !problem[0]) StringCchCopyW(problem, 256, late);
        if (jpeg_stream) jpeg_stream->Release();
        jpeg_stream = NULL;
        free(jpeg_now);
        free(jpeg_last);
        jpeg_now = jpeg_last = NULL;
        jpeg_now_capacity = jpeg_last_capacity = 0;
    }
    if (journal) fclose(journal);
    journal = NULL;
    if (fine_timer) timeEndPeriod(1);
    fine_timer = false;
    recording = false;
    if (written) save_settings();
    else remove_empty_series();
    SetWindowTextW(main_window, TITLE);
    ShowWindow(main_window, SW_RESTORE);
    SetForegroundWindow(main_window);
    update_controls();
    wchar_t status[300], time[20];
    if (!written)
        StringCchCopyW(status, 300, L"Съёмка остановлена до первого кадра. Папка опыта не создана.");
    else if (capture_video) {
        format_time(time, 20, (DWORD)((unsigned long long)avi.total * 1000 / rate), false);
        StringCchPrintfW(status, 300, L"Видеозапись завершена: %s.avi\nДлительность %s, кадров %u, из них повторов %u.",
                         series_name, time, avi.total, repeats);
    } else
        StringCchPrintfW(status, 300, L"Съёмка завершена: %s\nСохранено кадров: %u.", series_name, written);
    SetWindowTextW(status_label, status);
    if (problem[0]) alert(main_window, problem);
}

static bool save_png(BYTE *bits, int width, int height, const wchar_t *path,
                     wchar_t (&error)[192]) {
    using namespace Gdiplus;
    GpBitmap *image = NULL;
    // The capture buffer is a top-down 32-bit DIB. GDI+ still expects a positive height.
    Status status = DllExports::GdipCreateBitmapFromScan0(width, height, width * 4,
        PixelFormat32bppRGB, bits, &image);
    if (status != Ok) {
        StringCchPrintfW(error, 192, L"Не удалось подготовить кадр. Код GDI+: %d.", (int)status);
        return false;
    }
    status = DllExports::GdipSaveImageToFile(image, path, &PNG_ENCODER, NULL);
    DllExports::GdipDisposeImage(image);
    if (status != Ok) {
        StringCchPrintfW(error, 192, L"Не удалось записать PNG. Код GDI+: %d.", (int)status);
        return false;
    }
    return true;
}

// Сжимает кадр в JPEG и копирует его в jpeg_now.
static bool encode_jpeg(BYTE *bits, int width, int height, wchar_t (&error)[192]) {
    using namespace Gdiplus;
    GpBitmap *image = NULL;
    LARGE_INTEGER start = {};
    ULARGE_INTEGER empty = {}, end = {};
    Status status = DllExports::GdipCreateBitmapFromScan0(width, height, width * 4,
        PixelFormat32bppRGB, bits, &image);
    if (status == Ok) {
        EncoderParameters parameters;
        parameters.Count = 1;
        parameters.Parameter[0].Guid = QUALITY_PARAMETER;
        parameters.Parameter[0].NumberOfValues = 1;
        parameters.Parameter[0].Type = EncoderParameterValueTypeLong;
        parameters.Parameter[0].Value = &jpeg_quality;
        if (FAILED(jpeg_stream->SetSize(empty)) || FAILED(jpeg_stream->Seek(start, STREAM_SEEK_SET, NULL)))
            status = Win32Error;
        else
            status = DllExports::GdipSaveImageToStream(image, jpeg_stream, &JPEG_ENCODER, &parameters);
        DllExports::GdipDisposeImage(image);
    }
    if (status != Ok) {
        StringCchPrintfW(error, 192, L"Не удалось сжать кадр в JPEG. Код GDI+: %d.", (int)status);
        return false;
    }
    HGLOBAL memory = NULL;
    if (FAILED(jpeg_stream->Seek(start, STREAM_SEEK_CUR, &end)) || end.HighPart || !end.LowPart
            || FAILED(GetHGlobalFromStream(jpeg_stream, &memory))) {
        StringCchCopyW(error, 192, L"Не удалось получить сжатый кадр.");
        return false;
    }
    if (end.LowPart > jpeg_now_capacity) {
        BYTE *grown = (BYTE *)realloc(jpeg_now, end.LowPart);
        if (!grown) {
            StringCchCopyW(error, 192, L"Недостаточно памяти для кадра видео.");
            return false;
        }
        jpeg_now = grown;
        jpeg_now_capacity = end.LowPart;
    }
    const BYTE *data = (const BYTE *)GlobalLock(memory);
    if (!data) {
        StringCchCopyW(error, 192, L"Не удалось получить сжатый кадр.");
        return false;
    }
    memcpy(jpeg_now, data, end.LowPart);
    GlobalUnlock(memory);
    jpeg_now_size = end.LowPart;
    return true;
}

static bool journal_time(const SYSTEMTIME &now) {
    return fprintf(journal, "%04u-%02u-%02u %02u:%02u:%02u.%03u", (unsigned)now.wYear, (unsigned)now.wMonth,
                   (unsigned)now.wDay, (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond,
                   (unsigned)now.wMilliseconds) >= 0;
}

// Видео: кадр с номером slot показывается с момента slot/rate секунд от первого кадра.
// Если снимок не успел к своему месту, пропущенные места заполняются предыдущим кадром,
// поэтому длительность AVI совпадает с реальным временем записи.
static bool video_tick(wchar_t (&error)[192]) {
    if (!IsIconic(main_window)) return true;
    if (!region_valid(region)) {
        StringCchCopyW(error, 192, L"Размер экрана изменился. Выделите область заново.");
        return false;
    }
    const DWORD now = timeGetTime();
    unsigned slot = 0;
    if (written) {
        slot = (unsigned)((unsigned long long)(DWORD)(now - first_ms) * rate / 1000);
        if (slot <= last_slot) return true;
    }
    SYSTEMTIME local;
    GetLocalTime(&local);
    BYTE *bits = NULL;
    HBITMAP bitmap = screen_bitmap(region, &bits);
    if (!bitmap) {
        StringCchCopyW(error, 192, L"Не удалось снять кадр. Проверьте, что рабочий стол открыт.");
        return false;
    }
    bool ok = encode_jpeg(bits, region.right - region.left, region.bottom - region.top, error);
    DeleteObject(bitmap);
    for (unsigned missing = last_slot + 1; ok && written && missing < slot; ++missing)
        if ((ok = avi_frame(jpeg_last, jpeg_last_size, error))) ++repeats;
    if (!ok || !avi_frame(jpeg_now, jpeg_now_size, error)) return false;
    if (!written) first_ms = now;
    std::swap(jpeg_now, jpeg_last);
    std::swap(jpeg_now_capacity, jpeg_last_capacity);
    jpeg_last_size = jpeg_now_size;
    const DWORD elapsed = now - first_ms;
    last_slot = slot;
    ++written;
    if (written == 1) StringCchCopyW(last_folder, MAX_PATH, series_folder);
    if (fprintf(journal, "%u,%lu,", slot + 1, (unsigned long)elapsed) < 0 || !journal_time(local)
            || fprintf(journal, ",%lu\r\n", (unsigned long)((unsigned long long)slot * 1000 / rate)) < 0
            || fflush(journal)) {
        StringCchPrintfW(error, 192, L"Не удалось записать время кадра в video.csv. Код записи: %d. "
            L"Видео сохранено.", errno);
        return false;
    }
    show_progress(elapsed);
    return true;
}

static void capture_frame() {
    if (!IsIconic(main_window)) return;
    if (!region_valid(region)) {
        stop_capture(L"Размер экрана изменился. Выделите область заново.");
        return;
    }

    const DWORD now = timeGetTime(), elapsed = written ? now - first_ms : 0;
    SYSTEMTIME local;
    GetLocalTime(&local);
    BYTE *bits = NULL;
    HBITMAP bitmap = screen_bitmap(region, &bits);
    if (!bitmap) {
        stop_capture(L"Не удалось снять кадр. Проверьте, что рабочий стол открыт.");
        return;
    }

    wchar_t path[MAX_PATH], temp[MAX_PATH], name[MAX_PATH], time[20], error[192] = {};
    format_time(time, 20, elapsed, true);
    bool paths_ok = SUCCEEDED(StringCchPrintfW(name, MAX_PATH, L"%s_%s.png", file_prefix, time))
        && path_in(path, series_folder, name) && path_in(temp, series_folder, L"frame.tmp");
    if (!paths_ok) {
        StringCchCopyW(error, 192, L"Путь к файлу кадра слишком длинный.");
    } else if (save_png(bits, region.right - region.left, region.bottom - region.top, temp, error)
            && !MoveFileW(temp, path)) {
        DWORD code = GetLastError();
        StringCchPrintfW(error, 192, L"Не удалось присвоить имя файлу кадра. Код Windows: %lu.", code);
    }
    DeleteObject(bitmap);
    if (error[0]) {
        if (paths_ok) DeleteFileW(temp);
        stop_capture(error);
        return;
    }

    // Имя файла в последней колонке; запятая в названии образца берётся в кавычки.
    const bool quoted = wcschr(name, L',') != NULL;
    if (fprintf(journal, "%u,%lu,", written + 1, (unsigned long)elapsed) < 0 || !journal_time(local)
            || fputs(quoted ? ",\"" : ",", journal) == EOF || !write_utf8(journal, name)
            || fputs(quoted ? "\"\r\n" : "\r\n", journal) == EOF || fflush(journal)) {
        int code = errno;
        DeleteFileW(path);
        StringCchPrintfW(error, 192, L"Не удалось записать время кадра в frames.csv. Код записи: %d.", code);
        stop_capture(error);
        return;
    }

    if (!written) first_ms = now;
    ++written;
    if (written == 1) StringCchCopyW(last_folder, MAX_PATH, series_folder);
    show_progress(elapsed);
}

static void begin_capture() {
    if (!region_valid(region)) {
        alert(main_window, L"Сначала выделите область с изображением и температурой.");
        return;
    }
    BOOL parsed = FALSE;
    UINT value = GetDlgItemInt(main_window, video_mode ? RATE : INTERVAL, &parsed, FALSE);
    if (video_mode && (!parsed || value < 1 || value > MAX_RATE)) {
        alert(main_window, L"Введите частоту видео от 1 до 30 кадров в секунду.");
        return;
    }
    if (!video_mode && (!parsed || value < 1 || value > 60)) {
        alert(main_window, L"Введите интервал от 1 до 60 секунд.");
        return;
    }
    if (!folder_exists(root) && !CreateDirectoryW(root, NULL)) {
        alert(main_window, L"Папка сохранения недоступна. Выберите другую папку.");
        return;
    }
    save_settings();
    if (!ask_experiment()) return;
    save_settings();

    wchar_t problem[256];
    if (!plan_series(sample, inclusion, true, series_name, series_folder, problem)) {
        alert(main_window, problem);
        return;
    }
    StringCchPrintfW(file_prefix, MAX_PATH, L"%s_%s", sample, inclusion);
    capture_video = video_mode;
    journal_path[0] = video_path[0] = 0;
    written = repeats = last_slot = 0;
    shown_second = 0;
    path_in(journal_path, series_folder, capture_video ? L"video.csv" : L"frames.csv");
    if (capture_video) {
        wchar_t file[MAX_PATH];
        rate = value;
        StringCchPrintfW(file, MAX_PATH, L"%s.avi", series_name);
        if (!path_in(video_path, series_folder, file)
                || !avi_open(video_path, region.right - region.left, region.bottom - region.top, rate)
                || FAILED(CreateStreamOnHGlobal(NULL, TRUE, &jpeg_stream))) {
            if (avi.file) CloseHandle(avi.file);
            avi.file = NULL;
            jpeg_stream = NULL;
            remove_empty_series();
            alert(main_window, L"Не удалось создать видеофайл.");
            return;
        }
    } else delay_ms = value * 1000;

    journal = _wfopen(journal_path, L"wb");
    const char *header = capture_video ? "\xEF\xBB\xBF" "frame,elapsed_ms,local_time,video_ms\r\n"
                                       : "\xEF\xBB\xBF" "frame,elapsed_ms,local_time,file\r\n";
    if (!journal || fputs(header, journal) == EOF || fflush(journal)) {
        if (journal) fclose(journal);
        journal = NULL;
        if (avi.file) CloseHandle(avi.file);
        avi.file = NULL;
        if (jpeg_stream) jpeg_stream->Release();
        jpeg_stream = NULL;
        remove_empty_series();
        alert(main_window, L"Не удалось создать журнал кадров.");
        return;
    }
    fine_timer = timeBeginPeriod(1) == TIMERR_NOERROR;
    recording = true;
    update_controls();
    SetWindowTextW(status_label, L"Съёмка начнётся через 2 секунды.");
    ShowWindow(main_window, SW_MINIMIZE);
    if (!SetTimer(main_window, START_TIMER, 2000, NULL)) stop_capture(L"Не удалось запустить таймер.");
}

// ---------------------------------------------------------------- просмотр

static void free_frames() {
    for (unsigned i = 0; i < frame_count; ++i) free(frames[i].file);
    free(frames);
    frames = NULL;
    frame_count = 0;
}

static bool add_frame(const Frame &frame, unsigned &capacity) {
    if (frame_count == capacity) {
        unsigned grown = capacity ? capacity * 2 : 256;
        Frame *expanded = (Frame *)realloc(frames, grown * sizeof(Frame));
        if (!expanded) return false;
        frames = expanded;
        capacity = grown;
    }
    frames[frame_count++] = frame;
    return true;
}

static void release_review_image() {
    if (review_image) Gdiplus::DllExports::GdipDisposeImage(review_image);
    review_image = NULL;
    if (review_stream) review_stream->Release();
    review_stream = NULL;
}

static void load_review_image(const Frame &frame) {
    if (!review_video) {
        wchar_t path[MAX_PATH];
        if (frame.file && path_in(path, series_folder, frame.file))
            Gdiplus::DllExports::GdipLoadImageFromFile(path, &review_image);
        return;
    }
    if (!frame.size || frame.size > (256u << 20)) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, frame.size);
    void *data = memory ? GlobalLock(memory) : NULL;
    bool ok = data && file_read(review_file, frame.offset, data, frame.size);
    if (data) GlobalUnlock(memory);
    if (!ok || FAILED(CreateStreamOnHGlobal(memory, TRUE, &review_stream))) {
        if (memory) GlobalFree(memory);
        review_stream = NULL;
        return;
    }
    if (Gdiplus::DllExports::GdipLoadImageFromStream(review_stream, &review_image) != Gdiplus::Ok)
        review_image = NULL;
}

static void set_review_frame(unsigned index) {
    if (index >= frame_count) return;
    frame_index = index;
    release_review_image();
    load_review_image(frames[index]);
    wchar_t label[200], time[20];
    format_time(time, 20, frames[index].elapsed, false);
    StringCchPrintfW(label, 200, L"Кадр %u из %u   ·   %s от начала%s", index + 1, frame_count, time,
        !review_image ? L"   ·   кадр недоступен"
        : frames[index].repeated ? L"   ·   повтор предыдущего кадра" : L"");
    SetWindowTextW(frame_label, label);
    SendMessageW(frame_slider, TBM_SETPOS, TRUE, index);
    EnableWindow(previous_button, index > 0);
    EnableWindow(next_button, index + 1 < frame_count);
    EnableWindow(export_button, review_image != NULL);
    RECT area;
    GetClientRect(review_window, &area);
    area.top = 105;
    InvalidateRect(review_window, &area, TRUE);
}

static void export_frame() {
    wchar_t source[MAX_PATH], path[MAX_PATH], time[20];
    const Frame &frame = frames[frame_index];
    if (review_video) {
        format_time(time, 20, frame.elapsed, true);
        StringCchPrintfW(path, MAX_PATH, L"%s_%s.png", review_prefix, time);
    } else {
        if (!frame.file || !path_in(source, series_folder, frame.file)) return;
        StringCchCopyW(path, MAX_PATH, frame.file);
    }
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = review_window;
    dialog.lpstrFilter = L"Изображение PNG\0*.png\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    bool saved = review_video
        ? review_image && Gdiplus::DllExports::GdipSaveImageToFile(review_image, path, &PNG_ENCODER, NULL) == Gdiplus::Ok
        : CopyFileW(source, path, FALSE) != FALSE;
    if (!saved) alert(review_window, L"Не удалось сохранить копию кадра. Выберите другое имя или папку.");
}

static LRESULT CALLBACK review_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        frame_label = control(window, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 12, 13, 480, 21);
        frame_slider = control(window, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_NOTICKS, 10, 39, 500, 25, FRAME_SLIDER);
        previous_button = control(window, L"BUTTON", L"← Кадр", WS_TABSTOP, 12, 68, 93, 28, PREV);
        next_button = control(window, L"BUTTON", L"Кадр →", WS_TABSTOP, 112, 68, 93, 28, NEXT);
        export_button = control(window, L"BUTTON", L"Сохранить кадр…", WS_TABSTOP, 215, 68, 150, 28, EXPORT_FRAME);
        control(window, L"BUTTON", L"Закрыть", WS_TABSTOP, 375, 68, 95, 28, CLOSE_REVIEW);
        SendMessageW(frame_slider, TBM_SETRANGEMAX, TRUE, frame_count - 1);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *limits = (MINMAXINFO *)l;
        limits->ptMinTrackSize.x = 510;
        limits->ptMinTrackSize.y = 295;
        return 0;
    }
    case WM_SIZE:
        MoveWindow(frame_label, 12, 13, std::max(20, (int)LOWORD(l) - 24), 21, TRUE);
        MoveWindow(frame_slider, 10, 39, std::max(20, (int)LOWORD(l) - 20), 25, TRUE);
        InvalidateRect(window, NULL, TRUE);
        return 0;
    case WM_HSCROLL:
        if ((HWND)l == frame_slider) set_review_frame((unsigned)SendMessageW(frame_slider, TBM_GETPOS, 0, 0));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case PREV: if (frame_index) set_review_frame(frame_index - 1); break;
        case NEXT: set_review_frame(frame_index + 1); break;
        case EXPORT_FRAME: export_frame(); break;
        case CLOSE_REVIEW: DestroyWindow(window); break;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT p;
        HDC dc = BeginPaint(window, &p);
        if (review_image) {
            RECT r;
            GetClientRect(window, &r);
            UINT iw = 0, ih = 0;
            Gdiplus::DllExports::GdipGetImageWidth(review_image, &iw);
            Gdiplus::DllExports::GdipGetImageHeight(review_image, &ih);
            if (iw && ih && r.right > 24 && r.bottom > 120) {
                double ratio = std::min((r.right - 24) / (double)iw, (r.bottom - 120) / (double)ih);
                int width = std::max(1, (int)(iw * ratio)), height = std::max(1, (int)(ih * ratio));
                Gdiplus::GpGraphics *graphics = NULL;
                if (Gdiplus::DllExports::GdipCreateFromHDC(dc, &graphics) == Gdiplus::Ok) {
                    Gdiplus::DllExports::GdipSetInterpolationMode(graphics,
                        ratio > 1.0 ? Gdiplus::InterpolationModeNearestNeighbor : Gdiplus::InterpolationModeHighQualityBicubic);
                    Gdiplus::DllExports::GdipDrawImageRectI(graphics, review_image,
                        (r.right - width) / 2, 108 + (r.bottom - 120 - height) / 2, width, height);
                    Gdiplus::DllExports::GdipDeleteGraphics(graphics);
                }
            }
        }
        EndPaint(window, &p);
        return 0;
    }
    case WM_DESTROY:
        release_review_image();
        if (review_file) CloseHandle(review_file);
        review_file = NULL;
        free_frames();
        review_window = NULL;
        EnableWindow(main_window, TRUE);
        SetForegroundWindow(main_window);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

// Имя файла из колонки file журнала frames.csv (UTF-8, возможно в кавычках).
static wchar_t *journal_file(char *text) {
    text[strcspn(text, "\r\n")] = 0;
    if (text[0] == '"') {
        char *to = text;
        for (const char *from = text + 1; *from; ) {
            if (*from == '"') {
                if (from[1] != '"') break;
                ++from;
            }
            *to++ = *from++;
        }
        *to = 0;
    }
    wchar_t name[MAX_PATH];
    if (!text[0] || !MultiByteToWideChar(CP_UTF8, 0, text, -1, name, MAX_PATH) || wcspbrk(name, L"\\/:"))
        return NULL;
    return _wcsdup(name);
}

static bool load_png_series(FILE *input) {
    unsigned capacity = 0;
    char line[2048];
    while (fgets(line, sizeof(line), input)) {
        Frame frame = {};
        unsigned number = 0;
        unsigned long elapsed = 0;
        if (sscanf(line, "%u,%lu,", &number, &elapsed) != 2 || !number) continue;
        frame.number = number;
        frame.elapsed = elapsed;
        char *rest = line;
        for (int comma = 0; rest && comma < 3; ++comma) {
            rest = strchr(rest, ',');
            if (rest) ++rest;
        }
        frame.file = rest ? journal_file(rest) : NULL;
        if (!frame.file) {  // журнал без колонки file: имена версии 1.1
            wchar_t name[40];
            StringCchPrintfW(name, 40, L"frame_%06u.png", number);
            frame.file = _wcsdup(name);
        }
        if (!add_frame(frame, capacity)) {
            free(frame.file);
            return false;
        }
    }
    return true;
}

static bool scan_avi(HANDLE file, unsigned long long start, unsigned long long end,
                     unsigned &capacity, int depth) {
    unsigned long long p = start;
    while (p + 8 <= end) {
        BYTE head[12];
        if (!file_read(file, p, head, 8)) return false;
        const DWORD size = get32(head + 4);
        const unsigned long long body = p + 8;
        unsigned long long stop = body + size;
        if (!memcmp(head, "LIST", 4) || !memcmp(head, "RIFF", 4)) {
            if (size < 4 || stop > end) stop = end;  // незавершённая запись: размер не проставлен
            if (!file_read(file, body, head + 8, 4)) return false;
            if (depth < 8 && (!memcmp(head + 8, "hdrl", 4) || !memcmp(head + 8, "strl", 4)
                    || !memcmp(head + 8, "movi", 4) || !memcmp(head + 8, "AVI ", 4)
                    || !memcmp(head + 8, "AVIX", 4))
                    && !scan_avi(file, body + 4, stop, capacity, depth + 1)) return false;
            p = stop + (stop < end ? (size & 1) : 0);
            continue;
        }
        if (stop > end) break;  // обрезанный последний кадр
        if (!memcmp(head, "00dc", 4) || !memcmp(head, "00db", 4)) {
            Frame frame = {};
            frame.number = frame_count + 1;
            frame.offset = body;
            frame.size = size;
            if (!size && frame_count) {  // кадр нулевой длины означает повтор предыдущего
                frame.offset = frames[frame_count - 1].offset;
                frame.size = frames[frame_count - 1].size;
                frame.repeated = true;
            }
            if (!add_frame(frame, capacity)) return false;
        } else if (!memcmp(head, "strh", 4) && !review_rate && size >= 28) {
            BYTE strh[28];
            if (file_read(file, body, strh, 28) && !memcmp(strh, "vids", 4)) {
                review_scale = get32(strh + 20);
                review_rate = get32(strh + 24);
            }
        }
        p = stop + (size & 1);
    }
    return true;
}

// Время кадров видео берётся из video.csv; без журнала — из частоты кадров AVI.
static void load_video_times(const wchar_t *folder) {
    if (!review_rate || !review_scale) {
        review_rate = 5;
        review_scale = 1;
    }
    for (unsigned i = 0; i < frame_count; ++i)
        frames[i].elapsed = (DWORD)((unsigned long long)i * review_scale * 1000 / review_rate);
    wchar_t csv[MAX_PATH];
    FILE *input = path_in(csv, folder, L"video.csv") ? _wfopen(csv, L"rb") : NULL;
    if (!input) return;
    for (unsigned i = 0; i < frame_count; ++i) frames[i].repeated = true;
    char line[256];
    while (fgets(line, sizeof(line), input)) {
        unsigned number = 0;
        unsigned long elapsed = 0;
        if (sscanf(line, "%u,%lu,", &number, &elapsed) == 2 && number && number <= frame_count) {
            frames[number - 1].elapsed = elapsed;
            frames[number - 1].repeated = false;
        }
    }
    fclose(input);
    for (unsigned i = 1; i < frame_count; ++i)
        if (frames[i].repeated) frames[i].elapsed = frames[i - 1].elapsed;
    frames[0].repeated = false;
}

// Префикс для сохранения кадров видео: «Г-170_03» из «Г-170_03_2026-11-05_02.avi».
static bool date_suffix(const wchar_t *text, size_t n) {
    if (n <= 11) return false;
    const wchar_t *date = text + n - 11;
    for (int i = 1; i < 11; ++i)
        if (i == 5 || i == 8 ? date[i] != L'-' : !iswdigit(date[i])) return false;
    return date[0] == L'_';
}

static void video_prefix(const wchar_t *file) {
    StringCchCopyW(review_prefix, MAX_PATH, last_component(file));
    size_t n = wcslen(review_prefix);
    if (n > 4 && !_wcsicmp(review_prefix + n - 4, L".avi")) review_prefix[n -= 4] = 0;
    if (n > 3 && review_prefix[n - 3] == L'_' && iswdigit(review_prefix[n - 2])
            && iswdigit(review_prefix[n - 1]) && date_suffix(review_prefix, n - 3))
        n -= 3;
    if (date_suffix(review_prefix, n)) review_prefix[n - 11] = 0;  // иначе имя файла целиком
}

static bool find_video(const wchar_t *folder, wchar_t (&path)[MAX_PATH]) {
    wchar_t pattern[MAX_PATH], named[MAX_PATH];
    StringCchPrintfW(named, MAX_PATH, L"%s.avi", last_component(folder));
    if (path_in(path, folder, named) && file_exists(path)) return true;
    if (!path_in(pattern, folder, L"*.avi")) return false;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    FindClose(search);
    return path_in(path, folder, found.cFileName);
}

static void open_series(const wchar_t *folder) {
    wchar_t csv[MAX_PATH], video[MAX_PATH], title[MAX_PATH + 40];
    if (!path_in(csv, folder, L"frames.csv")) return;
    FILE *input = _wfopen(csv, L"rb");
    review_video = !input;
    review_rate = review_scale = 0;
    if (input) {
        bool loaded = load_png_series(input);
        fclose(input);
        if (!loaded) {
            free_frames();
            alert(main_window, L"Недостаточно памяти для журнала кадров.");
            return;
        }
        if (!frame_count) {
            alert(main_window, L"Не удалось прочитать ни одного кадра из журнала.");
            return;
        }
    } else {
        if (!find_video(folder, video)) {
            alert(main_window, L"В этой папке нет ни журнала frames.csv, ни видео AVI. Выберите папку одного опыта.");
            return;
        }
        review_file = CreateFileW(video, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        BYTE head[12];
        LARGE_INTEGER size = {};
        unsigned capacity = 0;
        if (review_file == INVALID_HANDLE_VALUE) review_file = NULL;
        bool ok = review_file && GetFileSizeEx(review_file, &size) && file_read(review_file, 0, head, 12)
            && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "AVI ", 4)
            && scan_avi(review_file, 0, (unsigned long long)size.QuadPart, capacity, 0);
        if (!ok || !frame_count) {
            if (review_file) CloseHandle(review_file);
            review_file = NULL;
            free_frames();
            alert(main_window, ok ? L"В видеофайле нет кадров." : L"Не удалось прочитать видеофайл AVI.");
            return;
        }
        load_video_times(folder);
        video_prefix(video);
    }
    StringCchCopyW(series_folder, MAX_PATH, folder);
    StringCchPrintfW(title, MAX_PATH + 40, L"Просмотр серии ГЖВ · %s", last_component(review_video ? video : folder));
    EnableWindow(main_window, FALSE);
    review_window = CreateWindowExW(0, REVIEW_CLASS, title,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, std::min(1060, GetSystemMetrics(SM_CXSCREEN) - 20),
        std::min(740, GetSystemMetrics(SM_CYSCREEN) - 50), main_window, NULL, instance, NULL);
    if (review_window) {
        set_review_frame(0);
        ShowWindow(review_window, SW_SHOW);
        SetFocus(frame_slider);
    } else {
        if (review_file) CloseHandle(review_file);
        review_file = NULL;
        free_frames();
        EnableWindow(main_window, TRUE);
        alert(main_window, L"Не удалось открыть окно просмотра.");
    }
}

// ---------------------------------------------------------------- главное окно

static LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        main_window = window;
        region_button = control(window, L"BUTTON", L"", WS_TABSTOP, 15, 14, 565, 30, REGION);
        folder_button = control(window, L"BUTTON", L"Папка сохранения…", WS_TABSTOP, 15, 58, 145, 26, FOLDER);
        folder_label = control(window, L"STATIC", L"", SS_PATHELLIPSIS, 169, 63, 411, 22);
        frames_radio = control(window, L"BUTTON", L"Кадры PNG, интервал", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                               15, 101, 140, 22, MODE_FRAMES);
        video_radio = control(window, L"BUTTON", L"Видео AVI, частота", BS_AUTORADIOBUTTON,
                              290, 101, 128, 22, MODE_VIDEO);
        interval_box = control(window, L"EDIT", L"1", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP,
                               157, 100, 36, 24, INTERVAL, WS_EX_CLIENTEDGE);
        control(window, L"STATIC", L"с", 0, 199, 104, 30, 20);
        rate_box = control(window, L"EDIT", L"5", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                           420, 100, 36, 24, RATE, WS_EX_CLIENTEDGE);
        control(window, L"STATIC", L"кадр/с", 0, 462, 104, 90, 20);
        SendMessageW(interval_box, EM_SETLIMITTEXT, 2, 0);
        SendMessageW(rate_box, EM_SETLIMITTEXT, 2, 0);
        start_button = control(window, L"BUTTON", L"Начать съёмку", WS_TABSTOP | WS_GROUP, 15, 141, 145, 33, START);
        stop_button = control(window, L"BUTTON", L"Остановить", WS_TABSTOP, 170, 141, 125, 33, STOP);
        last_button = control(window, L"BUTTON", L"Последняя серия", WS_TABSTOP, 305, 141, 132, 33, LAST);
        review_button = control(window, L"BUTTON", L"Открыть серию…", WS_TABSTOP, 447, 141, 133, 33, REVIEW);
        control(window, L"STATIC", L"На экране оставьте открытыми изображение и температуру.", 0, 15, 188, 565, 20);
        status_label = control(window, L"STATIC", L"", SS_NOPREFIX, 15, 212, 565, 40);
        hotkey_registered = RegisterHotKey(window, STOP_HOTKEY, MOD_CONTROL | MOD_SHIFT, VK_F9) != FALSE;
        SetWindowTextW(status_label, hotkey_registered ? L"Остановка съёмки — Ctrl+Shift+F9." :
            L"Для остановки разверните окно с панели задач и нажмите «Остановить».");
        load_settings();
        update_controls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case REGION:
            if (!recording && !changing_selection) {
                changing_selection = true;
                ShowWindow(window, SW_HIDE);
                if (!SetTimer(window, SELECT_TIMER, 300, NULL)) show_selection();
            }
            break;
        case FOLDER: {
            wchar_t selected[MAX_PATH];
            if (browse_folder(window, L"Куда сохранять опыты?", root, selected)) {
                StringCchCopyW(root, MAX_PATH, selected);
                update_controls();
                save_settings();
            }
            break;
        }
        case MODE_FRAMES:
        case MODE_VIDEO:
            if (!recording && HIWORD(w) == BN_CLICKED) {
                video_mode = SendMessageW(video_radio, BM_GETCHECK, 0, 0) == BST_CHECKED;
                update_controls();
                save_settings();
            }
            break;
        case START: if (!recording) begin_capture(); break;
        case STOP: if (recording) stop_capture(); break;
        case LAST: if (!recording) open_series(last_folder); break;
        case REVIEW: {
            wchar_t selected[MAX_PATH];
            if (!recording && browse_folder(window, L"Выберите папку одного опыта (frames.csv или видео AVI).",
                last_folder[0] ? last_folder : root, selected)) open_series(selected);
            break;
        }
        }
        return 0;
    case WM_TIMER:
        if (w == SELECT_TIMER) {
            KillTimer(window, SELECT_TIMER);
            show_selection();
        } else if (recording && w == START_TIMER) {
            KillTimer(window, START_TIMER);
            const UINT tick = capture_video ? std::max(10u, 1000 / rate / 8) : delay_ms;
            if (!SetTimer(window, CAPTURE_TIMER, tick, NULL))
                stop_capture(L"Не удалось запустить интервальную съёмку.");
            else if (capture_video) {
                wchar_t error[192] = {};
                if (!video_tick(error)) stop_capture(error);
            } else capture_frame();
        } else if (recording && w == CAPTURE_TIMER) {
            if (capture_video) {
                wchar_t error[192] = {};
                if (!video_tick(error)) stop_capture(error);
            } else capture_frame();
        }
        return 0;
    case WM_HOTKEY:
        if (recording && w == STOP_HOTKEY) stop_capture();
        return 0;
    case WM_ENDSESSION:
        if (w && recording) stop_capture();
        return 0;
    case WM_CLOSE:
        if (recording) stop_capture();
        save_settings();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (hotkey_registered) UnregisterHotKey(window, STOP_HOTKEY);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static bool register_class(const wchar_t *name, WNDPROC proc, LPCWSTR cursor) {
    WNDCLASSW cls = {};
    cls.hInstance = instance;
    cls.lpfnWndProc = proc;
    cls.lpszClassName = name;
    cls.hCursor = LoadCursorW(NULL, cursor);
    cls.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    cls.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    return RegisterClassW(&cls) != 0;
}

int WINAPI wWinMain(HINSTANCE handle, HINSTANCE, PWSTR, int show) {
    instance = handle;
    HRESULT com = CoInitialize(NULL);
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_BAR_CLASSES};
    InitCommonControlsEx(&common);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplus = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus, &input, NULL) != Gdiplus::Ok) {
        alert(NULL, L"Не удалось запустить системный модуль PNG (GDI+).");
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    if (register_class(MAIN_CLASS, main_proc, IDC_ARROW)
            && register_class(SELECT_CLASS, select_proc, IDC_CROSS)
            && register_class(REVIEW_CLASS, review_proc, IDC_ARROW)
            && register_class(EXPERIMENT_CLASS, experiment_proc, IDC_ARROW)) {
        RECT size = {0, 0, 595, 258};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&size, style, FALSE);
        main_window = CreateWindowExW(0, MAIN_CLASS, TITLE, style,
            CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
            NULL, NULL, instance, NULL);
    }
    if (main_window) {
        ShowWindow(main_window, show);
        MSG message;
        while (GetMessageW(&message, NULL, 0, 0) > 0) {
            HWND dialog = review_window ? review_window : main_window;
            if (!changing_selection && IsDialogMessageW(dialog, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    } else alert(NULL, L"Не удалось открыть окно программы.");
    Gdiplus::GdiplusShutdown(gdiplus);
    if (SUCCEEDED(com)) CoUninitialize();
    return main_window ? 0 : 1;
}
