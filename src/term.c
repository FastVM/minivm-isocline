/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>  // getenv

#include "common.h"
#include "tty.h"
#include "term.h"
#include "stringbuf.h" // str_next_ofs

#if defined(_WIN32)
#include <windows.h>
#define STDOUT_FILENO 1
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

#define RP_CSI      "\x1B["

struct term_s {
  int         fout;
  ssize_t     width;
  ssize_t     height;
  bool        nocolor;
  bool        silent;
  bool        raw_enabled;
  bool        buffered;
  stringbuf_t* buf;
  alloc_t*    mem;  
  #ifdef _WIN32
  HANDLE      hcon;
  WORD        hcon_default_attr;  
  WORD        hcon_orig_attr;
  DWORD       hcon_orig_mode;
  UINT        hcon_orig_cp;  
  COORD       hcon_save_cursor;
  #endif
};

static bool term_write_direct(term_t* term, const char* s, ssize_t n );
static bool term_vwritef(term_t* term, ssize_t max_needed, const char* fmt, va_list args );


//-------------------------------------------------------------
// Helpers
//-------------------------------------------------------------

rp_private void term_left(term_t* term, ssize_t n) {
  if (n <= 0) return;
  term_writef( term, 64, RP_CSI "%zdD", n );
}

rp_private void term_right(term_t* term, ssize_t n) {
  if (n <= 0) return;
  term_writef( term, 64, RP_CSI "%zdC", n );
}

rp_private void term_up(term_t* term, ssize_t n) {
  if (n <= 0) return;
  term_writef( term, 64, RP_CSI "%zdA", n );
}

rp_private void term_down(term_t* term, ssize_t n) {
  if (n <= 0) return;
  term_writef( term, 64, RP_CSI "%zdB", n );
}

rp_private void term_clear_line(term_t* term) {
  term_write( term, "\r" RP_CSI "2K");
}

rp_private void term_start_of_line(term_t* term) {
  term_write( term, "\r" );
}

rp_private ssize_t term_get_width(term_t* term) {
  return term->width;
}

rp_private ssize_t term_get_height(term_t* term) {
  return term->height;
}

rp_private void term_attr_reset(term_t* term) {
  term_write(term, RP_CSI "0m" );
}

rp_private void term_underline(term_t* term, bool on) {
  term_write(term, on ? RP_CSI "4m" : RP_CSI "24m" );
}

rp_private void term_color(term_t* term, rp_color_t color) {
  if (color == RP_COLOR_NONE || color == RP_COLOR_DEFAULT) return;
  term_writef(term, 64, RP_CSI "%dm", (int)color );
}


// Unused for now
/*
internal void term_bgcolor(term_t* term, rp_color_t color) {
  term_writef(term, RP_CSI "%dm", color + 10 );
}

internal void term_end_of_line(term_t* term) {
  term_right( term, 999 );
}

internal void term_clear_screen(term_t* term) {
  term_write( term, RP_CSI "2J" RP_CSI "H" );
}

internal void term_clear_line_from_cursor(term_t* term) {
  term_write( term, RP_CSI "0K");
}

internal void term_clear(term_t* term, ssize_t n) {
  if (n <= 0) return;
  char buf[RP_MAX_LINE];
  memset(buf,' ',(n >= RP_MAX_LINE ? RP_MAX_LINE-1 : n));
  buf[RP_MAX_LINE-1] = 0;
  term_write( term, buf );
}
*/



//-------------------------------------------------------------
// Formatted output
//-------------------------------------------------------------

rp_private bool term_writef(term_t* term, ssize_t max_needed, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int err = term_vwritef(term,max_needed,fmt,ap);
  va_end(ap);
  return err;
}

static bool term_vwritef(term_t* term, ssize_t max_needed, const char* fmt, va_list args ) {
  rp_unused(max_needed);
  bool buffering = term->buffered;
  term_start_buffered(term);
  sbuf_append_vprintf(term->buf, max_needed, fmt, args);
  if (!buffering) term_end_buffered(term);
  return true;
}


//-------------------------------------------------------------
// Write to the terminal
// The buffered functions are used to reduce cursor flicker
// during refresh
//-------------------------------------------------------------

rp_private void term_beep(term_t* term) {
  if (term->silent) return;
  fprintf(stderr,"\x7");
  fflush(stderr);
}


rp_private bool term_write(term_t* term, const char* s) {
  // todo: strip colors on nocolor
  ssize_t n = rp_strlen(s);
  return term_write_n(term,s,n);
}

rp_private bool term_write_n(term_t* term, const char* s, ssize_t n) {
  if (!term->buffered) {
    return term_write_direct(term,s,n);
  }
  else {
    // write to buffer to reduce flicker
    sbuf_append_n(term->buf, s, n);
    return true;
  }
}

rp_private void term_start_buffered(term_t* term) {
  if (term->buf == NULL) {
    term->buf = sbuf_new(term->mem, true);
    if (term->buf == NULL) {
      term->buffered = false;
      return;
    }
  }
  term->buffered = true;
}

rp_private bool term_end_buffered(term_t* term) {
  if (!term->buffered) return true;
  term->buffered = false;
  if (term->buf != NULL && sbuf_len(term->buf) > 0) {
    bool ok = term_write_direct(term, sbuf_string(term->buf), sbuf_len(term->buf));
    sbuf_clear(term->buf);
    if (!ok) return false;
  }
  return true;
}


//-------------------------------------------------------------
// Init
//-------------------------------------------------------------

static void term_init_raw(term_t* term);

rp_private term_t* term_new(alloc_t* mem, tty_t* tty, bool nocolor, bool silent, int fout ) 
{
  term_t* term = mem_zalloc_tp(mem, term_t);
  if (term == NULL) return NULL;

  term->fout   = (fout < 0 ? STDOUT_FILENO : fout);
  term->nocolor = nocolor;
  term->silent = silent;  
  term->mem    = mem;
  term->width  = 80;
  term->height = 25;
  
  // read COLUMS/LINES from the environment for a better initial guess.
  const char* env_columns = getenv("COLUMNS");
  if (env_columns != NULL) { sscanf(env_columns, "%zd", &term->width); }
  const char* env_lines = getenv("LINES");
  if (env_lines != NULL)   { sscanf(env_lines, "%zd", &term->height); }
  
  // initialize raw terminal output and terminal dimensions
  term_init_raw(term);
  term_update_dim(term,tty);
  return term;
}

rp_private bool term_is_interactive(const term_t* term) {
  rp_unused(term);
  // check dimensions (0 is used for debuggers)
  // if (term->width <= 0) return false; 
  
  // check editing support
  const char* eterm = getenv("TERM");
  debug_msg("term: TERM=%s\n", eterm);
  if (eterm != NULL &&
      (strstr("dumb|DUMB|cons25|CONS25|emacs|EMACS",eterm) != NULL)) {
    return false;
  }

  return true;
}

rp_private void term_enable_beep(term_t* term, bool enable) {
  term->silent = !enable;
}

rp_private void term_enable_color(term_t* term, bool enable) {
  term->nocolor = !enable;
}


rp_private void term_free(term_t* term) {
  if (term == NULL) return;
  term_end_buffered(term);  
  term_end_raw(term);
  sbuf_free(term->buf); term->buf = NULL;
  mem_free(term->mem, term);
}


//-------------------------------------------------------------
// Platform dependent: Write directly to the terminal
//-------------------------------------------------------------

#if !defined(_WIN32)
static bool term_write_console(term_t* term, const char* s, ssize_t n) {
  return (write(term->fout, s, to_size_t(n)) == n);
}

static bool term_write_esc(term_t* term, const char* s, ssize_t len) {
  if (term->nocolor && s[1]=='[' && s[len-1] == 'm') {
    ssize_t n = 1;
    sscanf(s + 2, "%zd", &n);
    if ((n >= 30 && n <= 49) || (n >= 90 && n <= 109)) {
      // ignore color
      return true;
    }
  }
  return term_write_console(term, s, len);
}

static bool term_write_direct(term_t* term, const char* s, ssize_t len ) {
  if (!term->nocolor) {
    return term_write_console(term, s, len);
  }
  else {
    // strip CSI color sequences
    ssize_t pos = 0;
    while (pos < len) {
      // handle non-escape sequences in bulk
      ssize_t nonesc = 0;
      ssize_t next;
      while ((next = str_next_ofs(s, len, pos+nonesc, true, NULL)) > 0 && s[pos + nonesc] != '\x1B') {
        nonesc += next;
      }
      if (nonesc > 0) {
        term_write_console(term, s+pos, nonesc);
        pos += nonesc;
      }
      if (next <= 0) break;

      // handle escape sequence (note: str_next_ofs considers whole CSI escape sequences at a time)
      if (next > 1 && s[pos] == '\x1B') {
        term_write_esc(term, s+pos, next);
      }
      else {
        term_write_console(term, s+pos, next);
      }
      pos += next;
    }
    return (pos == len);
  }
}

#else

//-------------------------------------------------------------
// On windows we do ansi escape emulation ourselves.
// (for compat pre-win10 systems)
//
// note: we use row/col as 1-based ANSI escape while windows X/Y coords are 0-based.
//-------------------------------------------------------------------------

static bool term_write_console(term_t* term, const char* s, ssize_t n ) {
  DWORD written;
  WriteConsoleA(term->hcon, s, (DWORD)(to_size_t(n)), &written, NULL);
  return (written == (DWORD)(to_size_t(n)));
}

static void term_get_cursor(term_t* term, ssize_t* row, ssize_t* col) {
  *row = 0;
  *col = 0;
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(term->hcon, &info)) return;
  *row = (ssize_t)info.dwCursorPosition.Y + 1;
  *col = (ssize_t)info.dwCursorPosition.X + 1;
}

static void term_move_cursor_to( term_t* term, ssize_t row, ssize_t col ) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo( term->hcon, &info )) return;
  if (col > info.dwSize.X) col = info.dwSize.X;
  if (row > info.dwSize.Y) row = info.dwSize.Y;
  if (col <= 0) col = 1;
  if (row <= 0) row = 1;
  COORD coord;
  coord.X = (SHORT)col - 1;
  coord.Y = (SHORT)row - 1;
  SetConsoleCursorPosition( term->hcon, coord);
}

static void term_cursor_save(term_t* term) {
  memset(&term->hcon_save_cursor, 0, sizeof(term->hcon_save_cursor));
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(term->hcon, &info)) return;
  term->hcon_save_cursor = info.dwCursorPosition;
}

static void term_cursor_restore(term_t* term) {
  if (term->hcon_save_cursor.X == 0) return;
  SetConsoleCursorPosition(term->hcon, term->hcon_save_cursor);
}

static void term_move_cursor( term_t* term, ssize_t drow, ssize_t dcol, ssize_t n ) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo( term->hcon, &info )) return;
  COORD cur = info.dwCursorPosition;
  ssize_t col = (ssize_t)cur.X + 1 + n*dcol;
  ssize_t row = (ssize_t)cur.Y + 1 + n*drow;
  term_move_cursor_to( term, row, col );
}

static void term_cursor_visible( term_t* term, bool visible ) {
  CONSOLE_CURSOR_INFO info;
  if (!GetConsoleCursorInfo(term->hcon,&info)) return;
  info.bVisible = visible;
  SetConsoleCursorInfo(term->hcon,&info);
}

static void term_erase_line( term_t* term, ssize_t mode ) {  
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo( term->hcon, &info )) return;
  DWORD written;
  COORD start;
  ssize_t length;
  if (mode == 2) {
    // to end of line    
    length = (ssize_t)info.srWindow.Right - info.dwCursorPosition.X + 1;
    start  = info.dwCursorPosition;
  }
  else if (mode == 1) {
    // to start of line
    start.X = 0;
    start.Y = info.dwCursorPosition.Y;
    length  = info.dwCursorPosition.X;
  }
  else {
    // entire line
    start.X = 0;
    start.Y = info.dwCursorPosition.Y;
    length = (ssize_t)info.srWindow.Right + 1;
  }
  FillConsoleOutputAttribute( term->hcon, 0, (DWORD)length, start, &written );
  FillConsoleOutputCharacterA( term->hcon, ' ', (DWORD)length, start, &written );
}

static void term_clear_screen(term_t* term, ssize_t mode) {
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(term->hcon, &info)) return;
  COORD start;
  start.X = 0;
  start.Y = 0;
  ssize_t length;
  ssize_t width = (ssize_t)info.dwSize.X;
  if (mode == 2) {
    // entire screen
    length = width * info.dwSize.Y;    
  }
  else if (mode == 1) {
    // to cursor
    length = (width * ((ssize_t)info.dwCursorPosition.Y - 1)) + info.dwCursorPosition.X;
  }
  else {
    // from cursor
    start  = info.dwCursorPosition;
    length = (width * ((ssize_t)info.dwSize.Y - info.dwCursorPosition.Y)) + (width - info.dwCursorPosition.X + 1);
  }
  DWORD written;
  FillConsoleOutputAttribute(term->hcon,   0, (DWORD)length, start, &written);
  FillConsoleOutputCharacterA(term->hcon, ' ', (DWORD)length, start, &written);
}

static WORD attr_color[8] = {
  0,                                  // black
  FOREGROUND_RED,                     // maroon
  FOREGROUND_GREEN,                   // green
  FOREGROUND_RED | FOREGROUND_GREEN,  // orange
  FOREGROUND_BLUE,                    // navy
  FOREGROUND_RED | FOREGROUND_BLUE,   // purple
  FOREGROUND_GREEN | FOREGROUND_BLUE, // teal
  FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE, // light gray
};

static void term_esc_attr( term_t* term, ssize_t cmd ) {
  WORD def_attr = term->hcon_default_attr;
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo( term->hcon, &info )) return;  
  WORD cur_attr = info.wAttributes;
  WORD attr = cur_attr; 
  if (cmd==0) {
    attr = def_attr;
  }
  else if (cmd == 4) {  // underline
    attr |= COMMON_LVB_UNDERSCORE;
  }
  else if (cmd == 24) {  // not underline
    attr &= ~COMMON_LVB_UNDERSCORE;
  }
  else if (cmd == 7) {  // reverse
    attr |= COMMON_LVB_REVERSE_VIDEO;
  }
  else if (cmd == 27) {  // not reverse
    attr &= ~COMMON_LVB_REVERSE_VIDEO;
  }
  else if (!term->nocolor) {
    if (cmd >= 30 && cmd <= 37) {  // fore ground
      attr = (attr & ~0x0F) | attr_color[cmd - 30];
    }
    else if (cmd >= 90 && cmd <= 97) {  // fore ground bright
      attr = (attr & ~0x0F) | attr_color[cmd - 90] | FOREGROUND_INTENSITY;
    }
    else if (cmd >= 40 && cmd <= 47) {  // back ground
      attr = (attr & ~0xF0) | (WORD)(attr_color[cmd - 40] << 4);
    }
    else if (cmd >= 100 && cmd <= 107) {  // back ground bright
      attr = (attr & ~0xF0u) | (WORD)(attr_color[cmd - 100] << 4) | BACKGROUND_INTENSITY;
    }
    else if (cmd == 39) {  // default fore ground
      attr = (attr & ~0x0F) | (def_attr & 0x0F);
    }
    else if (cmd == 49) {  // default back ground
      attr = (attr & ~0xF0) | (def_attr & 0xF0);
    }
  }
  if (attr != cur_attr) {
    SetConsoleTextAttribute(term->hcon, attr);
  }
}

static ssize_t esc_param( const char* s, ssize_t len, ssize_t def ) {
  rp_unused(len);
  ssize_t n = def;
  sscanf(s, "%zd", &n);
  return n;
}

static void esc_param2( const char* s, ssize_t len, ssize_t* p1, ssize_t* p2, ssize_t def ) {
  rp_unused(len);
  *p1 = def;
  *p2 = def;
  sscanf(s, "%zd;%zd", p1, p2);  
}

static void term_write_esc( term_t* term, const char* s, ssize_t len ) {
  ssize_t row;
  ssize_t col;
  if (s[1] == '[') {
    switch (s[len-1]) {
    case 'A':
      term_move_cursor(term, -1, 0, esc_param(s+2, len, 1));
      break;
    case 'B':
      term_move_cursor(term, 1, 0, esc_param(s+2, len, 1));
      break;
    case 'C':
      term_move_cursor(term, 0, 1, esc_param(s+2, len, 1));
      break;
    case 'D':
      term_move_cursor(term, 0, -1, esc_param(s+2, len, 1));
      break;
    case 'H': 
      esc_param2(s+2, len, &row, &col, 1);
      term_move_cursor_to(term, row, col);
      break;
    case 'K':
      term_erase_line(term, esc_param(s+2, len, 0));
      break;
    case 'm':
      term_esc_attr(term, esc_param(s+2, len, 0));
      break;

    // support some less standard escape codes (currently not used by repline)
    case 'E':  // line down
      term_get_cursor(term, &row, &col);
      row += esc_param(s+2, len, 1);
      term_move_cursor_to(term, row, 1);
      break;
    case 'F':  // line up
      term_get_cursor(term, &row, &col);
      row -= esc_param(s+2, len, 1);
      term_move_cursor_to(term, row, 1);
      break;
    case 'G':  // absolute column
      term_get_cursor(term, &row, &col);
      col = esc_param(s+2, len, 1);
      term_move_cursor_to(term, row, col);
      break;
    case 'J': 
      term_clear_screen(term, esc_param(s+2, len, 0));
      break;
    case 'h':
      if (strncmp(s+2, "?25h", 4) == 0) {
        term_cursor_visible(term, true);
      }
      break;
    case 'l': 
      if (strncmp(s+2, "?25l", 4) == 0) {
        term_cursor_visible(term, false);
      }
      break;
    case 's': 
      term_cursor_save(term);
      break;    
    case 'u': 
      term_cursor_restore(term);
      break;
    }
  }
  // otherwise ignore
}

static bool term_write_direct(term_t* term, const char* s, ssize_t len ) {
  term_cursor_visible(term,false); // reduce flicker
  ssize_t pos = 0;
  while( pos < len ) {
    // handle non-control in bulk
    ssize_t nonctrl = 0;
    ssize_t next;
    while( (next = str_next_ofs( s, len, pos+nonctrl, true, NULL )) > 0 && (uint8_t)s[pos + nonctrl] >= ' ') {
      nonctrl += next;
    }
    if (nonctrl > 0) {
      term_write_console(term, s+pos, nonctrl);
      pos += nonctrl;
    }    
    if (next <= 0) break;

    // handle control (note: str_next_ofs considers whole CSI escape sequences at a time)
    if (next > 1 && s[pos] == '\x1B') {
      term_write_esc(term, s+pos, next);
    }
    else {
      term_write_console( term, s+pos, next);
    }
    pos += next;
  }
  term_cursor_visible(term,true);
  assert(pos == len);
  return (pos == len); 
}

#endif


//-------------------------------------------------------------
// Enable/disable terminal raw mode
//-------------------------------------------------------------

#if !defined(_WIN32)

rp_private void term_start_raw(term_t* term) {
  if (term->raw_enabled) return; 
  term->raw_enabled = true;  
}

rp_private void term_end_raw(term_t* term) {
  if (!term->raw_enabled) return;
  term->raw_enabled = false;
}

static void term_init_raw(term_t* term) {
  rp_unused(term);
}

#else

rp_private void term_start_raw(term_t* term) {
  if (term->raw_enabled) return;
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo( term->hcon, &info )) {
    term->hcon_orig_attr = info.wAttributes;
  }
	GetConsoleMode( term->hcon, &term->hcon_orig_mode );
  term->hcon_orig_cp = GetConsoleOutputCP(); 
  SetConsoleOutputCP(65001);  // set to UTF-8
  SetConsoleMode( term->hcon, ENABLE_PROCESSED_OUTPUT   // for \r \n and \b
                             #ifdef ENABLE_LVB_GRID_WORLDWIDE 
                             | ENABLE_LVB_GRID_WORLDWIDE // for underline
                             #endif
                             #ifdef ENABLE_VIRTUAL_TERMINAL_PROCESSING 
                             // | ENABLE_VIRTUAL_TERMINAL_PROCESSING // we already emulate ourselves 
                             #endif
                             );
  term->raw_enabled = true;  
}

rp_private void term_end_raw(term_t* term) {
  if (!term->raw_enabled) return;
  SetConsoleMode( term->hcon, term->hcon_orig_mode );
  SetConsoleOutputCP(term->hcon_orig_cp);
  SetConsoleTextAttribute(term->hcon, term->hcon_orig_attr);
  term->raw_enabled = false;
}

static void term_init_raw(term_t* term) {
  term->hcon = GetStdHandle( STD_OUTPUT_HANDLE );
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo( term->hcon, &info )) {
    term->hcon_default_attr = info.wAttributes;
  }
}

#endif

//-------------------------------------------------------------
// Update terminal dimensions
//-------------------------------------------------------------

#if !defined(_WIN32)

static bool term_get_cursor_pos( term_t* term, tty_t* tty, int* row, int* col) 
{
  // send request
  if (!term_write(term, RP_CSI "6n")) return false;
 
  // parse response ESC[%d;%dR
  char buf[64];
  int len = 0;
  char c = 0;
  if (!tty_readc_noblock(tty,&c) || c != '\x1B') return false;
  if (!tty_readc_noblock(tty,&c) || c != '[')    return false;
  while( len < 63 ) {
    if (!tty_readc_noblock(tty,&c)) return false;
    if (!((c >= '0' && c <= '9') || (c == ';'))) break;
    buf[len] = c;
    len++;    
  }
  buf[len] = 0;
  return (sscanf(buf,"%d;%d",row,col) == 2);
}

static void term_set_cursor_pos( term_t* term, int row, int col ) {
  term_writef( term, 128, RP_CSI "%d;%dH", row, col );
}

rp_private bool term_update_dim(term_t* term, tty_t* tty) {
  int cols = 0;
  int rows = 0;
  struct winsize ws;
  if (ioctl(1, TIOCGWINSZ, &ws) >= 0) {
    // ioctl succeeded
    cols = ws.ws_col;  // debuggers return 0 for the column
    rows = ws.ws_row;
  }
  else {
    // determine width by querying the cursor position
    debug_msg("term: ioctl term-size failed: %d,%d\n", ws.ws_row, ws.ws_col);
    int col0 = 0;
    int row0 = 0;
    if (term_get_cursor_pos(term,tty,&row0,&col0)) {
      term_set_cursor_pos(term,999,999);
      int col1 = 0;
      int row1 = 0;
      if (term_get_cursor_pos(term,tty,&row1,&col1)) {
        cols = col1;
        rows = row1;
      }
      term_set_cursor_pos(term,row0,col0);
    }
    else {
      // cannot query position
      // return 0 column
    }
  }

  // update width and return if it changed.
  debug_msg("terminal dim: %d,%d\n", rows, cols);  
  bool changed = (term->width != cols || term->height != rows);
  term->width = cols;
  term->height = rows;
  return changed;  
}

#else

rp_private bool term_update_dim(term_t* term, tty_t* tty) {
  rp_unused(tty);
  if (term->hcon == 0) {
    term->hcon = GetConsoleWindow();
  }
  ssize_t rows = 0;
  ssize_t cols = 0;  
  CONSOLE_SCREEN_BUFFER_INFO sbinfo;  
  if (GetConsoleScreenBufferInfo(term->hcon, &sbinfo)) {
     cols = sbinfo.srWindow.Right - sbinfo.srWindow.Left + 1;
     rows = sbinfo.srWindow.Bottom - sbinfo.srWindow.Top + 1;
  }
  bool changed = (term->width != cols || term->height != rows);
  term->width = cols;
  term->height = rows;
  debug_msg("term: update dim: %zd, %zd\n", term->height, term->width );
  return changed;
}

#endif
