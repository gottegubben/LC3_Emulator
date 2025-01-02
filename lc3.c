#include <stdio.h>
#include <stdint.h>
#include <signal.h>

/* windows only */
#include <Windows.h>
#include <conio.h>  // _kbhit

enum {
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,
    R_COND,
    R_COUNT
};

enum {
    OP_BR = 0, /* Branch */
    OP_ADD,    /* Add */
    OP_LD,     /* Load */
    OP_ST,     /* Store */
    OP_JSR,    /* Jump register */
    OP_AND,    /* Bitwise and */
    OP_LDR,    /* Load register */
    OP_STR,    /* Store register */
    OP_RTI,    /* Unused */
    OP_NOT,    /* Bitwise not */
    OP_LDI,    /* Load indirect */
    OP_STI,    /* Store indirect */
    OP_JMP,    /* Jump */
    OP_RES,    /* Reserved (unused) */
    OP_LEA,    /* Load effective address */
    OP_TRAP    /* Execute trap */
};

enum {
    FL_POS = 1 << 0, /* P */
    FL_ZRO = 1 << 1, /* Z */
    FL_NEG = 1 << 2  /* N */
};

enum
{
    TRAP_GETC  = 0x20,  /* Get character from keyboard, not echoed onto the terminal. */
    TRAP_OUT   = 0x21,  /* Output a character. */
    TRAP_PUTS  = 0x22,  /* Output a word string. */
    TRAP_IN    = 0x23,  /* Get character from keyboard, echoed onto the terminal. */
    TRAP_PUTSP = 0x24,  /* Output a byte string. */
    TRAP_HALT  = 0x25   /* Halt the program. */
};

enum
{
    MR_KBSR = 0xFE00, /* Keyboard status. */
    MR_KBDR = 0xFE02  /* Keyboard data. */
};

#define MEMORY_MAX (1 << 16)
uint16_t memory[MEMORY_MAX]; /* RAM ~ 131 kB. */

uint16_t reg[R_COUNT];       /* Registers. */

/* ------------ WINDOWS SPECIFIC IMPLEMENTATION ------------ */

HANDLE hStdin = INVALID_HANDLE_VALUE;
DWORD fdwMode, fdwOldMode;

void disable_input_buffering()
{
    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(hStdin, &fdwOldMode); /* save old mode */
    fdwMode = fdwOldMode
            ^ ENABLE_ECHO_INPUT  /* no input echo */
            ^ ENABLE_LINE_INPUT; /* return when one or
                                    more characters are available */
    SetConsoleMode(hStdin, fdwMode); /* set new mode */
    FlushConsoleInputBuffer(hStdin); /* clear buffer */
}

void restore_input_buffering()
{
    SetConsoleMode(hStdin, fdwOldMode);
}

uint16_t check_key()
{
    return WaitForSingleObject(hStdin, 1000) == WAIT_OBJECT_0 && _kbhit();
}

/* --------------------------------------------------------- */

void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

uint16_t mem_read(uint16_t address) {
    if (address == MR_KBSR) {
        if (check_key()) {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else {
            memory[MR_KBSR] = 0;
        }
    }
    
    return memory[address];
}

void mem_write(uint16_t address, uint16_t value) {
    memory[address] = value;
}

/* Swaps from big endian to little endian! */
uint16_t swap16(uint16_t x)
{
    return (x << 8) | (x >> 8);
}

uint16_t sign_extend(uint16_t x, int bit_count) {
    if ((x >> (bit_count - 1)) & 1) {
        x |= 0xFFFF << bit_count;
    }
    return x;
}

void read_image_file(FILE* file) {
    /* First, read the origin (where to place the image). */
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    /* Secondly, read the whole image file to memory! */
    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    /* Swap to little endian! */
    while (read-- > 0) {
        *p = swap16(*p);
        ++p;
    }
}

int read_image(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    read_image_file(file);
    fclose(file);
    return 1;
}

void update_flags(uint16_t r) {
    if (reg[r] == 0) {
        reg[R_COND] = FL_ZRO;
    }
    else if (reg[r] >> 15) { /* A 1 in the far left bit spot indicates a negative value. */
        reg[R_COND] = FL_NEG;
    }
    else {
        reg[R_COND] = FL_POS;
    }
}

int main (int argc, const char* argv[]) {
    if (argc < 2) {
        /* Usage string. */
        printf("lc3 [image-file1] ... \n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j) {
        if (!read_image(argv[j])) {
            printf("Failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    /* One conditional flag should be set at any given time, set the Z flag. */
    reg[R_COND] = FL_ZRO;

    /* Set the PC register to the default value 0x3000. */
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;

    while (running) {
        /* Fetch new instruction! */
        uint16_t instr = mem_read(reg[R_PC]++);
        uint16_t op    = instr >> 12; /* The last 4 bits are of value to us for OP code! */

        switch (op) {
            case OP_BR: {
                uint16_t flags = (instr >> 9) & 0x7;

                uint16_t pcOffset9 = sign_extend(instr & 0x1FF, 9);

                if (flags & reg[R_COND]) {
                    reg[R_PC] += pcOffset9;
                }

                break;
            }
            case OP_ADD: {
                /* Destination register DR. */
                uint16_t dr = (instr >> 9) & 0x7;

                /* First operand (SR1). */
                uint16_t sr1 = (instr >> 6) & 0x7;

                uint16_t imm_flag = (instr >> 5) & 0x1;

                if (imm_flag) {
                    uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                    reg[dr] = reg[sr1] + imm5;
                }
                else {
                    /* Second operand (SR2). */
                    uint16_t sr2 = instr & 0x7;
                    reg[dr] = reg[sr1] + reg[sr2];
                }

                update_flags(dr);

                break;
            }
            case OP_LD: {
                /* The destination register. */
                uint16_t dr = (instr >> 9) & 0x7;

                uint16_t pcOffset9 = sign_extend(instr & 0x1FF, 9);

                reg[dr] = mem_read(reg[R_PC] + pcOffset9);

                update_flags(dr);

                break;
            }
            case OP_ST: {
                uint16_t sr = (instr >> 9) & 0x7;

                uint16_t pcOffset9 = sign_extend(instr & 0x1FF, 9);

                mem_write(reg[R_PC] + pcOffset9, reg[sr]);

                break;
            }
            case OP_JSR: {
                /* Save the PC in R7. */
                reg[R_R7] = reg[R_PC];

                if ((instr >> 11) & 0x1) {
                    uint16_t pcOffset11 = sign_extend(instr & 0x7FF, 11);
                    reg[R_PC] += pcOffset11; /* JSR. */
                } 
                else {
                    /* The base register. */
                    uint16_t r = (instr >> 6) & 0x7;
                    reg[R_PC] = reg[r]; /* JSRR. */
                }

                break;
            }
            case OP_AND: {
                /* Destination register (DR). */
                uint16_t dr = (instr >> 9) & 0x7;

                /* First operand (SR1). */
                uint16_t sr1 = (instr >> 6) & 0x7;

                uint16_t imm_flag = (instr >> 5) & 0x1;

                if (imm_flag) {
                    uint16_t imm5 = sign_extend(instr & 0x1F, 5);

                    reg[dr] = reg[sr1] & imm5;
                } 
                else {
                    /* Second operand (SR2). */
                    uint16_t sr2 = instr & 0x7;

                    reg[dr] = reg[sr1] & reg[sr2];
                }
                update_flags(dr);

                break;
            }
            case OP_LDR: {
                /* Destination register. */
                uint16_t dr = (instr >> 9) & 0x7;

                /* Base register (BaseR). */
                uint16_t r  = (instr >> 6) & 0x7;

                uint16_t offset6 = sign_extend(instr & 0x3F, 6);

                reg[dr] = mem_read(reg[r] + offset6);

                update_flags(dr);

                break;
            }
            case OP_STR: {
                uint16_t sr = (instr >> 9) & 0x7;

                uint16_t r  = (instr >> 6) & 0x7;

                uint16_t offset6 = sign_extend(instr & 0x3F, 6);

                mem_write(reg[r] + offset6, reg[sr]);

                break;
            }
            case OP_RTI: {

                /* Unused! */

                break;
            }
            case OP_NOT: {
                uint16_t dr = (instr >> 9) & 0x7;

                uint16_t sr = (instr >> 6) & 0x7;

                /* Bitwise complement. */
                reg[dr] = ~reg[sr];

                update_flags(dr);

                break;
            }
            case OP_LDI: {
                /* The destination register (DR). */
                uint16_t dr = (instr >> 9) & 0x7;

                uint16_t pcOffset9 = sign_extend(instr & 0x1FF, 9);

                reg[dr] = mem_read(mem_read(reg[R_PC] + pcOffset9));

                update_flags(dr);

                break;
            }
            case OP_STI: {
                uint16_t sr = (instr >> 9) & 0x7;

                uint16_t pcOffset9 = sign_extend(instr & 0x1FF, 9);

                mem_write(mem_read(reg[R_PC] + pcOffset9), reg[sr]);

                break;
            }
            case OP_JMP: {
                /* The register containing the jump address. */
                uint16_t r = (instr >> 6) & 0x7;

                reg[R_PC] = reg[r];

                break;
            }
            case OP_RES: {

                /* Reserved (NOP). */

                break;
            }
            case OP_LEA: {
                /* Destination register. */
                uint16_t dr = (instr >> 9) & 0x7;

                uint16_t pcOffset9 = sign_extend(instr & 0x1FF, 9);

                reg[dr] = reg[R_PC] + pcOffset9;

                update_flags(dr);

                break;
            }
            case OP_TRAP: {
                reg[R_R7] = reg[R_PC];

                switch (instr & 0xFF)
                {
                    case TRAP_GETC: {
                        reg[R_R0] = (uint16_t)getchar();
                        
                        update_flags(R_R0);
                        
                        break;
                    }
                    case TRAP_OUT: {
                        putc((char)reg[R_R0], stdout);

                        fflush(stdout);
                                            
                        break;
                    }
                    case TRAP_PUTS: {
                        uint16_t* c = memory + reg[R_R0];

                        while (*c) {
                            putc((char)*c, stdout);
                            ++c;
                        }
                        
                        fflush(stdout);
                        
                        break;
                    }
                    case TRAP_IN: {
                        printf("Enter a character...");

                        char c = getchar();

                        putc(c, stdout);

                        fflush(stdout);

                        reg[R_R0] = (uint16_t)c;

                        update_flags(R_R0);
                        
                        break;
                    }
                    case TRAP_PUTSP: {
                        uint16_t* c = memory + reg[R_R0];

                        while (*c) {
                            uint16_t c1 = *c & 0xFF;
                            uint16_t c2 = *c >> 8;

                            putc(c1, stdout);
                            if (c2) putc(c2, stdout);

                            ++c;
                        }

                        fflush(stdout);
                        
                        break;
                    }
                    case TRAP_HALT: {
                        puts("HALT");

                        fflush(stdout);

                        running = 0;

                        break;
                    }
                    default: {
                        break;
                    }
                }

                break;
            }
            default: {
                abort();

                break;
            }
        }
    }
    restore_input_buffering();
}