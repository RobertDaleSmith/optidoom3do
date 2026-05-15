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
 * Each held scancode OR-folds its Pad* bit into the joypad stream.
 * Multiple scancodes can map to the same Pad* bit (W and Up arrow
 * both fire PadUp), and one game action can come from either pad
 * or keyboard.
 *
 * "Strafe" maps to PadLeftShift / PadRightShift: OptiDoom's
 * INPUT_DPAD_ONLY input mode treats those exact bits as the
 * strafe-left / strafe-right buttons (user.c:228), which is
 * exactly the WASD strafe behaviour an FPS player expects.
 */
typedef struct {
  uint8  scancode;   /* PS/2 Set 2 byte. Bit 7 set = E0-extended. */
  Word   padbit;
} KbMap;

static const KbMap KB_MAP[] = {
  /* Movement (WASD) */
  { 0x1D, PadUp         },   /* W -> forward    */
  { 0x1B, PadDown       },   /* S -> back       */
  { 0x1C, PadLeftShift  },   /* A -> strafe L   */
  { 0x23, PadRightShift },   /* D -> strafe R   */

  /* Turn keys (arrows) -- E0-prefixed in PS/2 Set 2, our broker
   * shifts those into the upper half of the key matrix. */
  { 0x75 | 0x80, PadUp    }, /* Up arrow        */
  { 0x72 | 0x80, PadDown  }, /* Down arrow      */
  { 0x6B | 0x80, PadLeft  }, /* Left arrow      */
  { 0x74 | 0x80, PadRight }, /* Right arrow     */

  /* Q / E -- alternate turn-left / use (Doom convention). */
  { 0x15, PadLeft       },   /* Q -> turn L     */
  { 0x24, PadC          },   /* E -> use        */

  /* Action keys */
  { 0x29, PadA          },   /* Space -> fire   */
  { 0x14, PadA          },   /* L-Ctrl -> fire  */
  { 0x5A, PadC          },   /* Enter -> use    */

  /* Menu / map */
  { 0x76, PadStart      },   /* Esc  -> menu    */
  { 0x0D, PadX          },   /* Tab  -> automap */

  /* Weapon cycle -- L/R brackets feel natural in Doom. */
  { 0x54, PadLeftShift  },   /* [ -> prev (re-uses strafe; */
  { 0x5B, PadRightShift },   /* ]    same bit, no harm)    */
};

#define KB_MAP_LEN (sizeof KB_MAP / sizeof KB_MAP[0])


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
