/*
 *	source/keyboard.c
 *
 *	PBUS keyboard support for OptiDoom. Talks to the Portfolio Event
 *	Broker directly (parallel to the SDK's InitEventUtility pad/mouse
 *	subscription) and watches for KeyboardKeyPressed / Released /
 *	Update / DataArrived events.
 *
 *	Maps held PS/2 Set 2 scancodes onto the existing Pad* joypad bits
 *	so OptiDoom's existing game logic sees keyboard presses as pad
 *	presses with no game-code changes.
 */

#include "keyboard.h"
#include <event.h>
#include <kernel.h>
#include <kernelnodes.h>
#include <msgport.h>
#include <nodes.h>
#include <item.h>
#include <Portfolio.h>

/* ---- broker subscription state ---- */
static Item    kb_port_item   = -1;
static Item    kb_msg_item    = -1;
static Item    kb_broker_port = -1;
int32   kb_enabled     = 0;  /* exposed for debug instrumentation */
int32   kb_event_count = 0;  /* incremented each time we drain an event */

/* Live 256-bit key matrix from the broker. Bit N set = scancode N
 * held. The upper 128 bits hold E0-extended keys (matrix bit =
 * scancode | 0x80), so arrow keys land at 0xF5 / 0xF2 / 0x6B|0x80
 * / 0x74|0x80 etc. -- the convention the host KeyboardDriver
 * follows. */
static uint32  kb_matrix[8]      = { 0 };
static uint32  kb_matrix_prev[8] = { 0 };

/* One-shot weapon-select request. Set to a weapon enum value when
 * a number-key press transition is detected; consumed and reset by
 * readRequestedWeapon. -1 = no request. */
static int     kb_weapon_req     = -1;

/* PS/2 Set 2 scancodes for the number row 1-7. DOS Doom uses these
 * for direct weapon select: 1=fist/chainsaw, 2=pistol, 3=shotgun,
 * 4=chaingun, 5=rocket, 6=plasma, 7=BFG. */
static const uint8 KB_WEAPON_SCANCODES[7] = {
  0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D
};

/* DOS-style cheat-code typing. We watch every fresh letter-key press,
 * map it to A-Z, push into a 12-char ring buffer, and check if the
 * tail matches any known DOS Doom cheat. Match -> latch the cheat
 * ID for the game to read + dispatch. */
static char kb_cheat_buf[12] = { 0 };
static int  kb_cheat_req     = -1;

/* IDCLEV xx warp: 0-99 = level, -1 = no request. */
static int  kb_warp_req      = -1;

/* +/- one-shot screen-size adjust: +1 grow, -1 shrink, 0 idle. */
static int  kb_screensize_delta = 0;

/* Caps Lock auto-run toggle. 1 = persistent run on. The
 * _just_toggled flag is set on the press transition so the game
 * can display a one-shot status message. */
static int  kb_autorun           = 0;
static int  kb_autorun_just_toggled = 0;

/* PS/2 Set 2 unshifted ASCII for the alphabetic block. Index by
 * matrix bit (scancode); only A-Z scancodes are populated, the rest
 * stay 0 ('NUL' = "not a letter, skip"). */
static const char KB_SC_TO_LETTER[128] = {
  /* 0x00-0x0F */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
  /* 0x10-0x1F */ 0,0,0,0,0,'q','1',0, 0,0,'z','s','a','w','2',0,
  /* 0x20-0x2F */ 0,'c','x','d','e','4','3',0, 0,' ','v','f','t','r','5',0,
  /* 0x30-0x3F */ 0,'n','b','h','g','y','6',0, 0,0,'m','j','u','7','8',0,
  /* 0x40-0x4F */ 0,',','k','i','o','0','9',0, 0,'.','/','l',';','p','-',0,
  /* 0x50-0x5F */ 0,0,'\'',0,'[','=',0,0,         0,0,0,']',0,'\\',0,0,
  /* 0x60-0x6F */ 0,0,0,0,0,0,0,0,                 0,0,0,0,0,0,0,0,
  /* 0x70-0x7F */ 0,0,0,0,0,0,0,0,                 0,0,0,0,0,0,0,0
};

typedef struct {
  const char *code;
  int         id;
} CheatEntry;

static const CheatEntry KB_CHEATS[] = {
  { "iddqd",    KBD_CHEAT_IDDQD     },
  { "idkfa",    KBD_CHEAT_IDKFA     },
  { "idfa",     KBD_CHEAT_IDFA      },
  { "idclip",   KBD_CHEAT_IDCLIP    },
  { "iddt",     KBD_CHEAT_IDDT      },
  { "idmypos",  KBD_CHEAT_IDMYPOS   },
  { "idbeholdv",KBD_CHEAT_BEHOLD_V  },
  { "idbeholds",KBD_CHEAT_BEHOLD_S  },
  { "idbeholdi",KBD_CHEAT_BEHOLD_I  },
  { "idbeholdr",KBD_CHEAT_BEHOLD_R  },
  { "idbeholda",KBD_CHEAT_BEHOLD_A  },
  { "idbeholdl",KBD_CHEAT_BEHOLD_L  }
};
#define KB_CHEAT_COUNT (sizeof KB_CHEATS / sizeof KB_CHEATS[0])

static int kb_str_endswith(const char *buf, const char *suffix)
{
  int bl = 0, sl = 0, i;
  while (buf[bl])    bl++;
  while (suffix[sl]) sl++;
  if (sl > bl) return 0;
  for (i = 0; i < sl; i++) {
    if (buf[bl - sl + i] != suffix[i]) return 0;
  }
  return 1;
}

static void kb_cheat_push(char c)
{
  int i;
  int blen = sizeof kb_cheat_buf - 1;
  /* Shift left, append c at the end. */
  for (i = 0; i < blen - 1; i++) {
    kb_cheat_buf[i] = kb_cheat_buf[i + 1];
  }
  kb_cheat_buf[blen - 1] = c;
  kb_cheat_buf[blen]     = 0;

  for (i = 0; i < (int)KB_CHEAT_COUNT; i++) {
    if (kb_str_endswith(kb_cheat_buf, KB_CHEATS[i].code)) {
      kb_cheat_req      = KB_CHEATS[i].id;
      kb_cheat_buf[0]   = 0;  /* clear buffer after match */
      return;
    }
  }

  /* IDCLEV xx warp: detect "idclev" followed by exactly two digit
   * characters appended to the buffer. */
  {
    int bl = 0;
    while (kb_cheat_buf[bl]) bl++;
    if (bl >= 8) {
      const char *p = &kb_cheat_buf[bl - 8];
      if (p[0]=='i' && p[1]=='d' && p[2]=='c' && p[3]=='l' && p[4]=='e' && p[5]=='v'
          && p[6] >= '0' && p[6] <= '9'
          && p[7] >= '0' && p[7] <= '9') {
        kb_warp_req = (p[6] - '0') * 10 + (p[7] - '0');
        kb_cheat_buf[0] = 0;
      }
    }
  }
}

/* ---- scancode -> Pad* bit map ----
 *
 * Recreates the **1993 PC Doom default keyboard layout** -- the way
 * Doom would have been played on a 386 in the 90s. The bindings come
 * straight from the original DOS Doom manual (page 4):
 *
 *   Up / Down arrows   -- move forward / back
 *   Left / Right       -- turn
 *   Alt + Left / Right -- strafe (modifier; handled below)
 *   , / .              -- strafe left / right (direct, no modifier)
 *   Ctrl               -- fire
 *   Space              -- use / open
 *   Shift              -- run (speed modifier)
 *   Tab                -- automap
 *   Esc                -- menu
 *   Enter              -- menu select / confirm
 *
 * "Strafe" maps to PadLeftShift / PadRightShift: OptiDoom's
 * INPUT_DPAD_ONLY input mode treats those exact bits as the
 * strafe-left / strafe-right buttons (user.c:228).
 *
 * Fire / Use / Run map to PadA / PadB / PadC which are the *default*
 * PadAttack / PadUse / PadSpeed bits (data.c:21-23). If the player
 * remaps via the in-game options, those bits no longer correspond
 * to the same actions, but the keyboard still drives the underlying
 * pad bits the user originally bound.
 */
typedef struct {
  uint8  scancode;   /* PS/2 Set 2 byte. Bit 7 set = E0-extended. */
  Word   padbit;
} KbMap;

/* PS/2 Set 2 reference for scancodes used below:
 *   0x14 = L-Ctrl       0x12 = L-Shift
 *   0x29 = Space        0x59 = R-Shift
 *   0x5A = Enter        0x41 = ,
 *   0x76 = Esc          0x49 = .
 *   0x0D = Tab          0x11 = L-Alt (E0+0x11 = R-Alt)
 *   E0+0x75 = Up arrow  E0+0x72 = Down arrow
 *   E0+0x6B = Left      E0+0x74 = Right
 *   E0+0x14 = R-Ctrl    E0+0x11 = R-Alt
 *
 * E0-extended scancodes land at (byte | 0x80) in our 256-bit matrix
 * (the convention the KeyboardDriver follows when splitting regular
 * from extended keys). */
static const KbMap KB_MAP[] = {
  /* Movement / turn -- arrow keys */
  { 0x75 | 0x80, PadUp    },          /* Up arrow    -> forward */
  { 0x72 | 0x80, PadDown  },          /* Down arrow  -> back    */
  { 0x6B | 0x80, PadLeft  },          /* Left arrow  -> turn L  */
  { 0x74 | 0x80, PadRight },          /* Right arrow -> turn R  */

  /* Direct strafe (no Alt modifier required) */
  { 0x41, PadLeftShift  },            /* , -> strafe L          */
  { 0x49, PadRightShift },            /* . -> strafe R          */

  /* Action keys */
  { 0x14, PadA          },            /* L-Ctrl  -> fire        */
  { 0x14 | 0x80, PadA   },            /* R-Ctrl  -> fire        */
  { 0x29, PadB          },            /* Space   -> use         */
  { 0x12, PadC          },            /* L-Shift -> run         */
  { 0x59, PadC          },            /* R-Shift -> run         */

  /* Menu / map / confirm / pause. ESC = PadX (OptiDoom's options-menu
   * toggle, mirroring PC Doom's ESC = menu). Tab = PadXLeft (an
   * otherwise-unused pad bit we wired into AM_Control as a single-
   * button automap toggle, so PC Doom's Tab = automap works without
   * forcing the player to chord Use+Start). Enter = PadA (fire in
   * game, confirm in menus). The Pause/Break key drives PadStart --
   * its PS/2 scancode is the multi-byte E1 sequence, which the
   * driverlet collapses into synthetic scancode 0x84 with a clean
   * press-then-release pulse. P is intentionally NOT bound here so
   * it stays available for the IDCLIP cheat. */
  { 0x76, PadX          },            /* Esc        -> options menu */
  { 0x0D, PadXLeft      },            /* Tab        -> automap      */
  { 0x5A, PadA          },            /* Enter      -> confirm      */
  { 0x84, PadStart      },            /* Pause/Brk  -> pause        */
};

#define KB_MAP_LEN (sizeof KB_MAP / sizeof KB_MAP[0])

/* Modifier scancodes -- not in KB_MAP because they don't OR-fold
 * into a pad bit directly; they reshape arrow behaviour at the
 * post-mapping step. */
#define KB_SC_LALT  0x11
#define KB_SC_RALT  (0x11 | 0x80)

static int kb_held(uint8 scancode)
{
  return (kb_matrix[scancode >> 5] & (1u << (scancode & 0x1F))) != 0;
}


/* ---- broker connect / configure ----
 *
 * One-time setup at game boot. We FindNamedItem the broker port
 * (created by the eventbroker daemon at system start), then make
 * our own MsgPort + ConfigurationRequest subscribing to just the
 * keyboard event bits. The broker multicasts events, so the SDK's
 * pad/mouse subscription set up by InitEventUtility keeps running
 * unaffected.
 */
void initKeyboard(void)
{
  ConfigurationRequest config;
  Err err;
  ConfigurationRequest *cfgptr;

  kb_broker_port = FindNamedItem (MKNODEID (KERNELNODE, MSGPORTNODE),
                                  EventPortName);
  if (kb_broker_port < 0) return;

  kb_port_item = CreateMsgPort ("OptidoomKbPort", 0, 0);
  if (kb_port_item < 0) return;

  kb_msg_item = CreateMsg (NULL, 0, kb_port_item);
  if (kb_msg_item < 0) return;

  memset (&config, 0, sizeof config);
  config.cr_Header.ebh_Flavor = EB_Configure;
  config.cr_Category          = LC_Observer;
  /* JPT-style: subscribe to ALL device-class events so the modern
   * broker actually polls every driverlet (including pad). */
  config.cr_TriggerMask[0]    = EVENTBIT0_ControlButtonUpdate
                              | EVENTBIT0_ControlButtonPressed
                              | EVENTBIT0_ControlButtonReleased
                              | EVENTBIT0_ControlButtonArrived
                              | EVENTBIT0_MouseUpdate
                              | EVENTBIT0_MouseMoved
                              | EVENTBIT0_MouseButtonPressed
                              | EVENTBIT0_MouseButtonReleased
                              | EVENTBIT0_MouseDataArrived
                              | EVENTBIT0_KeyboardKeyPressed
                              | EVENTBIT0_KeyboardKeyReleased
                              | EVENTBIT0_KeyboardUpdate
                              | EVENTBIT0_KeyboardDataArrived;
  config.cr_QueueMax          = 10;

  cfgptr = &config;
  err = SendMsg (kb_broker_port, kb_msg_item,
                 cfgptr, sizeof config);
  if (err < 0) return;

  kb_enabled = 1;
}


/* ---- per-frame poll ----
 *
 * GetMsg in non-blocking mode drains everything pending on our
 * port. Each EB_EventRecord carries one or more EventFrames; we
 * walk them looking for KeyboardUpdate-flavoured frames and copy
 * the latest matrix snapshot. Then OR-fold held scancodes into
 * the returned pad bits.
 */
Word readKeyboardBits(void)
{
  Word bits = 0;
  Item msg;

  if (!kb_enabled) return 0;

  /* Drain pending messages. Each one is either:
   *  - the EB_Configure ack (replied to kb_msg_item itself); or
   *  - an EB_EventRecord with one or more EventFrames inside.
   * We just want the freshest key matrix, so latest write wins. */
  while ((msg = GetMsg (kb_port_item)) > 0)
    {
      Message *m = (Message *)LookupItem (msg);
      EventBrokerHeader *hdr;

      kb_event_count++;
      if (m == NULL) continue;
      hdr = (EventBrokerHeader *)m->msg_DataPtr;

      if (hdr != NULL && hdr->ebh_Flavor == EB_EventRecord)
        {
          EventFrame *frame = (EventFrame *)(hdr + 1);
          while (frame->ef_ByteCount != 0)
            {
              uint32 evn = frame->ef_EventNumber;
              if (evn == EVENTNUM_KeyboardKeyPressed
                  || evn == EVENTNUM_KeyboardKeyReleased
                  || evn == EVENTNUM_KeyboardUpdate
                  || evn == EVENTNUM_KeyboardDataArrived)
                {
                  KeyboardEventData *ked =
                    (KeyboardEventData *)frame->ef_EventData;
                  memcpy (kb_matrix, ked->ked_KeyMatrix,
                          sizeof kb_matrix);
                }
              frame = (EventFrame *)
                ((char *)frame + frame->ef_ByteCount);
            }
        }

      /* Reply ONLY to broker-originated messages. The EB_Configure
       * ACK arrives on kb_msg_item itself -- replying to our own
       * SendMsg ACK is illegal and corrupts kernel state. */
      if (msg != kb_msg_item) ReplyMsg (msg, 0, NULL, 0);
    }

  /* Press-transition detection for one-shot keys.
   * A bit set in kb_matrix but clear in kb_matrix_prev is a fresh
   * press this frame. */
  {
    int wi;
    int sc;
    /* Weapon select 1-7 */
    for (wi = 0; wi < 7; wi++)
      {
        uint8 wsc = KB_WEAPON_SCANCODES[wi];
        uint32 mask = 1u << (wsc & 0x1F);
        uint32 word_idx = wsc >> 5;
        if ((kb_matrix[word_idx] & mask)
            && !(kb_matrix_prev[word_idx] & mask))
          {
            kb_weapon_req = wi;
          }
      }
    /* Letter typing for DOS cheats. Only base scancodes 0x00-0x7F;
     * extended E0-prefixed keys never carry letters. */
    for (sc = 0; sc < 0x80; sc++)
      {
        char ch = KB_SC_TO_LETTER[sc];
        uint32 mask = 1u << (sc & 0x1F);
        uint32 word_idx = sc >> 5;
        if (ch == 0) continue;
        if ((kb_matrix[word_idx] & mask)
            && !(kb_matrix_prev[word_idx] & mask))
          {
            kb_cheat_push(ch);
          }
      }

    /* Caps Lock (sc 0x58) toggles persistent auto-run. */
    {
      uint8 cl = 0x58;
      uint32 mask = 1u << (cl & 0x1F);
      uint32 word_idx = cl >> 5;
      if ((kb_matrix[word_idx] & mask)
          && !(kb_matrix_prev[word_idx] & mask))
        {
          kb_autorun = !kb_autorun;
          kb_autorun_just_toggled = 1;
          /* Sync the physical Caps Lock LED on the 3DO keyboard.
           * EB_IssuePodCmd sends a 3-byte command:
           *   [0] GENERIC_Keyboard
           *   [1] GENERIC_KEYBOARD_SetLEDs
           *   [2] LED bitmask (CAPSLOCK on/off)
           * The keyboard pod number isn't known here without a
           * separate EB_DescribePods round-trip, so we broadcast to
           * all 8 pod slots. Non-keyboard pods return ER_NotSupported
           * and the broker silently swallows it. */
          {
            struct PodCmdMsg {
              EventBrokerHeader hdr;
              int32 pd_PodNumber;
              int32 pd_WaitFlag;
              int32 pd_DataByteCount;
              uint8 pd_Data[4];
            } cmd;
            int pod;
            memset(&cmd, 0, sizeof cmd);
            cmd.hdr.ebh_Flavor    = EB_IssuePodCmd;
            cmd.pd_WaitFlag       = 0;
            cmd.pd_DataByteCount  = 3;
            cmd.pd_Data[0]        = GENERIC_Keyboard;
            cmd.pd_Data[1]        = GENERIC_KEYBOARD_SetLEDs;
            cmd.pd_Data[2]        = kb_autorun
                                    ? KEYBOARD_LED_CAPSLOCK
                                    : 0;
            for (pod = 1; pod <= 8; pod++) {
              cmd.pd_PodNumber = pod;
              SendMsg(kb_broker_port, kb_msg_item,
                      &cmd, sizeof cmd);
            }
          }
        }
    }

    /* +/= (0x55) and -/_ (0x4E) one-shot screen-size adjust.
     * Both DOS Doom F5 grow and OptiDoom's screenSize option. */
    {
      uint8 plus  = 0x55;
      uint8 minus = 0x4E;
      uint32 pmask = 1u << (plus  & 0x1F);
      uint32 mmask = 1u << (minus & 0x1F);
      if ((kb_matrix[plus  >> 5] & pmask)
          && !(kb_matrix_prev[plus  >> 5] & pmask)) kb_screensize_delta = +1;
      if ((kb_matrix[minus >> 5] & mmask)
          && !(kb_matrix_prev[minus >> 5] & mmask)) kb_screensize_delta = -1;
    }

    memcpy (kb_matrix_prev, kb_matrix, sizeof kb_matrix);
  }

  /* OR-fold held scancodes into pad bits. */
  {
    int i;
    for (i = 0; i < KB_MAP_LEN; i++)
      {
        uint8 sc = KB_MAP[i].scancode;
        if (kb_matrix[sc >> 5] & (1u << (sc & 0x1F)))
          bits |= KB_MAP[i].padbit;
      }
  }

  /* Alt-strafe modifier (1993 PC Doom default behaviour): when
   * either Alt key is held, transform Left/Right arrow input from
   * "turn" to "strafe". This mirrors the DOS Doom alt-strafe
   * binding that gave you smooth strafing without committing to
   * the comma/period direct-strafe keys. */
  if (kb_held (KB_SC_LALT) || kb_held (KB_SC_RALT))
    {
      if (bits & PadLeft)
        {
          bits &= ~PadLeft;
          bits |= PadLeftShift;
        }
      if (bits & PadRight)
        {
          bits &= ~PadRight;
          bits |= PadRightShift;
        }
    }

  /* Caps Lock auto-run -- persistent PadC (run/speed) bit. Game
   * sees this as if Shift were always held. */
  if (kb_autorun) bits |= PadC;

  return bits;
}


/* ---- ReadJoyButtons override ----
 *
 * Re-implement the burger.lib helper here so the linker resolves
 * ReadJoyButtons against our copy first (keyboard.o is in the .o
 * list before burger.lib gets searched). Functionally identical
 * to lib/burger/readjoybuttons.c except we OR in the held-key
 * state -- so dmain.c's main game loop and modmenu.c's mod-menu
 * polling both see the keyboard with no further patches.
 *
 * LastJoyButtons[] is declared here too, otherwise the linker
 * would pull in burger.lib's readjoybuttons.o for that symbol,
 * which would then collide on the ReadJoyButtons definition.
 */
Word LastJoyButtons[4];

Word ReadJoyButtons(Word PadNum)
{
  ControlPadEventData cped;
  Word bits;

  cped.cped_ButtonBits = 0;
  GetControlPad (PadNum + 1, FALSE, &cped);
  bits = (Word)cped.cped_ButtonBits | readKeyboardBits ();
  if (PadNum < 4) LastJoyButtons[PadNum] = bits;
  return bits;
}

int readRequestedWeapon(void)
{
  int req = kb_weapon_req;
  kb_weapon_req = -1;
  return req;
}

int readRequestedCheat(void)
{
  int req = kb_cheat_req;
  kb_cheat_req = -1;
  return req;
}

int readRequestedWarp(void)
{
  int req = kb_warp_req;
  kb_warp_req = -1;
  return req;
}

int readRequestedScreenSize(void)
{
  int d = kb_screensize_delta;
  kb_screensize_delta = 0;
  return d;
}

int isAutoRunEnabled(void)
{
  return kb_autorun;
}

int readAutoRunToggled(void)
{
  int t = kb_autorun_just_toggled;
  kb_autorun_just_toggled = 0;
  return t;
}
