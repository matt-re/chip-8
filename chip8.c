#include <errno.h>
#include <locale.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#ifndef MIN
#define MIN(A,B) ((A)<(B)?(A):(B))
#endif

#define PC_ADDRESS	0x000
#define PCPREV_ADDRESS	0x002
#define I_ADDRESS	0x004
#define SIZE_ADDRESS	0x006
#define ACC_ADDRESS	0x008
#define SP_ADDRESS	0x00A
#define DELAY_ADDRESS	0x00B
#define SOUND_ADDRESS	0x00C
#define V_ADDRESS	0x00D
#define FONT_ADDRESS	0x01D
#define ERROR_ADDRESS	0x06D
#define PROG_ADDRESS	0x200
#define PROG_END	0xE9F
#define STACK_ADDRESS	0xEA0
#define VIDEO_ADDRESS	0xF00
#define MEMORY_SIZE	0x1000

#define ERROR_SIZE	127
#define FLAG_REG	15
#define FONT_SIZE	80
#define FONT_WIDTH	5
#define PROG_START	PROG_ADDRESS
#define PROG_MAX	(PROG_END - PROG_START + 1)
#define STACK_COUNT	12
#define VIDEO_BYTES	256
#define VIDEO_HEIGHT	32
#define VIDEO_WIDTH	64

#define CHIP8_ORIGINAL_QUIRKS (CHIP8_QUIRK_INCREMENT_I | CHIP8_QUIRK_RESET_VF | CHIP8_QUIRK_VBLANK_WAIT)

#define OPCODE_STR_MAX	18

#define MAX_PROGRAMS 10

#define DOUBLE_WIDTH_OUTPUTx

#define CHIP8_DEBUG_MSG(PROG,...)				\
do {								\
	char *e = (char *)&(PROG)->mem[ERROR_ADDRESS];		\
	int n = snprintf(e+1, ERROR_SIZE, __VA_ARGS__);		\
	if (n < 0 || n >= ERROR_SIZE) {				\
		n = snprintf(e+1, ERROR_SIZE,			\
			"error: %d debug message too long; "	\
			"expected %d, actual %d",		\
			 __LINE__, ERROR_SIZE-1, n);		\
	}							\
	if (n < 0 || n >= ERROR_SIZE) {				\
		*(uint8_t *)e = 0;				\
		*(uint8_t *)(e+1) = 0;				\
	}							\
} while(0);

#define BYTE_TO_BINARY(byte)		\
	((byte) & 0x80 ? '1' : '0'),	\
	((byte) & 0x40 ? '1' : '0'),	\
	((byte) & 0x20 ? '1' : '0'),	\
	((byte) & 0x10 ? '1' : '0'),	\
	((byte) & 0x08 ? '1' : '0'),	\
	((byte) & 0x04 ? '1' : '0'),	\
	((byte) & 0x02 ? '1' : '0'),	\
	((byte) & 0x01 ? '1' : '0')

#define SHORT_TO_BINARY(word)	BYTE_TO_BINARY(((word & 0xFF00)>>8)),BYTE_TO_BINARY(((word & 0xFF)))

#define CHIP8_DEBUG_KEY_STATE(PROG,KEY)						\
do {										\
	CHIP8_DEBUG_MSG((PROG),"keys "						\
		"down: %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c "			\
		"up: %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c",				\
		SHORT_TO_BINARY((KEY).down), SHORT_TO_BINARY((KEY).up));	\
	if (0) {								\
	fprintf(stderr,"keys "							\
		"down: %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c "			\
		"up: %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c\n",			\
		SHORT_TO_BINARY((KEY).down), SHORT_TO_BINARY((KEY).up));	\
	}									\
} while(0)

static uint8_t DemoRandomTimer[] =
{
	0x00, 0xE0, 0xC0, 0x0F, 0xF0, 0x29, 0x61, 0x1C,
	0x62, 0x0E, 0xD1, 0x25, 0x63, 0x1E, 0xF3, 0x15,
	0xF4, 0x07, 0x34, 0x00, 0x12, 0x10, 0xD1, 0x25,
	0x12, 0x02
};

static uint8_t *Demos[] = {
	DemoRandomTimer
};

static size_t DemosSize[] = {
	sizeof DemoRandomTimer
};

enum chip8_quirks
{
	CHIP8_QUIRK_NONE        = 0x00,
	CHIP8_QUIRK_SHIFT_VX    = 0x01, /* 8XY6 and 8XYE use VX for the source of the shift instead of VY */
	CHIP8_QUIRK_JUMP_FROM_X = 0x02, /* BNNN uses VX for jump offset intead of V0 */
	CHIP8_QUIRK_NO_CLIPPING = 0x04, /* DXYN wraps sprite instead of clipping at edges */
	CHIP8_QUIRK_INCREMENT_I = 0x08, /* FX55 and FX65 increments the I address */
	CHIP8_QUIRK_RESET_VF    = 0x10, /* 8XY1, 8XY2 and 8XY3 set VF to zero */
	CHIP8_QUIRK_VBLANK_WAIT = 0x20, /* DXYN a single sprite is drawn per VBLANK */
};

struct chip8_program
{
	uint8_t mem[MEMORY_SIZE];
} __attribute__ ((aligned (2)));

struct chip8_opcode
{
	uint16_t value;
	uint16_t nnn;
	uint8_t nn;
	uint8_t n;
	uint8_t vx;
	uint8_t vy;
};

struct keypad
{
	int64_t time[16];
	uint16_t down;
	uint16_t up;
	bool held;
};

static uint8_t Fonts[FONT_SIZE] = {
	0xF0, 0x90, 0x90, 0x90, 0xF0, /* Font 0 */
	0x20, 0x60, 0x20, 0x20, 0x70, /* Font 1 */
	0xF0, 0x10, 0xF0, 0x80, 0xF0, /* Font 2 */
	0xF0, 0x10, 0xF0, 0x10, 0xF0, /* Font 3 */
	0x90, 0x90, 0xF0, 0x10, 0x10, /* Font 4 */
	0xF0, 0x80, 0xF0, 0x10, 0xF0, /* Font 5 */
	0xF0, 0x80, 0xF0, 0x90, 0xF0, /* Font 6 */
	0xF0, 0x10, 0x20, 0x40, 0x40, /* Font 7 */
	0xF0, 0x90, 0xF0, 0x90, 0xF0, /* Font 8 */
	0xF0, 0x90, 0xF0, 0x10, 0xF0, /* Font 9 */
	0xF0, 0x90, 0xF0, 0x90, 0x90, /* Font A */
	0xE0, 0x90, 0xE0, 0x90, 0xE0, /* Font B */
	0xF0, 0x80, 0x80, 0x80, 0xF0, /* Font C */
	0xE0, 0x90, 0x90, 0x90, 0xE0, /* Font D */
	0xF0, 0x80, 0xF0, 0x80, 0xF0, /* Font E */
	0xF0, 0x80, 0xF0, 0x80, 0x80  /* Font F */
};
static volatile sig_atomic_t Quit = 0;
static volatile sig_atomic_t Stop = 0;
static volatile sig_atomic_t Dump = 0;
static struct chip8_program Programs[MAX_PROGRAMS];

static void
os_write(int fd, char *s, size_t n)
{
	while (n) {
		ssize_t r = write(fd, s, n);
		if (r < 0) {
			return;
		}
		s += r;
		n -= (size_t)r;
	}
}

static void
write_err(char *s, size_t n)
{
	os_write(STDERR_FILENO, s, n);
}

static void
write_str(char *s, size_t n)
{
	os_write(STDOUT_FILENO, s, n);
}

static void
write_byte(char s)
{
	os_write(STDOUT_FILENO, &s, 1);
}

static void
os_beep()
{
	write_byte(07);
}

static const char AnsiEscHome[] = { '\033', '[', 'H' };
static const char EndOfLine[] = { '\r', '\n' };
#ifdef DOUBLE_WIDTH_OUTPUT
static const char PixelOn[]  = { '\033', '[', '9', '2', 'm', 0xE2, 0x96, 0x88, 0xE2, 0x96, 0x88, '\033', '[', '0', 'm' };
static const char PixelOff[] = { '\033', '[', '3', '2', 'm', 0xE2, 0x96, 0x91, 0xE2, 0x96, 0x91, '\033', '[', '0', 'm' };
#else
static const char PixelOn[]  = { '\033', '[', '9', '2', 'm', 0xE2, 0x96, 0x88, '\033', '[', '0', 'm' };
static const char PixelOff[] = { '\033', '[', '3', '2', 'm', 0xE2, 0x96, 0x91, '\033', '[', '0', 'm' };
#endif
static char DrawBuffer[(sizeof AnsiEscHome) + (VIDEO_HEIGHT * VIDEO_WIDTH * (sizeof PixelOn)) + ((sizeof EndOfLine) * VIDEO_HEIGHT)];

static void
os_draw(uint8_t *video)
{
	size_t n = 0;
	for (size_t i = 0; i < sizeof AnsiEscHome; i++) {
		DrawBuffer[n++] = AnsiEscHome[i];
	}
	for (uint8_t y = 0; y < VIDEO_HEIGHT; y++) {
		for (uint8_t x = 0; x < VIDEO_WIDTH; x++) {
			uint8_t byte = video[(y * VIDEO_WIDTH + x) / 8];
			if (byte & (1 << (7 - x % 8))) {
				for (size_t i = 0; i < sizeof PixelOn; i++) {
					DrawBuffer[n++] = PixelOn[i];
				}
			} else {
				for (size_t i = 0; i < sizeof PixelOff; i++) {
					DrawBuffer[n++] = PixelOff[i];
				}
			}
		}
		for (size_t i = 0; i < sizeof EndOfLine; i++) {
			DrawBuffer[n++] = EndOfLine[i];
		}
	}
	write_str(DrawBuffer, n);
}

static bool
os_is_key_pressed()
{
	fd_set fd;
	FD_ZERO(&fd);
	FD_SET(STDIN_FILENO, &fd);
	struct timeval tv = {0, 0};
	return (select(STDIN_FILENO+1, &fd, NULL, NULL, &tv) > 0) && (FD_ISSET(STDIN_FILENO, &fd));
}

static bool
os_read_key(uint8_t *ch)
{
	/*
	 * Key mapping for QWERTY keyboard
	 *  Chip-8 |   KB
	 * -----------------
	 * 1 2 3 C | 1 2 3 4
	 * 4 5 6 D | Q W E R
	 * 7 8 9 E | A S D F
	 * A 0 B F | Z X C V
	 * -----------------
	 */
	unsigned char c;
	if (read(STDIN_FILENO, &c, sizeof c) > 0) {
		switch (c) {
		case '1':           { *ch = 0x01; return true; }
		case '2':           { *ch = 0x02; return true; }
		case '3':           { *ch = 0x03; return true; }
		case '4':           { *ch = 0x0C; return true; }
		case 'q': case 'Q': { *ch = 0x04; return true; }
		case 'w': case 'W': { *ch = 0x05; return true; }
		case 'e': case 'E': { *ch = 0x06; return true; }
		case 'r': case 'R': { *ch = 0x0D; return true; }
		case 'a': case 'A': { *ch = 0x07; return true; }
		case 's': case 'S': { *ch = 0x08; return true; }
		case 'd': case 'D': { *ch = 0x09; return true; }
		case 'f': case 'F': { *ch = 0x0E; return true; }
		case 'z': case 'Z': { *ch = 0x0A; return true; }
		case 'x': case 'X': { *ch = 0x00; return true; }
		case 'c': case 'C': { *ch = 0x0B; return true; }
		case 'v': case 'V': { *ch = 0x0F; return true; }
		}
	}
	return false;
}

static uint16_t
os_read_keys()
{
	uint16_t res = 0;
	int n = 4;
	while (os_is_key_pressed() && n--) {
		uint8_t ch;	
		if (!os_read_key(&ch)) {
			break;
		}
		res |= (1 << ch) & 0xFFFF;
	}
	return res;
}

static void
update_keypad(struct keypad *keypad, int64_t now, int timeout_ms)
{
	uint16_t values = os_read_keys();
	for (uint16_t key = 0; key < 16; key++) {
		uint16_t bit = (1 << key) & 0xFFFF;
		if (values & bit) {
			keypad->down |= bit;
			keypad->up &= ~bit;
			keypad->time[key] = now;
		}
	}
	/* check for key up */
	for (uint16_t key = 0; key < 16; key++) {
		uint16_t bit = (1 << key) & 0xFFFF;
		if ((now - keypad->time[key]) > (INT64_C(1000000) * timeout_ms)) {
			keypad->down &= ~bit;
			keypad->up |= bit;
			keypad->time[key] = now;
		}
	}
}

static int64_t
os_get_time()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

static void
os_wait_frame(int64_t start)
{
	int64_t now = os_get_time();
	int64_t elapsed = now - start;
	int64_t frame_rate = 16666667;
	if (elapsed < frame_rate) {
		struct timespec sleep = { .tv_sec = 0, .tv_nsec = frame_rate - elapsed };
		nanosleep(&sleep, NULL);
	}
}

static struct chip8_opcode
opcode_from_bytes(uint8_t hi, uint8_t lo)
{
	uint16_t opcode = (hi << 8 | lo) & 0xFFFF;
	return (struct chip8_opcode) {
		.value = opcode,
		.vx    = (opcode & 0x0F00) >> 8,
		.vy    = (opcode & 0x00F0) >> 4,
		.nnn   = (opcode & 0x0FFF),
		.nn    = (opcode & 0x00FF),
		.n     = (opcode & 0x000F)
	};
}

static bool
opcode_to_string(char *dst, size_t len, struct chip8_opcode opcode)
{
	switch (opcode.value & 0xF000) {
	case 0x0000:
		switch (opcode.value & 0x0FFF) {
		case 0x0E0: snprintf(dst, len, "cls"); return true;
		case 0x0EE: snprintf(dst, len, "ret"); return true;
		}
		break;
	case 0x1000: snprintf(dst, len, "jp   0x%03x",       opcode.nnn);           return true;
	case 0x2000: snprintf(dst, len, "call 0x%03x",       opcode.nnn);           return true;
	case 0x3000: snprintf(dst, len, "se   %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x4000: snprintf(dst, len, "sne  %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x5000: snprintf(dst, len, "se   %%%x, %%%x",   opcode.vx, opcode.vy); return true;
	case 0x6000: snprintf(dst, len, "ld   %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x7000: snprintf(dst, len, "add  %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x8000:
		switch (opcode.value & 0x000F) {
		case 0x0: snprintf(dst, len, "ld   %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0x1: snprintf(dst, len, "or   %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0x2: snprintf(dst, len, "and  %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0x3: snprintf(dst, len, "xor  %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0x4: snprintf(dst, len, "add  %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0x5: snprintf(dst, len, "sub  %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0x6: snprintf(dst, len, "shr  %%%x",       opcode.vx);            return true;
		case 0x7: snprintf(dst, len, "subn %%%x, %%%x", opcode.vx, opcode.vy); return true;
		case 0xE: snprintf(dst, len, "shl  %%%x",       opcode.vx);            return true;
		}
		break;
	case 0x9000: snprintf(dst, len, "sne  %%%x, %%%x",   opcode.vx, opcode.vy); return true;
	case 0xA000: snprintf(dst, len, "ld   %%i, 0x%03x",  opcode.nnn);           return true;
	case 0xB000: snprintf(dst, len, "jp   %%9, 0x%03x",  opcode.nnn);           return true;
	case 0xC000: snprintf(dst, len, "rnd  %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0xD000: snprintf(dst, len, "drw  %%%x, %%%x, 0x%02x", opcode.vx, opcode.vy, opcode.n); return true;
	case 0xE000:
		switch (opcode.value & 0x00FF) {
		case 0x9E: snprintf(dst, len, "skp  %%%x", opcode.vx); return true;
		case 0xA1: snprintf(dst, len, "skpn %%%x", opcode.vx); return true;
		}
		break;
	case 0xF000:
		switch (opcode.value & 0x00FF) {
		case 0x07: snprintf(dst, len, "ld   %%%x, $dt", opcode.vx); return true;
		case 0x0A: snprintf(dst, len, "ld   %%%x, $kb", opcode.vx); return true;
		case 0x15: snprintf(dst, len, "ld   $dt, %%%x", opcode.vx); return true;
		case 0x18: snprintf(dst, len, "ld   $st, %%%x", opcode.vx); return true;
		case 0x1E: snprintf(dst, len, "add  %%i, %%%x", opcode.vx); return true;
		case 0x29: snprintf(dst, len, "fnt  %%%x",      opcode.vx); return true;
		case 0x33: snprintf(dst, len, "bcd  %%%x",      opcode.vx); return true;
		case 0x55: snprintf(dst, len, "ld   %%i, %%%x", opcode.vx); return true;
		case 0x65: snprintf(dst, len, "ld   %%%x, %%i", opcode.vx); return true;
		}
		break;
	}
	return false;
}

static void
chip8_dump(FILE *dst, struct chip8_program *program, bool full)
{
	uint8_t *mem = program->mem;
	uint16_t len = *(uint16_t *)&mem[SIZE_ADDRESS];
	if (full) {
		uint16_t *addr  = (uint16_t *)&mem[I_ADDRESS];
		uint16_t *pc    = (uint16_t *)&mem[PC_ADDRESS];
		uint16_t *pcprev= (uint16_t *)&mem[PCPREV_ADDRESS];
		uint16_t *stack = (uint16_t *)&mem[STACK_ADDRESS];
		uint16_t *acc   = (uint16_t *)&mem[ACC_ADDRESS];
		uint8_t  *delay = &mem[DELAY_ADDRESS];
		uint8_t  *v     = &mem[V_ADDRESS];
		uint8_t  *sp    = &mem[SP_ADDRESS];
		uint8_t  *sound = &mem[SOUND_ADDRESS];
		char     *error = (char *)&mem[ERROR_ADDRESS];
		fprintf(dst, "Name      Addr   Value\n");
		fprintf(dst, "PC        0x%03X  0x%03X\n", PC_ADDRESS, *pc);
		fprintf(dst, "PC Prev   0x%03X  0x%03X\n", PCPREV_ADDRESS, *pcprev);
		fprintf(dst, "I         0x%03X  0x%03X 0x%03X\n", I_ADDRESS, *addr, mem[*addr]);
		fprintf(dst, "Size      0x%03X  0x%03X\n", SIZE_ADDRESS, len);
		fprintf(dst, "Accum     0x%03X  0x%04X\n", ACC_ADDRESS, *acc);
		fprintf(dst, "SP        0x%03X  0x%03X\n", SP_ADDRESS, *sp);
		fprintf(dst, "Delay     0x%03X  0x%02X\n", DELAY_ADDRESS, *delay);
		fprintf(dst, "Sound     0x%03X  0x%02X\n", SOUND_ADDRESS, *sound);
		fprintf(dst, "V         0x%03X  "
			"0:%02X 1:%02X 2:%02X 3:%02X 4:%02X 5:%02X 6:%02X 7:%02X 8:%02X 9:%02X A:%02X B:%02X C:%02X D:%02X E:%02X F:%02X\n",
			V_ADDRESS, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
		fprintf(dst, "Error     0x%03X  %s\n", ERROR_ADDRESS, error+1);
		fprintf(dst, "Flag      0x%03lX  0x%02X\n", &v[FLAG_REG] - mem, v[FLAG_REG]);
		fprintf(dst, "Font      0x%03X\n", FONT_ADDRESS);
		fprintf(dst, "Stack     0x%03X  ", STACK_ADDRESS);
		for (size_t i = 0; i < STACK_COUNT; i++) {
			fprintf(dst, "0x%04X", stack[i]);
			if (i < (STACK_COUNT-1)) {
				fprintf(dst, ", ");
			}
		}
		fprintf(dst, "\n");
		fprintf(dst, "Display   0x%03X\n", VIDEO_ADDRESS);
		struct chip8_opcode opcode;
		if (*pc >= PROG_START && *pc < PROG_END) {
			opcode = opcode_from_bytes(mem[*pc], mem[*(pc)+1]);
			fprintf(dst, "Opcode    0x%03X  0x%04X VX:0x%02X VY:0x%02X N:0x%X NN:0x%02X NNN:0x%03X\n",
				*pc, opcode.value, opcode.vx, opcode.vy, opcode.n, opcode.nn, opcode.nnn);
		} else {
			fprintf(dst, "Opcode    NA\n");
		}
		fprintf(dst, "\n");
	}
	if (full) {
		fprintf(dst, "     0 1  2 3  4 5  6 7  8 9  A B  C D  E F\n");
	}
	ptrdiff_t packidx = 0;
	ptrdiff_t packmin = PTRDIFF_MAX;
	ptrdiff_t packmax = full ? 16 : 1;
	uint8_t *code_beg = &mem[PROG_ADDRESS];
	uint8_t *beg = full ? mem : code_beg;
	uint8_t *end = full ? (beg + MEMORY_SIZE) : (code_beg + len);
	uint8_t *cur = beg;
	while (cur < end) {
		ptrdiff_t offset = cur - mem;
		char str[OPCODE_STR_MAX];
		uint8_t *next = ((cur+1) < end) ? cur+1 : NULL;
		bool is_opcode = (cur >= code_beg) && (cur < (mem + PROG_END)) && next &&
			         opcode_to_string(str, sizeof str, opcode_from_bytes(*cur, *next));
		if (is_opcode) {
			if (packidx) {
				fprintf(dst, "\n");
				packidx = 0;
			}
			fprintf(dst, "%03zx: %02x%02x %s\n", offset, *cur, *next, str);
			cur += 2;
		} else {
			if (packidx >= packmax || packidx >= packmin) {
				fprintf(dst, "\n");
				packidx = 0;
			}

			if (!packidx) {
				fprintf(dst, "%03zx: ", offset);
				if (offset % packmax == 0) {
					packmin = PTRDIFF_MAX;
				} else {
					ptrdiff_t nextalign = offset + packmax - (offset % packmax);
					packmin = nextalign - offset;
				}
			}
			else if (!(packidx & 1)) {
				fprintf(dst, " ");
			}

			if (*cur || !full) {
				fprintf(dst, "%02x", *cur);
			} else {
				fprintf(dst, "..");
			}

			packidx++;
			cur++;
		}
	}
	if (packidx) {
		fprintf(dst, "\n");
	}
	fprintf(dst, "\n");
}

static void
chip8_error(struct chip8_program *program)
{
	char *err = (char *)&program->mem[ERROR_ADDRESS];
	char *str = err + 1;
	unsigned char n = *(unsigned char *)err;
	if (n) {
		write_err(str, n);
	}
}

static void
chip8_exec(struct chip8_program *program, int ops_per_frame, int keypad_delay, enum chip8_quirks quirks)
{
	uint8_t  *mem   = program->mem;
	uint16_t *addr  = (uint16_t *)&mem[I_ADDRESS];
	uint16_t *pc    = (uint16_t *)&mem[PC_ADDRESS];
	uint16_t *pcprev= (uint16_t *)&mem[PCPREV_ADDRESS];
	uint16_t *stack = (uint16_t *)&mem[STACK_ADDRESS];
	uint16_t *acc   = (uint16_t *)&mem[ACC_ADDRESS];
	uint8_t  *video = &mem[VIDEO_ADDRESS];
	uint8_t  *delay = &mem[DELAY_ADDRESS];
	uint8_t  *v     = &mem[V_ADDRESS];
	uint8_t  *sp    = &mem[SP_ADDRESS];
	uint8_t  *sound = &mem[SOUND_ADDRESS];
	struct keypad keypad = { .time = {0}, .down = 0, .up = 0xFFFF, .held = false };

	for (;;) {
		if (Dump) {
			chip8_dump(stderr, program, true);
			Dump = 0;
		}
		if (Stop) {
			Stop = 0;
			break;
		}

		int64_t time_now = os_get_time();
		bool vblank_wait = false;

		for (int i = 0; i < ops_per_frame; i++) {
			if (*pc < PROG_START || *pc >= PROG_END) {
				CHIP8_DEBUG_MSG(program, "error: pc overflow (0x%03hX)\n", *pc);
				Stop = 1;
				break;
			}
			*pcprev = *pc;

			struct chip8_opcode opcode = opcode_from_bytes(mem[*pc], mem[(*pc)+1]);
			switch (opcode.value & 0xF000) {
			case 0x0000:
				switch (opcode.value & 0xFF) {
				case 0xE0:
					memset(video, 0, VIDEO_BYTES);
					*pc += 2;
					break;
				case 0xEE:
					if ((*sp-1) >= STACK_COUNT) {
						CHIP8_DEBUG_MSG(program, "error: stack overflow (0x%03x) pc (0x%03x)", *sp-1, *pc);
						Stop = 1;
						break;
					}
					--*sp;
					*pc = stack[*sp];
					break;
				default:
					/* advance over NOP and ignore RCA 1802 subroutines (0NNN) */
					*pc += 2;
					break;
				}
				break;
			case 0x1000:
				*pc = opcode.nnn;
				break;
			case 0x2000:
				if (*sp >= STACK_COUNT) {
					CHIP8_DEBUG_MSG(program, "error: stack overflow (0x%03x) pc (0x%03x)", *sp, *pc);
					Stop = 1;
					break;
				}
				stack[*sp] = *pc + 2;
				++*sp;
				*pc = opcode.nnn;
				break;
			case 0x3000:
				*pc += v[opcode.vx] == opcode.nn ? 4 : 2;
				break;
			case 0x4000:
				*pc += v[opcode.vx] != opcode.nn ? 4 : 2;
				break;
			case 0x5000:
				*pc += v[opcode.vx] == v[opcode.vy] ? 4 : 2;
				break;
			case 0x6000:
				v[opcode.vx] = opcode.nn;
				*pc += 2;
				break;
			case 0x7000:
				v[opcode.vx] += opcode.nn;
				*pc += 2;
				break;
			case 0x8000:
				switch (opcode.value & 0xF) {
				case 0x0:
					v[opcode.vx] = v[opcode.vy];
					*pc += 2;
					break;
				case 0x1:
					v[opcode.vx] |= v[opcode.vy];
					if (quirks & CHIP8_QUIRK_RESET_VF) {
						v[FLAG_REG] = 0;
					}
					*pc += 2;
					break;
				case 0x2:
					v[opcode.vx] &= v[opcode.vy];
					if (quirks & CHIP8_QUIRK_RESET_VF) {
						v[FLAG_REG] = 0;
					}
					*pc += 2;
					break;
				case 0x3:
					v[opcode.vx] ^= v[opcode.vy];
					if (quirks & CHIP8_QUIRK_RESET_VF) {
						v[FLAG_REG] = 0;
					}
					*pc += 2;
					break;
				case 0x4:
					*acc = v[opcode.vx] + v[opcode.vy];
					v[opcode.vx] = *acc & 0xFF;
					/* flag is 1 on overflow */
					v[FLAG_REG] = !!(*acc & 0xFF00);
					*pc += 2;
					break;
				case 0x5:
					*acc = v[opcode.vx] - v[opcode.vy];
					v[opcode.vx] = *acc & 0xFF;
					/* flag is 1 on no borrow */
					v[FLAG_REG] = !((*acc & 0x8000) >> 15);
					*pc += 2;
					break;
				case 0x6:
					*acc = (quirks & CHIP8_QUIRK_SHIFT_VX) ? v[opcode.vx] : v[opcode.vy];
					v[opcode.vx] = (*acc >> 1) & 0xFF;
					v[FLAG_REG]  = *acc & 1;
					*pc += 2;
					break;
				case 0x7:
					*acc = v[opcode.vy] - v[opcode.vx];
					v[opcode.vx] = *acc & 0xFF;
					/* flag is 1 on no borrow */
					v[FLAG_REG] = !((*acc & 0x8000) >> 15);
					*pc += 2;
					break;
				case 0xE:
					*acc = (quirks & CHIP8_QUIRK_SHIFT_VX) ? v[opcode.vx] : v[opcode.vy];
					v[opcode.vx] = (*acc << 1) & 0xFF;
					v[FLAG_REG]  = (*acc & 0x80) >> 7;
					*pc += 2;
					break;
				}
				break;
			case 0x9000:
				*pc += v[opcode.vx] != v[opcode.vy] ? 4 : 2;
				break;
			case 0xA000:
				*addr = opcode.nnn;
				*pc += 2;
				break;
			case 0xB000:
				if (quirks & CHIP8_QUIRK_JUMP_FROM_X) {
					*pc = opcode.nnn + v[opcode.vx];
				} else {
					*pc = opcode.nnn + v[0];
				}
				break;
			case 0xC000:
				v[opcode.vx] = (rand() % 256) & opcode.nn;
				*pc += 2;
				break;
			case 0xD000:
				uint8_t x0 = v[opcode.vx] % VIDEO_WIDTH;
				uint8_t y0 = v[opcode.vy] % VIDEO_HEIGHT;
				v[FLAG_REG] = 0;
				for (uint8_t y = 0; y < opcode.n; y++) {
					uint8_t yc = y0 + y;
					if (yc >= VIDEO_HEIGHT) {
						if (quirks & CHIP8_QUIRK_NO_CLIPPING) {
							yc %= VIDEO_HEIGHT;
						} else {
							break;
						}
					}
					uint8_t sprite = mem[(*addr + y) & 0xFFF];
					for (uint8_t sprite_mask = 1 << 7, x = 0; sprite_mask != 0; sprite_mask >>= 1, x++) {
						if (!(sprite & sprite_mask)) {
							continue;
						}
						uint8_t xc = x0 + x;
						if (xc >= VIDEO_WIDTH) {
							if (quirks & CHIP8_QUIRK_NO_CLIPPING) {
								xc %= VIDEO_WIDTH;
							} else {
								break;
							}
						}
						uint16_t byte = (yc * VIDEO_WIDTH + xc) / 8;
						uint8_t mask = (1 << (7 - xc % 8)) & 0xFF;
						v[FLAG_REG] |= !!(video[byte] & mask);
						video[byte] ^= mask;
					}
				}
				*pc += 2;
				if (quirks & CHIP8_QUIRK_VBLANK_WAIT) {
					vblank_wait = true;
				}
				break;
			case 0xE000:
				switch (opcode.value & 0xFF) {
				case 0x9E:
					*pc += (keypad.down & (1 << (v[opcode.vx] & 0xF))) ? 4 : 2;
					break;
				case 0xA1:
					*pc += (keypad.down & (1 << (v[opcode.vx] & 0xF))) ? 2 : 4;
					break;
				}
				break;
			case 0xF000:
				switch (opcode.value & 0xFF) {
				case 0x07:
					v[opcode.vx] = *delay;
					*pc += 2;
					break;
				case 0x0A:
					if (keypad.held) {
						if (keypad.up & (1 << (v[opcode.vx] & 0xF))) {
							keypad.held = false;
							*pc += 2;
						}
					} else if (keypad.down) {
						v[opcode.vx] = __builtin_ctz(keypad.down) & 0xF;
						keypad.held = true;
					}
					break;
				case 0x15:
					*delay = v[opcode.vx];
					*pc += 2;
					break;
				case 0x18:
					*sound = v[opcode.vx];
					*pc += 2;
					break;
				case 0x1E:
					*addr = (*addr + v[opcode.vx]) & 0xFFF;
					*pc += 2;
					break;
				case 0x29:
					*addr = (FONT_ADDRESS + (v[opcode.vx] & 0xF) * FONT_WIDTH) & 0xFFF;
					*pc += 2;
					break;
				case 0x33:
					mem[(*addr + 0) & 0xFFF] = v[opcode.vx] / 100;
					mem[(*addr + 1) & 0xFFF] = v[opcode.vx] / 10 % 10;
					mem[(*addr + 2) & 0xFFF] = v[opcode.vx] % 10;
					*pc += 2;
					break;
				case 0x55:
					for (uint8_t x = 0; x <= opcode.vx; x++) {
						mem[(*addr + x) & 0xFFF] = v[x];
					}
					if (quirks & CHIP8_QUIRK_INCREMENT_I) {
						*addr = (*addr + opcode.vx + 1) & 0xFFF;
					}
					*pc += 2;
					break;
				case 0x65:
					for (uint8_t x = 0; x <= opcode.vx; x++) {
						v[x] = mem[(*addr + x) & 0xFFF];
					}
					if (quirks & CHIP8_QUIRK_INCREMENT_I) {
						*addr = (*addr + opcode.vx + 1) & 0xFFF;
					}
					*pc += 2;
					break;
				}
				break;
			}

			if (*pcprev == *pc) {
				bool wait = (opcode.value & 0xF000) == 0xF000 && opcode.nn == 0x0A;
				bool halt = (opcode.value & 0xF000) == 0x1000 && opcode.nnn == *pc;
				if (!(wait || halt)) {
					CHIP8_DEBUG_MSG(program, "error: pc did not advance from 0x%03X; opcode 0x%04X", *pc, opcode.value);
					Dump = 1;
					Stop = 1;
				}
				break;
			}

			if (vblank_wait) {
				break;
			}
		}

		if (*delay) {
			--*delay;
		}

		if (*sound) {
			os_beep();
			--*sound;
		}

		os_wait_frame(time_now);

		os_draw(video);

		update_keypad(&keypad, time_now, keypad_delay);
		if (!Stop) {
			CHIP8_DEBUG_KEY_STATE(program, keypad);
		}
	}
}

static bool
chip8_init(struct chip8_program *dst, uint8_t *src, size_t size)
{
	if (size > PROG_MAX) {
		return false;
	}
	memset(dst, 0, sizeof *dst);
	memcpy(&dst->mem[PROG_ADDRESS], src, size);
	memcpy(&dst->mem[FONT_ADDRESS], Fonts, sizeof Fonts);
	*(uint16_t *)&dst->mem[SIZE_ADDRESS] = (uint16_t)size;
	*(uint16_t *)&dst->mem[PC_ADDRESS] = PROG_ADDRESS;
	return true;
}

static void
os_signal_handler(int signal)
{
	if (signal == SIGINT || signal == SIGTERM) {
		Stop = 1;
	} else if (signal == SIGQUIT) {
		Dump = 1;
		Stop = 1;
		Quit = 1;
	} else if (signal == SIGHUP) {
		Dump = 1;
	}
}

static void
os_init(void *state)
{
	signal(SIGHUP, os_signal_handler);
	signal(SIGINT, os_signal_handler);
	signal(SIGQUIT, os_signal_handler);
	signal(SIGTERM, os_signal_handler);

	char *s = "\033[?25l\033[2J\033[H";
	write_str(s, (sizeof s)-1);

	struct termios prev;
	tcgetattr(STDIN_FILENO, &prev);
	struct termios termios = prev;
	termios.c_lflag &= (tcflag_t)(~(ICANON | ECHO));
	termios.c_cc[VMIN] = 0;
	termios.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios);
	*(struct termios *)state = prev;
}

static void
os_term(void *termios)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, (struct termios *)termios);
	char *s = "\033[H\033[2J\033[?25h";
	write_str(s, (sizeof s)-1);
}

static size_t
load_file(char **files, size_t nfiles)
{
	uint8_t tmp[PROG_MAX+1];
	size_t i;
	for (i = 0; i < nfiles; i++) {
		FILE *file = fopen(files[i], "rb");
		if (!file) {
			fprintf(stderr, "error: cannot open program %s\n", files[i]);
			break;
		}

		size_t nread = fread(tmp, 1, sizeof tmp, file);
		int eof = feof(file);
		fclose(file);
		if (!(eof && nread)) {
			fprintf(stderr, "error: cannot read program %s\n", files[i]);
			break;
		}

		if (!chip8_init(&Programs[i], tmp, nread)) {
			fprintf(stderr, "error: cannot load program %s\n", files[i]);
			break;
		}
	}
	return i;
}

static size_t
load_builtin()
{
	size_t i;
	for (i = 0; i < (sizeof Demos / sizeof Demos[0]); i++) {
		if (!chip8_init(&Programs[i], Demos[i], DemosSize[i])) {
			fprintf(stderr, "error: cannot load program %zu\n", i);
			break;
		}
	}
	return i;
}

int
main(int argc, char **argv)
{
	bool disasm_and_quit = false;

	setlocale(LC_ALL, "en_US.UTF-8");
	srand((unsigned int)time(NULL));

	size_t num_progs = 0;
	--argc;
	++argv;
	if (argc) {
		if (strcmp(argv[argc-1], "-disasm") == 0) {
			disasm_and_quit = true;
			--argc;
		}
	}
	if (argc) {
		num_progs = load_file(argv, MIN((size_t)argc,MAX_PROGRAMS));
	} else {
		num_progs = load_builtin();
	}

	if (disasm_and_quit) {
		for (size_t i = 0; i < num_progs; i++) {
			chip8_dump(stderr, &Programs[i], false);
			if (i < (num_progs-1)) {
				fprintf(stderr, "\n");
			}
		}
		return 0;
	}

	if (!num_progs) {
		return 1;
	}

	struct termios old_state;
	os_init(&old_state);

	for (size_t i = 0; i < num_progs; i++) {
		chip8_exec(&Programs[i], 10, 30, CHIP8_QUIRK_SHIFT_VX);
		chip8_error(&Programs[i]);
		if (Quit) {
			break;
		}
	}

	os_term(&old_state);

	return 0;
}

