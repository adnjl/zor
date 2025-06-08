/*includes*/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*defines*/
#define ZOR_VERSION "0.0.1"
#define ZOR_TAB_STOP 8
#define ZOR_QUIT_TIMES 1
#define ZOR_COMMAND_BUFFER_SIZE 256

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorMappings {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN,
};

enum editorHighlight {
  HL_NORMAL = 0,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH,
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

enum editorModes { INSERT_MODE, NORMAL_MODE, COMMAND_MODE };

/*data*/

struct termios orig_termios;

struct editorSyntax {
  char *filetype;
  char **filematch;
  int flags;
};

typedef struct editorRow {
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
} editorRow;

struct editorConf {
  int cx, cy;
  int rx;
  int row_off;
  int col_off;
  int screen_rows;
  int screen_cols;
  int num_rows;
  editorRow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  char command_buffer[ZOR_COMMAND_BUFFER_SIZE];
  int command_len;
  struct editorSyntax *syntax;
  struct termios orig_termios;
  enum editorModes mode;
};

struct editorConf editorConf;

/*filetypes*/

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        HL_HIGHLIGHT_NUMBERS || HL_HIGHLIGHT_STRINGS,
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*prototypes*/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*terminal*/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &editorConf.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  tcgetattr(STDIN_FILENO, &editorConf.orig_termios);
  atexit(disableRawMode);

  struct termios raw = editorConf.orig_termios;

  tcgetattr(STDIN_FILENO, &raw);

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag &= ~(CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  };

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

int getCursorPosition(int *rows, int *cols) {

  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }

  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*syntax highlighter*/

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(editorRow *row) {
  row->hl = realloc(row->hl, row->rsize);
  memset(row->hl, HL_NORMAL, row->rsize);

  if (editorConf.syntax == NULL)
    return;

  int prev_sep = 1;
  int in_string = 0;

  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

    if (editorConf.syntax->flags & HL_HIGHLIGHT_STRINGS) {

      if (in_string) {
        row->hl[i] = HL_STRING;
        if (c == in_string)
          in_string = 0;
        i++;
        prev_sep = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          in_string = c;
          row->hl[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    if (editorConf.syntax->flags & HL_HIGHLIGHT_NUMBERS) {

      if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
          (c == '.' && prev_hl == HL_NUMBER)) {
        row->hl[i] = HL_NUMBER;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }
}

int editorSyntaxToColor(int hl) {
  switch (hl) {
  case HL_STRING:
    return 35;
  case HL_NORMAL:
    return 31;
  case HL_MATCH:
    return 105;
  default:
    return 37;
  }
}

void editorSelectSyntaxHighlight() {
  editorConf.syntax = NULL;
  if (editorConf.filename == NULL)
    return;

  char *ext = strrchr(editorConf.filename, '.');

  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct editorSyntax *s = &HLDB[j];
    unsigned int i = 0;
    while (s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.');
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
          (!is_ext && strstr(editorConf.filename, s->filematch[i]))) {
        editorConf.syntax = s;

        int file_row;
        for (file_row = 0; file_row < editorConf.num_rows; file_row++) {
          editorUpdateSyntax(&editorConf.row[file_row]);
        }

        return;
      }
    }
  }
}

/*row handler*/

int editorRowCxToRx(editorRow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (ZOR_TAB_STOP - 1) - (rx % ZOR_TAB_STOP);
    rx++;
  }
  return rx;
}

int editorRowRxToCx(editorRow *row, int rx) {
  int curr_rx = 0;
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t')
      curr_rx += (ZOR_TAB_STOP - 1) - (curr_rx % ZOR_TAB_STOP);
    curr_rx++;
    if (curr_rx > rx)
      return cx;
  }
  return cx;
}

void editorUpdateRow(editorRow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      tabs++;
  }
  free(row->render);
  row->render = malloc(row->size + tabs * (ZOR_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % ZOR_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}

void editorInsertRow(int pos, char *s, size_t len) {
  if (pos < 0 || pos > editorConf.num_rows)
    return;
  editorConf.row =
      realloc(editorConf.row, sizeof(editorRow) * (editorConf.num_rows + 1));
  memmove(&editorConf.row[pos + 1], &editorConf.row[pos],
          sizeof(editorRow) * (editorConf.num_rows - pos));
  editorConf.row =
      realloc(editorConf.row, sizeof(editorRow) * (editorConf.num_rows + 1));

  editorConf.row[pos].size = len;
  editorConf.row[pos].chars = malloc(len + 1);
  memcpy(editorConf.row[pos].chars, s, len);
  editorConf.row[pos].chars[len] = '\0';

  editorConf.row[pos].rsize = 0;
  editorConf.row[pos].render = NULL;
  editorConf.row[pos].hl = NULL;
  editorUpdateRow(&editorConf.row[pos]);

  editorConf.num_rows++;
  editorConf.dirty++;
}

void editorFreeRow(editorRow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

void editorDeleteRow(int pos) {
  if (pos < 0 || pos >= editorConf.num_rows)
    return;
  editorFreeRow(&editorConf.row[pos]);
  memmove(&editorConf.row[pos], &editorConf.row[pos + 1],
          sizeof(editorRow) * (editorConf.num_rows - pos - 1));
  editorConf.num_rows--;
  editorConf.dirty++;
}

void editorRowInsertChar(editorRow *row, int pos, int c) {
  if (pos < 0 || pos > row->size)
    pos = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[pos + 1], &row->chars[pos], row->size - pos + 1);
  row->size++;
  row->chars[pos] = c;
  editorUpdateRow(row);
  editorConf.dirty++;
}

void editorRowAppendString(editorRow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  editorConf.dirty++;
}

void editorRowDeleteChar(editorRow *row, int pos) {
  if (pos < 0 || pos >= row->size)
    return;
  memmove(&row->chars[pos], &row->chars[pos + 1], row->size - pos);
  row->size--;
  editorUpdateRow(row);
  editorConf.dirty++;
}

/*editor operations*/

void editorInsertChar(int c) {
  if (editorConf.cy == editorConf.num_rows) {
    editorInsertRow(editorConf.num_rows, "", 0);
  }
  editorRowInsertChar(&editorConf.row[editorConf.cy], editorConf.cx, c);
  editorConf.cx++;
}

void editorInsertNewline() {
  if (editorConf.cx == 0) {
    editorInsertRow(editorConf.cy, "", 0);
  } else {
    editorRow *row = &editorConf.row[editorConf.cy];
    editorInsertRow(editorConf.cy + 1, &row->chars[editorConf.cx],
                    row->size - editorConf.cx);
    row = &editorConf.row[editorConf.cy];
    row->size = editorConf.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  editorConf.cy++;
  editorConf.cx = 0;
}

void editorDeleteChar() {
  if (editorConf.cy == editorConf.num_rows)
    return;
  if (editorConf.cx == 0 && editorConf.cy == 0)
    return;
  editorRow *row = &editorConf.row[editorConf.cy];
  if (editorConf.cx > 0) {
    editorRowDeleteChar(row, editorConf.cx - 1);
    editorConf.cx--;
  } else {
    editorConf.cx = editorConf.row[editorConf.cy - 1].size;
    editorRowAppendString(&editorConf.row[editorConf.cy - 1], row->chars,
                          row->size);
    editorDeleteRow(editorConf.cy);
    editorConf.cy--;
  }
}

/*file i/o*/

char *editorRowsToString(int *buflen) {
  int totalLen = 0;
  int j;
  for (j = 0; j < editorConf.num_rows; j++) {
    totalLen += editorConf.row[j].size + 1;
  }
  buflen[0] = totalLen;

  char *buf = malloc(totalLen);
  char *p = buf;
  for (j = 0; j < editorConf.num_rows; j++) {
    memcpy(p, editorConf.row[j].chars, editorConf.row[j].size);
    p += editorConf.row[j].size;
    *p = '\n';
    p++;
  }
  return buf;
}

void editorOpen(char *filename) {
  free(editorConf.filename);
  editorConf.filename = strdup(filename);

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t line_cap = 0;
  ssize_t line_len;
  while ((line_len = getline(&line, &line_cap, fp)) != -1) {
    while (line_len > 0 &&
           (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
      line_len--;
    }
    editorInsertRow(editorConf.num_rows, line, line_len);
  }
  free(line);
  fclose(fp);
  editorConf.dirty = 0;
}

void editorSave() {
  if (editorConf.filename == NULL) {
    editorConf.filename = editorPrompt("Save as: %s", NULL);
    if (editorConf.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(editorConf.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        editorConf.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorExecuteCommand(char *command) {
  static int quit_times = ZOR_QUIT_TIMES;
  if (strcmp(command, "q") == 0) {
    if (editorConf.dirty) {
      editorSetStatusMessage("File has unsaved changes. Quit anyway? (y/n)");
      /*if (editorPrompt("File has unsaved changes. Quit anyway? (y/n)") == 'y')
       * {*/
      /*  write(STDOUT_FILENO, "\x1b[2J", 4);*/
      /*  write(STDOUT_FILENO, "\x1b[H", 3);*/
      /*  exit(0);*/
      /*}*/
      if (editorConf.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                               "Press Ctrl-Q %d more times to quit.",
                               quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
    } else {
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
    }
  } else if (strcmp(command, "q!") == 0) {
    exit(0);
  } else if (strcmp(command, "w") == 0) {
    editorSave();
  } else if (strcmp(command, "wq") == 0) {
    editorSave();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
  } else {
    editorSetStatusMessage("Unknown command: %s", command);
  }
}

/*search*/

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line;
  static char *saved_hl_search = NULL;

  if (saved_hl_search) {
    memcpy(editorConf.row[saved_hl_line].hl, saved_hl_search,
           editorConf.row[saved_hl_line].rsize);
    free(saved_hl_search);
    saved_hl_search = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) {
    direction = 1;
  }
  int current = last_match;

  for (int i = 0; i < editorConf.num_rows; i++) {
    current += direction;
    if (current == -1)
      current = editorConf.num_rows - 1;
    else if (current == editorConf.num_rows)
      current = 0;

    editorRow *row = &editorConf.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      editorConf.cy = current;
      editorConf.cx = editorRowRxToCx(row, match - row->render);
      editorConf.row_off = editorConf.num_rows;

      saved_hl_line = current;
      saved_hl_search = malloc(row->rsize);
      memcpy(saved_hl_search, row->hl, row->rsize);

      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      return;
    }
  }
}

void editorFind() {
  int saved_cx = editorConf.cx;
  int saved_cy = editorConf.cy;
  int saved_coll_off = editorConf.col_off;
  int saved_row_off = editorConf.row_off;

  char *query = editorPrompt("Search: %s", editorFindCallback);

  if (query) {
    free(query);
  } else {
    editorConf.cx = saved_cx;
    editorConf.cy = saved_cy;
    editorConf.col_off = saved_coll_off;
    editorConf.row_off = saved_row_off;
  }
}

/*buffer*/

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT                                                              \
  { NULL, 0 }

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);
  if (new == NULL)
    return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}
void abFree(struct abuf *ab) { free(ab->b); }

/*output*/

void editorScroll() {
  editorConf.rx = 0;
  if (editorConf.cy < editorConf.num_rows) {
    editorConf.rx =
        editorRowCxToRx(&editorConf.row[editorConf.cy], editorConf.cx);
  }

  editorConf.rx = editorConf.cx;
  if (editorConf.cy < editorConf.row_off) {
    editorConf.row_off = editorConf.cy;
  }
  if (editorConf.cy >= editorConf.row_off + editorConf.screen_rows) {
    editorConf.row_off = editorConf.cy - editorConf.screen_rows + 1;
  }
  if (editorConf.rx < editorConf.col_off) {
    editorConf.col_off = editorConf.rx;
  }
  if (editorConf.rx >= editorConf.col_off + editorConf.screen_cols) {
    editorConf.col_off = editorConf.rx - editorConf.screen_cols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < editorConf.screen_rows; y++) {
    int file_row = y + editorConf.row_off;
    if (file_row >= editorConf.num_rows) {
      if (editorConf.num_rows == 0 && y == editorConf.screen_rows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "ZOR EDITOR -- VERSION %s", ZOR_VERSION);
        if (welcomelen > editorConf.screen_cols)
          welcomelen = editorConf.screen_cols;
        int padding = (editorConf.screen_cols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--)
          abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = editorConf.row[file_row].rsize - editorConf.col_off;
      if (len < 0)
        len = 0;
      if (len > editorConf.screen_cols)
        len = editorConf.screen_cols;

      char *c = &editorConf.row[file_row].render[editorConf.col_off];
      unsigned char *hl = &editorConf.row[file_row].hl[editorConf.col_off];
      int current_color = -1;

      for (int j = 0; j < len; j++) {
        if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          int color = editorSyntaxToColor(hl[j]);
          if (color != current_color) {
            current_color = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  const char *mode;
  switch (editorConf.mode) {
  case NORMAL_MODE:
    mode = " NORMAL |";
    break;
  case INSERT_MODE:
    mode = " INSERT |";
    break;
  case COMMAND_MODE:
    mode = " COMMAND |";
    break;
  default:
    mode = " NORMAL |";
    break;
  }
  /*const char *mode =*/
  /*    (editorConf.mode == NORMAL_MODE) ? " NORMAL |" : " INSERT |";*/
  int len = snprintf(status, sizeof(status), "%s %.20s - %d lines %s", mode,
                     editorConf.filename ? editorConf.filename : "[No Name]",
                     editorConf.num_rows, editorConf.dirty ? "(modified)" : "");

  int rlen =
      snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
               editorConf.syntax ? editorConf.syntax->filetype : "no filetype",
               editorConf.cy + 1, editorConf.num_rows);
  if (len > editorConf.screen_cols)
    len = editorConf.screen_cols;
  abAppend(ab, status, len);
  while (len < editorConf.screen_cols) {
    if (editorConf.screen_cols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(editorConf.statusmsg);
  if (msglen > editorConf.screen_cols)
    msglen = editorConf.screen_cols;
  if (msglen)
    if (msglen && time(NULL) - editorConf.statusmsg_time < 5)
      abAppend(ab, editorConf.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
           (editorConf.cy - editorConf.row_off) + 1,
           (editorConf.rx - editorConf.col_off) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(editorConf.statusmsg, sizeof(editorConf.statusmsg), fmt, ap);
  va_end(ap);
  editorConf.statusmsg_time = time(NULL);
}

/*input*/

void editorHandleCommand(int c) {
  if (c == '\r') {
    editorConf.command_buffer[editorConf.command_len] = '\0';
    editorExecuteCommand(editorConf.command_buffer);
    editorConf.mode = NORMAL_MODE;
    editorSetStatusMessage("");
  } else if (c == 27) {
    editorConf.mode = NORMAL_MODE;
    editorSetStatusMessage("");
  } else if (c == BACKSPACE || c == CTRL_KEY('h')) {
    if (editorConf.command_len > 0) {
      editorConf.command_buffer[--editorConf.command_len] = '\0';
    }
  } else if (isprint(c) &&
             editorConf.command_len < ZOR_COMMAND_BUFFER_SIZE - 1) {
    editorConf.command_buffer[editorConf.command_len++] = c;
    editorConf.command_buffer[editorConf.command_len] = '\0';
  }
  editorSetStatusMessage(":%s", editorConf.command_buffer);
}

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) {
        buf[--buflen] = '\0';
      }
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback)
        callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
    if (callback)
      callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  editorRow *row = (editorConf.cy >= editorConf.num_rows)
                       ? NULL
                       : &editorConf.row[editorConf.cy];
  switch (key) {
  case ARROW_LEFT:
    if (editorConf.cx != 0) {

      editorConf.cx--;
    } else if (editorConf.cy > 0) {
      editorConf.cy--;
      editorConf.cx = editorConf.row[editorConf.cy].size;
    }
    break;
  case ARROW_RIGHT:
    /*if (editorConf.cx != editorConf.screen_cols - 1)*/
    if (row && row->size > editorConf.cx) {
      editorConf.cx++;
    } else if (row && editorConf.cx == row->size) {
      editorConf.cy++;
      editorConf.cx = 0;
    }
    break;
  case ARROW_UP:
    if (editorConf.cy != 0)
      editorConf.cy--;
    break;
  case ARROW_DOWN:
    if (editorConf.cy <= editorConf.num_rows)
      editorConf.cy++;
    break;
  }

  row = (editorConf.cy >= editorConf.num_rows) ? NULL
                                               : &editorConf.row[editorConf.cy];
  int row_len = row ? row->size : 0;
  if (editorConf.cx > row_len)
    editorConf.cx = row_len;
}

void editorProccessKeypress() {
  static int quit_times = ZOR_QUIT_TIMES;
  int c = editorReadKey();

  switch (editorConf.mode) {

  case NORMAL_MODE:
    switch (c) {
    case 'i':
    case 'I':
      editorConf.mode = INSERT_MODE;
      break;
    /*case 'v':*/
    /*case 'V':*/
    /*  editorConf.mode = VISUAL_MODE;*/
    /*  break;*/
    case ':':
      editorConf.mode = COMMAND_MODE;
      editorConf.command_len = 0;
      editorConf.command_buffer[0] = '\0';
      editorSetStatusMessage(":");
      break;
    case 'h':
      editorMoveCursor(ARROW_LEFT);
      break;
    case 'j':
      editorMoveCursor(ARROW_DOWN);
      break;
    case 'k':
      editorMoveCursor(ARROW_UP);
      break;
    case 'l':
      editorMoveCursor(ARROW_RIGHT);
      break;
    case '/':
      editorFind();
      break;

    case CTRL_KEY('u'):
    case CTRL_KEY('d'): {

      if (c == CTRL_KEY('u')) {
        editorConf.cy = editorConf.row_off;
      } else if (c == CTRL_KEY('d')) {
        editorConf.cy = editorConf.row_off + editorConf.screen_rows - 1;
        if (editorConf.cy > editorConf.num_rows)
          editorConf.cy = editorConf.num_rows;
      }

      int times = editorConf.screen_rows;
      while (times--) {
        editorMoveCursor(c == CTRL_KEY('u') ? ARROW_UP : ARROW_DOWN);
      }
    } break;

    case HOME_KEY:
      editorConf.cx = 0;
      break;
    case END_KEY:
      if (editorConf.cy < editorConf.num_rows)
        editorConf.cx = editorConf.row[editorConf.cy].size;
      break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY)
        editorMoveCursor(ARROW_RIGHT);
      editorDeleteChar();
      break;
    case PAGE_UP:
    case PAGE_DOWN: {
      if (c == PAGE_UP) {
        editorConf.cy = editorConf.row_off;
      } else if (c == PAGE_DOWN) {
        editorConf.cy = editorConf.row_off + editorConf.screen_rows - 1;
        if (editorConf.cy > editorConf.num_rows)
          editorConf.cy = editorConf.num_rows;
      }

      int times = editorConf.screen_rows;
      while (times--) {
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
    } break;
    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
      editorMoveCursor(c);
      break;
      /*case CTRL_KEY('q'):*/
      /*  if (editorConf.dirty && quit_times > 0) {*/
      /*    editorSetStatusMessage("WARNING!!! File has unsaved changes. "*/
      /*                           "Press Ctrl-Q %d more times to quit.",*/
      /*                           quit_times);*/
      /*    quit_times--;*/
      /*    return;*/
      /*  }*/
      /*  write(STDOUT_FILENO, "\x1b[2J", 4);*/
      /*  write(STDOUT_FILENO, "\x1b[H", 3);*/
      /*  exit(0);*/
      /*  break;*/
      /*case CTRL_KEY('s'):*/
      /*  editorSave();*/
      /*  break;*/
    }
    break;

  case INSERT_MODE:
    switch (c) {
    case '\r':
      editorInsertNewline();
      break;
    case CTRL_KEY('q'):
      if (editorConf.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                               "Press Ctrl-Q %d more times to quit.",
                               quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      editorConf.cx = 0;
      break;
    case END_KEY:
      if (editorConf.cy < editorConf.num_rows)
        editorConf.cx = editorConf.row[editorConf.cy].size;
      break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY)
        editorMoveCursor(ARROW_RIGHT);
      editorDeleteChar();
      break;
    case PAGE_UP:
    case PAGE_DOWN: {
      if (c == PAGE_UP) {
        editorConf.cy = editorConf.row_off;
      } else if (c == PAGE_DOWN) {
        editorConf.cy = editorConf.row_off + editorConf.screen_rows - 1;
        if (editorConf.cy > editorConf.num_rows)
          editorConf.cy = editorConf.num_rows;
      }

      int times = editorConf.screen_rows;
      while (times--) {
        editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
    } break;

    case ARROW_LEFT:
    case ARROW_RIGHT:
    case ARROW_UP:
    case ARROW_DOWN:
      editorMoveCursor(c);
      break;
    case CTRL_KEY('l'):
    case '\x1b':
      editorConf.mode = NORMAL_MODE;
      break;
    default:
      editorInsertChar(c);
      break;
    }
    break;
  case COMMAND_MODE:
    editorHandleCommand(c);
    break;
  }
  quit_times = ZOR_QUIT_TIMES;
}

/*init*/

void initEditor() {
  editorConf.cx = 0;
  editorConf.cy = 0;
  editorConf.rx = 0;
  editorConf.row_off = 0;
  editorConf.col_off = 0;
  editorConf.num_rows = 0;
  editorConf.row = NULL;
  editorConf.dirty = 0;
  editorConf.filename = NULL;
  editorConf.statusmsg[0] = '\0';
  editorConf.statusmsg_time = 0;
  editorConf.syntax = NULL;
  editorConf.mode = NORMAL_MODE;

  if (getWindowSize(&editorConf.screen_rows, &editorConf.screen_cols) == -1)
    die("getWindowSize");
  editorConf.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();

  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  /*editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");*/

  while (1) {
    editorRefreshScreen();
    editorProccessKeypress();
  };
  return 0;
}
