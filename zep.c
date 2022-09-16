/* zep.c,  Zep Emacs, Public Domain, Hugh Barney, 2017, Derived from: Anthony's Editor January 93 */
#include <stdlib.h>
#include <assert.h>
#include <curses.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>

#define E_NAME          "zep"
#define E_VERSION       "vCustom"
#define E_LABEL         "Zep:"
#define MSGLINE         (LINES-1)
#define CHUNK           8096L
#define K_BUFFER_LENGTH 256
#define MAX_FNAME       256
#define TEMPBUF         512
#define MIN_GAP_EXPAND  512
#define NOMARK          -1
#define STRBUF_M        64

typedef unsigned char Rune;
typedef ssize_t Point; // Byte index, or not found (< 0)

typedef struct {
	char *key_desc;                 /* name of bound function */
	char *key_bytes;		/* the string of bytes when this key is pressed */
	void (*func)(void);
} KeyBinding;

typedef struct {
	Point b_mark;	     	  /* the mark */
	Point b_point;          /* the point */
	Point b_page;           /* start of page */
	Point b_epage;          /* end of page */
	Rune *b_buf;            /* start of buffer */
	Rune *b_ebuf;           /* end of buffer */
	Rune *b_gap;            /* start of gap */
	Rune *b_egap;           /* end of gap */
	char w_top;	          /* Origin 0 top row of window */
	char w_rows;              /* no. of rows of text in window */
	int b_row;                /* cursor row */
	int b_col;                /* cursor col */
	// @todo Add a `b_col_goal` field to `buffer_t` and use it like the Sublime Text `xpos` concept
	// @body Emacs proper talks about the "Goal Column" concept, but usually in the sense of
	// @body setting it explicitly to override the default behaviour. All I want to recreate is
	// @body the default behaviour (not giving the user a way to explicitly set the Goal Column)
	char b_fname[MAX_FNAME + 1]; /* filename */
	char b_modified;          /* was modified */
} Buffer;

int done;
int msgflag;
char msgline[TEMPBUF];
KeyBinding *key_map;
Buffer *curbp;
Point nscrap = 0;
Rune *scrap = NULL;
char searchtext[STRBUF_M];

Buffer* new_buffer()
{
	Buffer *bp = (Buffer *)malloc(sizeof(Buffer));
	assert(bp != NULL);
	bp->b_point = 0;
	bp->b_mark = NOMARK;
	bp->b_page = 0;
	bp->b_epage = 0;
	bp->b_modified = 0;
	bp->b_buf = NULL;
	bp->b_ebuf = NULL;
	bp->b_gap = NULL;
	bp->b_egap = NULL;
	bp->b_fname[0] = '\0';
	bp->w_top = 0;	
	bp->w_rows = LINES - 2;
	return bp;
}

void fatal(char *msg)
{
	noraw();
	endwin();
	printf("\n" E_NAME " " E_VERSION ": %s\n", msg);
	exit(1);
}

int msg(char *msg, ...)
{
	va_list args;
	va_start(args, msg);
	(void)vsprintf(msgline, msg, args);
	va_end(args);
	msgflag = TRUE;
	return FALSE;
}

/* Given a buffer offset, convert it to a pointer into the buffer */
Rune * ptr(Buffer *bp, register Point offset)
{
	if (offset < 0) return (bp->b_buf);
	return (bp->b_buf+offset + (bp->b_buf + offset < bp->b_gap ? 0 : bp->b_egap-bp->b_gap));
}

/* Given a pointer into the buffer, convert it to a buffer offset */
Point pos(Buffer *bp, register Rune *cp)
{
	assert(bp->b_buf <= cp && cp <= bp->b_ebuf);
	return (cp - bp->b_buf - (cp < bp->b_egap ? 0 : bp->b_egap - bp->b_gap));
}

/* Enlarge gap by n chars, position of gap cannot change */
int growgap(Buffer *bp, Point n)
{
	Rune *new;
	Point buflen, newlen, xgap, xegap;
	assert(bp->b_buf <= bp->b_gap);
	assert(bp->b_gap <= bp->b_egap);
	assert(bp->b_egap <= bp->b_ebuf);
	xgap = bp->b_gap - bp->b_buf;
	xegap = bp->b_egap - bp->b_buf;
	buflen = bp->b_ebuf - bp->b_buf;
    
	/* reduce number of reallocs by growing by a minimum amount */
	n = (n < MIN_GAP_EXPAND ? MIN_GAP_EXPAND : n);
	newlen = buflen + n * sizeof (Rune);

	if (buflen == 0) {
		if (newlen < 0) fatal("Failed to allocate required memory");
		new = (Rune*) malloc((size_t) newlen);
		if (new == NULL) fatal("Failed to allocate required memory");
	} else {
		if (newlen < 0) return msg("Failed to allocate required memory");
		new = (Rune*) realloc(bp->b_buf, (size_t) newlen);
		if (new == NULL) return msg("Failed to allocate required memory");
	}

	/* Relocate pointers in new buffer and append the new extension to the end of the gap */
	bp->b_buf = new;
	bp->b_gap = bp->b_buf + xgap;      
	bp->b_ebuf = bp->b_buf + buflen;
	bp->b_egap = bp->b_buf + newlen;
	while (xegap < buflen--)
		*--bp->b_egap = *--bp->b_ebuf;
	bp->b_ebuf = bp->b_buf + newlen;

	assert(bp->b_buf < bp->b_ebuf);          /* Buffer must exist. */
	assert(bp->b_buf <= bp->b_gap);
	assert(bp->b_gap < bp->b_egap);          /* Gap must grow only. */
	assert(bp->b_egap <= bp->b_ebuf);
	return (TRUE);
}

Point movegap(Buffer *bp, Point offset)
{
	Rune *p = ptr(bp, offset);
	while (p < bp->b_gap)
		*--bp->b_egap = *--bp->b_gap;
	while (bp->b_egap < p)
		*bp->b_gap++ = *bp->b_egap++;
	assert(bp->b_gap <= bp->b_egap);
	assert(bp->b_buf <= bp->b_gap);
	assert(bp->b_egap <= bp->b_ebuf);
	return (pos(bp, bp->b_egap));
}

void save()
{
	FILE *fp;
	Point length;
	fp = fopen(curbp->b_fname, "w");
	if (fp == NULL) msg("Failed to open file \"%s\".", curbp->b_fname);
	(void) movegap(curbp, (Point) 0);
	length = (Point) (curbp->b_ebuf - curbp->b_egap);
	if (fwrite(curbp->b_egap, sizeof (char), (size_t) length, fp) != length) 
		msg("Failed to write file \"%s\".", curbp->b_fname);
	fclose(fp);
	curbp->b_modified = 0;
	msg("File \"%s\" %ld bytes saved.", curbp->b_fname, pos(curbp, curbp->b_ebuf));
}

/* reads file into buffer at point */
int insert_file(char *fn)
{
	FILE *fp;
	size_t len;
	struct stat sb;

	if (stat(fn, &sb) < 0) return msg("Failed to find file \"%s\".", fn);
	if (curbp->b_egap - curbp->b_gap < sb.st_size * sizeof (Rune) && !growgap(curbp, sb.st_size))
		return (FALSE);
	if ((fp = fopen(fn, "r")) == NULL) return msg("Failed to open file \"%s\".", fn);
	curbp->b_point = movegap(curbp, curbp->b_point);
	curbp->b_gap += len = fread(curbp->b_gap, sizeof (char), (size_t) sb.st_size, fp);
	if (fclose(fp) != 0) return msg("Failed to close file \"%s\".", fn);
	msg("File \"%s\" %ld bytes read.", fn, len);
	return (TRUE);
}

Rune *get_key(KeyBinding *keys, KeyBinding **key_return)
{
	KeyBinding *k;
	int submatch;
	static Rune buffer[K_BUFFER_LENGTH];
	static Rune *record = buffer;
	*key_return = NULL;

	/* if recorded bytes remain, return next recorded byte. */
	if (*record != '\0') {
		*key_return = NULL;
		return record++;
	}
	
	record = buffer; /* reset record buffer. */
	do {
		assert(K_BUFFER_LENGTH > record - buffer);
		*record++ = (unsigned)getch(); /* read and record one byte. */
		*record = '\0';

		/* if recorded bytes match any multi-byte sequence... */
		for (k = keys, submatch = 0; k->key_bytes != NULL; ++k) {
			Rune *p, *q;
			for (p = buffer, q = (Rune *)k->key_bytes; *p == *q; ++p, ++q) {
			        /* an exact match */
				if (*q == '\0' && *p == '\0') {
	    				record = buffer;
					*record = '\0';
					*key_return = k;
					return record; /* empty string */
				}
			}
			/* record bytes match part of a command sequence */
			if (*p == '\0' && *q != '\0') submatch = 1;
		}
	} while (submatch);
	/* nothing matched, return recorded bytes. */
	record = buffer;
	return (record++);
}

/* Reverse scan for start of logical line containing offset */
Point lnstart(Buffer *bp, register Point off)
{
	register Rune *p;
	do
		p = ptr(bp, --off);
	while (bp->b_buf < p && *p != '\n');
	return (bp->b_buf < p ? ++off : 0);
}

/* Forward scan for start of logical line segment containing 'finish' */
Point segstart(Buffer *bp, Point start, Point finish)
{
	Rune *p;
	int c = 0;
	Point scan = start;

	while (scan < finish) {
		p = ptr(bp, scan);
		if (*p == '\n') {
			c = 0;
			start = scan + 1;
		} else if (COLS <= c) {
			c = 0;
			start = scan;
		}
		++scan;
		c += *p == '\t' ? 8 - (c & 7) : 1;
	}
	return (c < COLS ? start : finish);
}

/* Forward scan for start of logical line segment following 'finish' */
Point segnext(Buffer *bp, Point start, Point finish)
{
	Rune *p;
	int c = 0;

	Point scan = segstart(bp, start, finish);
	for (;;) {
		p = ptr(bp, scan);
		if (bp->b_ebuf <= p || COLS <= c) break;
		++scan;
		if (*p == '\n') break;
		c += *p == '\t' ? 8 - (c & 7) : 1;
	}
	return (p < bp->b_ebuf ? scan : pos(bp, bp->b_ebuf));
}

/* Move up one screen line */
Point upup(Buffer *bp, Point off)
{
	Point curr = lnstart(bp, off);
	Point seg = segstart(bp, curr, off);
	if (curr < seg)
		off = segstart(bp, curr, seg-1);
	else
		off = segstart(bp, lnstart(bp,curr-1), curr-1);
	return (off);
}

/* Move down one screen line */
Point dndn(Buffer *bp, Point off) { return (segnext(bp, lnstart(bp,off), off)); }

/* Return the offset of a column on the specified line */
Point lncolumn(Buffer *bp, Point offset, int column)
{
	int c = 0;
	Rune *p;
	while ((p = ptr(bp, offset)) < bp->b_ebuf && *p != '\n' && c < column) {
		c += *p == '\t' ? 8 - (c & 7) : 1;
		++offset;
	}
	return (offset);
}

void modeline(Buffer *bp)
{
	int i;
	char temp[TEMPBUF];
	char mch;
	
	standout();
	move(bp->w_top + bp->w_rows, 0);
	// @todo Use proper line-drawing characters rather than ASCII-art in the modeline
	mch = bp->b_modified ? '*' : '=';
	sprintf(temp, "=%c " E_LABEL " == %s ", mch, bp->b_fname);
	addstr(temp);
	for (i = strlen(temp) + 1; i <= COLS; i++)
		addch('=');
	standend();
}

void dispmsg()
{
	move(MSGLINE, 0);
	if (msgflag) {
		addstr(msgline);
		msgflag = FALSE;
	}
	clrtoeol();
}

void display()
{
	Rune *p;
	int i, j, k;
	Buffer *bp = curbp;
	
	/* find start of screen, handle scroll up off page or top of file  */
	/* point is always within b_page and b_epage */
	if (bp->b_point < bp->b_page)
		bp->b_page = segstart(bp, lnstart(bp,bp->b_point), bp->b_point);

	/* reframe when scrolled off bottom */
	if (bp->b_epage <= bp->b_point) {
		/* Find end of screen plus one. */
		bp->b_page = dndn(bp, bp->b_point);
		/* if we scoll to EOF we show 1 blank line at bottom of screen */
		if (pos(bp, bp->b_ebuf) <= bp->b_page) {
			bp->b_page = pos(bp, bp->b_ebuf);
			i = bp->w_rows - 1;
		} else {
			i = bp->w_rows - 0;
		}
		/* Scan backwards the required number of lines. */
		while (0 < i--)
			bp->b_page = upup(bp, bp->b_page);
	}

	move(bp->w_top, 0); /* start from top of window */
	i = bp->w_top;
	j = 0;
	bp->b_epage = bp->b_page;
	
	/* paint screen from top of page until we hit maxline */ 
	while (1) {
		/* reached point - store the cursor position */
		if (bp->b_point == bp->b_epage) {
			bp->b_row = i;
			bp->b_col = j;
		}
		p = ptr(bp, bp->b_epage);
		if (bp->w_top + bp->w_rows <= i || bp->b_ebuf <= p) /* maxline */
			break;
		if (*p != '\r') {
			if (isprint(*p) || *p == '\t' || *p == '\n') {
				j += *p == '\t' ? 8-(j&7) : 1;
				addch(*p);
			} else {
				const char *ctrl = unctrl(*p);
				j += (int) strlen(ctrl);
				addstr(ctrl);
			}
		}
		if (*p == '\n' || COLS <= j) {
			j -= COLS;
			if (j < 0) j = 0;
			++i;
		}
		++bp->b_epage;
	}

	/* replacement for clrtobot() to bottom of window */
	for (k=i; k < bp->w_top + bp->w_rows; k++) {
		move(k, j); /* clear from very last char not start of line */
		clrtoeol();
		j = 0; /* thereafter start of line */
	}

	modeline(bp);
	dispmsg();
	move(bp->b_row, bp->b_col); /* set cursor */
	refresh();
}

void top() { curbp->b_point = 0; }
void bottom() {	curbp->b_epage = curbp->b_point = pos(curbp, curbp->b_ebuf); }
void left() { if (0 < curbp->b_point) --curbp->b_point; }
void right() { if (curbp->b_point < pos(curbp, curbp->b_ebuf)) ++curbp->b_point; }
void up() { curbp->b_point = lncolumn(curbp, upup(curbp, curbp->b_point),curbp->b_col); }
void down() { curbp->b_point = lncolumn(curbp, dndn(curbp, curbp->b_point),curbp->b_col); }
void lnbegin() { curbp->b_point = segstart(curbp, lnstart(curbp,curbp->b_point), curbp->b_point); }
// @todo Add Unsaved Changes/Modifications y/n prompt
// @body What's atto's approach?
void quit() { done = 1; }

void lnend()
{
	curbp->b_point = dndn(curbp, curbp->b_point);
	left();
}

void pgdown()
{
	curbp->b_page = curbp->b_point = upup(curbp, curbp->b_epage);
	while (0 < curbp->b_row--)
		down();
	curbp->b_epage = pos(curbp, curbp->b_ebuf);
}

void pgup()
{
	int i = curbp->w_rows;
	while (0 < --i) {
		curbp->b_page = upup(curbp, curbp->b_page);
		up();
	}
}

// @todo Add auto-indent support
// @body When return is pressed, discover the indentation part of the current line
// @body and copy it to the new line.

void insert(Rune c)
{
	assert(curbp->b_gap <= curbp->b_egap);
	if (curbp->b_gap == curbp->b_egap && !growgap(curbp, CHUNK)) return;
	curbp->b_point = movegap(curbp, curbp->b_point);
	*curbp->b_gap++ = c == '\r' ? '\n' : c;
	curbp->b_point = pos(curbp, curbp->b_egap);
	curbp->b_modified = 1;
}

void backsp()
{
	curbp->b_point = movegap(curbp, curbp->b_point);
	if (curbp->b_buf < curbp->b_gap) {
		--curbp->b_gap;
		curbp->b_modified = 1;
	}
	curbp->b_point = pos(curbp, curbp->b_egap);
}

void delete()
{
	curbp->b_point = movegap(curbp, curbp->b_point);
	if (curbp->b_egap < curbp->b_ebuf) {
		curbp->b_point = pos(curbp, ++curbp->b_egap);
		curbp->b_modified = 1;
	}
}

void set_mark()
{
	curbp->b_mark = (curbp->b_mark == curbp->b_point ? NOMARK : curbp->b_point);
	msg("Mark set");
}

void copy_cut(int cut)
{
	Rune *p;
	/* if no mark or point == marker, nothing doing */
	if (curbp->b_mark == NOMARK || curbp->b_point == curbp->b_mark) return;
	if (scrap != NULL) {
		free(scrap);
		scrap = NULL;
	}
	if (curbp->b_point < curbp->b_mark) {
		/* point above marker: move gap under point, region = marker - point */
		(void)movegap(curbp, curbp->b_point);
		p = ptr(curbp, curbp->b_point);
		nscrap = curbp->b_mark - curbp->b_point;
	} else {
		/* if point below marker: move gap under marker, region = point - marker */
		(void)movegap(curbp, curbp->b_mark);
		p = ptr(curbp, curbp->b_mark);
		nscrap = curbp->b_point - curbp->b_mark;
	}
	if ((scrap = (Rune*) malloc(nscrap)) == NULL) {
		msg("No more memory available.");
	} else {
		(void)memcpy(scrap, p, nscrap * sizeof (Rune));
		if (cut) {
			curbp->b_egap += nscrap; /* if cut expand gap down */
			curbp->b_point = pos(curbp, curbp->b_egap); /* set point to after region */
			curbp->b_modified = 1;
			msg("%ld bytes cut.", nscrap);
		} else {
			msg("%ld bytes copied.", nscrap);
		}
		curbp->b_mark = NOMARK;  /* unmark */
	}
}

void paste()
{
	if (nscrap <= 0) {
		msg("Nothing to paste.");
	} else if (nscrap < curbp->b_egap - curbp->b_gap || growgap(curbp, nscrap)) {
		curbp->b_point = movegap(curbp, curbp->b_point);
		memcpy(curbp->b_gap, scrap, nscrap * sizeof (Rune));
		curbp->b_gap += nscrap;
		curbp->b_point = pos(curbp, curbp->b_egap);
		curbp->b_modified = 1;
	}
}

void copy() { copy_cut(FALSE); }
void cut() { copy_cut(TRUE); }

void killtoeol()
{
	/* point = start of empty line or last char in file */
	if (*(ptr(curbp, curbp->b_point)) == 0xa || (curbp->b_point + 1 == ((curbp->b_ebuf - curbp->b_buf) - (curbp->b_egap - curbp->b_gap))) ) {
		delete();
	} else {
		curbp->b_mark = curbp->b_point;
		lnend();
		copy_cut(TRUE);
	}
}

Point search_forward(Buffer *bp, Point start_p, char *stext)
{
	Point end_p = pos(bp, bp->b_ebuf);
	Point p,pp;
	char* s;

	if (0 == strlen(stext)) return start_p;

	for (p=start_p; p < end_p; p++) {
		for (s=stext, pp=p; *s == *(ptr(bp, pp)) && *s !='\0' && pp < end_p; s++, pp++)
			;
		if (*s == '\0') return pp;
	}
	return -1;
}

void search()
{
	int cpos = 0;	
	int c;
	Point o_point = curbp->b_point;
	Point found;
	searchtext[0] = '\0';
	msg("Search: %s", searchtext);
	dispmsg();
	cpos = strlen(searchtext);

	for (;;) {
		refresh();
		switch(c = getch()) {
		case 0x1b: /* esc */
			searchtext[cpos] = '\0';
			flushinp(); /* discard any escape sequence without writing in buffer */
			return;
		case 0x07: /* ctrl-g */
			curbp->b_point = o_point;
			return;
		case 0x13: /* ctrl-s, do the search */
		case 0x0a: /* LF */
			found = search_forward(curbp, curbp->b_point, searchtext);
			if (found != -1 ) {
				curbp->b_point = found;
				msg("Search: %s", searchtext);
				display();
			} else {
				msg("Failing Search: %s", searchtext);
				dispmsg();
				curbp->b_point = 0;
			}
			break;
		case 0x7f: /* del, erase */
		case 0x08: /* backspace */
			if (cpos == 0)
				continue;
			searchtext[--cpos] = '\0';
			msg("Search: %s", searchtext);
			dispmsg();
			break;
		default:
			if (!isprint(c))
				break; /* the only non-printing chars we're interested in are handled above */
			if (cpos < STRBUF_M - 1) {
				searchtext[cpos++] = c;
				searchtext[cpos] = '\0';
				msg("Search: %s", searchtext);
				dispmsg();
			}
			break;
		}
	}
}

/* the key bindings:  desc, keys, func */
KeyBinding keymap[] = {
	{"C-a beginning-of-line    ", "\x01", lnbegin },
	{"C-b                      ", "\x02", left },
	{"C-d forward-delete-char  ", "\x04", delete },
	{"C-e end-of-line          ", "\x05", lnend },
	{"C-f                      ", "\x06", right },
	{"C-n                      ", "\x0E", down },
	{"C-p                      ", "\x10", up },
	{"C-h backspace            ", "\x08", backsp },
	{"C-k kill-to-eol          ", "\x0B", killtoeol },
	{"C-s search               ", "\x13", search },
	{"C-v                      ", "\x16", pgdown },
	{"C-w kill-region          ", "\x17", cut},
	{"C-y yank                 ", "\x19", paste},
	// @todo Add C-z to suspend the process
	// @body Like I did in femto
	{"C-space set-mark         ", "\x00", set_mark },
	{"esc @ set-mark           ", "\x1B\x40", set_mark },
	{"esc k kill-region        ", "\x1B\x6B", cut },
	{"esc v                    ", "\x1B\x76", pgup },
	{"esc w copy-region        ", "\x1B\x77", copy},
	{"esc < beg-of-buf         ", "\x1B\x3C", top },
	{"esc > end-of-buf         ", "\x1B\x3E", bottom },
	{"up previous-line         ", "\x1B\x5B\x41", up },
	{"down next-line           ", "\x1B\x5B\x42", down },
	{"left backward-character  ", "\x1B\x5B\x44", left },
	{"right forward-character  ", "\x1B\x5B\x43", right },
	{"home beginning-of-line   ", "\x1B\x4F\x48", lnbegin },
	{"end end-of-line          ", "\x1B\x4F\x46", lnend },
	{"DEL forward-delete-char  ", "\x1B\x5B\x33\x7E", delete },
	{"backspace delete-left    ", "\x7f", backsp },
	{"PgUp                     ", "\x1B\x5B\x35\x7E",pgup },
	{"PgDn                     ", "\x1B\x5B\x36\x7E", pgdown },
	{"C-x C-s save-buffer      ", "\x18\x13", save },  
	{"C-x C-c exit             ", "\x18\x03", quit },
	{"K_ERROR                  ", NULL, NULL }
};

int main(int argc, char **argv)
{
	if (argc != 2) fatal("usage: " E_NAME " filename\n");
	initscr();	
	raw();
	noecho();
	curbp = new_buffer();
	(void)insert_file(argv[1]);
	strncpy(curbp->b_fname, argv[1], MAX_FNAME);  /* save filename regardless */
	curbp->b_fname[MAX_FNAME] = '\0'; /* force truncation */
	if (!growgap(curbp, CHUNK)) fatal("Failed to allocate required memory.\n");
	key_map = keymap;

	while (!done) {
		display();
		KeyBinding *binding;
		Rune input = *(get_key(key_map, &binding));
		if (binding) {
			(binding->func)();
			continue;
		}
		if (isprint(input) || input == '\t' || input == '\n') {
			insert(input);
			continue;
		}
		msg("Not bound: %s (dec %d, hex 0x%X)", unctrl(input), input, input);
	}

	if (scrap != NULL) free(scrap);
	if (curbp != NULL) free(curbp);
	noraw();
	endwin();
	return 0;
}
