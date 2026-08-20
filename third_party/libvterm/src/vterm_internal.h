#ifndef __VTERM_INTERNAL_H__
#define __VTERM_INTERNAL_H__

#include "vterm.h"

#include <stdarg.h>

#if defined(__GNUC__)
# define INTERNAL __attribute__((visibility("internal")))
#else
# define INTERNAL
#endif

#ifdef DEBUG
# define DEBUG_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
# define DEBUG_LOG(...)
#endif

#define ESC_S "\x1b"

#define INTERMED_MAX 16

/* Neovim carries a stack overflow fix here (upstream commit 756751afa3,
 * "fix(terminal): stack overflow when too many csi args"): a CSI with more
 * than 16 subparameters (e.g. piped-back scrollback with dense SGR runs)
 * overflowed the args[] buffer. */
#define CSI_ARGS_MAX 32
#define CSI_LEADER_MAX 16

#define BUFIDX_PRIMARY   0
#define BUFIDX_ALTSCREEN 1

/* Kitty keyboard protocol flags, carried from Neovim's fork (upstream
 * commit 6f0bde11cc, "feat(terminal): add support for kitty keyboard
 * protocol"). https://sw.kovidgoyal.net/kitty/keyboard-protocol/#progressive-enhancement */
#define KEY_ENCODING_DISAMBIGUATE       0x1
#define KEY_ENCODING_REPORT_EVENTS      0x2
#define KEY_ENCODING_REPORT_ALTERNATE   0x4
#define KEY_ENCODING_REPORT_ALL_KEYS    0x8
#define KEY_ENCODING_REPORT_ASSOCIATED  0x10

typedef struct VTermEncoding VTermEncoding;

typedef struct {
  VTermEncoding *enc;

  // This size should be increased if required by other stateful encodings
  char           data[4*sizeof(uint32_t)];
} VTermEncodingInstance;

struct VTermPen
{
  VTermColor fg;
  VTermColor bg;
  unsigned int bold:1;
  unsigned int underline:2;
  unsigned int italic:1;
  unsigned int blink:1;
  unsigned int reverse:1;
  unsigned int conceal:1;
  unsigned int strike:1;
  unsigned int font:4; /* To store 0-9 */
  unsigned int small:1;
  unsigned int baseline:2;
  unsigned int dim:1;
  unsigned int overline:1;
};

typedef struct {
  bool disambiguate:1;
  bool report_events:1;
  bool report_alternate:1;
  bool report_all_keys:1;
  bool report_associated:1;
} VTermKeyEncodingFlags;

struct VTermKeyEncodingStack {
  VTermKeyEncodingFlags items[16];
  uint8_t size; /* Number of items in the stack: at least 1, at most
                 * ARRAY_SIZE(items). */
};

struct VTermState
{
  VTerm *vt;

  const VTermStateCallbacks *callbacks;
  void *cbdata;
  bool callbacks_has_premove;

  const VTermStateFallbacks *fallbacks;
  void *fbdata;

  int rows;
  int cols;

  /* Current cursor position */
  VTermPos pos;

  int at_phantom; /* True if we're on the "81st" phantom column to defer a wraparound */

  int scrollregion_top;
  int scrollregion_bottom; /* -1 means unbounded */
#define SCROLLREGION_BOTTOM(state) ((state)->scrollregion_bottom > -1 ? (state)->scrollregion_bottom : (state)->rows)
  int scrollregion_left;
#define SCROLLREGION_LEFT(state)  ((state)->mode.leftrightmargin ? (state)->scrollregion_left : 0)
  int scrollregion_right; /* -1 means unbounded */
#define SCROLLREGION_RIGHT(state) ((state)->mode.leftrightmargin && (state)->scrollregion_right > -1 ? (state)->scrollregion_right : (state)->cols)

  /* Bitvector of tab stops */
  unsigned char *tabstops;

  /* Primary and Altscreen; lineinfos[1] is lazily allocated as needed */
  VTermLineInfo *lineinfos[2];

  /* lineinfo will == lineinfos[0] or lineinfos[1], depending on altscreen */
  VTermLineInfo *lineinfo;
#define ROWWIDTH(state,row) ((state)->lineinfo[(row)].doublewidth ? ((state)->cols / 2) : (state)->cols)
#define THISROWWIDTH(state) ROWWIDTH(state, (state)->pos.row)

  /* Mouse state */
  int mouse_col, mouse_row;
  int mouse_buttons;
  int mouse_flags;
#define MOUSE_WANT_CLICK 0x01
#define MOUSE_WANT_DRAG  0x02
#define MOUSE_WANT_MOVE  0x04

  enum { MOUSE_X10, MOUSE_UTF8, MOUSE_SGR, MOUSE_RXVT } mouse_protocol;

  /* Last glyph output, for Unicode recombining purposes */
  uint32_t *combine_chars;
  size_t combine_chars_size; // Number of ELEMENTS in the above
  int combine_width; // The width of the glyph above
  VTermPos combine_pos;   // Position before movement

  struct {
    unsigned int keypad:1;
    unsigned int cursor:1;
    unsigned int autowrap:1;
    unsigned int insert:1;
    unsigned int newline:1;
    unsigned int cursor_visible:1;
    unsigned int cursor_blink:1;
    unsigned int cursor_shape:2;
    unsigned int alt_screen:1;
    unsigned int origin:1;
    unsigned int screen:1;
    unsigned int leftrightmargin:1;
    unsigned int bracketpaste:1;
    unsigned int report_focus:1;
    unsigned int theme_updates:1;
    unsigned int synchronized_output:1;
  } mode;

  VTermEncodingInstance encoding[4], encoding_utf8;
  int gl_set, gr_set, gsingle_set;

  struct VTermPen pen;

  VTermColor default_fg;
  VTermColor default_bg;
  VTermColor colors[16]; // Store the 8 ANSI and the 8 ANSI high-brights only

  int bold_is_highbright;

  unsigned int protected_cell : 1;

  /* Saved state under DEC mode 1048/1049 */
  struct {
    VTermPos pos;
    struct VTermPen pen;

    struct {
      unsigned int cursor_visible:1;
      unsigned int cursor_blink:1;
      unsigned int cursor_shape:2;
    } mode;
  } saved;

  /* Temporary state for DECRQSS parsing */
  union {
    char decrqss[4];
    struct {
      uint16_t mask;
      enum {
        SELECTION_INITIAL,
        SELECTION_SELECTED,
        SELECTION_QUERY,
        SELECTION_SET_INITIAL,
        SELECTION_SET,
        SELECTION_INVALID,
      } state : 8;
      uint32_t recvpartial;
      uint32_t sendpartial;
    } selection;
  } tmp;

  struct {
    const VTermSelectionCallbacks *callbacks;
    void *user;
    char *buffer;
    size_t buflen;
  } selection;

  /* Kitty keyboard protocol flag stack: one for the primary screen, one for
   * the altscreen. */
  struct VTermKeyEncodingStack key_encoding_stacks[2];
};

struct VTerm
{
  const VTermAllocatorFunctions *allocator;
  void *allocdata;

  int rows;
  int cols;

  struct {
    unsigned int utf8:1;
    unsigned int ctrl8bit:1;
  } mode;

  struct {
    enum VTermParserState {
      NORMAL,
      CSI_LEADER,
      CSI_ARGS,
      CSI_INTERMED,
      DCS_COMMAND,
      /* below here are the "string states" */
      OSC_COMMAND,
      OSC,
      DCS,
      APC,
      PM,
      SOS,
    } state;

    bool in_esc : 1;

    int intermedlen;
    char intermed[INTERMED_MAX];

    union {
      struct {
        int leaderlen;
        char leader[CSI_LEADER_MAX];

        int argi;
        long args[CSI_ARGS_MAX];
      } csi;
      struct {
        int command;
      } osc;
      struct {
        int commandlen;
        char command[CSI_LEADER_MAX];
      } dcs;
    } v;

    const VTermParserCallbacks *callbacks;
    void *cbdata;

    bool string_initial;

    bool emit_nul;
  } parser;

  /* len == malloc()ed size; cur == number of valid bytes */

  VTermOutputCallback *outfunc;
  void                *outdata;

  char  *outbuffer;
  size_t outbuffer_len;
  size_t outbuffer_cur;

  char  *tmpbuffer;
  size_t tmpbuffer_len;

  VTermState *state;
  VTermScreen *screen;
};

struct VTermEncoding {
  void (*init) (VTermEncoding *enc, void *data);
  void (*decode)(VTermEncoding *enc, void *data,
                 uint32_t cp[], int *cpi, int cplen,
                 const char bytes[], size_t *pos, size_t len);
};

typedef enum {
  ENC_UTF8,
  ENC_SINGLE_94
} VTermEncodingType;

void *vterm_allocator_malloc(VTerm *vt, size_t size);
void  vterm_allocator_free(VTerm *vt, void *ptr);

void vterm_push_output_bytes(VTerm *vt, const char *bytes, size_t len);
void vterm_push_output_vsprintf(VTerm *vt, const char *format, va_list args);
void vterm_push_output_sprintf(VTerm *vt, const char *format, ...);
void vterm_push_output_sprintf_ctrl(VTerm *vt, unsigned char ctrl, const char *fmt, ...);
void vterm_push_output_sprintf_str(VTerm *vt, unsigned char ctrl, bool term, const char *fmt, ...);

void vterm_state_free(VTermState *state);

void vterm_state_newpen(VTermState *state);
void vterm_state_resetpen(VTermState *state);
void vterm_state_setpen(VTermState *state, const long args[], int argcount);
int  vterm_state_getpen(VTermState *state, long args[], int argcount);
void vterm_state_savepen(VTermState *state, int save);

enum {
  C1_SS3 = 0x8f,
  C1_DCS = 0x90,
  C1_CSI = 0x9b,
  C1_ST  = 0x9c,
  C1_OSC = 0x9d,
};

void vterm_state_push_output_sprintf_CSI(VTermState *vts, const char *format, ...);

void vterm_screen_free(VTermScreen *screen);

VTermEncoding *vterm_lookup_encoding(VTermEncodingType type, char designation);

int vterm_unicode_width(uint32_t codepoint);
int vterm_unicode_is_combining(uint32_t codepoint);

#endif
