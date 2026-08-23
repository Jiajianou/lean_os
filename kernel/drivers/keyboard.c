#include "keyboard.h"

#include <stdint.h>

#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "input.h" /* system_api/include/input.h - KBD_MOD_* bits, M32 */

#define PS2_DATA_PORT 0x60
#define KEYBOARD_IRQ  1

#define SCANCODE_LSHIFT       0x2A
#define SCANCODE_RSHIFT       0x36
#define SCANCODE_LCTRL        0x1D /* M32 - see keyboard_modifiers */
#define SCANCODE_LALT         0x38
#define SCANCODE_RELEASE_BIT  0x80

/* M33: the four arrow keys, and only the four arrow keys, out of the
 * whole 0xE0-prefixed extended-scancode space (numpad Enter, right Ctrl/
 * Alt, media keys, ... none of that is decoded - see this file's own
 * header comment). Each arrives as two interrupts: 0xE0 itself, then the
 * real code byte on the very next one - extended_prefix (below) is what
 * ties those two together. */
#define SCANCODE_EXTENDED_PREFIX 0xE0
#define SCANCODE_EXT_UP           0x48
#define SCANCODE_EXT_LEFT         0x4B
#define SCANCODE_EXT_RIGHT        0x4D
#define SCANCODE_EXT_DOWN         0x50

/* Scancode set 1 make codes, index = scancode, 0 = no ASCII mapping
 * (modifiers, unmapped/extended keys). */
static const char unshifted_table[0x3A] = {
    /* 0x00 */ 0,    27,  '1', '2', '3', '4', '5', '6', '7', '8',
    /* 0x0A */ '9',  '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    /* 0x14 */ 't',  'y', 'u', 'i', 'o',  'p',  '[', ']', '\n', 0,
    /* 0x1E */ 'a',  's', 'd', 'f', 'g',  'h',  'j', 'k', 'l', ';',
    /* 0x28 */ '\'', '`', 0,   '\\', 'z', 'x',  'c', 'v', 'b', 'n',
    /* 0x32 */ 'm',  ',', '.', '/', 0,   '*',  0,   ' ',
};

static const char shifted_table[0x3A] = {
    /* 0x00 */ 0,    27,  '!', '@', '#', '$', '%', '^', '&', '*',
    /* 0x0A */ '(',  ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    /* 0x14 */ 'T',  'Y', 'U', 'I', 'O',  'P',  '{', '}', '\n', 0,
    /* 0x1E */ 'A',  'S', 'D', 'F', 'G',  'H',  'J', 'K', 'L', ':',
    /* 0x28 */ '"',  '~', 0,   '|', 'Z',  'X',  'C', 'V', 'B', 'N',
    /* 0x32 */ 'M',  '<', '>', '?', 0,   '*',  0,   ' ',
};

#define BUFFER_SIZE 256
static char buffer[BUFFER_SIZE];
static volatile uint32_t buf_head; /* next slot to write */
static volatile uint32_t buf_tail; /* next slot to read */

static volatile int shift_held;
static volatile int ctrl_held; /* M32 */
static volatile int alt_held;  /* M32 */
static volatile int extended_prefix; /* M33: set by a bare 0xE0 byte, consumed by the very next one */

static void buffer_push(char c) {
    uint32_t next = (buf_head + 1) % BUFFER_SIZE;
    if (next == buf_tail) {
        return; /* full: drop the character rather than overwrite unread input */
    }
    buffer[buf_head] = c;
    buf_head = next;
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
                    buffer_push((char)KBD_KEY_UP);
                    break;
                case SCANCODE_EXT_DOWN:
                    buffer_push((char)KBD_KEY_DOWN);
                    break;
                case SCANCODE_EXT_LEFT:
                    buffer_push((char)KBD_KEY_LEFT);
                    break;
                case SCANCODE_EXT_RIGHT:
                    buffer_push((char)KBD_KEY_RIGHT);
                    break;
                default:
                    break; /* an extended key this driver doesn't decode - ignored, not buffered */
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
    if (released || code >= sizeof(unshifted_table)) {
        return;
    }

    char c = shift_held ? shifted_table[code] : unshifted_table[code];
    if (c) {
        buffer_push(c);
    }
}

void keyboard_init(void) {
    buf_head = 0;
    buf_tail = 0;
    shift_held = 0;
    ctrl_held = 0;
    alt_held = 0;
    extended_prefix = 0;
    irq_register_handler(KEYBOARD_IRQ, keyboard_irq);
    pic_clear_mask(KEYBOARD_IRQ);
}

int keyboard_modifiers(void) {
    int mods = 0;
    if (ctrl_held) {
        mods |= KBD_MOD_CTRL;
    }
    if (alt_held) {
        mods |= KBD_MOD_ALT;
    }
    if (shift_held) {
        mods |= KBD_MOD_SHIFT;
    }
    return mods;
}

int keyboard_read(void) {
    if (buf_tail == buf_head) {
        return -1;
    }
    char c = buffer[buf_tail];
    buf_tail = (buf_tail + 1) % BUFFER_SIZE;
    return (int)(unsigned char)c;
}
