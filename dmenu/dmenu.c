/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <Imlib2.h>
#include <openssl/md5.h>
#include "draw.h"

#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#ifndef MIN
#define MIN(a,b)              ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b)              ((a) > (b) ? (a) : (b))
#endif
#define DEFFONT "Monospace 12"

typedef struct Item Item;
struct Item {
	char *text;
	char *image;
	Item *left, *right;
	Bool out;
};

static void appenditem(Item *item, Item **list, Item **last);
static void calcoffsets(void);
static char *cistrstr(const char *s, const char *sub);
static void drawmenu(void);
static void grabkeyboard(void);
static void insert(const char *str, ssize_t n);
static void keypress(XKeyEvent *ev);
static void match(void);
static size_t nextrune(int inc);
static void paste(void);
static void readstdin(void);
static void drawimage(void);
static void run(void);
static void cleanup(void);
static void jumptoindex(unsigned int index);
static void resizetoimageheight(int imageheight);
static void setup(void);
static void usage(void);

typedef enum image_mode {
	MODE_TOP_CENTER,
	MODE_CENTER,
	MODE_TOP,
	MODE_BOTTOM
} image_mode;

static char text[BUFSIZ] = "";
static int bh, mw, mh;
static int inputw, promptw;
static size_t cursor = 0;
static unsigned int selected = 0;
static ColorSet *normcol;
static ColorSet *selcol;
static ColorSet *outcol;
static Atom clip, utf8;
static DC *dc;
static Item *items = NULL;
static Item *matches, *matchend;
static Item *prev, *curr, *next, *sel;
static Window win;
static XIC xic;
static int mon = -1;
static int wrapselection = 0;
static int reallines = 0;
static int imagegaps = 4;
static int imagewidth = 86;
static int imageheight = 86;
static int longestedge = 0;
static int generatecache = 0;
static image_mode imagemode = MODE_TOP_CENTER;
static Imlib_Image image = NULL;

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static void createifnexist(const char *dir) {
	if(access(dir, F_OK) == 0) return;
	if(errno == EACCES) eprintf("no access to create directory: %s\n", dir);
	if(mkdir(dir, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1)
		eprintf("failed to create directory: %s\n", dir);
}

static void createifnexist_rec(const char *dir) {
	char *buf, *s = (char*)dir, *bs;
    if(!(buf = malloc(strlen(s)+1)))
		return;
	memset(buf, 0, strlen(s)+1);
	for(bs = buf; *s; ++s, ++bs) {
		if(*s == '/' && *buf) createifnexist(buf);
		*bs = *s;
	}
	free(buf);
}

static void loadimage(const char *file, int *width, int *height) {
	image = imlib_load_image(file);
	if (!image) return;
	imlib_context_set_image(image);
	*width = imlib_image_get_width();
	*height = imlib_image_get_height();
}

static void scaleimage(int *width, int *height)
{
	int nwidth, nheight;
	float aspect = 1.0f;

	if (imagewidth > *width)
		aspect = (float)(*width)/imagewidth;
	else
		aspect = (float)imagewidth/(*width);

	nwidth = *width * aspect;
	nheight = *height * aspect;
	if(nwidth == *width && nheight == *height) return;
	image = imlib_create_cropped_scaled_image(0,0,*width,*height,nwidth,nheight);
	imlib_free_image();
	if(!image) return;
	imlib_context_set_image(image);
	*width = nwidth;
	*height = nheight;
}

static time_t
mtime(const char *file) {
	struct stat statbuf;
	if(stat(file, &statbuf) == -1) return 0;
	return statbuf.st_mtime;
}

static void
loadimagecache(const char *file, int *width, int *height) {
	int slen = 0, i, cache = 1;
	unsigned char digest[MD5_DIGEST_LENGTH];
	char md5[MD5_DIGEST_LENGTH*2+1];
	char *xdg_cache, *home = NULL, *dsize, *buf;
	struct passwd *pw = NULL;

	/* just load and don't store or try cache */
	if(longestedge > 256) {
		loadimage(file, width, height);
		if (image) scaleimage(width, height);
		return;
	}

	/* try find image from cache first */
	if(!(xdg_cache = getenv("XDG_CACHE_HOME"))) {
		if(!(home = getenv("HOME")) && (pw = getpwuid(getuid())))
			home = pw->pw_dir;
		if(!home) {
			eprintf("could not find home directory");
			return;
		}
	}

	/* which cache do we try? */
	dsize = "normal";
	if (longestedge > 128) dsize = "large";

	slen = snprintf(NULL, 0, "file://%s", file)+1;
	if(!(buf = malloc(slen))) {
		eprintf("out of memory");
		return;
	}

	/* calculate md5 from path */
	sprintf(buf, "file://%s", file);
	MD5((unsigned char*)buf, slen, digest);
	free(buf);
	for(i = 0; i < MD5_DIGEST_LENGTH; ++i) sprintf(&md5[i*2], "%02x", (unsigned int)digest[i]);

	/* path for cached thumbnail */
	if(xdg_cache) slen = snprintf(NULL, 0, "%s/thumbnails/%s/%s.png", xdg_cache, dsize, md5)+1;
	else slen = snprintf(NULL, 0, "%s/.thumbnails/%s/%s.png", home, dsize, md5)+1;

	if(!(buf = malloc(slen))) {
		eprintf("out of memory");
		return;
	}

	if(xdg_cache) sprintf(buf, "%s/thumbnails/%s/%s.png", xdg_cache, dsize, md5);
	else sprintf(buf, "%s/.thumbnails/%s/%s.png", home, dsize, md5);

	loadimage(buf, width, height);
	if(image && mtime(buf) != mtime(file)) {
		imlib_free_image();
		image = NULL;
		remove(buf); /* this needs to be recreated anyway */
	} else if(image && *width < imagewidth && *height < imageheight) {
		imlib_free_image();
		image = NULL;
	} else if(image && (*width > imagewidth || *height > imageheight)) {
		scaleimage(width, height);
	}

	/* we are done */
    if(image) {
		free(buf);
		return;
	}

    /* we din't find anything from cache, or it was just wrong */
	loadimage(file, width, height);
	if(!image) {
		free(buf);
		return;
	}

	if (*width < imagewidth && *height < imageheight) {
		cache = 0;
	}
	scaleimage(width, height);
	if (cache) {
		struct utimbuf newtime;
		struct stat orig;
		stat(file, &orig);
		imlib_image_set_format("png");
		createifnexist_rec(buf);
		imlib_save_image(buf);
		newtime.actime = orig.st_atime;
		newtime.modtime = orig.st_mtime;
		utime(buf, &newtime);

	}
	free(buf);
}

int
main(int argc, char *argv[]) {
	Bool fast = False;
	int i;

	for(i = 1; i < argc; i++)
		/* these options take no arguments */
		if(!strcmp(argv[i], "-v")) {      /* prints version information */
			puts("dmenu-pango-imlib "VERSION", © 2006-2013 dmenu engineers, see LICENSE for details");
			exit(EXIT_SUCCESS);
		}
		else if(!strcmp(argv[i], "-b"))   /* appears at the bottom of the screen */
			topbar = False;
		else if(!strcmp(argv[i], "-f"))   /* grabs keyboard before reading stdin */
			fast = True;
		else if(!strcmp(argv[i], "-i")) { /* case-insensitive item matching */
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		}
		else if(!strcmp(argv[i], "-w")) /* wrap selection */
			wrapselection = 1;
		else if(!strcmp(argv[i], "-g")) /* generate image cache */
			generatecache = 1;
		else if(i+1 == argc)
			usage();
		/* these options take one argument */
		else if(!strcmp(argv[i], "-l"))   /* number of lines in vertical list */
			lines = atoi(argv[++i]);
		else if(!strcmp(argv[i], "-m"))
			mon = atoi(argv[++i]);
		else if(!strcmp(argv[i], "-p"))   /* adds prompt to left of input field */
			prompt = argv[++i];
		else if(!strcmp(argv[i], "-fn"))  /* font or font set */
			font = argv[++i];
		else if(!strcmp(argv[i], "-nb"))  /* normal background color */
			normbgcolor = argv[++i];
		else if(!strcmp(argv[i], "-nf"))  /* normal foreground color */
			normfgcolor = argv[++i];
		else if(!strcmp(argv[i], "-sb"))  /* selected background color */
			selbgcolor = argv[++i];
		else if(!strcmp(argv[i], "-sf"))  /* selected foreground color */
			selfgcolor = argv[++i];
		else if(!strcmp(argv[i], "-si"))  /* selected index */
			selected = atoi(argv[++i]);
		else if(!strcmp(argv[i], "-is")) {  /* image size */
			char buf[255];
			memset(buf, 0, sizeof(buf));
			memcpy(buf, argv[++i], sizeof(buf)-1);
			if(sscanf(buf, "%dx%d", &imagewidth, &imageheight) == 1)
				imageheight = imagewidth;
		} else if(!strcmp(argv[i], "-ia")) {/* image alignment */
			char *arg = argv[++i];
			if (!strcmp(arg, "center")) imagemode = MODE_CENTER;
			if (!strcmp(arg, "top")) imagemode = MODE_TOP;
			if (!strcmp(arg, "bottom")) imagemode = MODE_BOTTOM;
			if (!strcmp(arg, "top-center-gapless")) imagegaps = 0;
			if (!strcmp(arg, "center-gapless")) {
				imagegaps = 0;
				imagemode = MODE_CENTER;
			}
			if (!strcmp(arg, "top-gapless")) {
				imagegaps = 0;
				imagemode = MODE_TOP;
			}
			if (!strcmp(arg, "bottom-gapless")) {
				imagegaps = 0;
				imagemode = MODE_BOTTOM;
			}
		}
		else
			usage();

	longestedge = MAX(imagewidth, imageheight);

	dc = initdc();
	initfont(dc, (font?font:DEFFONT));
	atexit(cleanup);

	if(fast) {
		grabkeyboard();
		readstdin();
	}
	else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}

void
cleanup(void) {
	if(image) {
		imlib_free_image();
		image = NULL;
	}
	freecol(dc, normcol);
	freecol(dc, selcol);
	XDestroyWindow(dc->dpy, win);
	XUngrabKeyboard(dc->dpy, CurrentTime);
	freedc(dc);
}

void
appenditem(Item *item, Item **list, Item **last) {
	if(*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

void
calcoffsets(void) {
	int i, n;

	if(lines > 0)
		n = lines * bh;
	else
		n = mw - (promptw + inputw + textw(dc, "<") + textw(dc, ">"));
	/* calculate which items will begin the next page and previous page */
	for(i = 0, next = curr; next; next = next->right)
		if((i += (lines > 0) ? bh : MIN(textw(dc, next->text), n)) > n)
			break;
	for(i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if((i += (lines > 0) ? bh : MIN(textw(dc, prev->left->text), n)) > n)
			break;
}

char *
cistrstr(const char *hay, const char *needle) {
	size_t i, r, p, len, len2;
	p = 0; r = 0;
	if (!strcasecmp(hay, needle)) return (char*)hay;
	if ((len = strlen(hay)) < (len2 = strlen(needle))) return NULL;
	for (i = 0; i != len; ++i) {
		if (p == len2) return (char*)&hay[r]; /* THIS IS IT! */
		if (toupper(hay[i]) == toupper(needle[p++])) {
			if (!r) r = i; /* could this be.. */
		} else { if (r) i = r; r = 0; p = 0; } /* ..nope, damn it! */
	}
	if (p == len2) return (char*)&hay[r]; /* THIS IS IT! */
	return NULL;
}

void
drawmenu(void) {
	int curpos;
	Item *item;

	dc->x = 0;
	dc->y = 0;
	dc->h = bh;
	drawrect(dc, 0, 0, mw, mh, True, normcol->BG);

	if(prompt && *prompt) {
		dc->w = promptw;
		drawtext(dc, prompt, selcol);
		dc->x = dc->w;
	}
	/* draw input field */
	dc->w = (lines > 0 || !matches) ? mw - dc->x : inputw;
	drawtext(dc, text, normcol);
	if((curpos = textnw(dc, text, cursor) + dc->h/2 - 2) < dc->w)
		drawrect(dc, curpos, 2, 1, dc->h - 4, True, normcol->FG);

	if(lines > 0) {
		/* draw vertical list */
		if(longestedge && imagewidth) dc->x = imagewidth+imagegaps;
		dc->w = mw - dc->x;
		for(item = curr; item != next; item = item->right) {
			dc->y += dc->h;
			drawtext(dc, item->text, (item == sel) ? selcol :
			                         (item->out)   ? outcol : normcol);
		}
	}
	else if(matches) {
		/* draw horizontal list */
		dc->x += inputw;
		dc->w = textw(dc, "<");
		if(curr->left)
			drawtext(dc, "<", normcol);
		for(item = curr; item != next; item = item->right) {
			dc->x += dc->w;
			dc->w = MIN(textw(dc, item->text), mw - dc->x - textw(dc, ">"));
			drawtext(dc, item->text, (item == sel) ? selcol :
			                         (item->out)   ? outcol : normcol);
		}
		dc->w = textw(dc, ">");
		dc->x = mw - dc->w;
		if(next)
			drawtext(dc, ">", normcol);
	}
	mapdc(dc, win, mw, mh);
}

void
grabkeyboard(void) {
	int i;

	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for(i = 0; i < 1000; i++) {
		if(XGrabKeyboard(dc->dpy, DefaultRootWindow(dc->dpy), True,
		                 GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		usleep(1000);
	}
	eprintf("cannot grab keyboard\n");
}

void
insert(const char *str, ssize_t n) {
	if(strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if(n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

void
keypress(XKeyEvent *ev) {
	char buf[32];
	int len;
	KeySym ksym = NoSymbol;
	Status status;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	if(status == XBufferOverflow)
		return;
	if(ev->state & ControlMask)
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: ksym = XK_Return;    break;
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return;    break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while(cursor > 0 && text[nextrune(-1)] == ' ')
				insert(NULL, nextrune(-1) - cursor);
			while(cursor > 0 && text[nextrune(-1)] != ' ')
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_y: /* paste selection */
			XConvertSelection(dc->dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			exit(EXIT_FAILURE);
		default:
			return;
		}
	else if(ev->state & Mod1Mask)
		switch(ksym) {
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	switch(ksym) {
	default:
		if(!iscntrl(*buf))
			insert(buf, len);
		break;
	case XK_Delete:
		if(text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if(cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
		if(text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if(next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while(next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		exit(EXIT_FAILURE);
	case XK_Home:
		if(sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
		if(cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		if(lines > 0)
			return;
		/* fallthrough */
	case XK_Up:
		if(sel && sel->left) {
			if ((sel = sel->left)->right == curr) {
				curr = prev;
				calcoffsets();
			}
		} else if(wrapselection) {
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while(next && (curr = curr->right))
				calcoffsets();
			sel = matchend;
		}
		break;
	case XK_Next:
		if(!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
		if(!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		puts((sel && !(ev->state & ShiftMask)) ? sel->text : text);
		if(!(ev->state & ControlMask))
			exit(EXIT_SUCCESS);
		sel->out = True;
		break;
	case XK_Right:
		if(text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		if(lines > 0)
			return;
		/* fallthrough */
	case XK_Down:
		if(sel && sel->right) {
			if ((sel = sel->right) == next) {
				curr = next;
				calcoffsets();
			}
		} else if(wrapselection) {
			sel = curr = matches;
			calcoffsets();
		}
		break;
	case XK_Tab:
		if(!sel)
			return;
		strncpy(text, sel->text, sizeof text - 1);
		text[sizeof text - 1] = '\0';
		cursor = strlen(text);
		match();
		break;
	}
	drawmenu();
}

void
match(void) {
	static char **tokv = NULL;
	static int tokn = 0;
	static size_t oldlen = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len;
	Item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	/* small optimization */
	len = strlen(text);
	if (!matches && oldlen && len >= oldlen)
		return;
	oldlen = len;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for(s = strtok(buf, " "); s; tokv[tokc-1] = s, s = strtok(NULL, " "))
		if(++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			eprintf("cannot realloc %u bytes\n", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	for(item = items; item && item->text; item++) {
		for(i = 0; i < tokc; i++)
			if(!fstrstr(item->text, tokv[i]))
				break;
		if(i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if(!tokc || !fstrncmp(tokv[0], item->text, len+1))
			appenditem(item, &matches, &matchend);
		else if(!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if(lprefix) {
		if(matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		}
		else
			matches = lprefix;
		matchend = prefixend;
	}
	if(lsubstr) {
		if(matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		}
		else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

size_t
nextrune(int inc) {
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for(n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc);
	return n;
}

void
paste(void) {
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	XGetWindowProperty(dc->dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
	                   utf8, &da, &di, &dl, &dl, (unsigned char **)&p);
	insert(p, (q = strchr(p, '\n')) ? q-p : (ssize_t)strlen(p));
	XFree(p);
	drawmenu();
}

void
readstdin(void) {
	char buf[sizeof text], *p, *maxstr = NULL;
	size_t i, max = 0, size = 0;
	int w, h;
	char *limg = NULL;

	/* read each line from stdin and add it to the item list */
	for(i = 0; fgets(buf, sizeof buf, stdin); i++) {
		if(i+1 >= size / sizeof *items)
			if(!(items = realloc(items, (size += BUFSIZ))))
				eprintf("cannot realloc %u bytes:", size);
		if((p = strchr(buf, '\n')))
			*p = '\0';
		if(!(items[i].text = strdup(buf)))
			eprintf("cannot strdup %u bytes:", strlen(buf)+1);
		items[i].out = False;
		if(strlen(items[i].text) > max)
			max = strlen(maxstr = items[i].text);

		/* read image */
		if(!strncmp("IMG:", items[i].text, strlen("IMG:"))) {
			if(!(items[i].image = malloc(strlen(items[i].text)+1)))
				eprintf("cannot malloc %u bytes\n", strlen(items[i].text));
			if(sscanf(items[i].text, "IMG:%[^\t]", items[i].image)) {
				if(!(items[i].image = realloc(items[i].image, strlen(items[i].image)+1)))
					eprintf("cannot realloc %u bytes\n", strlen(items[i].image)+1);
				items[i].text += strlen("IMG:")+strlen(items[i].image)+1;
			} else {
				free(items[i].image);
				items[i].image = NULL;
			}
		} else items[i].image = NULL;

		/* cache image immediatly */
		if(generatecache && longestedge <= 256 && items[i].image && strcmp(items[i].image, limg?limg:"")) {
			loadimagecache(items[i].image, &w, &h);
			fprintf(stderr, "-!- Generating thumbnail for: %s\n", items[i].image);
		}
		if(items[i].image) limg = items[i].image;
	}
	if(items) {
		items[i].text = NULL;
		items[i].image = NULL;
	}
	if(!limg) imagewidth = imageheight = longestedge = imagegaps = 0;
	inputw = maxstr ? textw(dc, maxstr) : 0;
	lines = MIN(lines, i);
}

void
drawimage(void) {
	static int width = 0, height = 0;
	static char *limg = NULL;

	if(sel && sel->image && strcmp(sel->image, limg?limg:"")) {
		if(longestedge) loadimagecache(sel->image, &width, &height);
	} else if((!sel || !sel->image) && image) {
		imlib_free_image();
		image = NULL;
	}
	if(image && longestedge) {
		int leftmargin = imagegaps;
		if(mh != bh+height+imagegaps*2) {
			resizetoimageheight(height);
		}
		if(imagemode == MODE_TOP) {
			imlib_render_image_on_drawable(leftmargin+(imagewidth-width)/2, bh+imagegaps);
		} else if(imagemode == MODE_BOTTOM) {
			imlib_render_image_on_drawable(leftmargin+(imagewidth-width)/2, mh-height-imagegaps);
		} else if(imagemode == MODE_CENTER) {
			imlib_render_image_on_drawable(leftmargin+(imagewidth-width)/2, (mh-bh-height)/2+bh);
		} else {
			int minh = MIN(imageheight, mh-bh-imagegaps*2);
			if (height > width) minh = height;
			imlib_render_image_on_drawable(leftmargin+(imagewidth-width)/2, (minh-height)/2+bh+imagegaps);
		}
	}

	if(sel) limg = sel->image;
	else limg = NULL;
}

void
run(void) {
	XEvent ev;

	while(!XNextEvent(dc->dpy, &ev)) {
		if(XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case Expose:
			if(ev.xexpose.count == 0) {
				mapdc(dc, win, mw, mh);
				drawimage();
			}
			break;
		case KeyPress:
			keypress(&ev.xkey);
			drawimage();
			break;
		case SelectionNotify:
			if(ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if(ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dc->dpy, win);
			break;
		}
	}
}

void
jumptoindex(unsigned int index) {
	unsigned int i;
	sel = curr = matches;
	calcoffsets();
	for(i = 1; i < index; ++i) {
		if(sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
		}
	}
}

void
resizetoimageheight(int imageheight) {
	int omh = mh, olines = lines;
	lines = reallines;
	if(lines * bh < imageheight+imagegaps*2) lines = (imageheight+imagegaps*2)/bh;
	mh = (lines + 1) * bh;
	if(mh-bh < imageheight+imagegaps*2) mh = imageheight+imagegaps*2+bh;
	if(!win || omh == mh) return;
	XResizeWindow(dc->dpy, win, mw, mh);
	resizedc(dc, mw, mh);

	if(olines != lines) {
		Item *item;
		unsigned int i = 1;
		for (item = matches; item && item != sel; item = item->right) ++i;
		jumptoindex(i);
	}
	drawmenu();
}

void
setup(void) {
	int x, y, screen = DefaultScreen(dc->dpy);
	Window root = RootWindow(dc->dpy, screen);
	XSetWindowAttributes swa;
	XIM xim;
#ifdef XINERAMA
	int n;
	XineramaScreenInfo *info;
#endif

	normcol = initcolor(dc, normfgcolor, normbgcolor);
	selcol = initcolor(dc, selfgcolor, selbgcolor);
	outcol = initcolor(dc, outfgcolor, outbgcolor);

	clip = XInternAtom(dc->dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dc->dpy, "UTF8_STRING", False);

	/* calculate menu geometry */
	bh = dc->font.height + 2;
	lines = MAX(lines, 0);
	reallines = lines;
	resizetoimageheight(imageheight);

#ifdef XINERAMA
	if((info = XineramaQueryScreens(dc->dpy, &n))) {
		int a, j, di, i = 0, area = 0;
		unsigned int du;
		Window w, pw, dw, *dws;
		XWindowAttributes wa;

		XGetInputFocus(dc->dpy, &w, &di);
		if(mon != -1 && mon < n)
			i = mon;
		if(!i && w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if(XQueryTree(dc->dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while(w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if(XGetWindowAttributes(dc->dpy, pw, &wa))
				for(j = 0; j < n; j++)
					if((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if(mon == -1 && !area && XQueryPointer(dc->dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for(i = 0; i < n; i++)
				if(INTERSECT(x, y, 1, 1, info[i]))
					break;

		x = info[i].x_org;
		y = info[i].y_org + (topbar ? 0 : info[i].height - mh);
		mw = info[i].width;
		XFree(info);
	}
	else
#endif
	{
		x = 0;
		y = topbar ? 0 : DisplayHeight(dc->dpy, screen) - mh;
		mw = DisplayWidth(dc->dpy, screen);
	}
	promptw = (prompt && *prompt) ? textw(dc, prompt) : 0;
	inputw = MIN(inputw, mw/3);

	/* create menu window */
	swa.override_redirect = True;
	swa.background_pixel = normcol->BG;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask;
	win = XCreateWindow(dc->dpy, root, x, y, mw, mh, 0,
	                    DefaultDepth(dc->dpy, screen), CopyFromParent,
	                    DefaultVisual(dc->dpy, screen),
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);

	/* open input methods */
	xim = XOpenIM(dc->dpy, NULL, NULL, NULL);
	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dc->dpy, win);
	resizedc(dc, mw, mh);

	imlib_set_cache_size(8192 * 1024);
	imlib_context_set_blend(1);
	imlib_context_set_dither(1);
	imlib_set_color_usage(128);
	imlib_context_set_display(dc->dpy);
	imlib_context_set_visual(DefaultVisual(dc->dpy, screen));
	imlib_context_set_colormap(DefaultColormap(dc->dpy, screen));
	imlib_context_set_drawable(win);

    match();
	jumptoindex(selected);
	drawmenu();
}

void
usage(void) {
	fputs("usage: dmenu [-b] [-f] [-i] [-g] [-l lines] [-p prompt] [-fn font] [-m monitor]\n"
	      "             [-nb color] [-nf color] [-sb color] [-sf color] [-si index] [-is size] [-ia align] [-v]\n", stderr);
	exit(EXIT_FAILURE);
}
