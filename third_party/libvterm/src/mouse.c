#include "vterm_internal.h"

#include "utf8.h"

/* Index of the lowest set bit, matching what Neovim's xctz() (nvim/math.h)
 * gives the caller this fix ports from (upstream commit 06a1f82f1c,
 * "feat(terminal): forward X1 and X2 mouse events"). Never called with x
 * == 0 below. */
static int lowest_set_bit(unsigned int x)
{
  int i;
  for(i = 0; !(x & 1); i++, x >>= 1)
    ;
  return i;
}

static void output_mouse(VTermState *state, int code, int pressed, int modifiers, int col, int row)
{
  modifiers <<= 2;

  switch(state->mouse_protocol) {
  case MOUSE_X10:
    if(col + 0x21 > 0xff)
      col = 0xff - 0x21;
    if(row + 0x21 > 0xff)
      row = 0xff - 0x21;

    if(!pressed)
      code = 3;

    /* X10 has no encoding for the X1/X2 buttons (code bit 0x80); drop the
     * event rather than sending a garbled one. */
    if(code & 0x80)
      break;

    vterm_push_output_sprintf_ctrl(state->vt, C1_CSI, "M%c%c%c",
        (code | modifiers) + 0x20, col + 0x21, row + 0x21);
    break;

  case MOUSE_UTF8:
    {
      char utf8[18]; size_t len = 0;

      if(!pressed)
        code = 3;

      len += fill_utf8((code | modifiers) + 0x20, utf8 + len);
      len += fill_utf8(col + 0x21, utf8 + len);
      len += fill_utf8(row + 0x21, utf8 + len);
      utf8[len] = 0;

      vterm_push_output_sprintf_ctrl(state->vt, C1_CSI, "M%s", utf8);
    }
    break;

  case MOUSE_SGR:
    vterm_push_output_sprintf_ctrl(state->vt, C1_CSI, "<%d;%d;%d%c",
        code | modifiers, col + 1, row + 1, pressed ? 'M' : 'm');
    break;

  case MOUSE_RXVT:
    if(!pressed)
      code = 3;

    vterm_push_output_sprintf_ctrl(state->vt, C1_CSI, "%d;%d;%dM",
        code | modifiers, col + 1, row + 1);
    break;
  }
}

void vterm_mouse_move(VTerm *vt, int row, int col, VTermModifier mod)
{
  VTermState *state = vt->state;

  if(col == state->mouse_col && row == state->mouse_row)
    return;

  state->mouse_col = col;
  state->mouse_row = row;

  if((state->mouse_flags & MOUSE_WANT_DRAG && state->mouse_buttons) ||
     (state->mouse_flags & MOUSE_WANT_MOVE)) {
    /* Neovim carries a fix here (upstream commit 06a1f82f1c, "feat
     * (terminal): forward X1 and X2 mouse events"): report the lowest-
     * numbered held button (1-4, or 8-11 for X1/X2) instead of assuming
     * button 4 whenever none of 1-3 is held, which mis-reported X1/X2
     * drags as button 4/wheel. */
    if(state->mouse_buttons) {
      int button = lowest_set_bit((unsigned int)state->mouse_buttons) + 1;
      if(button < 4)
        output_mouse(state, button-1 + 0x20, 1, mod, col, row);
      else if(button >= 8 && button < 12)
        output_mouse(state, button-8 + 0x80 + 0x20, 1, mod, col, row);
    }
    else {
      output_mouse(state, 3 + 0x20, 1, mod, col, row);
    }
  }
}

void vterm_mouse_button(VTerm *vt, int button, bool pressed, VTermModifier mod)
{
  VTermState *state = vt->state;

  int old_buttons = state->mouse_buttons;

  /* X1/X2 (buttons 8-11) share the held-buttons bitmask with 1-3, same as
   * upstream commit 06a1f82f1c ("feat(terminal): forward X1 and X2 mouse
   * events") does. */
  if((button > 0 && button <= 3) || (button >= 8 && button <= 11)) {
    if(pressed)
      state->mouse_buttons |= (1 << (button-1));
    else
      state->mouse_buttons &= ~(1 << (button-1));
  }

  /* Most of the time we don't get button releases from 4/5/6/7 */
  if(state->mouse_buttons == old_buttons && (button < 4 || button > 7))
    return;

  if(!state->mouse_flags)
    return;

  if(button < 4) {
    output_mouse(state, button-1, pressed, mod, state->mouse_col, state->mouse_row);
  }
  else if(button < 8) {
    output_mouse(state, button-4 + 0x40, pressed, mod, state->mouse_col, state->mouse_row);
  }
  else if(button < 12) {
    output_mouse(state, button-8 + 0x80, pressed, mod, state->mouse_col, state->mouse_row);
  }
}
