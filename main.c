#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>


#define CTRL_KEY(k)           ((k) & 0x1f)

#define CLEAR_ENTIRE_SCREEN   "\x1b[2J"
#define CLEAR_LINE            "\x1b[K"
#define INIT_CURSOR_POSITION  "\x1b[H"
#define INVISIBLE_CURSOR      "\x1b[?25l"
#define VISIBLE_CURSOR        "\x1b[?25h"
#define INVERT_COLORS         "\x1b[7m"
#define REINVERT_COLORS       "\x1b[m"

#define LINE_SIZE             80

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};


typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

struct editorConfig {
  int c_x, c_y;
  int row_offset;
  int col_offset;
  int rows;
  int cols;
  int n_rows;
  erow *row;
  char *filename;
  char last_save[64];
  struct termios orig_termios;
};

struct editorConfig EDI;


void die(const char *msg) {
  write(STDOUT_FILENO, CLEAR_ENTIRE_SCREEN, 4);
  write(STDOUT_FILENO, INIT_CURSOR_POSITION, 3);

  perror(msg);

  exit(EXIT_FAILURE);
}

void disable_raw_mode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EDI.orig_termios) == -1)
    die("tcsetattr");
}

void enable_raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &EDI.orig_termios) == -1) 
    die("tcgetattr failed");
  atexit(disable_raw_mode);

  struct termios raw = EDI.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr failed");
}

int editor_read_key(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read failed");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int get_cursor_position(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return EXIT_FAILURE;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return EXIT_FAILURE;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return EXIT_FAILURE;

  return EXIT_SUCCESS;
}

int get_window_size(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
      return EXIT_FAILURE;
    return get_cursor_position(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return EXIT_SUCCESS;
  }
}

void editor_update_row(erow *row) {
  free(row->render);
  row->render = malloc(row->size + 1);
  int j;
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    row->render[idx++] = row->chars[j];
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editor_append_row(char *s, size_t len) {
  EDI.row = realloc(EDI.row, sizeof(erow) * (EDI.n_rows + 1));

  int at = EDI.n_rows;
  EDI.row[at].size = len;
  EDI.row[at].chars = malloc(len + 1);
  memcpy(EDI.row[at].chars, s, len);
  EDI.row[at].chars[len] = '\0';
  
  EDI.row[at].rsize = 0;
  EDI.row[at].render = NULL;
  editor_update_row(&EDI.row[at]);

  EDI.n_rows++;
}

void editor_row_insert_char(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editor_update_row(row);
}

void editor_insert_char(int c) {
  if (EDI.c_y == EDI.n_rows) {
    editor_append_row("", 0);
  }
  editor_row_insert_char(&EDI.row[EDI.c_y], EDI.c_x, c);
  EDI.c_x++;
}

char *edior_rows_to_string(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < EDI.n_rows; j++)
    totlen += EDI.row[j].size + 1;
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < EDI.n_rows; j++) {
    memcpy(p, EDI.row[j].chars, EDI.row[j].size);
    p += EDI.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editor_open(char *filename) {
  free(EDI.filename);
  EDI.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editor_append_row(line, linelen);
  }
  free(line);
  fclose(fp);
}

void editor_save(void) {
  if (EDI.filename == NULL) return;
  int len;
  char *buf = edior_rows_to_string(&len);
  int fd = open(EDI.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        strftime(EDI.last_save, sizeof(EDI.last_save), "%c", tm);
        return;
      }
    }
    close(fd);
  }
  free(buf);
}


struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abuf_append(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abuf_free(struct abuf *ab) {
  free(ab->b);
}

void editor_scroll(void) {
  if (EDI.c_y < EDI.row_offset) EDI.row_offset = EDI.c_y;
  if (EDI.c_y >= EDI.row_offset + EDI.rows) 
    EDI.row_offset = EDI.c_y - EDI.rows + 1;

  if (EDI.c_x < EDI.col_offset) EDI.col_offset = EDI.c_x;
  if (EDI.c_x >= EDI.col_offset + EDI.cols) 
    EDI.col_offset = EDI.c_x - EDI.cols + 1;
}

void editor_draw_rows(struct abuf *ab) {
  int i;
  for (i = 0; i < EDI.rows; i++) {
    int file_row = i + EDI.row_offset;
    if (file_row >= EDI.n_rows) {
      if (EDI.n_rows == 0 && i == EDI.rows / 3) {
        char msg[80];
        int msglen = snprintf(msg, sizeof(msg), 
                              "type CTRL + q and give me text");
        if (msglen > EDI.cols) msglen = EDI.cols;
        int padding = (EDI.cols - msglen) / 2;
        if (padding) {
          abuf_append(ab, "~", 1);
          padding--;
        }
        while (padding--) abuf_append(ab, " ", 1);
        abuf_append(ab, msg, msglen);
      } else {
        abuf_append(ab, "~", 1);
      }
    } else {
      int len = EDI.row[file_row].rsize - EDI.col_offset;
      if (len < 0) len = 0;
      abuf_append(ab, &EDI.row[file_row].render[EDI.col_offset], len);
    }

    abuf_append(ab, CLEAR_LINE, 3);
    abuf_append(ab, "\r\n", 2);
  }
}

void editor_draw_status_bar(struct abuf *ab) {
  abuf_append(ab, INVERT_COLORS, 4);
  char status[80], rstatus[80];

  int len = snprintf(status, sizeof(status), " %.20s %s", 
                     EDI.filename ? EDI.filename : "unsaved", EDI.last_save);
  int rlen = snprintf(rstatus, sizeof(rstatus), "~%d ", 
                      EDI.n_rows * LINE_SIZE);
  if (len > EDI.cols) len = EDI.cols;
  abuf_append(ab, status, len);

  while (len < EDI.cols) {
    if (EDI.cols - len == rlen) {
      abuf_append(ab, rstatus, rlen);
      break;
    } else {
      abuf_append(ab, " ", 1);
      len++;
    }
  }

  abuf_append(ab, REINVERT_COLORS, 3);
}

void editor_refresh_screen(void) {
  editor_scroll();

  struct abuf ab = ABUF_INIT;

  abuf_append(&ab, INVISIBLE_CURSOR, 6);
  abuf_append(&ab, INIT_CURSOR_POSITION, 3);

  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (EDI.c_y + EDI.row_offset) + 1, 
                                            (EDI.c_x + EDI.col_offset) + 1);
  abuf_append(&ab, buf, strlen(buf));

  abuf_append(&ab, VISIBLE_CURSOR, 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abuf_free(&ab);
}


void editor_move_cursor(int key) {
  erow *row = (EDI.c_y >= EDI.n_rows) ? NULL : &EDI.row[EDI.c_y];

  switch (key) {
    case ARROW_LEFT:
      if (EDI.c_x != 0) {
        EDI.c_x--;
      } else if (EDI.c_y > 0) {
        EDI.c_y--;
        EDI.c_x = EDI.row[EDI.c_y].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && EDI.c_x < row->size) {
        EDI.c_x++;
      } else if (row && EDI.c_x == row->size) {
        EDI.c_y++;
        EDI.c_x = 0;
      }
      break;
    case ARROW_UP:
      if (EDI.c_y != 0) EDI.c_y--;
      break;
    case ARROW_DOWN:
      if (EDI.c_y < EDI.n_rows) EDI.c_y++;
      break;
  }

  row = (EDI.c_y >= EDI.n_rows) ? NULL : &EDI.row[EDI.c_y];
  int row_len = row ? row->size : 0;
  if (EDI.c_x > row_len) EDI.c_x = row_len;
}

void editor_process_keypress(void) {
  int c = editor_read_key();

  switch (c) {
    case '\r':
      break;
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, CLEAR_ENTIRE_SCREEN, 4);
      write(STDOUT_FILENO, INIT_CURSOR_POSITION, 3);
      exit(EXIT_SUCCESS);
      break;

    case HOME_KEY:
      EDI.c_x = 0;
      break;

    case END_KEY:
      if (EDI.c_y < EDI.n_rows) 
        EDI.c_x = EDI.row[EDI.c_y].size;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          EDI.c_y = EDI.row_offset;
        } else if (c == PAGE_DOWN) {
          EDI.c_y = EDI.row_offset + EDI.rows - 1;
          if (EDI.c_y > EDI.n_rows) EDI.c_y = EDI.n_rows;
        } 

        int times = EDI.rows;
        while (times--)
          editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    case CTRL_KEY('s'):
      editor_save();
      break;

    case BACKSPACE:
      break;

    default:
      editor_insert_char(c);
      break;
  }
}

void editor_init(void) {
  EDI.c_x = 0;
  EDI.c_y = 0;
  EDI.n_rows = 0;
  EDI.col_offset = 0;
  EDI.row_offset = 0;
  EDI.row = NULL;
  EDI.filename = NULL;

  if (get_window_size(&EDI.rows, &EDI.cols) == -1) 
    die("get_window_size");
  EDI.rows -= 1;
}

int main(int argc, char *argv[]) {
  enable_raw_mode();
  editor_init();
  if (argc >= 2) {
    editor_open(argv[1]);
  }

  while (1) {
    editor_refresh_screen();
    editor_process_keypress();
  }

  return EXIT_SUCCESS;
}
