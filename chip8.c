#include <errno.h>
#include <limits.h>
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

#define PROGRAM_MAX_SIZE (0xEA0 - 0x200)
#define STACK_MAX_SIZE   32

static uint8_t DemoRandomTimer[] =
{
	0x00, 0xE0, 0xC0, 0x0F, 0xF0, 0x29, 0x61, 0x1C,
	0x62, 0x0E, 0xD1, 0x25, 0x63, 0x1E, 0xF3, 0x15,
	0xF4, 0x07, 0x34, 0x00, 0x12, 0x10, 0xD1, 0x25,
	0x12, 0x02
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
	CHIP8_QUIRK_ORIGINAL    = CHIP8_QUIRK_INCREMENT_I | CHIP8_QUIRK_RESET_VF | CHIP8_QUIRK_VBLANK_WAIT
};

struct chip8_program
{
	uint16_t pc;
	uint16_t sp;
	uint16_t stack;
	uint16_t i;
	uint16_t v;
	uint16_t bm;
	uint16_t len;
	uint8_t sound;
	uint8_t timer;
	uint8_t mem[0x1000];
};

struct chip8_context
{
	struct chip8_program *program;
	int opcodes_per_frame;
	int keypad_response_time;
	enum chip8_quirks quirks;
};

struct chip8_opcode
{
	uint16_t nnn;
	uint8_t nn;
	uint8_t n;
	uint8_t vx;
	uint8_t vy;
	uint8_t group;
};

struct keypad
{
	int64_t time[16];
	uint16_t down;
	uint16_t up;
	uint8_t held_key; /* UCHAR_MAX = not waiting, 0..15 = waiting for release of this key */
	int64_t held_key_time; /* timestamp when held_key was first pressed */
};

static uint8_t Fonts[] = {
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
static volatile sig_atomic_t Stop = 0;
static volatile sig_atomic_t Dump = 0;

static void
os_write(int fd, char *s, size_t n)
{
	while (n) {
		ssize_t r = write(fd, s, n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		s += r;
		n -= (size_t)r;
	}
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
os_beep(void)
{
	write_byte(07);
}

#define PIXEL_SIZE	3
#define BITMAP_STRIDE	(64 * PIXEL_SIZE + 2)
static char BitmapDest[8 + 32 * BITMAP_STRIDE + 4];

static void
os_bit_blit(uint8_t *src)
{
	char *dst = BitmapDest;
	*dst++ = '\033';
	*dst++ = '[';
	*dst++ = 'H';
	*dst++ = '\033';
	*dst++ = '[';
	*dst++ = '9';
	*dst++ = '2';
	*dst++ = 'm';
	for (unsigned y = 0; y < 32; y++) {
		for (unsigned x = 0; x < 64; x++) {
			*dst++ = 0xE2;
			*dst++ = 0x96;
			unsigned byte = src[(y * 64 + x) / 8];
			unsigned bit = 1 << (7 - x % 8);
			*dst++ = (byte & bit) ? 0x88 : 0x91;
		}
		*dst++ = '\r';
		*dst++ = '\n';
	}
	*dst++ = '\033';
	*dst++ = '[';
	*dst++ = '0';
	*dst++ = 'm';
	write_str(BitmapDest, sizeof BitmapDest);
}

static bool
os_is_key_pressed(void)
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
os_read_keys(void)
{
	uint16_t res = 0;
	while (os_is_key_pressed()) {
		uint8_t ch;
		if (os_read_key(&ch)) {
			res |= (1 << ch) & 0xFFFF;
		}
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
os_get_time(void)
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
		.group = (opcode & 0xF000) >> 12,
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
	switch (opcode.group) {
	case 0x0:
		switch (opcode.nnn) {
		case 0xE0: snprintf(dst, len, "cls"); return true;
		case 0xEE: snprintf(dst, len, "ret"); return true;
		}
		break;
	case 0x1: snprintf(dst, len, "jp   0x%03x",       opcode.nnn);           return true;
	case 0x2: snprintf(dst, len, "call 0x%03x",       opcode.nnn);           return true;
	case 0x3: snprintf(dst, len, "se   %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x4: snprintf(dst, len, "sne  %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x5: snprintf(dst, len, "se   %%%x, %%%x",   opcode.vx, opcode.vy); return true;
	case 0x6: snprintf(dst, len, "ld   %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x7: snprintf(dst, len, "add  %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0x8:
		switch (opcode.n) {
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
	case 0x9: snprintf(dst, len, "sne  %%%x, %%%x",   opcode.vx, opcode.vy); return true;
	case 0xA: snprintf(dst, len, "ld   %%i, 0x%03x",  opcode.nnn);           return true;
	case 0xB: snprintf(dst, len, "jp   %%0, 0x%03x",  opcode.nnn);           return true;
	case 0xC: snprintf(dst, len, "rnd  %%%x, 0x%02x", opcode.vx, opcode.nn); return true;
	case 0xD: snprintf(dst, len, "drw  %%%x, %%%x, 0x%02x", opcode.vx, opcode.vy, opcode.n); return true;
	case 0xE:
		switch (opcode.nn) {
		case 0x9E: snprintf(dst, len, "skp  %%%x", opcode.vx); return true;
		case 0xA1: snprintf(dst, len, "skpn %%%x", opcode.vx); return true;
		}
		break;
	case 0xF:
		switch (opcode.nn) {
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
	if (full) {
		uint8_t *stack = &mem[program->stack];
		uint8_t *v = &mem[program->v];
		fprintf(dst, "PC       0x%03X\n", program->pc);
		fprintf(dst, "I        0x%03X\n", program->i);
		fprintf(dst, "SP       0x%03X\n", program->sp);
		fprintf(dst, "Timer    0x%02X\n", program->timer);
		fprintf(dst, "Sound    0x%02X\n", program->sound);
		fprintf(dst, "V        0x%03X  "
			"0:%02X 1:%02X 2:%02X 3:%02X 4:%02X 5:%02X 6:%02X 7:%02X 8:%02X 9:%02X A:%02X B:%02X C:%02X D:%02X E:%02X F:%02X\n",
			program->v, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
		fprintf(dst, "Stack    0x%03X  ", program->stack);
		for (size_t i = 0; i < 32; i+=2) {
			fprintf(dst, "0x%03X", stack[i] << 8 | stack[i+1]);
			if (i < 30) {
				fprintf(dst, ", ");
			}
		}
		fprintf(dst, "\n");
		fprintf(dst, "Bitmap   0x%03X\n", program->bm);
		struct chip8_opcode opcode = opcode_from_bytes(mem[program->pc], mem[program->pc+1]);
		fprintf(dst, "Opcode   0x%03X Group:0x%01X VX:0x%02X VY:0x%02X N:0x%X NN:0x%02X NNN:0x%03X\n",
			mem[program->pc] << 8 | mem[program->pc+1], opcode.group, opcode.vx, opcode.vy, opcode.n, opcode.nn, opcode.nnn);
		fprintf(dst, "\n");
	}
	if (full) {
		fprintf(dst, "     0 1  2 3  4 5  6 7  8 9  A B  C D  E F\n");
	}
	ptrdiff_t packidx = 0;
	ptrdiff_t packmin = PTRDIFF_MAX;
	ptrdiff_t packmax = full ? 16 : 1;
	uint8_t *code_beg = &mem[full ? 0x1FC : 0x200];
	uint8_t *beg = full ? mem : code_beg;
	uint8_t *end = full ? (mem + (sizeof program->mem)) : (code_beg + program->len);
	uint8_t *cur = beg;
	while (cur < end) {
		ptrdiff_t offset = cur - mem;
		char str[18];
		uint8_t *next = ((cur+1) < end) ? cur+1 : NULL;
		/* some docs say opcodes start on even addresses but ROMs, such as INVADERS, start with a
		 * jump to an odd address. need to fix this to parse the opcodes for jumps/call and support
		 * odd addresses
		 */
		bool is_opcode = (cur >= code_beg) && (cur < (mem + 0xE9F)) && next && /*!((uintptr_t)cur & 1) &&*/
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
chip8_exec(struct chip8_context *context)
{
	struct chip8_program *program = context->program;
	enum chip8_quirks quirks = context->quirks;
	uint8_t *mem = program->mem;
	uint8_t *stack = &mem[program->stack];
	uint8_t *bitmap = &mem[program->bm];
	uint8_t *v = &mem[program->v];
	struct keypad keypad = { .time = {0}, .down = 0, .up = 0xFFFF, .held_key = UCHAR_MAX, .held_key_time = 0 };
	uint16_t last_pc;
	uint16_t temp;
	int64_t timer_last = os_get_time();
	int64_t timer_accumulator = 0;

	for (;;) {
		if (Dump) {
			chip8_dump(stderr, program, true);
			Dump = 0;
		}
		if (Stop) {
			break;
		}

		int64_t time_now = os_get_time();
		bool sprite_drawn = false;

		for (int i = 0; i < context->opcodes_per_frame; i++) {
			last_pc = program->pc;

			if (program->pc < 0x1FC || program->pc + 1 > 0xE9F) {
				Dump = 1;
				Stop = 1;
				break;
			}

			struct chip8_opcode opcode = opcode_from_bytes(mem[program->pc], mem[program->pc+1]);
			switch (opcode.group) {
			case 0x0:
				switch (opcode.nnn) {
				case 0xE0:
					memset(&mem[program->bm], 0, 256);
					program->pc += 2;
					break;
				case 0xEE:
					if (program->sp < 2) {
						Dump = 1;
						Stop = 1;
						break;
					}
					program->pc = (stack[program->sp-2] << 8 | stack[program->sp-1]) & 0xFFFF;
					program->sp -= 2;
					break;
				default:
					/* RCA 1802 subroutines (0NNN) */
					program->pc += 2;
					break;
				}
				break;
			case 0x1:
				program->pc = opcode.nnn;
				break;
			case 0x2:
				if (program->sp + 2 > STACK_MAX_SIZE) {
					Dump = 1;
					Stop = 1;
					break;
				}
				stack[program->sp + 0] = ((program->pc + 2) >> 8);
				stack[program->sp + 1] = ((program->pc + 2) & 0xFF);
				program->sp += 2;
				program->pc = opcode.nnn;
				break;
			case 0x3:
				program->pc += v[opcode.vx] == opcode.nn ? 4 : 2;
				break;
			case 0x4:
				program->pc += v[opcode.vx] != opcode.nn ? 4 : 2;
				break;
			case 0x5:
				program->pc += v[opcode.vx] == v[opcode.vy] ? 4 : 2;
				break;
			case 0x6:
				v[opcode.vx] = opcode.nn;
				program->pc += 2;
				break;
			case 0x7:
				v[opcode.vx] += opcode.nn;
				program->pc += 2;
				break;
			case 0x8:
				switch (opcode.n) {
				case 0x0:
					v[opcode.vx] = v[opcode.vy];
					program->pc += 2;
					break;
				case 0x1:
					v[opcode.vx] |= v[opcode.vy];
					if (quirks & CHIP8_QUIRK_RESET_VF) {
						v[0xF] = 0;
					}
					program->pc += 2;
					break;
				case 0x2:
					v[opcode.vx] &= v[opcode.vy];
					if (quirks & CHIP8_QUIRK_RESET_VF) {
						v[0xF] = 0;
					}
					program->pc += 2;
					break;
				case 0x3:
					v[opcode.vx] ^= v[opcode.vy];
					if (quirks & CHIP8_QUIRK_RESET_VF) {
						v[0xF] = 0;
					}
					program->pc += 2;
					break;
				case 0x4:
					temp = v[opcode.vx] + v[opcode.vy];
					v[opcode.vx] = temp & 0xFF;
					/* flag is 1 on overflow */
					v[0xF] = !!(temp & 0xFF00);
					program->pc += 2;
					break;
				case 0x5:
					temp = v[opcode.vx] - v[opcode.vy];
					v[opcode.vx] = temp & 0xFF;
					/* flag is 1 on no borrow */
					v[0xF] = !((temp & 0x8000) >> 15);
					program->pc += 2;
					break;
				case 0x6:
					temp = (quirks & CHIP8_QUIRK_SHIFT_VX) ? v[opcode.vx] : v[opcode.vy];
					v[opcode.vx] = (temp >> 1) & 0xFF;
					v[0xF] = temp & 1;
					program->pc += 2;
					break;
				case 0x7:
					temp = v[opcode.vy] - v[opcode.vx];
					v[opcode.vx] = temp & 0xFF;
					/* flag is 1 on no borrow */
					v[0xF] = !((temp & 0x8000) >> 15);
					program->pc += 2;
					break;
				case 0xE:
					temp = (quirks & CHIP8_QUIRK_SHIFT_VX) ? v[opcode.vx] : v[opcode.vy];
					v[opcode.vx] = (temp << 1) & 0xFF;
					v[0xF] = (temp & 0x80) >> 7;
					program->pc += 2;
					break;
				}
				break;
			case 0x9:
				program->pc += v[opcode.vx] != v[opcode.vy] ? 4 : 2;
				break;
			case 0xA:
				program->i = opcode.nnn;
				program->pc += 2;
				break;
			case 0xB:
				if (quirks & CHIP8_QUIRK_JUMP_FROM_X) {
					program->pc = opcode.nnn + v[opcode.vx];
				} else {
					program->pc = opcode.nnn + v[0];
				}
				break;
			case 0xC:
				v[opcode.vx] = arc4random_uniform(256) & opcode.nn;
				program->pc += 2;
				break;
			case 0xD: {
				uint8_t x0 = v[opcode.vx] % 64;
				uint8_t y0 = v[opcode.vy] % 32;
				v[0xF] = 0;
				for (uint8_t y = 0; y < opcode.n; y++) {
					uint8_t yc = y0 + y;
					if (yc >= 32) {
						if (quirks & CHIP8_QUIRK_NO_CLIPPING) {
							yc %= 32;
						} else {
							break;
						}
					}
					uint8_t sprite = mem[(program->i + y) & 0xFFF];
					for (uint8_t sprite_mask = 1 << 7, x = 0; sprite_mask != 0; sprite_mask >>= 1, x++) {
						if (!(sprite & sprite_mask)) {
							continue;
						}
						uint8_t xc = x0 + x;
						if (xc >= 64) {
							if (quirks & CHIP8_QUIRK_NO_CLIPPING) {
								xc %= 64;
							} else {
								break;
							}
						}
						uint16_t byte = (yc * 64 + xc) / 8;
						uint8_t byte_mask = (1 << (7 - xc % 8)) & 0xFF;
						v[0xF] |= !!(bitmap[byte] & byte_mask);
						bitmap[byte] ^= byte_mask;
						sprite_drawn = true;
					}
				}
				program->pc += 2;
				break;
			}
			case 0xE:
				switch (opcode.nn) {
				case 0x9E:
					program->pc += (keypad.down & (1 << (v[opcode.vx] & 0xF))) ? 4 : 2;
					break;
				case 0xA1:
					program->pc += (keypad.down & (1 << (v[opcode.vx] & 0xF))) ? 2 : 4;
					break;
				}
				break;
			case 0xF:
				switch (opcode.nn) {
				case 0x07:
					v[opcode.vx] = program->timer;
					program->pc += 2;
					break;
				case 0x0A:
					if (keypad.held_key != UCHAR_MAX) {
						if (keypad.down & (1 << keypad.held_key)) {
							keypad.held_key_time = time_now;
						} else if (time_now - keypad.held_key_time > INT64_C(1000000) * context->keypad_response_time) {
							keypad.held_key = UCHAR_MAX;
							program->pc += 2;
						}
					} else if (keypad.down) {
						keypad.held_key = __builtin_ctz(keypad.down) & 0xF;
						keypad.held_key_time = time_now;
						v[opcode.vx] = keypad.held_key;
					}
					break;
				case 0x15:
					program->timer = v[opcode.vx];
					program->pc += 2;
					break;
				case 0x18:
					program->sound = v[opcode.vx];
					program->pc += 2;
					break;
				case 0x1E:
					/* font data starts at mem[0] */
					program->i = (program->i + v[opcode.vx]) & 0xFFF;
					program->pc += 2;
					break;
				case 0x29:
					program->i = ((v[opcode.vx] & 0xF) * 5) & 0xFFF;
					program->pc += 2;
					break;
				case 0x33:
					mem[(program->i + 0) & 0xFFF] = v[opcode.vx] / 100;
					mem[(program->i + 1) & 0xFFF] = v[opcode.vx] / 10 % 10;
					mem[(program->i + 2) & 0xFFF] = v[opcode.vx] % 10;
					program->pc += 2;
					break;
				case 0x55:
					for (uint8_t x = 0; x <= opcode.vx; x++) {
						mem[(program->i + x) & 0xFFF] = v[x];
					}
					if (quirks & CHIP8_QUIRK_INCREMENT_I) {
						program->i = (program->i + opcode.vx + 1) & 0xFFF;
					}
					program->pc += 2;
					break;
				case 0x65:
					for (uint8_t x = 0; x <= opcode.vx; x++) {
						v[x] = mem[(program->i + x) & 0xFFF];
					}
					if (quirks & CHIP8_QUIRK_INCREMENT_I) {
						program->i = (program->i + opcode.vx + 1) & 0xFFF;
					}
					program->pc += 2;
					break;
				}
				break;
			}

			if ((quirks & CHIP8_QUIRK_VBLANK_WAIT) && sprite_drawn) {
				break;
			}

			if (last_pc == program->pc) {
				bool wait = opcode.group == 0xF && opcode.nn == 0x0A;
				bool halt = opcode.group == 0x1 && opcode.nnn == program->pc;
				if (!(wait || halt)) {
					Dump = 1;
					Stop = 1;
				}
				break;
			}
		}

		int64_t timer_now = os_get_time();
		timer_accumulator += timer_now - timer_last;
		timer_last = timer_now;
		while (timer_accumulator >= INT64_C(16666667)) {
			timer_accumulator -= INT64_C(16666667);
			if (program->timer) {
				--program->timer;
			}
			if (program->sound) {
				os_beep();
				--program->sound;
			}
		}

		os_wait_frame(time_now);
		os_bit_blit(&mem[program->bm]);
		update_keypad(&keypad, os_get_time(), context->keypad_response_time);
	}
}

static bool
chip8_init(struct chip8_program *program, uint8_t *data, size_t size)
{
	if (size > PROGRAM_MAX_SIZE) {
		return false;
	}

	/* only required when may want a full memory dump; avoids
	 * parsing uninitialized memory as opcodes at end of program
	 */
	memset(program, 0, sizeof *program);

	/* Memory map https://www.laurencescotford.net/2020/07/14/chip-8-ram-or-memory-management-with-chip-8
	 * Copy font data to somewhere in the range [0x0,0x1FC); place at 0 for opcode FX29
	 *
	 * V registers, stack, and bitmap are aliased in mem[] at 0xEF0, 0xEA0, and
	 * 0xF00 respectively. This is intentional and matches the COSMAC VIP layout.
	 * A ROM writing to high addresses via the I register can corrupt emulator state.
	 */
	uint16_t font_offset   = 0x000;
	uint16_t boot_offset   = 0x1FC;
	uint16_t prog_offset   = 0x200;
	uint16_t stack_offset  = 0xEA0;
	uint16_t reg_offset    = 0xEF0;
	uint16_t bitmap_offset = 0xF00;
	memcpy(program->mem + font_offset, Fonts, sizeof Fonts);
	memcpy(program->mem + prog_offset, data, size);
	program->pc    = boot_offset;
	program->stack = stack_offset;
	program->v     = reg_offset;
	program->bm    = bitmap_offset;
	program->len   = (uint16_t)size;
	program->i     = 0;
	program->sound = 0;
	program->timer = 0;
	program->sp    = 0;
	/* Boot sequence: CLS (00E0) then JP 0x200 (1200) to program start.
	 * JP replaces the COSMAC VIP SYS call (004B) which was a no-op. */
	program->mem[boot_offset + 0] = 0x00;
	program->mem[boot_offset + 1] = 0xE0;
	program->mem[boot_offset + 2] = 0x12;
	program->mem[boot_offset + 3] = 0x00;
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
	} else if (signal == SIGHUP) {
		Dump = 1;
	}
}

static struct termios
os_init(void)
{
	signal(SIGHUP, os_signal_handler);
	signal(SIGINT, os_signal_handler);
	signal(SIGQUIT, os_signal_handler);
	signal(SIGTERM, os_signal_handler);

	char s[] = "\033[?25l\033[2J\033[H";
	write_str(s, (sizeof s)-1);

	struct termios prev;
	tcgetattr(STDIN_FILENO, &prev);
	struct termios termios = prev;
	termios.c_lflag &= (tcflag_t)(~(ICANON | ECHO));
	termios.c_cc[VMIN] = 0;
	termios.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &termios);
	return prev;
}

static void
os_term(struct termios *termios)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, termios);
	char s[] = "\033[H\033[2J\033[?25h";
	write_str(s, (sizeof s)-1);
}

static bool
load_file(char *filename, struct chip8_program *dst)
{
	uint8_t tmp[PROGRAM_MAX_SIZE+1];
	FILE *file = fopen(filename, "rb");
	if (!file) {
		fprintf(stderr, "error: cannot open program %s\n", filename);
		return false;
	}
	size_t nread = fread(tmp, 1, sizeof tmp, file);
	int eof = feof(file);
	fclose(file);
	if (!(eof && nread)) {
		fprintf(stderr, "error: cannot read program %s\n", filename);
		return false;
	}
	if (!chip8_init(dst, tmp, nread)) {
		fprintf(stderr, "error: cannot load program %s\n", filename);
		return false;
	}
	return true;
}

int
main(int argc, char **argv)
{
	struct chip8_program program;
	bool disasm_and_quit = false;

	setlocale(LC_ALL, "en_US.UTF-8");
	--argc;
	++argv;
	if (argc) {
		if (strcmp(*argv, "-disasm") == 0) {
			disasm_and_quit = true;
			--argc;
			++argv;
		}
	}
	if (argc) {
		if (!load_file(*argv, &program)) {
			return 1;
		}
	} else {
		if (!chip8_init(&program, DemoRandomTimer, sizeof DemoRandomTimer)) {
			fprintf(stderr, "error: cannot load demo program\n");
			return 1;
		}
	}

	if (disasm_and_quit) {
		chip8_dump(stderr, &program, false);
		return 0;
	}

	struct termios old_state = os_init();

	struct chip8_context context = {
		.program = &program,
		.opcodes_per_frame = 10,
		.keypad_response_time = 150,
		.quirks = CHIP8_QUIRK_SHIFT_VX
	};
	chip8_exec(&context);

	os_term(&old_state);

	return 0;
}

