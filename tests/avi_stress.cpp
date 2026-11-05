// Нагрузочная проверка записи AVI той же функцией, что и в программе: пишет много кадров
// JPEG подряд (по кругу из нескольких файлов), чтобы получить файл больше 1–4 ГБ (OpenDML).
// Сборка: i686-w64-mingw32-g++-win32 -O2 -static tests/avi_stress.cpp -o avi_stress.exe -lgdiplus -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lwinmm -lgdi32
// Запуск:  avi_stress.exe out.avi кадров ширина высота лимит_МБ a.jpg b.jpg ...
#define wWinMain linkam_main_unused
#include "../linkam_capture.cpp"
#undef wWinMain

int main(int argc, char **argv) {
    if (argc < 7) return 2;
    unsigned count = strtoul(argv[2], NULL, 10), limit_mb = strtoul(argv[5], NULL, 10);
    int width = atoi(argv[3]), height = atoi(argv[4]);
    const int images = argc - 6;
    BYTE *data[16];
    DWORD size[16];
    for (int i = 0; i < images && i < 16; ++i) {
        FILE *f = fopen(argv[6 + i], "rb");
        if (!f) return 3;
        fseek(f, 0, SEEK_END);
        size[i] = ftell(f);
        fseek(f, 0, SEEK_SET);
        data[i] = (BYTE *)malloc(size[i]);
        if (fread(data[i], 1, size[i], f) != size[i]) return 4;
        fclose(f);
    }
    wchar_t path[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path, MAX_PATH);
    if (!avi_open(path, width, height, 5)) return 5;
    if (limit_mb) avi.limit = (unsigned long long)limit_mb << 20;
    wchar_t error[192] = {};
    unsigned i = 0;
    for (; i < count; ++i)
        if (!avi_frame(data[i % images], size[i % images], error)) break;
    printf("written %u frames, %u RIFF closed so far, end %llu bytes\n", i, avi.super, avi.end);
    if (error[0]) {
        char text[400];
        WideCharToMultiByte(CP_UTF8, 0, error, -1, text, 400, NULL, NULL);
        printf("stopped: %s\n", text);
    }
    wchar_t late[192] = {};
    bool ok = avi_finish(5, late);
    printf("finish %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
