#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#define OUT_OUTLINE_PRECISION 8
#define CLIP_DEFAULT_PRECISION 0
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "8080.h"

#pragma comment(lib, "winmm.lib")

extern "C" int cpu8080_step(CPU8080* cpu);
extern "C" void cpu8080_reset(CPU8080* cpu);

#define SYSTEM_CLOCK_HZ 1780000 // Частота ядра (1.78 МГц)

// --- Константы ОБНОВЛЕННОЙ архитектуры ---
const DWORD ROM_SIZE = 4096;      // 4 КБ ПЗУ (0x0000 - 0x0FFF)
const DWORD RAM_SIZE = 2048;      // ИСПРАВЛЕНО: 2 КБ ОЗУ под новый дамп
const DWORD RAM_START = 0x8000;    // ИСПРАВЛЕНО: Начало ОЗУ по адресу 0x8000
const DWORD RAM_END = 0x87FF;    // ИСПРАВЛЕНО: Конец ОЗУ по адресу 0x87FF

BYTE rom[ROM_SIZE];
BYTE ram[RAM_SIZE];

wchar_t last_read_key_char = L'-';
char display_digits[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
char display_latch[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
int digit_lifetime[8] = { 0 };
const int MAX_LIFETIME = 15; 
bool keys_pressed[12] = { false }; 
bool emulator_running = true;

// Текущее состояние динамика от Порта C
bool rk_port_c_speaker_state = false;

// --- ПАРАМЕТРЫ ЗВУКОВОГО ПОТОКА ---
const int AUDIO_SAMPLE_RATE = 22050;
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHeader[2];
short* audioBuffers[2] = { NULL, NULL };
const int AUDIO_BUF_SIZE = 1024;

// --- Кольцевой буфер синхронизации INTE ---
const int INTE_RING_BUF_SIZE = 16384;
short inte_ring_buffer[INTE_RING_BUF_SIZE] = { 0 };
volatile int inte_ring_write_ptr = 0;
volatile int inte_ring_read_ptr = 0;

HWND hEditDisplay = NULL;
HWND hEditDebug = NULL;
HWND hEditRAM = NULL;      // Изменено: отображает регион 0x82B0 - 0x82FF
HWND hEditRAM_Low = NULL;  // Изменено: отображает начальный регион ОЗУ 0x8000 - 0x807F
HWND hBtnKeys[12] = { NULL };
UINT_PTR timerId = 0;
WNDPROC OldButtonProc = NULL; 

char DecodeSegment(BYTE segments) {
    BYTE mask = (~segments) & 0x7F; // Индикатор с общим анодом
//    BYTE mask = segments & 0x7F; // Индикатор с общим катодом, для AON580_TEST.bin
    switch (mask) {
        // Стандартные символы
    case 0x3F: return '0'; case 0x06: return '1'; case 0x5B: return '2';
    case 0x4F: return '3'; case 0x66: return '4'; case 0x6D: return '5';
    case 0x7D: return '6'; case 0x07: return '7'; case 0x7F: return '8';
    case 0x6F: return '9'; case 0x77: return 'A'; case 0x7C: return 'b';
    case 0x39: return 'C'; case 0x5E: return 'd'; case 0x79: return 'E';
    case 0x71: return 'F'; case 0x40: return '-'; case 0x00: return ' ';

        // Одиночные сегменты (для пошагового теста)
    case 0x01: return 'a'; // Верхний
    case 0x02: return 'b'; // Верхний правый
    case 0x04: return 'c'; // Нижний правый
    case 0x08: return 'd'; // Нижний
    case 0x10: return 'e'; // Нижний левый
    case 0x20: return 'f'; // Верхний левый

    default: return 'H';
    }
}

void Reset(CPU8080* cpu) {
    cpu8080_reset(cpu);
}

void UpdateHardwareState(CPU8080* cpu) {
    // 1. DYNAMIC INDICATOR DISPATCH (7-Segment multiplexer emulation)
    int a0 = (cpu->ppi_pb >> 0) & 1;
    int a1 = (cpu->ppi_pb >> 1) & 1;
    int a2 = (cpu->ppi_pb >> 2) & 1;
    int digit_index = (a2 << 2) | (a1 << 1) | a0;
    if (digit_index >= 0 && digit_index < 8) {
        char current_char = DecodeSegment(cpu->ppi_pa);
        if (current_char != ' ') {
            display_latch[digit_index] = current_char;
            digit_lifetime[digit_index] = MAX_LIFETIME;
        }
    }

    // 2. КЛАВИАТУРА (Мультиплексор 155КП1)
    int kp1_address = (cpu->ppi_pb >> 4) & 0x0F;
    bool key_detected_this_tick = false;
    if (kp1_address >= 0 && kp1_address < 12 && keys_pressed[kp1_address]) {
        cpu->ppi_pc |= 0x04; // Активная единица на PC2
        key_detected_this_tick = true;
        const wchar_t* layout = L"0123456789*#";
        if (kp1_address >= 0 && kp1_address <= 11) {
            last_read_key_char = layout[kp1_address];
        }
    }
    else {
        cpu->ppi_pc &= ~0x04;
    }
    if (!key_detected_this_tick) {
        bool any_mouse_hold = false;
        for (int i = 0; i < 12; ++i) { if (keys_pressed[i]) any_mouse_hold = true; }
        if (!any_mouse_hold) last_read_key_char = L'-';
    }
}

BYTE PortInput(CPU8080* cpu, BYTE port) {
    if (port < 0xC0 || port > 0xC3) return 0xFF;

    switch (port) {
    case 0xC0: return cpu->ppi_pa;
    case 0xC1: return cpu->ppi_pb;
    case 0xC2:
    {
        // Inject stable hardware clock token (PC3) prior to state validation
        cpu->ppi_pc |= 0x08;

        // Handle timer delay strobe bit (PC6)
        if (cpu->ppi_pc & 0x40) cpu->ppi_pc &= ~0x40;
        else cpu->ppi_pc |= 0x40;

        HWND hCheckC1 = GetDlgItem(GetActiveWindow(), 200);
        HWND hCheckC0 = GetDlgItem(GetActiveWindow(), 201);

        // Sync external hook button to the emulator structure
        if (hCheckC1 && SendMessageW(hCheckC1, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            cpu->ppi_pc &= ~0x02; // Line PC1 down -> Handset off hook
        }
        else {
            cpu->ppi_pc |= 0x02;  // Line PC1 high -> Handset idle
        }

        if (hCheckC0 && SendMessageW(hCheckC0, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            cpu->ppi_pc |= 0x01;
        }
        else {
            cpu->ppi_pc &= ~0x01;
        }

        // Execute matrix mapping updates live on the port line
        UpdateHardwareState(cpu);

        return cpu->ppi_pc;
    }
    case 0xC3: return cpu->ppi_ctrl;
    }
    return 0xFF;
}

void PortOutput(CPU8080* cpu, BYTE port, BYTE value) {
    if (port < 0xC0 || port > 0xC3) return;

    // Фиксируем старое состояние звука перед записью
    BYTE old_sound = cpu->ppi_pc & 0x90;

    switch (port) {
    case 0xC0: cpu->ppi_pa = value; break;
    case 0xC1: cpu->ppi_pb = value; break;
    case 0xC2: cpu->ppi_pc = value; break;
    case 0xC3:
        cpu->ppi_ctrl = value;
        if ((value & 0x80) == 0) { // Побитный режим установки/сброса Порта C
            int bit = (value >> 1) & 0x07;
            if (value & 1) cpu->ppi_pc |= (1 << bit);
            else cpu->ppi_pc &= ~(1 << bit);
        }
        break;
    }
    UpdateHardwareState(cpu);

    // ОБНОВЛЕННЫЙ АСИНХРОННЫЙ АУДИО-ДЕТЕКТОР ДЛЯ ПОРТА C
    // Если на линиях PC7 или PC4 есть единица — включаем динамик, если ноль — выключаем.
    // Это выполняется мгновенно и не тормозит эмулятор!
    if (cpu->ppi_pc & 0x90) {
        rk_port_c_speaker_state = true;
    }
    else {
        rk_port_c_speaker_state = false;
    }
}

extern "C" uint8_t cpu8080_read(CPU8080* cpu, uint16_t address) {
    // Порты КР580ВВ55: ловим либо отображение на память (0xC000..0xC003), 
    // либо зеркалированный адрес портов ввода-вывода (0xC0C0, 0xC1C1, 0xC2C2, 0xC3C3)
    if ((address >= 0xC000 && address <= 0xC003) ||
        ((address & 0xFF) >= 0xC0 && (address & 0xFF) <= 0xC3 && (address >> 8) == (address & 0xFF)))
    {
        return PortInput(cpu, 0xC0 + (address & 0x03));
    }

    if (address < ROM_SIZE) return rom[address];
    if (address >= RAM_START && address <= RAM_END) return ram[address - RAM_START];
    return 0xFF;
}

extern "C" void cpu8080_write(CPU8080* cpu, uint16_t address, uint8_t value) {
    if ((address >= 0xC000 && address <= 0xC003) ||
        ((address & 0xFF) >= 0xC0 && (address & 0xFF) <= 0xC3 && (address >> 8) == (address & 0xFF)))
    {
        PortOutput(cpu, 0xC0 + (address & 0x03), value);
        return;
    }

    if (address >= RAM_START && address <= RAM_END) {
        ram[address - RAM_START] = value;
    }
}

static CPU8080 cpu;

static const int KeyAddressMap[] = {
    1, 2, 3, 
    4, 5, 6, 
    7, 8, 9, 
    10, 0, 11 
};

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int id = GetWindowLongPtrW(hwnd, GWLP_ID) - 100;
    if (id >= 0 && id < 12) {
        int hw_address = KeyAddressMap[id];
        if (msg == WM_LBUTTONDOWN) {
            keys_pressed[hw_address] = true;
            UpdateHardwareState(&cpu);
        }
        else if (msg == WM_LBUTTONUP) {
            keys_pressed[hw_address] = false;
            UpdateHardwareState(&cpu);
        }
    }
    return CallWindowProcW(OldButtonProc, hwnd, msg, wParam, lParam);
}

bool LoadROM(const wchar_t* filename) {
    Reset(&cpu);
    FILE* f = NULL;
    if (_wfopen_s(&f, filename, L"rb") != 0 || !f) return false;
    memset(rom, 0, ROM_SIZE);
    fread(rom, 1, ROM_SIZE, f);
    fclose(f);
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            hEditDisplay = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L" ", 
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER, 
                20, 20, 340, 40, hwnd, NULL, NULL, NULL);
            
            HFONT hFontDisp = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                DEFAULT_CHARSET, OUT_OUTLINE_PRECISION, CLIP_DEFAULT_PRECISION, 
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Courier New");
            SendMessageW(hEditDisplay, WM_SETFONT, (WPARAM)hFontDisp, TRUE);

            hEditDebug = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", 
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_MULTILINE, 
                380, 20, 240, 310, hwnd, NULL, NULL, NULL);
            
            HFONT hFontDebug = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                DEFAULT_CHARSET, OUT_OUTLINE_PRECISION, CLIP_DEFAULT_PRECISION, 
                CLEARTYPE_QUALITY, FIXED_PITCH, L"Courier New");
            SendMessageW(hEditDebug, WM_SETFONT, (WPARAM)hFontDebug, TRUE);

            hEditRAM_Low = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_MULTILINE | WS_VSCROLL,
                20, 385, 600, 150, hwnd, NULL, NULL, NULL); 
            SendMessageW(hEditRAM_Low, WM_SETFONT, (WPARAM)hFontDebug, TRUE);

            hEditRAM = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_READONLY | ES_MULTILINE | WS_VSCROLL,
                20, 550, 600, 120, hwnd, NULL, NULL, NULL);
            SendMessageW(hEditRAM, WM_SETFONT, (WPARAM)hFontDebug, TRUE);

            const wchar_t* keyLabels[] = {
                L"1", L"2", L"3",
                L"4", L"5", L"6",
                L"7", L"8", L"9",
                L"*", L"0", L"#"
            };
            int btnIndex = 0;
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) {
                    hBtnKeys[btnIndex] = CreateWindowW(L"BUTTON", keyLabels[btnIndex], 
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                        20 + c * 113, 80 + r * 50, 100, 40, 
                        hwnd, (HMENU)(INT_PTR)(100 + btnIndex), NULL, NULL);
                    
                    if (OldButtonProc == NULL) {
                        OldButtonProc = (WNDPROC)SetWindowLongPtrW(hBtnKeys[btnIndex], GWLP_WNDPROC, (LONG_PTR)ButtonSubclassProc);
                    } else {
                        SetWindowLongPtrW(hBtnKeys[btnIndex], GWLP_WNDPROC, (LONG_PTR)ButtonSubclassProc);
                    }
                    btnIndex++;
                }
            }
            HWND hCheckC1Init = CreateWindowW(L"BUTTON", L"Трубка поднята (Вход C1 = 0)", 
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                20, 285, 340, 20, hwnd, (HMENU)200, NULL, NULL);
            SendMessageW(hCheckC1Init, BM_SETCHECK, BST_CHECKED, 0); 

            CreateWindowW(L"BUTTON", L"Дополнительный вход (Вход C0 = 1)", 
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 
                20, 310, 340, 20, hwnd, (HMENU)201, NULL, NULL);

            CreateWindowW(L"BUTTON", L"Аппаратный сброс (RESET)", 
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                20, 335, 340, 35, hwnd, (HMENU)202, NULL, NULL);

            timerId = SetTimer(hwnd, 1, 20, NULL);
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == 200) {
                HWND hCheckC1 = GetDlgItem(hwnd, 200);
                if (SendMessageW(hCheckC1, BM_GETCHECK, 0, 0) == BST_CHECKED) cpu.ppi_pc &= ~0x02;
                else cpu.ppi_pc |= 0x02;
                UpdateHardwareState(&cpu);
            }
            else if (wmId == 201) {
                HWND hCheckC0 = GetDlgItem(hwnd, 201);
                if (SendMessageW(hCheckC0, BM_GETCHECK, 0, 0) == BST_CHECKED) cpu.ppi_pc |= 0x01;
                else cpu.ppi_pc &= ~0x01;
                UpdateHardwareState(&cpu);
            }
            else if (wmId == 202) {
                Reset(&cpu);
                last_read_key_char = L'-';
                HWND hCheckC1 = GetDlgItem(hwnd, 200);
                HWND hCheckC0 = GetDlgItem(hwnd, 201);
                if (SendMessageW(hCheckC1, BM_GETCHECK, 0, 0) != BST_CHECKED) cpu.ppi_pc |= 0x02; 
                if (SendMessageW(hCheckC0, BM_GETCHECK, 0, 0) == BST_CHECKED) cpu.ppi_pc |= 0x01; 
                memset(ram, 0, RAM_SIZE);
                for (int d = 0; d < 8; ++d) { display_digits[d] = display_latch[d] = ' '; digit_lifetime[d] = 0; }
                UpdateHardwareState(&cpu);
            }
            break;
        }
        case WM_DESTROY:
            KillTimer(hwnd, timerId);
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

const double PIT_CLOCK_HZ = 1780000.0;
const double TICKS_PER_SAMPLE = PIT_CLOCK_HZ / (double)AUDIO_SAMPLE_RATE;

// Генерирование звука через КР580ВИ53 на основе накопленной фазы частоты
void FillAudioBuffer(short* buffer, int samplesCount) {
    static double ticks_accumulator = 0.0;

    for (int i = 0; i < samplesCount; i++) {
        ticks_accumulator += TICKS_PER_SAMPLE;
        int ticks_to_consume = (int)ticks_accumulator;
        ticks_accumulator -= ticks_to_consume;

        if (ticks_to_consume <= 0) {
            buffer[i] = (i > 0) ? buffer[i - 1] : 0;
            continue;
        }

        int active_channels = 0;
        int mixed_signal = 0;

        // 2. Извлечение СИНХРОННОГО микшированного сигнала из кольцевого буфера
        int mixed_hardware_signal = 0;
        if (inte_ring_read_ptr != inte_ring_write_ptr) {
            mixed_hardware_signal = inte_ring_buffer[inte_ring_read_ptr];
            inte_ring_read_ptr = (inte_ring_read_ptr + 1) % INTE_RING_BUF_SIZE;
        }
        else {
            // Если буфер пуст, стабилизируем текущее состояние звука
            int inte_part = cpu.EI ? 3000 : -3000;
            int port_c_part = rk_port_c_speaker_state ? 3000 : -3000;
            mixed_hardware_signal = inte_part + port_c_part;
        }

        // 3. Финальная запись сэмпла в звуковую карту Windows
        if (active_channels > 0) {
            int vi53_signal = mixed_signal * 5000 / active_channels;
            buffer[i] = (short)((vi53_signal + mixed_hardware_signal) / 2);
        }
        else {
            buffer[i] = (short)mixed_hardware_signal;
        }
    }
}

void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    if (uMsg == WOM_DONE) {
        WAVEHDR* pHdr = (WAVEHDR*)dwParam1;
        FillAudioBuffer((short*)pHdr->lpData, AUDIO_BUF_SIZE);
        waveOutWrite(hwo, pHdr, sizeof(WAVEHDR));
    }
}

static double audio_ticks_accumulator = 0.0;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (!LoadROM(L"rom.bin")) {
        MessageBoxW(NULL, L"Файл прошивки ПЗУ 'rom.bin' не найден! Будет использована пустая память.", 
            L"Внимание", MB_OK | MB_ICONWARNING);
    }

    WNDCLASSEXW wc;
    wc.cbSize = sizeof(WNDCLASSEXW); wc.style = 0; wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0; wc.cbWndExtra = 0; wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION); wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszMenuName = NULL;
    wc.lpszClassName = L"K580SimulatorClass"; wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    
    if (!RegisterClassExW(&wc)) return 0;
    
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"K580SimulatorClass", 
        L"Симулятор К580ВМ80 / ВВ55 (С отладчиком и ОЗУ)", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 735, NULL, NULL, hInstance, NULL);
    
    if (!hwnd) return 0;
    ShowWindow(hwnd, nCmdShow); UpdateWindow(hwnd);
    
    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = 1; wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE; wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)WaveOutCallback, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
        for (int i = 0; i < 2; i++) {
            audioBuffers[i] = new short[AUDIO_BUF_SIZE];
            memset(audioBuffers[i], 0, AUDIO_BUF_SIZE * sizeof(short));
            waveHeader[i].lpData = (LPSTR)audioBuffers[i];
            waveHeader[i].dwBufferLength = AUDIO_BUF_SIZE * sizeof(short);
            waveHeader[i].dwFlags = 0;
            waveOutPrepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
            waveOutWrite(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
        }
    }

    MSG msg;
    LARGE_INTEGER frequency;
    LARGE_INTEGER last_hardware_time;
    double internal_cycles_debt = 0.0;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&last_hardware_time);

    while (emulator_running) {
        // Ожидание системных событий во избежание 100% загрузки процессора ПК
        MsgWaitForMultipleObjectsEx(0, NULL, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                emulator_running = false;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!emulator_running) break;

        // --- ДИСПЕТЧЕР И ТАКТОВАЯ СИНХРОНИЗАЦИЯ ЯДРА ---
        LARGE_INTEGER current_hardware_time;
        QueryPerformanceCounter(&current_hardware_time);
        double elapsed_seconds = (double)(current_hardware_time.QuadPart - last_hardware_time.QuadPart) / frequency.QuadPart;

        if (elapsed_seconds > 0.1) elapsed_seconds = 0.1; // Защита от лагов Windows

        if (elapsed_seconds > 0.0) {
            last_hardware_time = current_hardware_time;
            internal_cycles_debt += elapsed_seconds * SYSTEM_CLOCK_HZ; // Накапливаем такты ПК

            int cycles_to_execute = (int)internal_cycles_debt;
            if (cycles_to_execute > 0) {
                while (cycles_to_execute > 0) {
                    int elapsed_ticks = cpu8080_step(&cpu);

                    cycles_to_execute -= elapsed_ticks;
                    internal_cycles_debt -= elapsed_ticks;

                    // Накапливаем процессорные такты для генерации звука
                    audio_ticks_accumulator += elapsed_ticks;
                    while (audio_ticks_accumulator >= TICKS_PER_SAMPLE) {
                        audio_ticks_accumulator -= TICKS_PER_SAMPLE;

                        // Вычисляем уровень сигнала для INTE
                        int inte_part = cpu.EI ? 3000 : -3000;

                        // Вычисляем уровень сигнала для Порта C
                        int port_c_part = rk_port_c_speaker_state ? 3000 : -3000;

                        // Смешиваем (микшируем) оба сигнала в один сэмпл
                        short sample = (short)(inte_part + port_c_part);

                        // Записываем в кольцевой буфер
                        int next_write = (inte_ring_write_ptr + 1) % INTE_RING_BUF_SIZE;

                        // Защита от переполнения буфера
                        if (next_write != inte_ring_read_ptr) {
                            inte_ring_buffer[inte_ring_write_ptr] = sample;
                            inte_ring_write_ptr = next_write;
                        }
                    }

                    // Отсчитываем такты до кадрового прерывания 50 Гц
                    cpu.cycles_until_interrupt -= elapsed_ticks;
                    if (cpu.cycles_until_interrupt <= 0) {
                        cpu.cycles_until_interrupt += 35600; // На запуск следующего кадра

                        // Real-time title bar hardware tracer injection
                        wchar_t titleBuf[128];
                        swprintf_s(titleBuf, 128,
                            L"Simulator K580VM80 | Port B (Lines PB7-PB0): [%d%d%d%d %d%d%d%d] (Hex: 0x%02X)",
                            (cpu.ppi_pb >> 7) & 1, (cpu.ppi_pb >> 6) & 1, (cpu.ppi_pb >> 5) & 1, (cpu.ppi_pb >> 4) & 1,
                            (cpu.ppi_pb >> 3) & 1, (cpu.ppi_pb >> 2) & 1, (cpu.ppi_pb >> 1) & 1, (cpu.ppi_pb >> 0) & 1,
                            cpu.ppi_pb);

                        // Push the formatted bit pattern directly into the application window header
                        SetWindowTextW(hwnd, titleBuf);

                        // Логика плавного затухания динамической индикации дисплея
                        for (int d = 0; d < 8; ++d) {
                            if (digit_lifetime[d] > 0) {
                                digit_lifetime[d]--;
                                display_digits[d] = display_latch[d];
                            }
                            else {
                                display_digits[d] = ' '; display_latch[d] = ' ';
                            }
                        }

                        // Обновление текстового табло индикации
                        wchar_t textBuf[33];
                        swprintf_s(textBuf, 33, L"[ %c%c%c%c%c%c%c%c ]",
                            display_digits[0], display_digits[1], display_digits[2], display_digits[3],
                            display_digits[4], display_digits[5], display_digits[6], display_digits[7]);
                        SetWindowTextW(hEditDisplay, textBuf);

                        // Считываем скан-код кнопки строго из подтвержденной ячейки 0x82CE
                        BYTE firmware_key_code = ram[0x82CE - 0x8000];

                        // Считываем состояние рычага трубки из верифицированной ячейки 0x82B8
                        BYTE hook_status = ram[0x82B8 - 0x8000];

                        // Динамически вычисляем виртуальный статус ROM_MODE для отладчика:
                        // Если трубка поднята (hook_status == 0) и был считан код кнопки '4' (0x04)
                        // или микропрограмма уже начала обработку (например, PC ушел в верхние адреса)
                        BYTE firmware_mode_flag = 0x00;
                        if (hook_status == 0x00) {
                            // Если считана кнопка "4", фиксируем активацию режима эхо-печати
                            if (firmware_key_code == 0x04 || cpu.PC > 0x0100) {
                                firmware_mode_flag = 0x01;
                            }
                        }

                        // Формирование лога регистров CPU и переменных ПЗУ
                        wchar_t debugBuf[513];
                        swprintf_s(debugBuf, 512,
                            L"--- REGISTERS ---\r\nA: 0x%02X\r\nBC: 0x%02X%02X\r\nDE: 0x%02X%02X\r\nHL: 0x%02X%02X\r\nSP: 0x%04X\r\nPC: 0x%04X\r\n\r\n--- FLAGS ---\r\nS:%d Z:%d AC:%d P:%d CY:%d\r\n\r\n--- STATE ---\r\nHALT: %s\r\nINTE: %s\r\nGUI_KEY: %c\r\n\r\n--- ROM VARIABLES ---\r\nROM_KEY: 0x%02X\r\nROM_MODE: 0x%02X\r\nLINE: 0x%02X",
                            cpu.A, cpu.B, cpu.C, cpu.D, cpu.E, cpu.H, cpu.L, cpu.SP, cpu.PC,
                            (cpu.S) ? 1 : 0, (cpu.Z) ? 1 : 0, (cpu.AC) ? 1 : 0, (cpu.P) ? 1 : 0, (cpu.CY) ? 1 : 0,
                            cpu.halted ? L"TRUE" : L"FALSE", cpu.EI ? L"TRUE" : L"FALSE", last_read_key_char,
                            firmware_key_code,   // Теперь покажет реальный код из 0x421F (вместо застывшего 0x76)
                            firmware_mode_flag,  // Покажет 0x01 при успешном переключении режима
                            hook_status          // Покажет состояние из 0x421E
                        );
                        SetWindowTextW(hEditDebug, debugBuf);

                        // Отрисовка дампа RAM Tracker (0x82B0 - 0x82FF, идеальный фокус на все переменные АТС)
                        wchar_t ramDump[8192] = L"--- RAM TRACKER (0x82B0 - 0x82FF) ---\r\n";
                        wchar_t ramLine[128];
                        for (WORD addr = 0x82B0; addr <= 0x82FF; addr += 16) {
                            swprintf_s(ramLine, 128, L"0x%04X: ", addr);
                            wcscat_s(ramDump, 8192, ramLine);
                            for (int b = 0; b < 16; ++b) {
                                swprintf_s(ramLine, 128, L"%02X ", ram[addr - 0x8000 + b]);
                                wcscat_s(ramDump, 8192, ramLine);
                            }
                            wcscat_s(ramDump, 8192, L" | ");
                            for (int b = 0; b < 16; ++b) {
                                BYTE val = ram[addr - 0x8000 + b];
                                swprintf_s(ramLine, 128, L"%c", (val >= 32 && val <= 126) ? (wchar_t)val : L'.');
                                wcscat_s(ramDump, 8192, ramLine);
                            }
                            wcscat_s(ramDump, 8192, L"\r\n");
                        }
                        SetWindowTextW(hEditRAM, ramDump);
                        // --- НОВОЕ: Отрисовка дампа памяти для региона 0x8000 - 0x807F ---
                        wchar_t ramLowDump[4096] = L"--- RAM TRACKER (0x8000 - 0x807F) ---\r\n";
                        wchar_t ramLowLine[128];
                        for (WORD addr = 0x8000; addr <= 0x807F; addr += 16) {
                            swprintf_s(ramLowLine, 128, L"0x%04X: ", addr);
                            wcscat_s(ramLowDump, 4096, ramLowLine);
                            for (int b = 0; b < 16; ++b) {
                                swprintf_s(ramLowLine, 128, L"%02X ", ram[addr - 0x8000 + b]);
                                wcscat_s(ramLowDump, 4096, ramLowLine);
                            }
                            wcscat_s(ramLowDump, 4096, L" | ");
                            for (int b = 0; b < 16; ++b) {
                                BYTE val = ram[addr - 0x8000 + b];
                                swprintf_s(ramLowLine, 128, L"%c", (val >= 32 && val <= 126) ? (wchar_t)val : L'.');
                                wcscat_s(ramLowDump, 4096, ramLowLine);
                            }
                            wcscat_s(ramLowDump, 4096, L"\r\n");
                        }
                        SetWindowTextW(hEditRAM_Low, ramLowDump);
                    }
                }
            }
        }

        if (internal_cycles_debt <= 0.0) {
            Sleep(1); // Освобождаем CPU ПК
        }
    }

    // --- Безопасная очистка аудиодескрипторов перед выходом ---
    if (hWaveOut) {
        for (int i = 0; i < 2; i++) {
            waveOutUnprepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
            delete[] audioBuffers[i];
        }
        waveOutClose(hWaveOut);
    }
    return (int)msg.wParam;
}
