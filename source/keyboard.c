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
static int32   kb_enabled     = 0;

/* Live 256-bit key matrix from the broker. Bit N set = scancode N
 * held. The upper 128 bits hold E0-extended keys (matrix bit =
 * scancode | 0x80), so arrow keys land at 0xF5 / 0xF2 / 0x6B|0x80
 * / 0x74|0x80 etc. -- the convention the host KeyboardDriver
 * follows. */
static uint32  kb_matrix[8] = { 0 };

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

  /* Menu / map / confirm */
  { 0x76, PadStart      },            /* Esc   -> menu          */
  { 0x0D, PadX          },            /* Tab   -> automap       */
  { 0x5A, PadA          },            /* Enter -> confirm       */
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
  config.cr_TriggerMask[0]    = EVENTBIT0_KeyboardKeyPressed
                              | EVENTBIT0_KeyboardKeyReleased
                              | EVENTBIT0_KeyboardUpdate
                              | EVENTBIT0_KeyboardDataArrived;
  config.cr_QueueMax          = 10;

  /* Send Configure; broker replies on kb_msg_item when it has
   * accepted the subscription. We don't wait synchronously --
   * the reply gets drained by readKeyboardBits's GetMsg loop
   * alongside event messages. */
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

      if (m == NULL) continue;
      hdr = (EventBrokerHeader *)m->msg_DataPtr;
      if (hdr == NULL)
        {
          ReplyMsg (msg, 0, NULL, 0);
          continue;
        }

      if (hdr->ebh_Flavor == EB_EventRecord)
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

      /* EB_EventRecord messages get ReplyMsg'd back to the
       * broker so it can recycle them. The Configure ack is also
       * just acknowledged. */
      if (msg != kb_msg_item) ReplyMsg (msg, 0, NULL, 0);
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
