#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>

/* Interpreter flag constants */
#define RUN_FLAG 1
#define CRASH_FLAG 2
#define JUMP_FLAG 4
#define CARRY_FLAG 8
#define ZERO_FLAG 16
// TODO overflow flag
#define INTERRUPT_ENABLE 64
// enable interrupts NEXT instruction
#define INTERRUPT_ENABLE_NEXT 128
#define WAIT_FLAG 256

#define ROM_SIZE 65536

#define SCALE 4

#define VBLANK_INTERRUPT 0x80
#define HBLANK_INTERRUPT 0x88
#define KEYBOARD_INTERRUPT 0x90

// Number of palettes.
#define N_PALETTES 8
// lg(colors per palette)
#define N_PALETTE_BITS 3
#define N_PRIORITY_BITS 1

// colors per palette
#define N_COLORS (1 << N_PALETTE_BITS)
#define N_PIXEL_BITS (N_PALETTE_BITS + N_PRIORITY_BITS)

// sprite dimensions
#define SPRITE_WIDTH 8
#define SPRITE_HEIGHT 8

// bytes per sprite
#define SPRITE_BYTES (SPRITE_WIDTH * SPRITE_HEIGHT * (N_PALETTE_BITS + N_PRIORITY_BITS) / 8)

//#define DEBUG

#ifdef DEBUG
int debug_counter = 0;
int instr_counter = 0;
#endif

typedef uint32_t u32;

typedef int32_t i32;

typedef uint16_t u16;

typedef int16_t i16;

typedef uint8_t u8;

typedef int8_t i8;

SDL_Window *window;
SDL_Surface *screen;
SDL_Texture *texture;
SDL_Renderer *renderer;

int widescreen = 1;
int SCRW;
int SCRH;

char rom_title[31];

// if we fail to do the key interrupt once because
// we're already in an interrupt, store it here for a sec
// and try again
u8 backup_key = 0xFF;

// uh, this can be arbitrarily big I guess
// I'll probably heap-allocate this later
unsigned char rom_buffer[ROM_SIZE];

// Here's our machine!
typedef struct interp {
    // 12 general use registers
    u16 a;
    u16 b;
    u16 c;
    u16 d;

    u16 e;
    u16 f;
    u16 g;
    u16 h;

    u16 i;
    u16 j;
    u16 k;
    u16 l;

    // data bank register (actually only 8 bits)
    u16 dbr;

    // program bank register (also only 8 bits)
    u16 pbr;

    // stack pointer register
    u16 sp;

    // program counter
    u16 pc;

    // interpreter flags
    u16 flags;

    // pointer to ROM data
    u8 *rom;

    // 16k of sweet sweet RAM
    u8 mem[16384];

    // Last keyboard button pressed
    u8 last_key;

    struct ppu *ppu;
} interp;

// "PPU" stuff
typedef struct ppu {
    // Horizontal/vertical drawing offset
    // (-128 to +127)
    u8 sprite_h_offset;
    u8 sprite_v_offset;
    u8 bg_h_offset;
    u8 bg_v_offset;
    u8 fg_h_offset;
    u8 fg_v_offset;
    //
    // palette data % 0rrrrrgg gggbbbbb
    //
    // (8 sprite palettes + 8 tile palettes)
    // x 8 colors each x 2 bytes/color = 256 bytes
    u8 palette_data[256];
    //
    // 32 x 32 background tilemap
    // 2 bytes/tile
    //
    // 2 x 32 x 32 = 2K
    //
    // format %ppp?hv?n %iiiiiiii
    // p = palette; i = pattern index
    // n = high/low half of pattern table
    // h,v = horizontal/vertical flip
    u8 bg_map_data[2048];
    //
    // 32x32 foreground tilemap
    //
    // same as above
    u8 fg_map_data[2048];
    //
    // OAM - positions of sprites on screen
    //
    // 4 bytes per sprite:
    // %ppplhvsn %iiiiiiii %xxxxxxxx %yyyyyyyy
    // p = palette; i = sprite index; x,y = coords
    // l = layer (if 1, show above fg map)
    // h,v = horizontal/vertical flip
    // s = size (if 1, 16x16 else 8x8)
    // n = high/low half of pattern table
    // 256 sprites on screen max. 256 x 4 = 1K
    u8 oam[1024];
    //
    // sprite/tile data
    //
    // 4bpp, first bit is 'priority bit' for layering
    // (priority bit set on bg tile = shows in front of
    //  non-priority sprite pixels)
    // other 3 bits are the color
    // i know, it's weird but i had to have an excuse
    // to use only 8 colors rather than 16 for the
    // a e s t h e t i c
    // anyway we have 512 tiles x 1/2 byte/pixel
    // x 8 pixels wide x 8 pixels tall = 16K
    u8 pattern_offset;
    u8 pattern_table[16384];
} ppu;

void do_instr(interp *I);

int interrupt(interp *I, u16 addr);

void init_ppu(ppu *p);

int init_draw();
void draw(interp *I);

void handle_keydown(interp *I, SDL_KeyboardEvent key);

void insert_string(u8 *mem, u16 offset, int length, char *str) {
    int i = 0;
    while (i < length) {
        mem[offset * 2 + i] = str[i];
        i++;
    }
}

u16 *get_reg(interp *I, char reg_id) {
    // given register id (0-7), return a pointer to the right register
    switch(reg_id) {
        case 0:  return &I->a;   break;
        case 1:  return &I->b;   break;
        case 2:  return &I->c;   break;
        case 3:  return &I->d;   break;
        case 4:  return &I->e;   break;
        case 5:  return &I->f;   break;
        case 6:  return &I->g;   break;
        case 7:  return &I->h;   break;
        case 8:  return &I->i;   break;
        case 9:  return &I->j;   break;
        case 10: return &I->k;   break;
        case 11: return &I->l;   break;
        case 12: return &I->dbr; break;
        case 13: return &I->pbr; break;
        case 14: return &I->sp;  break;
        case 15: return &I->pc;  break;
        default:
            fprintf(stderr, "internal error: invalid reg id %d\n", reg_id);
#ifdef DEBUG
            debug_counter = 0;
#endif
            return 0;
    }
}

// Uh, it's apparently implementation-defined for C as to
// whether right shift is arithmetic or logical. So we've
// got to work around it both ways, hooray.
// So now we have 'sra' (arithmetic right shift)
// and 'srl' (logical right shift).
u16 sra(u16 val, int amt) {
    int signbit = val & 0x8000;
    val = ((val >> amt) & 0x3fff) | signbit;
    return val;
}

u16 srl(u16 val, int amt) {
    int signbit = val & 0x8000;
    val = ((val >> amt) & 0x3fff) | (signbit ? (1 << (15 - amt)) : 0);
    return val;
}

void store_byte(interp *I, u16 addr, u8 value) {
    // we have 16k of ram, always mapped to 0x0000~0x4000 no matter what bank
    if (addr < 0x4000) {
        I->mem[addr] = value;
    }

    // a 32k chunk of ROM always mapped to upper half of bank, but not writable!
    if (addr > 0x8000) {
        fprintf(stderr, "Attempt to write to ROM-mapped location $%02X:%04X "
                "(pc: $%02X:%04X)\n", I->dbr, addr, I->pbr, I->pc);
#ifdef DEBUG
        debug_counter = 0;
#endif
    }

    /* TODO come up with the rest of the memory mapping!!
     * t o o   t i r e d   for this right now */

    /*
    // 0x8000-0x9fff is always the first 8k of RAM
    else if (addr < 0xa000) {
        I->mem[addr - 0x8000] = value;
    }

    // writing to 0xa000-0xbfff writes to the current banked chunk of RAM
    // (there are 8 of these, although #0 is always also mapped to
    // 0x8000-0x9fff)
    else if (addr < 0xc000) {
        I->mem[addr - 0xa000 + I->ram_bank * 0x2000] = value;
    }

    // $c000 - $cfff is reserved for video-related stuff (tho not all
    // of it is used atm.)

    // $c000 - $c7ff is background tilemap
    else if (addr < 0xc800) {
        I->ppu->bg_map_data[addr - 0xc000] = value;
    }
    // $c800 - $cfff is foreground tilemap
    else if (addr < 0xd000) {
        I->ppu->fg_map_data[addr - 0xc800] = value;
    }
    // $d000 - $d3ff is OAM
    else if (addr < 0xd400) {
        I->ppu->oam[addr - 0xd000] = value;
    }
    // $d400 - $d4ff is palette data
    else if (addr < 0xd500) {
        I->ppu->palette_data[addr - 0xd400] = value;
    }
    // $d500 - $d57f is 128 bytes of the low half of the
    // pattern table at offset [$cffb] * 32
    else if (addr < 0xd57f) {
        I->ppu->pattern_table[I->ppu->pattern_offset * 32 + addr - 0xd500] = value;
    }
    // $d580 - $d5ff is 128 bytes of the high half of the
    // pattern table at offset [$cffb] * 32
    else if (addr < 0xd600) {
        I->ppu->pattern_table[(I->ppu->pattern_offset * 32
                                + addr - 0xd580 + 8192) % 0x10000] = value;
    }
    // 505 bytes at $d600 - $d7f8 is currently unused, but reserved
    else if (addr < 0xcff9) {
        // nothing happens
        printf("Unimplemented writing to %04X\n", addr);
#ifdef DEBUG
        debug_counter = 0;
#endif
    }
    // $d7f9 is the pattern table offset value
    else if (addr == 0xd7f9) {
        I->ppu->pattern_offset = value;
    }
    // $d7fa is the BG layer's horizontal offset (signed)
    else if (addr == 0xd7fa) {
        I->ppu->bg_h_offset = value;
    }
    // $d7fb is the BG layer's vertical offset (signed)
    else if (addr == 0xd7fb) {
        I->ppu->bg_v_offset = value;
    }
    // $d7fc is the FG layer's horizontal offset (signed)
    else if (addr == 0xd7fc) {
        I->ppu->fg_h_offset = value;
    }
    // $d7fd is the FG layer's vertical offset (signed)
    else if (addr == 0xd7fd) {
        I->ppu->fg_v_offset = value;
    }
    // $d7fe is the sprite layer's horizontal offset (signed)
    else if (addr == 0xd7fe) {
        I->ppu->sprite_h_offset = value;
    }
    // $d7ff is the sprite layer's vertical offset (signed)
    else if (addr == 0xd7ff) {
        I->ppu->sprite_v_offset = value;
    } else if (addr == 0xff02) {
        // doesn't do anything
        printf("Attempted write to read-only HW register $FF02 (keyboard key)\n");
#ifdef DEBUG
        debug_counter = 0;
#endif
    }


    else {
        printf("Unimplemented writing to %04X\n", addr);
#ifdef DEBUG
        debug_counter = 0;
#endif
    }
    */
}

u8 load_byte(interp *I, u16 addr, int bank) {
    // Reading below $4000 returns stuff in first 16k of ROM, always
    if (addr < 0x4000) {
        return I->rom[addr];
    }

    // Reading $4000 - $7fff returns stuff in a different 16k chunk of ROM
    else if (addr < 0x8000) {
        return I->rom[addr + bank * 0x4000];
    }

    // Reading $8000 - $9fff returns values in first 8k of RAM
    else if (addr < 0xa000) {
        return I->mem[addr - 0x8000];
    }

    // Reading $a000 - $bfff returns values in other 8k of RAM
    else if (addr < 0xc000) {
        return I->mem[addr - 0xa000 + bank * 0x2000];
    }

    // $c000 - $c7ff is background tilemap
    else if (addr < 0xc800) {
        return I->ppu->bg_map_data[addr - 0xc000];
    }
    // $c800 - $cfff is foreground tilemap
    else if (addr < 0xd000) {
        return I->ppu->fg_map_data[addr - 0xc800];
    }
    // $d000 - $c3ff is OAM
    else if (addr < 0xd400) {
        return I->ppu->oam[addr - 0xd000];
    }
    // $d400 - $d47f is palette data
    else if (addr < 0xd500) {
        return I->ppu->palette_data[addr - 0xd400];
    }
    // $d500 - $d57f is 128 bytes of the low half of the
    // pattern table at offset [$cffb] * 32
    else if (addr < 0xd580) {
        return I->ppu->pattern_table[I->ppu->pattern_offset * 32 + addr - 0xd500];
    }
    // $d580 - $d5ff is 128 bytes of the high half of the
    // pattern table at offset [$cffb] * 32
    else if (addr < 0xd5ff) {
        return I->ppu->pattern_table[(I->ppu->pattern_offset * 32
                                        + addr - 0xd580 + 8192) & 0x10000];
    }
    // $d600 - $d7f8 is currently unused, but reserved
    else if (addr < 0xd7f9) {
        /* nothing happens */
        printf("Unimplemented reading from %04X\n", addr);
#ifdef DEBUG
        debug_counter = 0;
#endif
        return 0;
    }
    // $d7f9 is the pattern table offset value
    else if (addr == 0xd7f9) {
        return I->ppu->pattern_offset;
    }
    // $d7fa is the BG layer's horizontal offset (signed)
    else if (addr == 0xd7fa) {
        return I->ppu->bg_h_offset;
    }
    // $d7fb is the BG layer's vertical offset (signed)
    else if (addr == 0xd7fb) {
        return I->ppu->bg_v_offset;
    }
    // $d7fc is the FG layer's horizontal offset (signed)
    else if (addr == 0xd7fc) {
        return I->ppu->fg_h_offset;
    }
    // $d7fd is the FG layer's vertical offset (signed)
    else if (addr == 0xd7fd) {
        return I->ppu->fg_v_offset;
    }
    // $d7fe is the sprite layer's horizontal offset (signed)
    else if (addr == 0xd7fe) {
        return I->ppu->sprite_h_offset;
    }
    // $d7ff is the sprite layer's vertical offset (signed)
    else if (addr == 0xd7ff) {
        return I->ppu->sprite_v_offset;
    }

    else if (addr == 0xff02) {
        return I->last_key;
    }

    else {
        printf("Unimplemented reading from %04X\n", addr);
#ifdef DEBUG
        debug_counter = 0;
#endif
        return 0;
    }
}

void store_word(interp *I, u16 addr, u16 value) {
    if (addr % 2 == 1) {
        fprintf(stderr, "Unaligned word write to $%04X (pc: $%04X)\n", addr, I->pc);
#ifdef DEBUG
        debug_counter = 0;
#endif
        return;
    }

    u8 hival = srl(value, 8) & 0xff;
    u8 loval = value & 0xff;

    store_byte(I, addr, hival);
    store_byte(I, addr + 1, loval);
}

u16 load_word(interp *I, u16 addr, int bank) {
    if (addr % 2 == 1) {
        fprintf(stderr, "Unaligned word read at $%04X (pc: $%04X)\n", addr, I->pc);
#ifdef DEBUG
        debug_counter = 0;
#endif
        return 0;
    }

    u8 hival = load_byte(I, addr, bank);
    u8 loval = load_byte(I, addr+1, bank);

    return ((u16)hival << 8) | loval;
}

int main (int argc, char **argv) {
    if (argc != 2) {
        printf("Please supply a file name.\n");
        return 0;
    }

    if (!init_draw()) {
        fprintf(stderr, "Unable to initialize video.\n");
        return -1;
    }

    interp I;
    ppu P;
    init_ppu(&P);

    I.ppu = &P;
    I.flags = RUN_FLAG | INTERRUPT_ENABLE;

    // program starts at 0x0100, after a 256-byte header
    I.pbr = 0;
    I.dbr = 0;

    I.pc = 0x0100;

    // stack starts here... probably should fix this
    I.sp = 0x9ffe;

    // all other registers start at 0
    I.a = I.b = I.c = I.d = I.e = I.f
        = I.g = I.h = I.i = I.j = I.k = I.l = 0;

    FILE *rom = fopen(argv[1], "rb");

    size_t size = fread(rom_buffer, 1, ROM_SIZE, rom);

    printf("Read %lu bytes from ROM.\n", size);

    I.rom = rom_buffer;

    strncpy(rom_title, (char*)&rom_buffer[2], 30);
    rom_title[30] = '\0';
    printf("Loaded: %s\n", rom_title);

    unsigned int time = SDL_GetTicks();

    draw(&I);
    while (I.flags & RUN_FLAG) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            // quit when we close the window
            // otherwise infinite loops become terrible
            if (event.type == SDL_QUIT) {
                I.flags &= ~RUN_FLAG;
            } else if (event.type == SDL_KEYDOWN) {
                // Later we'll have a 'controller mode' as well.
                // For now, we just have a keyboard.
                handle_keydown(&I, event.key);
            }
        }
#ifdef DEBUG
        if (SDL_GetTicks() - time >= (debug_counter ? 17 : 2000)) {
#else
        if (SDL_GetTicks() - time >= 17) {
#endif
            draw(&I);
            time = SDL_GetTicks();
            // vblank interrupt
            interrupt(&I, VBLANK_INTERRUPT);
        }
        if (I.flags & INTERRUPT_ENABLE_NEXT) {
            I.flags &= ~INTERRUPT_ENABLE_NEXT;
            I.flags |= INTERRUPT_ENABLE;
        }
        if (backup_key != 0xff) {
            // Try again with the key code
            I.last_key = backup_key;
            if (interrupt(&I, KEYBOARD_INTERRUPT)) {
                backup_key = 0xff;
            }
        }
        if (!(I.flags & WAIT_FLAG)) {
            do_instr(&I);
#ifdef DEBUG
            if (debug_counter > 0) debug_counter --;
            instr_counter ++;
            if (debug_counter == 0) {
                char cmd[20] = "";
                int cont = 0;
                while (!cont) {
                    cmd[0] = '\0';
                    printf("debugger[%d]> ", instr_counter);
                    fgets(cmd, 20, stdin);
                    if (!strcmp(cmd, "\n") || !strcmp(cmd, "cont\n")) {
                        cont = 1;
                    } else if (!strcmp(cmd, "run\n") || !strcmp(cmd, "r\n")) {
                        debug_counter = -1;
                        cont = 1;
                    } else if (strlen(cmd) == 0 || !strcmp(cmd, "exit\n") || !strcmp(cmd, "q\n")) {
                        I.flags &= ~RUN_FLAG;
                        cont = 1;
                    } else if (!strcmp(cmd, "state\n") || !strcmp(cmd, "s\n")) {
                        printf("==== STATE ====\n");
                        printf("Reg: a: %04X b: %04X c: %04X d: %04X\n", I.a, I.b, I.c, I.d);
                        printf("     e: %04X f: %04X g: %04X h: %04X\n", I.e, I.f, I.g, I.h);
                        printf("     i: %04X j: %04X k: %04X l: %04X\n", I.i, I.j, I.k, I.l);
                        printf("     DB %04X PB %04X SP %04X PC %04X\n", I.dbr, I.pbr, I.sp, I.pc);
                    } else if (!strcmp(cmd, "help\n")) {
                        printf("* Press enter or type \"cont\" to advance one instruction.\n");
                        printf("* Type \"state\" or \"s\" to print register state.\n");
                        printf("* Type \"run\" or \"r\" to make it run normally.\n");
                        printf("* Type a number to run normally for that many instructions.\n");
                        printf("* Type \"exit\" or \"q\" to end the program.\n");
                        printf("  (You can also quit by pressing Control-D.)\n");
                    } else if (atoi(cmd) >= 0) {
                        debug_counter = atoi(cmd);
                        cont = 1;
                    } else {
                        printf("Unknown debugger command\n");
                    }
                }
            }
#endif
        }
    }

    printf("==== FINAL STATE ====\n");
    printf("Reg: a: %04X b: %04X c: %04X d: %04X\n", I.a, I.b, I.c, I.d);
    printf("     e: %04X f: %04X g: %04X h: %04X\n", I.e, I.f, I.g, I.h);
    printf("     i: %04X j: %04X k: %04X l: %04X\n", I.i, I.j, I.k, I.l);
    printf("     DB %04X PB %04X SP %04X PC %04X\n", I.dbr, I.pbr, I.sp, I.pc);

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

void do_instr(interp *I) {
    u16 instr = load_word(I, I->pc, I->pbr);

    // first 4 bits are the opcode
    u16 instrtype = srl(instr, 12) & 0xf;

#ifdef DEBUG
    printf("Instruction @ 0x%04X: 0x%04X\n", I->pc, instr);
#endif

    int ok = 0;

    // how much to increment pc by after performing instruction
    u8 pc_increment = 2;

    if (instrtype == 0x0) {
        // 0000: miscellaneous
        // get the 12 remaining bits
        u16 subcode = srl(instr, 8) & 0xf;
        u16 rest = instr & 0xff;
        if (subcode == 0) {
            // code 0 = 'special' instructions
            if (rest == 0xff) {
                // 0x00ff = STOP
                I->flags &= ~RUN_FLAG;
                printf("Stop.\n");
                ok = 1;
            } else if (rest == 0x01) {
                // 0x0001 = NOP
                ok = 1;
            } else if (rest == 0x02) {
                // 0x0002 = HALT
                I->flags |= WAIT_FLAG;
                ok = 1;
            } else if (rest == 0x28) {
                // 0x0028 = CLC
                // (clear carry flag)
                I->flags &= ~CARRY_FLAG;
            } else if (rest == 0xaa) {
                // 0x00aa = RETURN
                // pops return address off stack and jumps to it
                u16 retaddr = load_word(I, I->sp, I->dbr);
                I->sp += 2;
                I->pc = retaddr;
                // don't increase pc
                pc_increment = 0;
                ok = 1;
            } else if (rest == 0xab) {
                // 0x00ab = RETI
                // return and enable interrupts
                u16 retaddr = load_word(I, I->sp, I->dbr);
                I->sp += 2;
                I->pc = retaddr;
                I->flags |= INTERRUPT_ENABLE_NEXT;
                // don't increase pc
                pc_increment = 0;
                ok = 1;
            } else if (rest == 0xdd) {
                // 0x00dd = disable interrupts
                I->flags &= ~INTERRUPT_ENABLE;
                ok = 1;
            } else if (rest == 0xee) {
                // 0x00ee = enable interrupts
                I->flags |= INTERRUPT_ENABLE_NEXT;
                ok = 1;
            }
        } else if (subcode == 1) {
            // PUSH
            //      0000 0001 xxxx ----
            // xxxx = register to push
            I->sp -= 2;
            u8 reg_idx = srl(rest, 4);
            u16 *push_reg = get_reg(I, reg_idx);
            store_word(I, I->sp, *push_reg);
            ok = 1;
        } else if (subcode == 2) {
            // POP
            //      0000 0010 xxxx ----
            // xxxx = register to pop into
            u8 reg_idx = srl(rest, 4);
            u16 *pop_reg = get_reg(I, reg_idx);
            *pop_reg = load_word(I, I->sp, I->dbr);
            I->sp += 2;
            ok = 1;
        } else if (subcode == 3) {
            // Jump to register
            //      0000 0011 xxxx ----
            // xxx = register containing address to jump to
            u8 reg_idx = srl(rest, 4);
            u16 *jump_reg = get_reg(I, reg_idx);
            I->pc = *jump_reg;
            // don't increment pc
            pc_increment = 0;
            ok = 1;
        } else if (subcode == 4) {
            // Swap two registers
            //      0000 0100 xxxx yyyy
            // xxxx, yyyy = registers to swap
            u8 reg_idx1 = srl(rest, 4);
            u8 reg_idx2 = (rest & 0xf);
            u16 *r1 = get_reg(I, reg_idx1);
            u16 *r2 = get_reg(I, reg_idx2);
            *r1 ^= *r2;
            *r2 ^= *r1;
            *r1 ^= *r2;
            ok = 1;
        }
    } else if ((instrtype & 0x8) == 0x8) {
        // prefix 1 = arithmetic instructions
        //
        // format: 1oooooxx xxyyyyyy
        //
        //  ooooo = arithmetic operation
        //   xxxx = dest register (like x86, also a source for eg add)
        // yyyyyy = other src register, or special value

        ok = 1;

        u8 op       = srl(instr, 10) & 0x1f;
        u8 dest_idx = srl(instr,  6) &  0xf;
        u8 src_idx  = srl(instr,  0) & 0x3f;

        u8 carry = (I->flags & CARRY_FLAG) ? 1 : 0;

        // reset flags for MATH
        I->flags &= ~CARRY_FLAG;
        I->flags &= ~ZERO_FLAG;

        u16 *dest = get_reg(I, dest_idx);

        u16 srcval;

        if (src_idx < 0x10) {
            // yy yyyy = 00 rrrr, where rrrr is register ID
            u16 *src = get_reg(I, src_idx & 0xf);
            srcval = *src;
        } else if (src_idx < 0x20) {
            // yy yyyy = 01 vvvv, where vvvv is a small immediate
            // value from 0-15 (can be used for immediate bitshifts,
            // bit testing, etc. where we only need these values)
            srcval = src_idx & 0xf;
        } else if (src_idx == 0x20) {
            // yy yyyy = 10 0000
            // this means there's a 16-bit immediate value following
            // the instruction; we use this as the second operand

            // Get the immediate value
            srcval = load_word(I, I->pc + 2, I->pbr);
            pc_increment += 2;
        } else if (src_idx == 0x21) {
            // yy yyyy = 10 0001
            // This is just a special code for -1, since it's probably
            // a common thing and we don't want to waste 16 bits on it.
            // (e.g. if we want to compare to -1 or w/e)
            srcval = 0xFFFF;
        } else if (src_idx >= 0x24 && src_idx < 0x30) {
            // yy yyyy = 10 vvvv
            // vvvv = a value from 4 to 15
            // this is shorthand for (1 << vvvv), so we can do powers
            // of two without using an extra byte. Handy for bitmasks, etc.

            // 1<<0 through 1<<3 (1, 2, 4, 8) are handled by 01vvvv, above.
            // (since they're less than 15)
            srcval = 1 << (src_idx - 0x20);
        } else {
            // uh I don't know what 0x22 or 0x23 or 0x3? should be yet
            // but we've got some room here to expand!
            fprintf(stderr, "Unknown source operand $%X for "
                    "arithmetic instruction\n", src_idx);
            srcval = 0;
            ok = 0;
        }

        if (op == 0x00) {
            // Move register/load immediate
            *dest = srcval;
        } else if (op == 0x01) {
            // Addition!
            if ((int)*dest + (int)srcval > 0xFFFF) I->flags |= CARRY_FLAG;
            *dest += srcval;
        } else if (op == 0x02) {
            // Subtraction
            if ((int)*dest - (int)srcval < 0) I->flags |= CARRY_FLAG;
            *dest -= srcval;
        } else if (op == 0x03) {
            // Unsigned multiplication
            if ((u32)*dest * (u32)srcval > 0xFFFF) I->flags |= CARRY_FLAG;
            *dest = (u16)((u32)*dest * (u32)srcval);
        } else if (op == 0x04) {
            // Signed multiplication

            // I think this is right? (I hope this is right...)
            // First convert to signed, then sign-extend. :/
            i32 sdest = (i32)(i16)*dest;
            i32 ssrc = (i32)(i16)srcval;
            if (sdest * ssrc >= 0x8000) I->flags |= CARRY_FLAG;
            *dest = (u16)(i16)(sdest * ssrc);

        } else if (op == 0x05) {
            // Unsigned division
            *dest /= srcval;
        } else if (op == 0x06) {
            // Signed division
            *dest = ((i16)*dest / (i16)srcval);
        } else if (op == 0x07) {
            // Unsigned modulo
            *dest = ((*dest % srcval) + srcval) % srcval;
        } else if (op == 0x08) {
            // Signed modulo
            // Non-stupid signed modulo, though
            *dest = (u16)(((i16)*dest % srcval) + srcval) % srcval;
        } else if (op == 0x09) {
            // Bitwise AND
            *dest &= srcval;
        } else if (op == 0x0a) {
            // Bitwise OR
            *dest |= srcval;
        } else if (op == 0x0b) {
            // Bitwise XOR
            *dest ^= srcval;
        } else if (op == 0x0c) {
            // Bitwise negation (doesn't use src)
            *dest = ~*dest;
        } else if (op == 0x0d) {
            // Arithmetic negation (doesn't use src)
            *dest = 0xFFFF - *dest + 1;
        } else if (op == 0x0e) {
            // Increment dest (doesn't use src)
            // (Sets carry flag if the thing wrapped around)
            if (*dest == 0xFFFF) I->flags |= CARRY_FLAG;
            (*dest)++;
        } else if (op == 0x0f) {
            // Decrement dest (doesn't use src)
            // (Also sets carry flag if the thing wrapped around)
            if (*dest == 0x0000) I->flags |= CARRY_FLAG;
            (*dest)--;
        } else if (op == 0x10) {
            // Logical left shift
            // (Also sets carry flag if the thing wrapped around)
            if (*dest >= 0x8000) I->flags |= CARRY_FLAG;
            *dest <<= srcval;
        } else if (op == 0x11) {
            // Logical right shift
            *dest = srl(*dest, srcval);
        } else if (op == 0x12) {
            // Arithmetic right shift
            *dest = sra(*dest, srcval);
        } else if (op == 0x13) {
            // Bit rotate left
            u8 amount = srcval & 0xf;
            *dest = srl(*dest, 16 - amount) | (u16)(*dest << amount);
        } else if (op == 0x14) {
            // Bit rotate right
            u8 amount = srcval & 0xf;
            *dest = (*dest << (16 - amount)) | srl(*dest, amount);
        } else if (op == 0x15) {
            // Bit test
            u8 bit = srcval & 0xf;
            if (!(*dest & (1 << bit))) {
                I->flags |= ZERO_FLAG;
            }
        } else if (op == 0x16) {
            // Add with carry
            if ((u32)*dest + (u32)srcval + carry > 0xFFFF) {
                I->flags |= CARRY_FLAG;
            }
            *dest += srcval + carry;
        } else if (op == 0x17) {
            // Subtract with carry
            if ((i32)*dest - (i32)srcval - carry < 0x0000) {
                I->flags |= CARRY_FLAG;
            }
            *dest -= srcval + carry;
        } else if (op == 0x18) {
            // Multiply with carry
            if ((u32)*dest * (u32)srcval + carry > 0xFFFF) {
                I->flags |= CARRY_FLAG;
            }
            *dest = *dest * srcval + carry;
        /* unused operation space here */
        } else if (op == 0x1e) {
            // Unsigned comparison
            if (*dest < srcval) I->flags |= CARRY_FLAG;
            if (*dest == srcval) I->flags |= ZERO_FLAG;
        } else if (op == 0x1f) {
            // Signed comparison
            if ((i16)*dest < (i16)srcval) I->flags |= CARRY_FLAG;
            if (*dest == srcval) I->flags |= ZERO_FLAG;
        } else {
            ok = 0;
        }

        if (*dest == 0 && op < 0x1e) {
            I->flags |= ZERO_FLAG;
        }
    } else if ((instrtype & 0xc) == 0x4) {
        // prefix 01: jump
        //
        //      01ooooaa aaaaaaaa
        //
        // oooo = jump type
        //          0: unconditional
        //          1: equal (ZF)
        //          2: not equal (~ZF)
        //          3: less than (CF)
        //          4: greater than or equal to (~CF)
        //          5: less than or equal to (ZF|CF)
        //          6: greater than (~(ZF|CF))
        //              * TODO add signed jumps *
        //         15: subroutine (save return address)
        // a... = jump offset if relative jump
        //          (measured in words, so we can jump
        //           +/- 512 words, where instructions
        //           are either 1 or 2 words)
        //          (NOTE: if a = 0, uses an immediate
        //           value following the instruction as
        //           the address to jump to, rather than
        //           a relative jump direction)
        u8  op       = srl(instr, 10) &    0xf;
        u16 offset   = srl(instr,  0) & 0x03ff;

        u8 should_jump = 0;

        switch (op) {
            case  0: should_jump = 1; break;
            case  1: should_jump = (I->flags & ZERO_FLAG); break;
            case  2: should_jump = !(I->flags & ZERO_FLAG); break;
            case  3: should_jump = (I->flags & CARRY_FLAG); break;
            case  4: should_jump = !(I->flags & CARRY_FLAG); break;
            case  5: should_jump = (I->flags & (ZERO_FLAG | CARRY_FLAG)); break;
            case  6: should_jump = !(I->flags & (ZERO_FLAG | CARRY_FLAG)); break;
            case 15: should_jump = 1; break;
            default: fprintf(stderr, "Unknown jump condition %d\n", op);
        }

        //if (op != 0 && op != 15) printf("jump type: %d; should jump? %d\n", op, should_jump);

        if (should_jump) {
            if (op == 15) {
                // push return address for subroutine call
                I->sp -= 2;
                if (offset == 0) {
                    store_word(I, I->sp, I->pc + 4);
                } else {
                    store_word(I, I->sp, I->pc + 2);
                }
            }

            if (offset != 0) {
                // relative jump
                // sign-extend from 10 to 16 bits
                u16 signbit = offset & 0x0200;
                i16 soffset = (signed) offset;

                if (signbit) {
                    soffset -= 0x0400;
                }

                I->pc += soffset * 2;
            } else {
                // absolute jump
                u16 new_addr = load_word(I, I->pc + 2, I->pbr);
                I->pc = new_addr;
            }

            // don't advance pc
            pc_increment = 0;
        } else if (offset == 0) {
            // if not jumping, need to jump over immediate address
            pc_increment += 2;
        }


        ok = 1;
    } else if ((instrtype & 0xe) == 0x2) {
        // prefix 001: Load/store instructions
        //      001ooxxx x0yyyyyy
        //     oo = operation type (load/store word/byte)
        //   xxxx = register to load/store into/from
        // yyyyyy = register w/ memory location (possibly imm. offset follows)

        ok = 1;

        u8 op     = srl(instr, 11) &  0x3;
        u8 reg_id = srl(instr,  7) &  0xf;
        u8 mem_id = srl(instr,  0) & 0x3f;

        u16 *reg = get_reg(I, reg_id);

        u16 addr;
        if (mem_id < 0x10) {
            // yy yyyy = 00 rrrr; address in register rrrr
            addr = *get_reg(I, mem_id & 0xf);
        } else if (mem_id < 0x20) {
            // yy yyyy = 01 rrrr; address in rrrr + imm. offset following
            addr = *get_reg(I, mem_id & 0xf);
            addr += load_word(I, I->pc + 2, I->dbr);
            pc_increment += 2;
        } else if (mem_id == 0x20) {
            // yy yyyy = 10 0000; no register, immediate address following
            addr = load_word(I, I->pc + 2, I->dbr);
            pc_increment += 2;
        } else {
            fprintf(stderr, "Unknown address mode $%X for load/store "
                    "(pc: $%04X)\n", mem_id, I->pc);
            addr = 0;
            ok = 0;
        }

        if (op == 0) {
            // Load word
            *reg = load_word(I, addr, I->dbr);
            //printf("Load word at $%04X: $%04X\n", addr, *reg);
        } else if (op == 1) {
            // Load byte
            *reg = load_byte(I, addr, I->dbr);
            //printf("Load byte at $%04X: $%02X\n", addr, *reg);
        } else if (op == 2) {
            // Store word
            store_word(I, addr, *reg);
            //printf("Store word $%04X at $%04X\n", *reg, addr);
        } else if (op == 3) {
            // Store byte
            store_byte(I, addr, (*reg) & 0xff);
            //printf("Store byte $%02X at $%04X\n", *reg, addr);
        }
    }
    /* unused instruction space: prefix 0001 isn't anything */

    if (!ok) {
        // TODO put up a dialogue box or something on error! jeez, rude
        printf("Unknown opcode: $%X at PC $%X\n", instr, I->pc);
#ifdef DEBUG
        debug_counter = 0;
#else
        // crash :(
        I->flags &= ~RUN_FLAG;
        I->flags |= CRASH_FLAG;
#endif
    } else {
        I->pc += pc_increment;
    }

    /*if (instr != 0x01 && (instr & 0xfc00) != 0x4000) {
        printf("Instr: $%04X\n", instr);
        printf("Reg: a: %04X b: %04X c: %04X d: %04X\n", I->a, I->b, I->c, I->d);
        printf("     e: %04X f: %04X g: %04X h: %04X\n", I->e, I->f, I->g, I->h);
        printf("     i: %04X j: %04X k: %04X l: %04X\n", I->i, I->j, I->k, I->l);
        printf("     m: %04X n: %04X SP %04X PC %04X\n", I->dbr, I->pbr, I->sp, I->pc);
    }*/
}

int interrupt(interp *I, u16 addr) {
    // Do an interrupt. Push the current pc to the stack,
    // disable interrupts, and jump to the specified address.
    // (But not if interrupts are disabled.)
    if (!(I->flags & INTERRUPT_ENABLE)) {
        //printf("Interrupts disabled. :(\n");
        return 0;
    }
    //printf("Interrupted... [%d]\n");
    I->sp -= 2;
    store_word(I, I->sp, I->pc);
    I->flags &= ~INTERRUPT_ENABLE;
    I->flags &= ~WAIT_FLAG;
    I->pc = addr;
    return 1;
}

int init_draw() {
    window = NULL;
    screen = NULL;

    if (widescreen) {
        SCRW = 240;
        SCRH = 144;
    } else {
        SCRW = 240;
        SCRH = 176;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL. :(\n");
        return 0;
    }

    window = SDL_CreateWindow("cricket",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              SCRW * SCALE, SCRH * SCALE,
                              SDL_WINDOW_SHOWN);

    screen = SDL_GetWindowSurface(window);

    SDL_RenderSetLogicalSize(SDL_GetRenderer(window), SCRW, SCRH);

    if (!window) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        return 0;
    }

    renderer = SDL_CreateRenderer(window, -1, 0);

    if (!renderer) {
        fprintf(stderr, "Failed to create renderer: %s\n", SDL_GetError());
        return 0;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, SCRW, SCRH);

    if (!texture) {
        fprintf(stderr, "Failed to create texture: %s\n", SDL_GetError());
        return 0;
    }

    return 1;
}

void handle_keydown(interp *I, SDL_KeyboardEvent key) {
    const u8 SHIFT = 1 << 6;
    const u8 CTRL = 1 << 7;
    u8 keycode = 0;
    int do_interrupt = 1;
    // bit 7 = control
    // bit 6 = shift
    // this leaves 64 unique characters
    if (key.keysym.sym == SDLK_SPACE) {
        // code 0 = space
        keycode = 0;
    } else if (key.keysym.sym >= 'a' && key.keysym.sym <= 'z') {
        // code 1-26 = a-z
        keycode = key.keysym.sym - 'a' + 1;
    } else if (key.keysym.sym >= '0' && key.keysym.sym <= '9') {
        // code 27-36 = 0-9
        keycode = key.keysym.sym - '0' + 27;
    } else switch (key.keysym.sym) {
        case ',':
            keycode = 37;
            break;
        case '<':
            keycode = 37 | SHIFT;
            break;
        case '.':
            keycode = 38;
            break;
        case '>':
            keycode = 38 | SHIFT;
            break;
        case ';':
            keycode = 39;
            break;
        case ':':
            keycode = 39 | SHIFT;
            break;
        case '=':
            keycode = 40;
            break;
        case '+':
            keycode = 40 | SHIFT;
            break;
        case '/':
            keycode = 41;
            break;
        case '?':
            keycode = 41 | SHIFT;
            break;
        case '-':
            keycode = 42;
            break;
        case '_':
            keycode = 42 | SHIFT;
            break;
        case '\'':
            keycode = 43;
            break;
        case '"':
            keycode = 43 | SHIFT;
            break;
        case SDLK_ESCAPE:
            keycode = 56;
            break;
        case SDLK_UP:
            keycode = 57;
            break;
        case SDLK_DOWN:
            keycode = 58;
            break;
        case SDLK_LEFT:
            keycode = 59;
            break;
        case SDLK_RIGHT:
            keycode = 60;
            break;
        case SDLK_RETURN:
            keycode = 61;
            break;
        case SDLK_BACKSPACE:
            keycode = 62;
            break;
        case '!':
            keycode = 28 | SHIFT;
            break;
        case '@':
            keycode = 29 | SHIFT;
            break;
        case '#':
            keycode = 30 | SHIFT;
            break;
        case '$':
            keycode = 31 | SHIFT;
            break;
        case '%':
            keycode = 32 | SHIFT;
            break;
        case '^':
            keycode = 33 | SHIFT;
            break;
        case '&':
            keycode = 34 | SHIFT;
            break;
        case '*':
            keycode = 35 | SHIFT;
            break;
        case '(':
            keycode = 36 | SHIFT;
            break;
        case ')':
            keycode = 27 | SHIFT;
            break;
        default:
            //printf("Oh no unhandled key %c [%d]\n", key.keysym.sym, key.keysym.sym);
            do_interrupt = 0;
    }
    SDL_Keymod mods = SDL_GetModState();

    if (mods & KMOD_SHIFT) {
        keycode |= SHIFT;
    }

    if (mods & KMOD_CTRL) {
        keycode |= CTRL;
    }

    I->last_key = keycode;

    if (do_interrupt && !interrupt(I, KEYBOARD_INTERRUPT)) {
        backup_key = keycode;
    } else {
        backup_key = 0xff;
    }
}

u32 get_palette_color(u16 color) {
    // Convert 15-bit color to 24-bit color
    u32 r = (color >> 10) & 0x1f;
    u32 g = (color >>  5) & 0x1f;
    u32 b = (color >>  0) & 0x1f;

    r = r * 255 / 31;
    g = g * 255 / 31;
    b = b * 255 / 31;

    return (r << 16) | (g << 8) | b;
}

void scanline(interp *I, int line_num) {
    u32 tile_palettes[N_PALETTES][N_COLORS];
    u32 sprite_palettes[N_PALETTES][N_COLORS];

    u32 line_colors[SCRW];
    u8  line_priorities[SCRW];

    for (int pal = 0; pal < N_PALETTES; pal++) {
        for (int i = 0; i < N_COLORS; i++) {
            u8 pal_color_hi = I->ppu->palette_data[pal * N_COLORS * 2 + i * 2];
            u8 pal_color_lo = I->ppu->palette_data[pal * N_COLORS * 2 + i * 2 + 1];
            u16 pal_color = (pal_color_hi << 8) | pal_color_lo;
            tile_palettes[pal][i] = get_palette_color(pal_color);
        }
    }

    for (int pal = 0; pal < N_PALETTES; pal++) {
        for (int i = 0; i < N_COLORS; i++) {
            u8 pal_color_hi = I->ppu->palette_data[128 + pal * N_COLORS * 2 + i * 2];
            u8 pal_color_lo = I->ppu->palette_data[128 + pal * N_COLORS * 2 + i * 2 + 1];
            u16 pal_color = (pal_color_hi << 8) | pal_color_lo;
            sprite_palettes[pal][i] = get_palette_color(pal_color);
        }
    }

    for (int i = 0; i < SCRW; i++) {
        // Default background color
        line_colors[i] = tile_palettes[0][0];
        line_priorities[i] = 0;
    }

    // Tiles per row
    int row_width = 32;

    // Back tile layer
    // Which row of tiles are we drawing?
    int bg_row_num = (((line_num + I->ppu->bg_v_offset) / SPRITE_HEIGHT) % row_width + row_width) % row_width;
    // Which row of that row are we drawing? (i.e. y=0-7)
    u8 bg_tile_row = ((line_num + I->ppu->bg_v_offset) % SPRITE_HEIGHT + SPRITE_HEIGHT) % SPRITE_HEIGHT;

    for (int t = 0; t < row_width; t++) {
        // Get the bytes that describe our tile
        u8 info = I->ppu->bg_map_data[bg_row_num * row_width * 2 + t * 2];
        u16 idx = I->ppu->bg_map_data[bg_row_num * row_width * 2 + t * 2 + 1];

        int horiz_flip = 0, vert_flip = 0;
        if (info & 0x1) idx += 256;
        if (info & 0x4) vert_flip = 1;
        if (info & 0x8) horiz_flip = 1;

        u8 palette = (info & 0xe0) >> 5;

        u8 tile_row = vert_flip ? 7 - bg_tile_row : bg_tile_row;

        // get the appropriate row of the tile (4 bytes)
        u8 *tile_bytes = &I->ppu->pattern_table[idx * SPRITE_BYTES
                                                + tile_row * (SPRITE_BYTES / SPRITE_HEIGHT)];

        int x = t * SPRITE_WIDTH - I->ppu->bg_h_offset;

        // SPRITE_BYTES / SPRITE_HEIGHT = # of bytes per sprite row.
        // so i iterates over each byte of this row of the sprite.
        for (int i = 0; i < SPRITE_BYTES / SPRITE_HEIGHT; i++) {
            // 8 / N_PIXEL_BITS = how many pixels are packed into
            // a byte. so j iterates over each pixel of the current
            // sprite byte
            for (int j = 0; j < 8 / N_PIXEL_BITS; j++) {
                // ~0 = all 1's; ~0 << NPB = all 1's except w/ NPB 0's at the end
                // ~(~0 << NPB) = all 0's but with NPB 1's at the end
                u8 mask = ~(~0 << N_PALETTE_BITS);
                // to mask out priority bits
                u8 priority_mask = ~(~0 << N_PRIORITY_BITS);
                // if j = 0, for instance, we want this offset to give us the *high*
                // bits of the byte
                u8 pixel_offset = 8 - (j + 1) * N_PIXEL_BITS;
                u8 coloridx = (tile_bytes[i] >> pixel_offset) & mask;

                u8 priority_val = ((tile_bytes[i] >> pixel_offset) >> N_PALETTE_BITS) & priority_mask;

                u8 pixelx;
                if (!horiz_flip) {
                    // calculate our x position. also mod by 256 so we wrap around
                    pixelx = (x + i * (8 / N_PIXEL_BITS) + j) % 256;
                } else {
                    // calculate our x position but do it backwards tile-wise
                    pixelx = (x + 7 - (i * (8 / N_PIXEL_BITS) + j)) % 256;
                }

                if (pixelx >= 0 && pixelx < SCRW && coloridx != 0) {
                    line_colors[pixelx] = tile_palettes[palette][coloridx];
                    line_priorities[pixelx] = priority_val * 2;
                }
            }
        }
    }

    for (int spr = 0; spr < 256; spr++) {
        u8 info = I->ppu->oam[spr * 4];
        u16 idx = I->ppu->oam[spr * 4 + 1];

        u8 x = I->ppu->oam[spr * 4 + 2] - I->ppu->sprite_h_offset;
        u8 y = I->ppu->oam[spr * 4 + 3] - I->ppu->sprite_v_offset;

        // Check layer flag
        u8 base_priority = (info & 0x10) ? 5 : 1;

        int horiz_flip = 0, vert_flip = 0;
        if (info & 0x1) idx += 256;
        if (info & 0x4) vert_flip = 1;
        if (info & 0x8) horiz_flip = 1;
        u8 sprite_size = (info & 0x2) ? 16 : 8; // 16px sprite flag
        // TODO actually handle 16px sprites

        u8 palette = (info & 0xe0) >> 5;

        u8 sprite_row = (((line_num - y) % 256) + 256) % 256;

        sprite_row = vert_flip ? 7 - sprite_row : sprite_row;

        if (sprite_row >= sprite_size) {
            continue;
        }

        // get the appropriate row of the tile (4 bytes)
        u8 *sprite_bytes = &I->ppu->pattern_table[idx * SPRITE_BYTES
                                                + sprite_row * (SPRITE_BYTES / SPRITE_HEIGHT)];

        // SPRITE_BYTES / SPRITE_HEIGHT = # of bytes per sprite row.
        // so i iterates over each byte of this row of the sprite.
        for (int i = 0; i < SPRITE_BYTES / SPRITE_HEIGHT; i++) {
            // 8 / N_PIXEL_BITS = how many pixels are packed into
            // a byte. so j iterates over each pixel of the current
            // sprite byte
            for (int j = 0; j < 8 / N_PIXEL_BITS; j++) {
                // ~0 = all 1's; ~0 << NPB = all 1's except w/ NPB 0's at the end
                // ~(~0 << NPB) = all 0's but with NPB 1's at the end
                u8 mask = ~(~0 << N_PALETTE_BITS);
                // to mask out priority bits
                u8 priority_mask = ~(~0 << N_PRIORITY_BITS);
                // if j = 0, for instance, we want this offset to give us the *high*
                // bits of the byte
                u8 pixel_offset = 8 - (j + 1) * N_PIXEL_BITS;
                u8 coloridx = (sprite_bytes[i] >> pixel_offset) & mask;

                u8 priority_val = ((sprite_bytes[i] >> pixel_offset) >> N_PALETTE_BITS) & priority_mask;

                u8 pixelx;
                if (!horiz_flip) {
                    // calculate our x position. also mod by 256 so we wrap around
                    pixelx = (x + i * (8 / N_PIXEL_BITS) + j) % 256;
                } else {
                    // calculate our x position but do it backwards tile-wise
                    pixelx = (x + 7 - (i * (8 / N_PIXEL_BITS) + j)) % 256;
                }

                if (pixelx >= 0 && pixelx < SCRW && coloridx != 0
                        && base_priority + priority_val * 2 > line_priorities[pixelx]) {
                    line_colors[pixelx] = sprite_palettes[palette][coloridx];
                    line_priorities[pixelx] = base_priority + priority_val * 2;
                }
            }
        }
    }

    // Front tile layer
    // Which row of tiles are we drawing?
    int fg_row_num = (((line_num + I->ppu->fg_v_offset) / 8) % 32 + 32) % 32;
    // Which row of that row are we drawing? (i.e. y=0-7)
    u8 fg_tile_row = ((line_num + I->ppu->fg_v_offset) % 8 + 8) % 8;
    for (int t = 0; t < row_width; t++) {
        // Get the bytes that describe our tile
        u8 info = I->ppu->fg_map_data[fg_row_num * row_width * 2 + t * 2];
        u16 idx = I->ppu->fg_map_data[fg_row_num * row_width * 2 + t * 2 + 1];

        int horiz_flip = 0, vert_flip = 0;
        if (info & 0x1) idx += 256;
        if (info & 0x4) vert_flip = 1;
        if (info & 0x8) horiz_flip = 1;

        u8 palette = (info & 0xe0) >> 5;

        u8 tile_row = vert_flip ? 7 - fg_tile_row : fg_tile_row;

        // get the appropriate row of the tile (4 bytes)
        u8 *tile_bytes = &I->ppu->pattern_table[idx * SPRITE_BYTES
                                                + tile_row * (SPRITE_BYTES / SPRITE_HEIGHT)];

        int x = t * SPRITE_WIDTH - I->ppu->fg_h_offset;

        // SPRITE_BYTES / SPRITE_HEIGHT = # of bytes per sprite row.
        // so i iterates over each byte of this row of the sprite.
        for (int i = 0; i < SPRITE_BYTES / SPRITE_HEIGHT; i++) {
            // 8 / N_PIXEL_BITS = how many pixels are packed into
            // a byte. so j iterates over each pixel of the current
            // sprite byte
            for (int j = 0; j < 8 / N_PIXEL_BITS; j++) {
                // ~0 = all 1's; ~0 << NPB = all 1's except w/ NPB 0's at the end
                // ~(~0 << NPB) = all 0's but with NPB 1's at the end
                u8 mask = ~(~0 << N_PALETTE_BITS);
                // mask out priority bits
                u8 priority_mask = ~(~0 << N_PRIORITY_BITS);
                // if j = 0, for instance, we want this offset to give us the *high*
                // bits of the byte
                u8 pixel_offset = 8 - (j + 1) * N_PIXEL_BITS;
                u8 coloridx = (tile_bytes[i] >> pixel_offset) & mask;

                u8 priority_val = ((tile_bytes[i] >> pixel_offset) >> N_PALETTE_BITS) & priority_mask;

                u8 pixelx;
                if (!horiz_flip) {
                    // calculate our x position. also mod by 256 so we wrap around
                    pixelx = (x + i * (8 / N_PIXEL_BITS) + j) % 256;
                } else {
                    // calculate our x position but do it backwards tile-wise
                    pixelx = (x + 7 - (i * (8 / N_PIXEL_BITS) + j)) % 256;
                }


                if (pixelx >= 0 && pixelx < SCRW && coloridx != 0
                        && line_priorities[pixelx] < 4 + priority_val * 2) {
                    line_colors[pixelx] = tile_palettes[palette][coloridx];
                    line_priorities[pixelx] = 4 + priority_val * 2;
                }
            }
        }
    }

    for (int i = 0; i < SCRW; i++) {
        u8 r = (line_colors[i] >> 16) & 0xff;
        u8 g = (line_colors[i] >>  8) & 0xff;
        u8 b = (line_colors[i] >>  0) & 0xff;
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderDrawPoint(renderer, i, line_num);
    }
}

void draw(interp *I) {
    SDL_SetRenderTarget(renderer, texture);
    SDL_RenderClear(renderer);

    for (int y = 0; y < SCRH; y++) {
        scanline(I, y);
        if (interrupt(I, HBLANK_INTERRUPT)) {
            while (!(I->flags & INTERRUPT_ENABLE_NEXT) && (I->flags & RUN_FLAG)) {
                do_instr(I);
            }
            I->flags |= INTERRUPT_ENABLE;
            I->flags &= ~INTERRUPT_ENABLE_NEXT;
        }
    }
    SDL_SetRenderTarget(renderer, NULL);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

void init_ppu(ppu *p) {
    p->sprite_h_offset = 0;
    p->sprite_v_offset = 0;
    p->bg_h_offset = 0;
    p->bg_v_offset = 0;
    p->fg_h_offset = 0;
    p->fg_v_offset = 0;

    for (int i = 0; i < 256; i++) {
        p->palette_data[i] = 0xFF;
    }

    for (int i = 0; i < 2048; i++) {
        p->bg_map_data[i] = 0xFF;
        p->fg_map_data[i] = 0xFF;
    }

    for (int i = 0; i < 1024; i++) {
        p->oam[i] = 0x00;
    }

    p->pattern_offset = 0x00;

    for (int i = 0; i < 0x4000; i++) {
        p->pattern_table[i] = 0x00;
    }
}
