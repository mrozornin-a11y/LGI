#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <objbase.h>
#include <gdiplus.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <strsafe.h>

static const wchar_t *TITLE = L"Съёмка ГЖВ · 1.1";
static const wchar_t *SELECT_CLASS = L"LinkamCaptureSelection";
static const wchar_t *REVIEW_CLASS = L"LinkamCaptureReview";
static const CLSID PNG_ENCODER = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
static const UINT_PTR START_TIMER = 1, CAPTURE_TIMER = 2, SELECT_TIMER = 3;
static const int STOP_HOTKEY = 1;
enum Control { REGION = 100, FOLDER, START, STOP, REVIEW, LAST, INTERVAL,
               PREV, NEXT, FRAME_SLIDER, EXPORT_FRAME, CLOSE_REVIEW };

struct Frame {
    unsigned number;
    DWORD elapsed;
};

static HINSTANCE instance;
static HWND main_window, status_label, folder_label, region_button, folder_button;
static HWND start_button, stop_button, interval_box, review_button, last_button;
static HWND review_window, frame_slider, frame_label, previous_button, next_button, export_button;
static RECT region;
static wchar_t root[MAX_PATH], last_folder[MAX_PATH], series_folder[MAX_PATH], ini[MAX_PATH];
static FILE *journal;
static bool recording, hotkey_registered, dragging, changing_selection;
static DWORD started, delay_ms;
static unsigned written;
static HBITMAP selection_image;
static POINT drag_start, drag_end;
static Frame *frames;
static unsigned frame_count, frame_index;
static Gdiplus::GpImage *review_image;

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
    WritePrivateProfileStringW(L"capture", L"root", root, ini);
    WritePrivateProfileStringW(L"capture", L"last", last_folder, ini);
}

static void load_settings() {
    wchar_t base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, base)))
        path_in(root, base, L"LinkamCapture");
    if (!root[0]) GetCurrentDirectoryW(MAX_PATH, root);
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, base))
            || !path_in(ini, base, L"LinkamCapture.ini")) {
        ini[0] = 0;
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
    EnableWindow(interval_box, !recording);
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

static void stop_capture(const wchar_t *error = NULL) {
    KillTimer(main_window, START_TIMER);
    KillTimer(main_window, CAPTURE_TIMER);
    if (journal) fclose(journal);
    journal = NULL;
    recording = false;
    if (written) save_settings();
    ShowWindow(main_window, SW_RESTORE);
    SetForegroundWindow(main_window);
    update_controls();
    wchar_t status[170];
    StringCchPrintfW(status, 170, L"Съёмка завершена. Сохранено кадров: %u.", written);
    SetWindowTextW(status_label, status);
    if (error) alert(main_window, error);
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

static void capture_frame() {
    if (!IsIconic(main_window)) return;
    if (!region_valid(region)) {
        stop_capture(L"Размер экрана изменился. Выделите область заново.");
        return;
    }

    DWORD elapsed = GetTickCount() - started;
    SYSTEMTIME now;
    GetLocalTime(&now);
    BYTE *bits = NULL;
    HBITMAP bitmap = screen_bitmap(region, &bits);
    if (!bitmap) {
        stop_capture(L"Не удалось снять кадр. Проверьте, что рабочий стол открыт.");
        return;
    }

    wchar_t path[MAX_PATH], temp[MAX_PATH], name[40], error[192] = {};
    StringCchPrintfW(name, 40, L"frame_%06u.png", written + 1);
    bool paths_ok = path_in(path, series_folder, name) && path_in(temp, series_folder, L"frame.tmp");
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

    if (fprintf(journal, "%u,%lu,%04u-%02u-%02u %02u:%02u:%02u.%03u,frame_%06u.png\r\n",
                written + 1, (unsigned long)elapsed, (unsigned)now.wYear, (unsigned)now.wMonth,
                (unsigned)now.wDay, (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond,
                (unsigned)now.wMilliseconds, written + 1) < 0 || fflush(journal)) {
        int code = errno;
        DeleteFileW(path);
        StringCchPrintfW(error, 192, L"Не удалось записать время кадра в frames.csv. Код записи: %d.", code);
        stop_capture(error);
        return;
    }

    ++written;
    if (written == 1) StringCchCopyW(last_folder, MAX_PATH, series_folder);
    wchar_t label[130];
    StringCchPrintfW(label, 130, L"Идёт съёмка. Кадров: %u%s", written,
        hotkey_registered ? L". Остановка — Ctrl+Shift+F9." : L". Для остановки откройте окно с панели задач.");
    SetWindowTextW(status_label, label);
}

static void begin_capture() {
    if (!region_valid(region)) {
        alert(main_window, L"Сначала выделите область с изображением и температурой.");
        return;
    }
    BOOL parsed = FALSE;
    UINT seconds = GetDlgItemInt(main_window, INTERVAL, &parsed, FALSE);
    if (!parsed || seconds < 1 || seconds > 60) {
        alert(main_window, L"Введите интервал от 1 до 60 секунд.");
        return;
    }
    if (!folder_exists(root) && !CreateDirectoryW(root, NULL)) {
        alert(main_window, L"Папка сохранения недоступна. Выберите другую папку.");
        return;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[48], name[60], csv[MAX_PATH];
    StringCchPrintfW(stamp, 48, L"%04u-%02u-%02u_%02u-%02u-%02u", (unsigned)now.wYear,
        (unsigned)now.wMonth, (unsigned)now.wDay, (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
    unsigned suffix = 0;
    for (;;) {
        if (suffix) StringCchPrintfW(name, 60, L"%s_%02u", stamp, suffix);
        else StringCchCopyW(name, 60, stamp);
        if (!path_in(series_folder, root, name) || wcslen(series_folder) > MAX_PATH - 26) {
            alert(main_window, L"Путь слишком длинный. Выберите папку ближе к корню диска.");
            return;
        }
        if (CreateDirectoryW(series_folder, NULL)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS || ++suffix >= 100) {
            alert(main_window, L"Не удалось создать папку опыта.");
            return;
        }
    }
    path_in(csv, series_folder, L"frames.csv");
    journal = _wfopen(csv, L"wb");
    if (!journal || fputs("frame,elapsed_ms,local_time,file\r\n", journal) == EOF || fflush(journal)) {
        if (journal) fclose(journal);
        journal = NULL;
        DeleteFileW(csv);
        RemoveDirectoryW(series_folder);
        alert(main_window, L"Не удалось создать журнал кадров.");
        return;
    }
    written = 0;
    delay_ms = seconds * 1000;
    recording = true;
    update_controls();
    SetWindowTextW(status_label, L"Съёмка начнётся через 2 секунды.");
    ShowWindow(main_window, SW_MINIMIZE);
    if (!SetTimer(main_window, START_TIMER, 2000, NULL)) stop_capture(L"Не удалось запустить таймер.");
}

static void set_review_frame(unsigned index) {
    if (index >= frame_count) return;
    frame_index = index;
    if (review_image) Gdiplus::DllExports::GdipDisposeImage(review_image);
    review_image = NULL;
    wchar_t file[40], path[MAX_PATH], label[130];
    StringCchPrintfW(file, 40, L"frame_%06u.png", frames[index].number);
    if (path_in(path, series_folder, file))
        Gdiplus::DllExports::GdipLoadImageFromFile(path, &review_image);
    DWORD ms = frames[index].elapsed;
    StringCchPrintfW(label, 130, L"Кадр %u из %u   ·   %lu:%02lu.%03lu от начала%s",
        index + 1, frame_count, (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60),
        (unsigned long)(ms % 1000), review_image ? L"" : L"   ·   файл недоступен");
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
    wchar_t file[40], source[MAX_PATH], path[MAX_PATH];
    StringCchPrintfW(file, 40, L"frame_%06u.png", frames[frame_index].number);
    if (!path_in(source, series_folder, file)) return;
    StringCchCopyW(path, MAX_PATH, file);
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = review_window;
    dialog.lpstrFilter = L"Изображение PNG\0*.png\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog) && !CopyFileW(source, path, FALSE))
        alert(review_window, L"Не удалось сохранить копию кадра. Выберите другое имя или папку.");
}

static LRESULT CALLBACK review_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        frame_label = control(window, L"STATIC", L"", SS_LEFT, 12, 13, 400, 21);
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
        if (review_image) Gdiplus::DllExports::GdipDisposeImage(review_image);
        review_image = NULL;
        free(frames);
        frames = NULL;
        frame_count = 0;
        review_window = NULL;
        EnableWindow(main_window, TRUE);
        SetForegroundWindow(main_window);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static void open_series(const wchar_t *folder) {
    wchar_t csv[MAX_PATH];
    if (!path_in(csv, folder, L"frames.csv")) return;
    FILE *input = _wfopen(csv, L"rb");
    if (!input) {
        alert(main_window, L"В этой папке нет журнала frames.csv. Выберите папку одного опыта.");
        return;
    }
    unsigned capacity = 0;
    char line[256];
    while (fgets(line, sizeof(line), input)) {
        Frame frame;
        if (sscanf(line, "%u,%lu,", &frame.number, &frame.elapsed) != 2 || !frame.number) continue;
        if (frame_count == capacity) {
            unsigned grown = capacity ? capacity * 2 : 256;
            Frame *expanded = (Frame *)realloc(frames, grown * sizeof(Frame));
            if (!expanded) {
                free(frames);
                frames = NULL;
                frame_count = 0;
                break;
            }
            frames = expanded;
            capacity = grown;
        }
        frames[frame_count++] = frame;
    }
    fclose(input);
    if (!frame_count) {
        alert(main_window, L"Не удалось прочитать ни одного кадра из журнала.");
        return;
    }
    StringCchCopyW(series_folder, MAX_PATH, folder);
    EnableWindow(main_window, FALSE);
    review_window = CreateWindowExW(0, REVIEW_CLASS, L"Просмотр серии ГЖВ",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, std::min(1060, GetSystemMetrics(SM_CXSCREEN) - 20),
        std::min(740, GetSystemMetrics(SM_CYSCREEN) - 50), main_window, NULL, instance, NULL);
    if (review_window) {
        set_review_frame(0);
        ShowWindow(review_window, SW_SHOW);
        SetFocus(frame_slider);
    } else {
        free(frames);
        frames = NULL;
        frame_count = 0;
        EnableWindow(main_window, TRUE);
        alert(main_window, L"Не удалось открыть окно просмотра.");
    }
}

static LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE:
        main_window = window;
        region_button = control(window, L"BUTTON", L"", WS_TABSTOP, 15, 14, 565, 30, REGION);
        folder_button = control(window, L"BUTTON", L"Папка сохранения…", WS_TABSTOP, 15, 58, 145, 26, FOLDER);
        folder_label = control(window, L"STATIC", L"", SS_PATHELLIPSIS, 169, 63, 411, 22);
        control(window, L"STATIC", L"Интервал, с", 0, 15, 105, 90, 20);
        interval_box = control(window, L"EDIT", L"1", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
                               107, 101, 43, 25, INTERVAL, WS_EX_CLIENTEDGE);
        SendMessageW(interval_box, EM_SETLIMITTEXT, 2, 0);
        control(window, L"STATIC", L"На экране оставьте открытыми изображение и температуру.", 0, 167, 105, 413, 22);
        start_button = control(window, L"BUTTON", L"Начать съёмку", WS_TABSTOP, 15, 141, 145, 33, START);
        stop_button = control(window, L"BUTTON", L"Остановить", WS_TABSTOP, 170, 141, 125, 33, STOP);
        last_button = control(window, L"BUTTON", L"Последняя серия", WS_TABSTOP, 305, 141, 132, 33, LAST);
        review_button = control(window, L"BUTTON", L"Открыть серию…", WS_TABSTOP, 447, 141, 133, 33, REVIEW);
        status_label = control(window, L"STATIC", L"", 0, 15, 190, 565, 40);
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
        case START: if (!recording) begin_capture(); break;
        case STOP: if (recording) stop_capture(); break;
        case LAST: if (!recording) open_series(last_folder); break;
        case REVIEW: {
            wchar_t selected[MAX_PATH];
            if (!recording && browse_folder(window, L"Выберите папку одного опыта с frames.csv.",
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
            started = GetTickCount();
            if (SetTimer(window, CAPTURE_TIMER, delay_ms, NULL)) capture_frame();
            else stop_capture(L"Не удалось запустить интервальную съёмку.");
        } else if (recording && w == CAPTURE_TIMER) capture_frame();
        return 0;
    case WM_HOTKEY:
        if (recording && w == STOP_HOTKEY) stop_capture();
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
    if (register_class(L"LinkamCaptureMain", main_proc, IDC_ARROW)
            && register_class(SELECT_CLASS, select_proc, IDC_CROSS)
            && register_class(REVIEW_CLASS, review_proc, IDC_ARROW)) {
        RECT size = {0, 0, 595, 230};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&size, style, FALSE);
        main_window = CreateWindowExW(0, L"LinkamCaptureMain", TITLE, style,
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
