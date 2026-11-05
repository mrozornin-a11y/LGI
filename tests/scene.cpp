// Тестовая сцена вместо Linksys32: «включение» (движущийся круг), температура и
// миллисекунды от запуска сцены. Нужна только для проверки записи под Wine/Windows.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <cstdio>

static DWORD start;

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_TIMER: InvalidateRect(w, NULL, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(40, 60, 80));
        FillRect(mem, &r, bg);
        DeleteObject(bg);
        DWORD t = timeGetTime() - start;
        int x = 40 + (int)((t / 10) % 300);
        HBRUSH ball = CreateSolidBrush(RGB(230, 230, 200));
        HGDIOBJ ob = SelectObject(mem, ball);
        Ellipse(mem, x, 60, x + 50, 110);
        Ellipse(mem, 200, 150, 200 + (int)(t / 50 % 40), 150 + (int)(t / 50 % 40));
        SelectObject(mem, ob);
        DeleteObject(ball);
        HFONT font = CreateFontW(30, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Arial");
        HGDIOBJ of = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 0));
        wchar_t text[64];
        swprintf(text, 64, L"T = %.1f \u00B0C", 25.0 + t / 1000.0 * 0.5);
        TextOutW(mem, 20, 180, text, lstrlenW(text));
        swprintf(text, 64, L"t = %lu ms", (unsigned long)t);
        TextOutW(mem, 20, 220, text, lstrlenW(text));
        SelectObject(mem, of);
        DeleteObject(font);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    timeBeginPeriod(1);
    start = timeGetTime();
    WNDCLASSW c = {};
    c.lpfnWndProc = proc;
    c.hInstance = inst;
    c.lpszClassName = L"Linksys32Scene";
    c.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&c);
    HWND w = CreateWindowExW(0, c.lpszClassName, L"Linksys32 (тестовая сцена)", WS_POPUP | WS_VISIBLE | WS_BORDER,
                             600, 380, 400, 280, NULL, NULL, inst, NULL);
    SetTimer(w, 1, 10, NULL);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
