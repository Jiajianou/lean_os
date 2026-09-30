#include "keyboard.h"

#include "scheduler/scheduler.h"

#include <stdint.h>

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "drivers/kernel_log.h"
#include "input.h"
#include "ps2_controller.h"
#include "scheduler/scheduler_diagnostics.h"

#define PS2_DATA_PORT 0x60
#define KEYBOARD_IRQ  1

#define SCANCODE_LSHIFT       0x2A
#define SCANCODE_RSHIFT       0x36
#define SCANCODE_LCTRL        0x1D
#define SCANCODE_LALT         0x38
#define SCANCODE_D            0x20
#define SCANCODE_RELEASE_BIT  0x80

#define SCANCODE_F1  0x3B
#define SCANCODE_F10 0x44
#define SCANCODE_F11 0x57
#define SCANCODE_F12 0x58

#define SCANCODE_EXTENDED_PREFIX 0xE0
#define SCANCODE_EXT_UP           0x48
#define SCANCODE_EXT_LEFT         0x4B
#define SCANCODE_EXT_RIGHT        0x4D
#define SCANCODE_EXT_DOWN         0x50

static const char unshifted_table[0x3A] = {
      0,    27,  '1', '2', '3', '4', '5', '6', '7', '8',
      '9',  '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
      't',  'y', 'u', 'i', 'o',  'p',  '[', ']', '\n', 0,
      'a',  's', 'd', 'f', 'g',  'h',  'j', 'k', 'l', ';',
      '\'', '`', 0,   '\\', 'z', 'x',  'c', 'v', 'b', 'n',
      'm',  ',', '.', '/', 0,   '*',  0,   ' ',
};

static const char shifted_table[0x3A] = {
      0,    27,  '!', '@', '#', '$', '%', '^', '&', '*',
      '(',  ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
      'T',  'Y', 'U', 'I', 'O',  'P',  '{', '}', '\n', 0,
      'A',  'S', 'D', 'F', 'G',  'H',  'J', 'K', 'L', ':',
      '"',  '~', 0,   '|', 'Z',  'X',  'C', 'V', 'B', 'N',
      'M',  '<', '>', '?', 0,   '*',  0,   ' ',
};

#define BUFFER_SIZE 256
static char buffer[BUFFER_SIZE];
static uint8_t mods_buffer[BUFFER_SIZE];
static volatile int last_read_mods;
static volatile uint32_t buffer_head;
static volatile uint32_t buffer_tail;

static volatile int shift_held;
static volatile int ctrl_held;
static volatile int alt_held;
static volatile int extended_prefix;

static int current_modifiers(void) {
    int mods = 0;
    if (ctrl_held) {
        mods |= KEYBOARD_MOD_CTRL;
    }
    if (alt_held) {
        mods |= KEYBOARD_MOD_ALT;
    }
    if (shift_held) {
        mods |= KEYBOARD_MOD_SHIFT;
    }
    return mods;
}

static void buffer_push(char c) {
    uint32_t next = (buffer_head + 1) % BUFFER_SIZE;
    if (next == buffer_tail) {
        return;
    }
    buffer[buffer_head] = c;
    mods_buffer[buffer_head] = (uint8_t)current_modifiers();
    buffer_head = next;
    scheduler_wake_all(SCHEDULER_KEYBOARD_CHAN);
    scheduler_wake_object(SCHEDULER_INPUT_OBJECT);
}

static void keyboard_irq(isr_regs_t *regs) {
    (void)regs;
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == SCANCODE_EXTENDED_PREFIX) {
        extended_prefix = 1;
        return;
    }
    if (extended_prefix) {
        extended_prefix = 0;
        uint8_t ext_code = scancode & (uint8_t)~SCANCODE_RELEASE_BIT;
        int ext_released = (scancode & SCANCODE_RELEASE_BIT) != 0;
        if (!ext_released) {
            switch (ext_code) {
                case SCANCODE_EXT_UP:
                    buffer_push((char)KEYBOARD_KEY_UP);
                    break;
                case SCANCODE_EXT_DOWN:
                    buffer_push((char)KEYBOARD_KEY_DOWN);
                    break;
                case SCANCODE_EXT_LEFT:
                    buffer_push((char)KEYBOARD_KEY_LEFT);
                    break;
                case SCANCODE_EXT_RIGHT:
                    buffer_push((char)KEYBOARD_KEY_RIGHT);
                    break;
                default:
                    break;
            }
        }
        return;
    }

    uint8_t code = scancode & (uint8_t)~SCANCODE_RELEASE_BIT;
    int released = (scancode & SCANCODE_RELEASE_BIT) != 0;

    if (code == SCANCODE_LSHIFT || code == SCANCODE_RSHIFT) {
        shift_held = !released;
        return;
    }
    if (code == SCANCODE_LCTRL) {
        ctrl_held = !released;
        return;
    }
    if (code == SCANCODE_LALT) {
        alt_held = !released;
        return;
    }
    if (!released) {
        if (code >= SCANCODE_F1 && code <= SCANCODE_F10) {
            buffer_push((char)KEYBOARD_KEY_FUNCTION(1 + (code - SCANCODE_F1)));
            return;
        }
        if (code == SCANCODE_F11) {
            buffer_push((char)KEYBOARD_KEY_FUNCTION(11));
            return;
        }
        if (code == SCANCODE_F12) {
            buffer_push((char)KEYBOARD_KEY_FUNCTION(12));
            return;
        }
    }

    if (released || code >= sizeof(unshifted_table)) {
        return;
    }
    /* Answered here, in the interrupt, rather than by any program: on a machine
       with no serial port it is the only way to ask a frozen desktop what it is doing. */
    if (code == SCANCODE_D && ctrl_held && alt_held) {
        scheduler_diagnostics_toggle();
        return;
    }

    char c = shift_held ? shifted_table[code] : unshifted_table[code];
    if (c) {
        buffer_push(c);
    }
}

void keyboard_init(void) {
    buffer_head = 0;
    buffer_tail = 0;
    shift_held = 0;
    ctrl_held = 0;
    alt_held = 0;
    last_read_mods = 0;
    extended_prefix = 0;

    uint32_t present = ps2_controller_init();

    irq_register_handler(KEYBOARD_IRQ, keyboard_irq);
    if (present & PS2_KEYBOARD_PORT_PRESENT) {
        irq_enable_line(KEYBOARD_IRQ);
    } else {
        kernel_log_puts("[kbd] no PS/2 keyboard port - IRQ1 left masked. A USB keyboard "
                   "reaches the same ring buffer through the xHCI driver.\n");
    }
}

int keyboard_modifiers(void) {
    return last_read_mods;
}

void keyboard_inject(char ch, int mods) {
    uint32_t next = (buffer_head + 1) % BUFFER_SIZE;
    if (next == buffer_tail) {
        return;
    }
    buffer[buffer_head] = ch;
    mods_buffer[buffer_head] = (uint8_t)mods;
    buffer_head = next;
    scheduler_wake_all(SCHEDULER_KEYBOARD_CHAN);
    scheduler_wake_object(SCHEDULER_INPUT_OBJECT);
}

int keyboard_peek(void) {
    return buffer_tail != buffer_head;
}

int keyboard_read(void) {
    if (buffer_tail == buffer_head) {
        return -1;
    }
    char c = buffer[buffer_tail];
    last_read_mods = (int)mods_buffer[buffer_tail];
    buffer_tail = (buffer_tail + 1) % BUFFER_SIZE;
    return (int)(unsigned char)c;
}
