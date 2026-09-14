#ifndef AOS_CONSOLE_INPUT_H
#define AOS_CONSOLE_INPUT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <contracts/cc_contract.h>

/* Common translation for CC and compatibility VMM console callers. */
static inline bool aos_console_input_event_to_byte(uint32_t event_type, uint32_t keycode,
                                       uint8_t *byte)
{
    if (byte == NULL || event_type != CC_INPUT_KEY_DOWN) return false;

    if ((keycode & 0xffffff00u) == 0x100u) {
        *byte = (uint8_t)(keycode & 0xffu);
        return true;
    }
    if (keycode >= 0x04u && keycode <= 0x1du) {
        *byte = (uint8_t)('a' + (keycode - 0x04u));
        return true;
    }
    if (keycode >= 0x1eu && keycode <= 0x26u) {
        *byte = (uint8_t)('1' + (keycode - 0x1eu));
        return true;
    }

    switch (keycode) {
    case 0x27u: *byte = '0'; return true;
    case 0x28u: *byte = '\r'; return true;
    case 0x29u: *byte = 0x1bu; return true;
    case 0x2au: *byte = 0x7fu; return true;
    case 0x2bu: *byte = '\t'; return true;
    case 0x2cu: *byte = ' '; return true;
    case 0x2du: *byte = '-'; return true;
    case 0x2eu: *byte = '='; return true;
    case 0x2fu: *byte = '['; return true;
    case 0x30u: *byte = ']'; return true;
    case 0x31u: *byte = '\\'; return true;
    case 0x33u: *byte = ';'; return true;
    case 0x34u: *byte = '\''; return true;
    case 0x35u: *byte = '`'; return true;
    case 0x36u: *byte = ','; return true;
    case 0x37u: *byte = '.'; return true;
    case 0x38u: *byte = '/'; return true;
    default: return false;
    }
}
#endif
