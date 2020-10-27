/**
 * atmega2560+max3421e usb hid to amiga 1000/500/2000 keyboard adapter.
 * adapts usbhid keyboards to output kbclock/kbdata signals suitable for
 * most of the 68000-based devices (excluding the a600). might be
 * suitable for cdtv; i don't know, i don't have one.
 *
 * nota bene:
 * - the atmega2560 is very likely overkill for this job - a 328/32u4
 *   will probably do the job just fine; i may adapt this in future
 * - i intended to write this without the arduino libraries, but the
 *   excellent usb host shield library uses it copiously and i suspect
 *   the dependencies are inextricable - i have caved in and accepted
 *   this, this is my life now
 * - the (overkill) atmega2560 may serve a useful purpose in future if
 *   i can bring in additional features: floppy drive emulation, also
 *   mouse and joystick with some form of intelligent switching between
 *   joystick and mouse, but the mega adk is a convenient and
 *   inexpensive board with usb and the correct voltage level, so it
 *   filled a requirement
 * - the code doesn't check that malloc is successful; i'm making a lot
 *   of assumptions here but we're allocating small amounts of ram so i
 *   think we'll live (just about)
 *
 * license: GPL-2
 * author: nine <nine@aphlor.org>
 * homepage: https://github.com/borb/amigahid
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <hidcomposite.h>
#include <usbhub.h>
#include <SPI.h>
#include <stdio.h>
#include <stdarg.h>

extern "C"
{
#   include "uart.h"
}

// debug
#ifndef DEBUG_USB
#   define DEBUG_USB       0x00 // 0xff for maximum, 0x00 for off
#endif

/**
 * arduino pins we're going to use (@todo what to do with floppy & power/filter in future?).
 * if you change these, don't forget to update the port and data direction register to the corresponding
 * port, e.g. DB0/PORTB/DDRB
 */
#define AMIGAHW_CLOCK   PL0
#define AMIGAHW_CLOCK_PORT \
                        PORTL
#define AMIGAHW_CLOCK_DIRREG \
                        DDRL

#define AMIGAHW_DATA    PL2
#define AMIGAHW_DATA_PORT \
                        PORTL
#define AMIGAHW_DATA_DIRREG \
                        DDRL

#define AMIGAHW_RESET   PL4
#define AMIGAHW_RESET_PORT \
                        PORTL
#define AMIGAHW_RESET_DIRREG \
                        DDRL

// old keyboard hid buffer size
#define HID_BUF_MAX     32

// hid code for menu key
#define HID_MENU_CODE   0x65

// usbhid input modifier bitmap (byte 0 of hid buffer)
#define MOD_LCTRL       0
#define MOD_LSHIFT      1
#define MOD_LALT        2
#define MOD_LWIN        3
#define MOD_RCTRL       4
#define MOD_RSHIFT      5
#define MOD_RALT        6
#define MOD_RWIN        7

// modifier test
#define TEST_MOD(A, B)          (A & (1 << B))

// macro to simplify setting/clearing bits
#define BIT_SET(REGISTER, BIT)      REGISTER |= (1 << BIT)
#define BIT_CLEAR(REGISTER, BIT)    REGISTER &= ~(1 << BIT)

// amiga keycodes (transcribed from amiga developer cd 2.1)
#define AMIGA_BACKTICK  0x00 // backtick / shifted tilde
#define AMIGA_ONE       0x01 // 1 / shifted exclaim
#define AMIGA_TWO       0x02 // 2 / shifted at
#define AMIGA_THREE     0x03 // 3 / shifted hash
#define AMIGA_FOUR      0x04 // 4 / shifted dollar
#define AMIGA_FIVE      0x05 // 5 / shifted percent
#define AMIGA_SIX       0x06 // 6 / shifted caret
#define AMIGA_SEVEN     0x07 // 7 / shifted ampersand
#define AMIGA_EIGHT     0x08 // 8 / shifted asterisk
#define AMIGA_NINE      0x09 // 9 / shifted open parens
#define AMIGA_ZERO      0x0a // 0 / shifted close parens
#define AMIGA_DASH      0x0b // dash / shifted underscore
#define AMIGA_EQUALS    0x0c // equals / shifted plus
#define AMIGA_BACKSLASH 0x0d // backslash / shifted pipe
#define AMIGA_SPARE1    0x0e
#define AMIGA_KPZERO    0x0f
#define AMIGA_Q         0x10
#define AMIGA_W         0x11
#define AMIGA_E         0x12
#define AMIGA_R         0x13
#define AMIGA_T         0x14
#define AMIGA_Y         0x15
#define AMIGA_U         0x16
#define AMIGA_I         0x17
#define AMIGA_O         0x18
#define AMIGA_P         0x19
#define AMIGA_OSQPARENS 0x1a // open square parens / shifted open curly parens
#define AMIGA_CSQPARENS 0x1b // close square parents / shifted close curly parens
#define AMIGA_SPARE2    0x1c
#define AMIGA_KPONE     0x1d
#define AMIGA_KPTWO     0x1e
#define AMIGA_KPTHREE   0x1f
#define AMIGA_A         0x20
#define AMIGA_S         0x21
#define AMIGA_D         0x22
#define AMIGA_F         0x23
#define AMIGA_G         0x24
#define AMIGA_H         0x25
#define AMIGA_J         0x26
#define AMIGA_K         0x27
#define AMIGA_L         0x28
#define AMIGA_SEMICOLON 0x29 // semicolon / shifted colon
#define AMIGA_QUOTE     0x2a // quote / shifted doublequote
#define AMIGA_INTLRET   0x2b // international only, return
#define AMIGA_SPARE3    0x2c
#define AMIGA_KPFOUR    0x2d
#define AMIGA_KPFIVE    0x2e
#define AMIGA_KPSIX     0x2f
#define AMIGA_INTLSHIFT 0x30 // international only, left shift
#define AMIGA_Z         0x31
#define AMIGA_X         0x32
#define AMIGA_C         0x33
#define AMIGA_V         0x34
#define AMIGA_B         0x35
#define AMIGA_N         0x36
#define AMIGA_M         0x37
#define AMIGA_COMMA     0x38 // comma / shifted less than
#define AMIGA_PERIOD    0x39 // period / shifted greater than
#define AMIGA_SLASH     0x3a // slash / shifted question mark
#define AMIGA_SPARE7    0x3b
#define AMIGA_KPPERIOD  0x3c
#define AMIGA_KPSEVEN   0x3d
#define AMIGA_KPEIGHT   0x3e
#define AMIGA_KPNINE    0x3f
#define AMIGA_SPACE     0x40
#define AMIGA_BACKSP    0x41
#define AMIGA_TAB       0x42
#define AMIGA_KPENTER   0x43
#define AMIGA_RETURN    0x44
#define AMIGA_ESC       0x45
#define AMIGA_DELETE    0x46
#define AMIGA_SPARE4    0x47
#define AMIGA_SPARE5    0x48
#define AMIGA_SPARE6    0x49
#define AMIGA_KPDASH    0x4a
// 0x4b absent
#define AMIGA_UP        0x4c
#define AMIGA_DOWN      0x4d
#define AMIGA_RIGHT     0x4e
#define AMIGA_LEFT      0x4f
#define AMIGA_F1        0x50
#define AMIGA_F2        0x51
#define AMIGA_F3        0x52
#define AMIGA_F4        0x53
#define AMIGA_F5        0x54
#define AMIGA_F6        0x55
#define AMIGA_F7        0x56
#define AMIGA_F8        0x57
#define AMIGA_F9        0x58
#define AMIGA_F10       0x59
#define AMIGA_KPOPAREN  0x5a // open bracket
#define AMIGA_KPCPAREN  0x5b
#define AMIGA_KPSLASH   0x5c
#define AMIGA_KPAST     0x5d // asterisk abbreviated
#define AMIGA_KPPLUS    0x5e
#define AMIGA_HELP      0x5f
#define AMIGA_LSHIFT    0x60 // modifier
#define AMIGA_RSHIFT    0x61 // modifier
#define AMIGA_CAPSLOCK  0x62 // modifier
#define AMIGA_CTRL      0x63 // modifier
#define AMIGA_LALT      0x64 // modifier
#define AMIGA_RALT      0x65 // modifier
#define AMIGA_LAMIGA    0x66 // modifier
#define AMIGA_RAMIGA    0x67 // modifier
// 0x68 - 0x7f absent (except 0x78)
#define AMIGA_RESET     0x78
#define AMIGA_INITPOWER 0xfd
#define AMIGA_TERMPOWER 0xfe
#define AMIGA_UNKNOWN   0xff

// setReport bitmasks for keyboard status leds
#define REP_NUMLOCK     0x01
#define REP_CAPSLOCK    0x02
#define REP_SCROLLLOCK  0x04

// bInterfaceProtocol constants
#define B_IF_PROTOCOL_KEYBOARD \
                        0x01

/**
 * this layout is very US-centric right now, which may not be a bad thing, but @todo check for non-US maps
 * interesting how hid keyboards are alphabetical, amiga are qwerty layout. actually not interesting at all.
 */
static const uint8_t mapHidToAmiga[256] = {
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_A,         AMIGA_B,         AMIGA_C,         AMIGA_D,         // 0x00 (position of first key on line)
    AMIGA_E,         AMIGA_F,         AMIGA_G,         AMIGA_H,         AMIGA_I,         AMIGA_J,         AMIGA_K,         AMIGA_L,         // 0x08
    AMIGA_M,         AMIGA_N,         AMIGA_O,         AMIGA_P,         AMIGA_Q,         AMIGA_R,         AMIGA_S,         AMIGA_T,         // 0x10
    AMIGA_U,         AMIGA_V,         AMIGA_W,         AMIGA_X,         AMIGA_Y,         AMIGA_Z,         AMIGA_ONE,       AMIGA_TWO,       // 0x18
    AMIGA_THREE,     AMIGA_FOUR,      AMIGA_FIVE,      AMIGA_SIX,       AMIGA_SEVEN,     AMIGA_EIGHT,     AMIGA_NINE,      AMIGA_ZERO,      // 0x20
    AMIGA_RETURN,    AMIGA_ESC,       AMIGA_BACKSP,    AMIGA_TAB,       AMIGA_SPACE,     AMIGA_DASH,      AMIGA_EQUALS,    AMIGA_OSQPARENS, // 0x28
    AMIGA_CSQPARENS, AMIGA_BACKSLASH, AMIGA_UNKNOWN,   AMIGA_SEMICOLON, AMIGA_QUOTE,     AMIGA_BACKTICK,  AMIGA_COMMA,     AMIGA_PERIOD,    // 0x30
    AMIGA_SLASH,     AMIGA_CAPSLOCK,  AMIGA_F1,        AMIGA_F2,        AMIGA_F3,        AMIGA_F4,        AMIGA_F5,        AMIGA_F6,        // 0x38
    AMIGA_F7,        AMIGA_F8,        AMIGA_F9,        AMIGA_F10,       AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x40
    AMIGA_UNKNOWN,   AMIGA_HELP,      AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_DELETE,    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_RIGHT,     // 0x48
    AMIGA_LEFT,      AMIGA_DOWN,      AMIGA_UP,        AMIGA_UNKNOWN,   AMIGA_KPSLASH,   AMIGA_KPAST,     AMIGA_KPDASH,    AMIGA_KPPLUS,    // 0x50
    AMIGA_KPENTER,   AMIGA_KPONE,     AMIGA_KPTWO,     AMIGA_KPTHREE,   AMIGA_KPFOUR,    AMIGA_KPFIVE,    AMIGA_KPSIX,     AMIGA_KPSEVEN,   // 0x58
    AMIGA_KPEIGHT,   AMIGA_KPNINE,    AMIGA_KPZERO,    AMIGA_KPPERIOD,  AMIGA_UNKNOWN,   AMIGA_RAMIGA,    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x60
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x68
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x70
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x78
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x80
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x88
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x90
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0x98
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xa0
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xa8
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xb0
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xb8
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xc0
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xc8
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xd0
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xd8
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xe0
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xe8
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   // 0xf0
    AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN,   AMIGA_UNKNOWN    // 0xf8
};

enum SYNC_STATE { IDLE, SYNC };
volatile uint8_t sync_state = IDLE;

// this interrupt service routine raises and drops the sync signal so the amiga knows the keyboard is still there
ISR(TIMER1_COMPA_vect)
{
    // isr doesn't run until TIMER1 is setup in AmigaHID
    BIT_CLEAR(AMIGAHW_DATA_PORT, AMIGAHW_DATA);
    sync_state = SYNC;
}

// extend HIDComposite, replace SelectInterface & ParseHIDData to select & process keyboards
class AmigaHID : public HIDComposite
{
    uint8_t old_buf_len;
    uint8_t *old_buf;
    bool caps_lock;

    public:
        AmigaHID(USB *p) : HIDComposite(p) {};
        void Setup(USB *p);

    protected:
        void ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf);
        bool SelectInterface(uint8_t iface, uint8_t proto);

    private:
        void DebugPrint(const char *fmt, ...);
        void SendAmiga(uint8_t keycode);
        bool KeyInBuffer(uint8_t code, uint8_t len, uint8_t *buf);
        void InitiateAmigaReset();
        void EndAmigaReset();
        bool TrinityCheck(uint8_t len, uint8_t *buf);
};

// set the board up before we start
void AmigaHID::Setup(USB *p)
{
    old_buf_len = 8;
    old_buf = malloc(HID_BUF_MAX);
    uint8_t i;

    for (i = 0; i < 8; i++)
        old_buf[i] = 0;

    // sort out the amiga-side ports & issue reset before getting messy with serial & usb
    noInterrupts();

    AMIGAHW_CLOCK_DIRREG = 0;
    AMIGAHW_CLOCK_PORT = 0;
    AMIGAHW_DATA_DIRREG = 0;
    AMIGAHW_DATA_PORT = 0;
    AMIGAHW_RESET_DIRREG = 0;
    AMIGAHW_RESET_PORT = 0;

    BIT_SET(AMIGAHW_CLOCK_DIRREG, AMIGAHW_CLOCK);
    BIT_SET(AMIGAHW_DATA_DIRREG, AMIGAHW_DATA);
    BIT_SET(AMIGAHW_RESET_DIRREG, AMIGAHW_RESET);

    BIT_SET(AMIGAHW_CLOCK_PORT, AMIGAHW_CLOCK);
    BIT_SET(AMIGAHW_DATA_PORT, AMIGAHW_DATA);
    BIT_SET(AMIGAHW_RESET_PORT, AMIGAHW_RESET);

    /**
     * setup the amiga keyboard sync signal timer; TIMER1 is used because it's 16-bit
     * (thanks again for t33bu's wireless-amiga-keyboard for avr-side logic)
     */
    BIT_SET(TCCR1B, WGM12);
    BIT_SET(TIMSK1, OCIE1A);
    OCR1A = 0x3d09;
    BIT_SET(TCCR1B, CS12);
    BIT_SET(TCCR1B, CS10);

    // restart interrupts, and the sync signal timer should start
    interrupts();

    // send the amiga the startup notifications (thanks t33bu!)
    _delay_ms(1000);
    SendAmiga(AMIGA_INITPOWER);
    _delay_us(200);
    SendAmiga(AMIGA_TERMPOWER);

#ifdef DEBUG
    DebugPrint("Amiga HID adapter for Arduino ADK/MAX3421E by nine https://github.com/borb/amigahid\n");
    DebugPrint("Starting in debug mode.\n");
#endif

    if (p->Init() == -1) {
        DebugPrint("USB did not start successfully - aborting.\n");
        abort(); // does avr-libc abort? does it just while(1){} ?
    }

    UsbDEBUGlvl = DEBUG_USB;

    // caps lock defaults to off
    caps_lock = false;

    // delay whilst devices enumerate (may not be needed)
    _delay_ms(200);
}

// print out debug messages
void AmigaHID::DebugPrint(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

#ifdef DEBUG
    vprintf(fmt, args);
#endif

    va_end(args);
}

// select only keyboards for data
bool AmigaHID::SelectInterface(uint8_t iface, uint8_t proto)
{
    /**
     * bInterfaceProtocol 1 is keyboard, 2 is mouse; some keyboards have a mouse controller even if it's
     * never used
     */
    if (proto == B_IF_PROTOCOL_KEYBOARD) {
        DebugPrint("HID keyboard attached\n");
        return true;
    }

    // reject everything if it's not a keyboard
    DebugPrint("HID device attached and ignored (not keyboard)\n");
    return false;
}

// send a keycode to the amiga
void AmigaHID::SendAmiga(uint8_t keycode)
{
    /**
     * again, huge thanks to t33bu for the wireless-amiga-keyboard source. so useful.
     * this is paraphrased somewhat, since i'm not left shifting before reaching here.
     */
    uint8_t i, bit = 0x80, skeycode;

    // check for unknown keycode and ignore
    if (keycode == AMIGA_UNKNOWN) {
        DebugPrint("Cowardly refusing to send unknown keycode to Amiga.\n");
        return;
    }

    // roll keycode left, moving bit 7 to bit 0 if needed
    skeycode = keycode;
    skeycode <<= 1;
    if (keycode & 0x80)
        skeycode |= 1;

#ifdef DEBUG
    DebugPrint("Sending 0x%02x: ", keycode);

    if (keycode & 0x80)
        DebugPrint("keyup\n");
    else
        DebugPrint("keydown\n");
#endif

    /**
     * this explains things very well:
     * https://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node0173.html
     * (if link is dead, amiga dev cd 2.1 keyboard timing diagram)
     */
    for (i = 0; i < 8; i++) {
        if (skeycode & bit)
            BIT_CLEAR(AMIGAHW_DATA_PORT, AMIGAHW_DATA);
        else
            BIT_SET(AMIGAHW_DATA_PORT, AMIGAHW_DATA);

        _delay_us(20); // hold keyboard data line low for 20us before sending clock
        BIT_CLEAR(AMIGAHW_CLOCK_PORT, AMIGAHW_CLOCK);
        _delay_us(20); // hold clock low for 20us
        BIT_SET(AMIGAHW_CLOCK_PORT, AMIGAHW_CLOCK);
        _delay_us(50); // hold clock high for 50us

        // rshift bit before next iteration
        bit >>= 1;
    }

    BIT_SET(AMIGAHW_DATA_PORT, AMIGAHW_DATA);
    BIT_CLEAR(AMIGAHW_DATA_DIRREG, AMIGAHW_DATA);   // set data line to input
    _delay_ms(5); // handshake wait up to 143ms
    BIT_SET(AMIGAHW_DATA_DIRREG, AMIGAHW_DATA);     // set data line to output
}

// called on each packet event returned
void AmigaHID::ParseHIDData(USBHID *hid, uint8_t ep, bool is_rpt_id, uint8_t len, uint8_t *buf)
{
    uint8_t i, translated_code, leds;
    bool caps_trap;

    // i hate caps lock so much
    caps_trap = false;

    // process the buffer contents
    if (len && buf)  {
        // first byte is the modifier bitmap; note that some keys such as menu are not modifiers
        if (buf[0] != old_buf[0]) {
            // modifier state change

            if (TEST_MOD(buf[0], MOD_LALT) && !TEST_MOD(old_buf[0], MOD_LALT)) SendAmiga(AMIGA_LALT); // left alt down
            if (!TEST_MOD(buf[0], MOD_LALT) && TEST_MOD(old_buf[0], MOD_LALT)) SendAmiga(AMIGA_LALT | 0x80); // left alt up

            if (TEST_MOD(buf[0], MOD_RALT) && !TEST_MOD(old_buf[0], MOD_RALT)) SendAmiga(AMIGA_RALT); // right alt down
            if (!TEST_MOD(buf[0], MOD_RALT) && TEST_MOD(old_buf[0], MOD_RALT)) SendAmiga(AMIGA_RALT | 0x80); // right alt up

            if (TEST_MOD(buf[0], MOD_LSHIFT) && !TEST_MOD(old_buf[0], MOD_LSHIFT)) SendAmiga(AMIGA_LSHIFT); // left shift down
            if (!TEST_MOD(buf[0], MOD_LSHIFT) && TEST_MOD(old_buf[0], MOD_LSHIFT)) SendAmiga(AMIGA_LSHIFT | 0x80); // left shift up

            if (TEST_MOD(buf[0], MOD_RSHIFT) && !TEST_MOD(old_buf[0], MOD_RSHIFT)) SendAmiga(AMIGA_RSHIFT); // right shift down
            if (!TEST_MOD(buf[0], MOD_RSHIFT) && TEST_MOD(old_buf[0], MOD_RSHIFT)) SendAmiga(AMIGA_RSHIFT | 0x80); // right shift up

            if (TEST_MOD(buf[0], MOD_LWIN) && !TEST_MOD(old_buf[0], MOD_LWIN)) SendAmiga(AMIGA_LAMIGA); // left windows key down
            if (!TEST_MOD(buf[0], MOD_LWIN) && TEST_MOD(old_buf[0], MOD_LWIN)) SendAmiga(AMIGA_LAMIGA | 0x80); // left windows key up

            /**
             * ctrl is a fickle one because a usb keyboard usually has two and an amiga has one; map both to ctrl
             * states: left or right down and neither previously down. so if either are down when neither /were/ down, ctrl down,
             * and if neither are down and either /were/ down, ctrl up. right? right. i think.
             */
            if ((TEST_MOD(buf[0], MOD_LCTRL) || TEST_MOD(buf[0], MOD_RCTRL)) &&
                (!TEST_MOD(old_buf[0], MOD_LCTRL) && !TEST_MOD(old_buf[0], MOD_RCTRL))) SendAmiga(AMIGA_CTRL); // ctrl down

            if ((!TEST_MOD(buf[0], MOD_LCTRL) && !TEST_MOD(buf[0], MOD_RCTRL)) &&
                (TEST_MOD(old_buf[0], MOD_LCTRL) || TEST_MOD(old_buf[0], MOD_RCTRL))) SendAmiga(AMIGA_CTRL | 0x80); // ctrl up

            /**
             * right windows key is the only modifier we don't handle here, but aside from several apple keyboards,
             * i have no pc keyboards with one. and inexplicably the apple keyboard doesn't work (likely power, plus
             * apple's keyboard is even a pain in the ass to use on actual apple devices owing to REALLY odd
             * proprietary power commands outside of the normal usb descriptor block specification, like idevices).
             * since apple keyboards don't have menu, but they don't work anyway (see previous moan), i am not going
             * to lose any sleep; menu is right-amiga.
             */
        }

        // handle key up events
        for (i = 2; i < old_buf_len; i++) {
            // check if a key in the last buffer iteration is absent from the current iteration, and release it if so
            if (old_buf[i] && !KeyInBuffer(old_buf[i], len, buf)) {
                translated_code = mapHidToAmiga[old_buf[i]];

                if (translated_code == AMIGA_CAPSLOCK) {
                    DebugPrint("Caps lock on up event\n");
                    caps_trap = true;
                }

                if (caps_trap && caps_lock)
                    DebugPrint("Not sending key up event for toggling caps lock on\n");
                else
                    SendAmiga(translated_code | 0x80); // key up
            }
        }

        // handle key down events
        for (i = 2; i < len; i++) {
            // check if a key in the current buffer iteration is absent from the previous iteration, and send down event if so
            if (buf[i] && !KeyInBuffer(buf[i], old_buf_len, old_buf)) {
                translated_code = mapHidToAmiga[buf[i]];

                // check if that key was caps lock and adjust the class property (only on down)
                if (translated_code == AMIGA_CAPSLOCK) {
                    DebugPrint("Caps lock on down event: ");
                    if (caps_lock) {
                        DebugPrint("turning caps lock off\n");
                        caps_lock = false;
                        caps_trap = true;
                    } else {
                        DebugPrint("turning caps lock on\n");
                        caps_lock = true;
                        caps_trap = true;
                    }
                }

                if (caps_trap && !caps_lock)
                    DebugPrint("Not sending key down event for toggling caps lock off\n");
                else
                    SendAmiga(translated_code); // key down
            }
        }

        // change caps lock led on keyboard (amiga has no num/scroll lock leds, so ignore)
        if (caps_trap) {
            DebugPrint("Calling hid::SetReport to update keyboard LED status\n");

            if (caps_lock)
                leds = REP_CAPSLOCK;
            else
                leds = 0;

            // ep, iface, report_type, report_id, nbytes, dataptr
            hid->SetReport(0, 0, 2, 0, 1, &leds);
        }

        /**
         * check for ctrl-amiga-amiga and issue reset.
         * @todo in future, implement "reset warning" as per
         * https://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/node0178.html
         * i notice that linux-m68k on the amiga (waaaay back in the mid 1990s) used to emergency sync in
         * preparation for hard reset then attempt to drag out the reset until the very last ms in order to
         * prevent data loss. i don't actually know if amigaos responds to AMIGA_RESET (0x78). i'd like to think
         * it does (adcd suggests it does).
         */
        if ((TrinityCheck(old_buf_len, old_buf) == false) && (TrinityCheck(len, buf) == true))
            InitiateAmigaReset();
        if ((TrinityCheck(old_buf_len, old_buf) == true) && (TrinityCheck(len, buf) == false))
            EndAmigaReset();

        DebugPrint("[end processing iteration]\n");
    }

    // after processing, store the current buffer iteration so we can use it as a reference for next time
    if (len > HID_BUF_MAX) {
        DebugPrint("FATAL ERROR: HID iteration has buffer exceeding size of HID_BUF_MAX. Aborting all operations.\n");
        abort();
    }
    old_buf_len = len;
    memcpy(old_buf, buf, len); // sure hope size_t can occupy uint8_t
}

bool AmigaHID::TrinityCheck(uint8_t len, uint8_t *buf)
{
    uint8_t counter;

    // (n.b. i'm being epic lazy here and ignoring right control for reset purposes. i am the worst.)
    if (buf[0] & (1 << MOD_LCTRL))
        counter++;
    if (buf[0] & (1 << MOD_LWIN))
        counter++;
    if (KeyInBuffer(HID_MENU_CODE, len, buf))
        counter++;

    return counter == 3;
}

bool AmigaHID::KeyInBuffer(uint8_t code, uint8_t len, uint8_t *buf)
{
    // shortcut early and avoid the loop; len of 2 should never happen though
    if ((len == 0) || (len <= 2))
        return false;

    for (uint8_t i = 2; i < len; i++)
        if (code == buf[i])
            return true;

    return false;
}

// reset key sequence down
void AmigaHID::InitiateAmigaReset()
{
    DebugPrint("*** AMIGA RESET *** holding reset line\n");
    BIT_CLEAR(AMIGAHW_RESET_PORT, AMIGAHW_RESET);
}

// reset key sequence up
void AmigaHID::EndAmigaReset()
{
    DebugPrint("*** AMIGA RESET *** clearing reset line\n");
    BIT_SET(AMIGAHW_RESET_PORT, AMIGAHW_RESET);
}

USB         Usb;
USBHub      Hub(&Usb);
AmigaHID    amigaHid(&Usb);

// usual arduino setup
void setup()
{
#ifdef DEBUG
    // debug is on; init serial
    uart_init();
#endif

    // run setup
    amigaHid.Setup(&Usb);
}

// usual arduino loop
void loop()
{
    // perform usb operations
    Usb.Task();

    // stop sync if one timer iteration has passed
    if ((sync_state == SYNC) && (TCNT1 > 0)) {
        BIT_SET(AMIGAHW_DATA_PORT, AMIGAHW_DATA);
        sync_state = IDLE;
    }
}
