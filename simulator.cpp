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

#pragma comment(lib, "winmm.lib")

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

// --- ЭМУЛЯЦИЯ БИПЕРА INTE ---
bool rk_inte_speaker_state = false; // Текущее состояние динамика от INTE

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
    BYTE mask = (~segments) & 0x7F;
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

#pragma pack(push, 1)
struct CPU8080 {
	BYTE A, B, C, D, E, H, L;
	WORD PC, SP;
    bool S, Z, AC, P, CY;
	bool EI;
	bool halted; // <-- ДОБАВИТЬ: флаг останова процессора
    bool INTE;
    int cycles_until_interrupt = 35600; // 1780000 Гц / 50 Гц = 35600 тактов на кадр
    bool int_pending = false;           // Флаг того, что прерывание пришло от дисплея

	// Три порта КР580ВВ55 (ППА)
	BYTE ppi_pa, ppi_pb, ppi_pc, ppi_ctrl;

	void Reset() {
		PC = 0x0000;
		SP = 0x87FF;
		A = B = C = D = E = H = L = 0;
		S = Z = AC = P = CY = false;
		EI = false;
		halted = false; // <-- ДОБАВИТЬ: сброс флага при перезагрузке
		ppi_pa = ppi_pb = ppi_pc = 0xFF; ppi_ctrl = 0x9B;
        rk_inte_speaker_state = false;
	}

	void SetFlagsZSP(BYTE res) {
		Z = (res == 0);
		S = ((res & 0x80) != 0);
		BYTE Ty = res;
		Ty ^= Ty >> 4; Ty ^= Ty >> 2; Ty ^= Ty >> 1;
		P = !(Ty & 1);
	}

	void SetFlagsINR(BYTE old_val, BYTE new_val) {
		AC = ((old_val & 0x0F) + 1 > 0x0F);
		SetFlagsZSP(new_val);
	}

	void SetFlagsDCR(BYTE old_val, BYTE new_val) {
		AC = ((old_val & 0x0F) == 0);
		SetFlagsZSP(new_val);
	}

	void SetFlagsAdd(BYTE r1, BYTE r2, int res) {
		CY = (res > 0xFF);
		AC = (((r1 & 0xF) + (r2 & 0xF)) > 0xF);
		SetFlagsZSP((BYTE)res);
	}

	void SetFlagsSub(BYTE r1, BYTE r2, int res) {
		CY = (res < 0);
		AC = (((r1 & 0xF) - (r2 & 0xF)) < 0);
		SetFlagsZSP((BYTE)res);
	}

	WORD GetHL() const { return (H << 8) | L; }
	void SetHL(WORD val) { H = val >> 8; L = val & 0xFF; }
	WORD GetBC() const { return (B << 8) | C; }
	void SetBC(WORD val) { B = val >> 8; C = val & 0xFF; }
	WORD GetDE() const { return (D << 8) | E; }
	void SetDE(WORD val) { D = val >> 8; E = val & 0xFF; }

	BYTE GetReg(int r) {
		switch (r) {
		case 0: return B; case 1: return C; case 2: return D; case 3: return E;
		case 4: return H; case 5: return L; case 6: return Read(GetHL()); default: return A;
		}
	}

	void SetReg(int r, BYTE val) {
		switch (r) {
		case 0: B = val; break; case 1: C = val; break; case 2: D = val; break; case 3: E = val; break;
		case 4: H = val; break; case 5: L = val; break; case 6: Write(GetHL(), val); break; default: A = val; break;
		}
	}

    void UpdateHardwareState() {
        // 1. DYNAMIC INDICATOR DISPATCH (7-Segment multiplexer emulation)
        int a0 = (ppi_pb >> 0) & 1;
        int a1 = (ppi_pb >> 1) & 1;
        int a2 = (ppi_pb >> 2) & 1;
        int digit_index = (a2 << 2) | (a1 << 1) | a0;
        if (digit_index >= 0 && digit_index < 8) {
            char current_char = DecodeSegment(ppi_pa);
            if (current_char != ' ') {
                display_latch[digit_index] = current_char;
                digit_lifetime[digit_index] = MAX_LIFETIME;
            }
        }

        // 2. КЛАВИАТУРА (Мультиплексор 155КП1)
        int kp1_address = (ppi_pb >> 4) & 0x0F;
        bool key_detected_this_tick = false;
        if (kp1_address >= 0 && kp1_address < 12 && keys_pressed[kp1_address]) {
            ppi_pc |= 0x04; // Активная единица на PC2
            key_detected_this_tick = true;
            const wchar_t* layout = L"0123456789*#";
            if (kp1_address >= 0 && kp1_address <= 11) {
                last_read_key_char = layout[kp1_address];
            }
        }
        else {
            ppi_pc &= ~0x04;
        }
        if (!key_detected_this_tick) {
            bool any_mouse_hold = false;
            for (int i = 0; i < 12; ++i) { if (keys_pressed[i]) any_mouse_hold = true; }
            if (!any_mouse_hold) last_read_key_char = L'-';
        }
    }

    BYTE PortInput(BYTE port) {
        if (port < 0xC0 || port > 0xC3) return 0xFF;

        switch (port) {
        case 0xC0: return ppi_pa;
        case 0xC1: return ppi_pb;
        case 0xC2:
        {
            // Inject stable hardware clock token (PC3) prior to state validation
            ppi_pc |= 0x08;

            // Handle timer delay strobe bit (PC6)
            if (ppi_pc & 0x40) ppi_pc &= ~0x40;
            else ppi_pc |= 0x40;

            HWND hCheckC1 = GetDlgItem(GetActiveWindow(), 200);
            HWND hCheckC0 = GetDlgItem(GetActiveWindow(), 201);

            // Sync external hook button to the emulator structure
            if (hCheckC1 && SendMessageW(hCheckC1, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                ppi_pc &= ~0x02; // Line PC1 down -> Handset off hook
            }
            else {
                ppi_pc |= 0x02;  // Line PC1 high -> Handset idle
            }

            if (hCheckC0 && SendMessageW(hCheckC0, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                ppi_pc |= 0x01;
            }
            else {
                ppi_pc &= ~0x01;
            }

            // Execute matrix mapping updates live on the port line
            UpdateHardwareState();

            return ppi_pc;
        }
        case 0xC3: return ppi_ctrl;
        }
        return 0xFF;
    }

    void PortOutput(BYTE port, BYTE value) {
        if (port < 0xC0 || port > 0xC3) return;

        // Фиксируем старое состояние звука перед записью
        BYTE old_sound = ppi_pc & 0x90;

        switch (port) {
        case 0xC0: ppi_pa = value; break;
        case 0xC1: ppi_pb = value; break;
        case 0xC2: ppi_pc = value; break;
        case 0xC3:
            ppi_ctrl = value;
            if ((value & 0x80) == 0) { // Побитный режим установки/сброса Порта C
                int bit = (value >> 1) & 0x07;
                if (value & 1) ppi_pc |= (1 << bit);
                else ppi_pc &= ~(1 << bit);
            }
            break;
        }
        UpdateHardwareState();

        // ОБНОВЛЕННЫЙ АСИНХРОННЫЙ АУДИО-ДЕТЕКТОР ДЛЯ ПОРТА C
        // Если на линиях PC7 или PC4 есть единица — включаем динамик, если ноль — выключаем.
        // Это выполняется мгновенно и не тормозит эмулятор!
        if (ppi_pc & 0x90) {
            rk_port_c_speaker_state = true;
        }
        else {
            rk_port_c_speaker_state = false;
        }
    }

    BYTE Read(WORD address) {
        // ИСПРАВЛЕНИЕ: Превращаем 0xC000..0xC003 в порты 0xC0..0xC3
        if (address >= 0xC000 && address <= 0xC003) {
            return PortInput((BYTE)(0xC0 + (address & 0x03)));
        }
        if (address < ROM_SIZE) return rom[address];
        if (address >= RAM_START && address <= RAM_END) return ram[address - RAM_START];
        return 0xFF;
    }

    void Write(WORD address, BYTE value) {
        // ИСПРАВЛЕНИЕ: Превращаем 0xC000..0xC003 в порты 0xC0..0xC3
        if (address >= 0xC000 && address <= 0xC003) {
            PortOutput((BYTE)(0xC0 + (address & 0x03)), value);
            return;
        }

        if (address >= RAM_START && address <= RAM_END) {
            ram[address - RAM_START] = value;
        }
    }

	int Step() {
		// Таблица базовых тактов для всех 256 инструкций Intel 8080
		static const int lut_cycles[256] = {
			4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7,  4,  // 00-0F
			4, 10, 7,  5,  5,  5,  7,  4,  4, 10, 7,  5,  5,  5,  7,  4,  // 10-1F
			4, 10, 16, 5,  5,  5,  7,  4,  4, 10, 16, 5,  5,  5,  7,  4,  // 20-2F
			4, 10, 13, 5,  10, 10, 10, 4,  4, 10, 13, 5,  5,  5,  7,  4,  // 30-3F
			5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 40-4F
			5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 50-5F
			5, 5,  5,  5,  5,  5,  7,  5,  5, 5,  5,  5,  5,  5,  7,  5,  // 60-6F
			7, 7,  7,  7,  7,  7,  7,  7,  5, 5,  5,  5,  5,  5,  7,  5,  // 70-7F
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // 80-8F
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // 90-9F
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // A0-AF
			4, 4,  4,  4,  4,  4,  7,  4,  4, 4,  4,  4,  4,  4,  7,  4,  // B0-BF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // C0-CF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // D0-DF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11, // E0-EF
			5, 10, 10, 10, 11, 11, 7,  11, 5, 10, 10, 10, 11, 17, 7,  11  // F0-FF
		};

		// 1. Проверяем аппаратное прерывание перед декодированием инструкции
		if (int_pending && EI) {
			int_pending = false;
			halted = false;
		}

		// 2. Если процессор в состоянии HLT
		if (halted) {
			return 4; // Будем возвращать по 4 такта
		}

		BYTE op = Read(PC++);
		int cycles = lut_cycles[op];

		// Группа команд MOV r1, r2 (исключая MOV M, M, которая является HLT 0x76)
		if ((op & 0xC0) == 0x40 && op != 0x76) {
			SetReg((op >> 3) & 7, GetReg(op & 7));
			return cycles;
		}

		switch (op) {
			// NOP инструкции
		case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28:
		case 0x30: case 0x38: return cycles;

			// Останавливаем процессор до аппаратного прерывания
		case 0x76: halted = true; return cycles;

			// MVI r, imm
		case 0x06: B = Read(PC++); return cycles;
		case 0x0E: C = Read(PC++); return cycles;
		case 0x16: D = Read(PC++); return cycles;
		case 0x1E: E = Read(PC++); return cycles;
		case 0x26: H = Read(PC++); return cycles;
		case 0x2E: L = Read(PC++); return cycles;
		case 0x36: Write(GetHL(), Read(PC++)); return cycles;
		case 0x3E: A = Read(PC++); return cycles;

			// LXI rp, imm
		case 0x01: { WORD l = Read(PC++); WORD h = Read(PC++); SetBC((h << 8) | l); return cycles; }
		case 0x11: { WORD l = Read(PC++); WORD h = Read(PC++); SetDE((h << 8) | l); return cycles; }
		case 0x21: { WORD l = Read(PC++); WORD h = Read(PC++); SetHL((h << 8) | l); return cycles; }
		case 0x31: { WORD l = Read(PC++); WORD h = Read(PC++); SP = (h << 8) | l; return cycles; }

				 // ADD r
		case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
		case 0x86: case 0x87: { BYTE src = GetReg(op & 7); int r = A + src; SetFlagsAdd(A, src, r); A = (BYTE)r; return cycles; }

				 // ADC r
		case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D:
		case 0x8E: case 0x8F: { BYTE src = GetReg(op & 7); int r = A + src + (CY ? 1 : 0); SetFlagsAdd(A, src, r); A = (BYTE)r; return cycles; }

				 // SUB r
		case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
		case 0x96: case 0x97: { BYTE src = GetReg(op & 7); int r = A - src; SetFlagsSub(A, src, r); A = (BYTE)r; return cycles; }

				 // SBB r
		case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D:
		case 0x9E: case 0x9F: { BYTE src = GetReg(op & 7); int r = A - src - (CY ? 1 : 0); SetFlagsSub(A, src, r); A = (BYTE)r; return cycles; }

				 // ANA r
		case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
		case 0xA6: case 0xA7: { A &= GetReg(op & 7); CY = false; AC = true; SetFlagsZSP(A); return cycles; }

				 // XRA r
		case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD:
		case 0xAE: case 0xAF: { A ^= GetReg(op & 7); CY = false; AC = false; SetFlagsZSP(A); return cycles; }

				 // ORA r
		case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
		case 0xB6: case 0xB7: { A |= GetReg(op & 7); CY = false; AC = false; SetFlagsZSP(A); return cycles; }

				 // CMP r
		case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD:
		case 0xBE: case 0xBF: { BYTE src = GetReg(op & 7); int r = A - src; SetFlagsSub(A, src, r); return cycles; }

				 // Операции с непосредственным значением (Иммедиат)
		case 0xC6: { BYTE imm = Read(PC++); int r = A + imm; SetFlagsAdd(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xCE: { BYTE imm = Read(PC++); int r = A + imm + (CY ? 1 : 0); SetFlagsAdd(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xD6: { BYTE imm = Read(PC++); int r = A - imm; SetFlagsSub(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xDE: { BYTE imm = Read(PC++); int r = A - imm - (CY ? 1 : 0); SetFlagsSub(A, imm, r); A = (BYTE)r; return cycles; }
		case 0xE6: { A &= Read(PC++); CY = false; AC = true; SetFlagsZSP(A); return cycles; }
		case 0xEE: { A ^= Read(PC++); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
		case 0xF6: { A |= Read(PC++); CY = false; AC = false; SetFlagsZSP(A); return cycles; }
		case 0xFE: { BYTE imm = Read(PC++); int r = A - imm; SetFlagsSub(A, imm, r); return cycles; }

				 // Безусловные JMP / CALL / RET
		case 0xC3: { WORD l = Read(PC++); WORD h = Read(PC++); PC = (h << 8) | l; return cycles; }
		case 0xCD: { WORD l = Read(PC++); WORD h = Read(PC++); Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; return cycles; }
		case 0xC9: { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; return cycles; }

				 // Возвраты по условию (При переходе тратится 11 тактов вместо 5 базовых)
		case 0xC0: if (!Z) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xC8: if (Z) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xD0: if (!CY) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xD8: if (CY) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xE0: if (!P) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xE8: if (P) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xF0: if (!S) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;
		case 0xF8: if (S) { WORD l = Read(SP++); WORD h = Read(SP++); PC = (h << 8) | l; cycles = 11; } return cycles;

			// Переходы по условию (Всегда занимают 10 тактов в 8080)
		case 0xC2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!Z) { PC = (h << 8) | l; } return cycles; }
		case 0xCA: { WORD l = Read(PC++); WORD h = Read(PC++); if (Z) { PC = (h << 8) | l; } return cycles; }
		case 0xD2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!CY) { PC = (h << 8) | l; } return cycles; }
		case 0xDA: { WORD l = Read(PC++); WORD h = Read(PC++); if (CY) { PC = (h << 8) | l; } return cycles; }
		case 0xE2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!P) { PC = (h << 8) | l; } return cycles; }
		case 0xEA: { WORD l = Read(PC++); WORD h = Read(PC++); if (P) { PC = (h << 8) | l; } return cycles; }
		case 0xF2: { WORD l = Read(PC++); WORD h = Read(PC++); if (!S) { PC = (h << 8) | l; } return cycles; }
		case 0xFA: { WORD l = Read(PC++); WORD h = Read(PC++); if (S) { PC = (h << 8) | l; } return cycles; }

				 // Вызовы по условию (При вызове тратится 17 тактов вместо 11 базовых)
		case 0xC4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!Z) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xCC: { WORD l = Read(PC++); WORD h = Read(PC++); if (Z) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xD4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!CY) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xDC: { WORD l = Read(PC++); WORD h = Read(PC++); if (CY) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xE4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!P) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xEC: { WORD l = Read(PC++); WORD h = Read(PC++); if (P) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xF4: { WORD l = Read(PC++); WORD h = Read(PC++); if (!S) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }
		case 0xFC: { WORD l = Read(PC++); WORD h = Read(PC++); if (S) { Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (h << 8) | l; cycles = 17; } return cycles; }

				 // INX / DCX
		case 0x03: SetBC(GetBC() + 1); return cycles;
		case 0x13: SetDE(GetDE() + 1); return cycles;
		case 0x23: SetHL(GetHL() + 1); return cycles;
		case 0x33: SP++; return cycles;
		case 0x0B: SetBC(GetBC() - 1); return cycles;
		case 0x1B: SetDE(GetDE() - 1); return cycles;
		case 0x2B: SetHL(GetHL() - 1); return cycles;
		case 0x3B: SP--; return cycles;

			// INR / DCR
		case 0x04: { BYTE old = B; B++; SetFlagsINR(old, B); return cycles; }
		case 0x05: { BYTE old = B; B--; SetFlagsDCR(old, B); return cycles; }
		case 0x0C: { BYTE old = C; C++; SetFlagsINR(old, C); return cycles; }
		case 0x0D: { BYTE old = C; C--; SetFlagsDCR(old, C); return cycles; }
		case 0x14: { BYTE old = D; D++; SetFlagsINR(old, D); return cycles; }
		case 0x15: { BYTE old = D; D--; SetFlagsDCR(old, D); return cycles; }
		case 0x1C: { BYTE old = E; E++; SetFlagsINR(old, E); return cycles; }
		case 0x1D: { BYTE old = E; E--; SetFlagsDCR(old, E); return cycles; }
		case 0x24: { BYTE old = H; H++; SetFlagsINR(old, H); return cycles; }
		case 0x25: { BYTE old = H; H--; SetFlagsDCR(old, H); return cycles; }
		case 0x2C: { BYTE old = L; L++; SetFlagsINR(old, L); return cycles; }
		case 0x2D: { BYTE old = L; L--; SetFlagsDCR(old, L); return cycles; }
		case 0x34: { BYTE old = Read(GetHL()); BYTE v = old + 1; Write(GetHL(), v); SetFlagsINR(old, v); return cycles; }
		case 0x35: { BYTE old = Read(GetHL()); BYTE v = old - 1; Write(GetHL(), v); SetFlagsDCR(old, v); return cycles; }
		case 0x3C: { BYTE old = A; A++; SetFlagsINR(old, A); return cycles; }
		case 0x3D: { BYTE old = A; A--; SetFlagsDCR(old, A); return cycles; }

				 // DAD rp
		case 0x09: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetBC(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
		case 0x19: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetDE(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
		case 0x29: { unsigned int r = (unsigned int)GetHL() + (unsigned int)GetHL(); CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }
		case 0x39: { unsigned int r = (unsigned int)GetHL() + (unsigned int)SP;    CY = (r > 0xFFFF); SetHL((WORD)(r & 0xFFFF)); return cycles; }

				 // PUSH / POP
		case 0xC5: Write(--SP, B); Write(--SP, C); return cycles;
		case 0xD5: Write(--SP, D); Write(--SP, E); return cycles;
		case 0xE5: Write(--SP, H); Write(--SP, L); return cycles;
		case 0xF5: { BYTE psw = (S << 7) | (Z << 6) | (0 << 5) | (AC << 4) | (0 << 3) | (P << 2) | (2) | (CY ? 1 : 0); Write(--SP, A); Write(--SP, psw); return cycles; }
		case 0xC1: C = Read(SP++); B = Read(SP++); return cycles;
		case 0xD1: E = Read(SP++); D = Read(SP++); return cycles;
		case 0xE1: L = Read(SP++); H = Read(SP++); return cycles;
		case 0xF1: { BYTE psw = Read(SP++); A = Read(SP++); S = (psw & 0x80) != 0; Z = (psw & 0x40) != 0; AC = (psw & 0x10) != 0; P = (psw & 0x04) != 0; CY = (psw & 0x01) != 0; return cycles; }

				 // Прямая адресация (STA, LDA, SHLD, LHLD)
		case 0x32: { WORD l = Read(PC++); WORD h = Read(PC++); Write((h << 8) | l, A); return cycles; }
		case 0x3A: { WORD l = Read(PC++); WORD h = Read(PC++); A = Read((h << 8) | l); return cycles; }
		case 0x22: { WORD l = Read(PC++); WORD h = Read(PC++); WORD a = (h << 8) | l; Write(a, L); Write(a + 1, H); return cycles; }
		case 0x2A: { WORD l = Read(PC++); WORD h = Read(PC++); WORD a = (h << 8) | l; L = Read(a); H = Read(a + 1); return cycles; }

				 // Косвенная адресация
		case 0x02: Write(GetBC(), A); return cycles;
		case 0x12: Write(GetDE(), A); return cycles;
		case 0x0A: A = Read(GetBC()); return cycles;
		case 0x1A: A = Read(GetDE()); return cycles;

			// Циклические сдвиги аккумулятора
		case 0x07: { CY = (A & 0x80) != 0; A = (BYTE)(((A << 1) | (CY ? 1 : 0)) & 0xFF); return cycles; }
		case 0x0F: { CY = (A & 1) != 0; A = (BYTE)(((A >> 1) | (CY ? 0x80 : 0)) & 0xFF); return cycles; }
		case 0x17: { bool old_cy = CY; CY = (A & 0x80) != 0; A = (BYTE)(((A << 1) | (old_cy ? 1 : 0)) & 0xFF); return cycles; }
		case 0x1F: { bool old_cy = CY; CY = (A & 1) != 0; A = (BYTE)(((A >> 1) | (old_cy ? 0x80 : 0)) & 0xFF); return cycles; }

				 // RST 0 - RST 7
		case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
			Write(--SP, PC >> 8); Write(--SP, PC & 0xFF); PC = (op & 0x38); return cycles;

			// OUT port
        case 0xD3: PortOutput(Read(PC++), A); break; 
            // IN port
        case 0xDB: A = PortInput(Read(PC++)); break;

				 // Десятичная коррекция аккумулятора (DAA)
		case 0x27: {
			BYTE corr = 0; bool new_cy = CY;
			if ((A & 0x0F) > 9 || AC) corr |= 0x06;
			if (A > 0x99 || CY) { corr |= 0x60; new_cy = true; }
			int res = A + corr; AC = ((A & 0x0F) + (corr & 0x0F)) > 0x0F;
			A = (BYTE)res; CY = new_cy; SetFlagsZSP(A); return cycles;
		}

				 // Специальные команды
		case 0x2F: A = (BYTE)(~A & 0xFF); return cycles;
		case 0x37: CY = true; return cycles;
		case 0x3F: CY = !CY; return cycles;
		case 0xEB: { BYTE t = D; D = H; H = t; t = E; E = L; L = t; return cycles; }
		case 0xE3: { BYTE l = Read(SP); BYTE h = Read(SP + 1); Write(SP, L); Write(SP + 1, H); L = l; H = h; return cycles; }
		case 0xF9: SP = GetHL(); return cycles;
		case 0xE9: PC = GetHL(); return cycles;
		case 0xFB: EI = true; INTE = true; rk_inte_speaker_state = true; return cycles;  // EI включает INTE
		case 0xF3: EI = false; INTE = false; rk_inte_speaker_state = false; return cycles; // DI выключает INTE
		}
		return cycles;
	}
};
#pragma pack(pop)

CPU8080 cpu;


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
            cpu.UpdateHardwareState();
        }
        else if (msg == WM_LBUTTONUP) {
            keys_pressed[hw_address] = false;
            cpu.UpdateHardwareState();
        }
    }
    return CallWindowProcW(OldButtonProc, hwnd, msg, wParam, lParam);
}

bool LoadROM(const wchar_t* filename) {
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
                cpu.UpdateHardwareState();
            }
            else if (wmId == 201) {
                HWND hCheckC0 = GetDlgItem(hwnd, 201);
                if (SendMessageW(hCheckC0, BM_GETCHECK, 0, 0) == BST_CHECKED) cpu.ppi_pc |= 0x01;
                else cpu.ppi_pc &= ~0x01;
                cpu.UpdateHardwareState();
            }
            else if (wmId == 202) {
                cpu.PC = 0x0000;
                cpu.SP = 0x87FF;
                cpu.A = cpu.B = cpu.C = cpu.D = cpu.E = cpu.H = cpu.L = 0;
                cpu.halted = false; cpu.INTE = false;
                cpu.ppi_pa = cpu.ppi_pb = cpu.ppi_ctrl = 0;
                last_read_key_char = L'-';
                HWND hCheckC1 = GetDlgItem(hwnd, 200);
                HWND hCheckC0 = GetDlgItem(hwnd, 201);
                cpu.ppi_pc = 0;
                if (SendMessageW(hCheckC1, BM_GETCHECK, 0, 0) != BST_CHECKED) cpu.ppi_pc |= 0x02; 
                if (SendMessageW(hCheckC0, BM_GETCHECK, 0, 0) == BST_CHECKED) cpu.ppi_pc |= 0x01; 
                memset(ram, 0, RAM_SIZE);
                for (int d = 0; d < 8; ++d) { display_digits[d] = display_latch[d] = ' '; digit_lifetime[d] = 0; }
                cpu.UpdateHardwareState();
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
            int inte_part = rk_inte_speaker_state ? 3000 : -3000;
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
    cpu.PC = 0x0000; 
    cpu.SP = 0x87FF; // ИСПРАВЛЕНО: Стартовая инициализация вершины стека нового ОЗУ на 0x87FF
    cpu.halted = false; cpu.INTE = false;
    cpu.ppi_pa = 0; cpu.ppi_pb = 0; cpu.ppi_pc = 0x00; cpu.ppi_ctrl = 0;

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

    const double SYSTEM_CLOCK_HZ = 1780000.0; // Частота ядра (1.78 МГц)
    double time_since_last_interrupt = 0.0;

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
                    int elapsed_ticks = cpu.Step();

                    cycles_to_execute -= elapsed_ticks;
                    internal_cycles_debt -= elapsed_ticks;

                    // Накапливаем процессорные такты для генерации звука
                    audio_ticks_accumulator += elapsed_ticks;
                    while (audio_ticks_accumulator >= TICKS_PER_SAMPLE) {
                        audio_ticks_accumulator -= TICKS_PER_SAMPLE;

                        // Вычисляем уровень сигнала для INTE
                        int inte_part = rk_inte_speaker_state ? 3000 : -3000;

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
                        cpu.int_pending = true; // Выставляем запрос на прерывание для процессора
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
                            cpu.halted ? L"TRUE" : L"FALSE", cpu.INTE ? L"TRUE" : L"FALSE", last_read_key_char,
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
