/* See LICENSE file for copyright and license details. */
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include "draw.h"

#ifndef MAX
#define MAX(a, b)  ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#endif

void
drawrect(DC *dc, int x, int y, unsigned int w, unsigned int h, Bool fill, unsigned long color) {
	XSetForeground(dc->dpy, dc->gc, color);
	if(fill)
		XFillRectangle(dc->dpy, dc->canvas, dc->gc, dc->x + x, dc->y + y, w, h);
	else
		XDrawRectangle(dc->dpy, dc->canvas, dc->gc, dc->x + x, dc->y + y, w-1, h-1);
}

void
drawtext(DC *dc, const char *text, ColorSet *col) {
	char buf[BUFSIZ];
	size_t mn, n = strlen(text);

	/* shorten text if necessary */
	for(mn = MIN(n, sizeof buf); (text[mn] & 0xc0) == 0x80  || textnw(dc, text, mn) + dc->font.height/2 > dc->w; mn--)
		if(mn == 0)
			return;
	memcpy(buf, text, mn);
	if(mn < n)
		for(n = MAX(mn-3, 0); n < mn; buf[n++] = '.');

	drawrect(dc, 0, 0, dc->w, dc->h, True, col->BG);
	drawtextn(dc, buf, mn, col);
}

void
drawtextn(DC *dc, const char *text, size_t n, ColorSet *col) {
	int x = dc->x + dc->font.height/2;
	int y = dc->y + 1;

	XSetForeground(dc->dpy, dc->gc, col->FG);
	if(!dc->xftdraw) eprintf("error, xft drawable does not exist");
	pango_layout_set_text(dc->plo, text, n);
	pango_xft_render_layout(dc->xftdraw, &col->FG_xft, dc->plo, x * PANGO_SCALE, y * PANGO_SCALE);
}

void
eprintf(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if(fmt[0] != '\0' && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	}
	exit(EXIT_FAILURE);
}

void
freecol(DC *dc, ColorSet *col) {
	if (!col) return;
	XftColorFree(dc->dpy, DefaultVisual(dc->dpy, DefaultScreen(dc->dpy)),
			DefaultColormap(dc->dpy, DefaultScreen(dc->dpy)), &col->FG_xft);
	free(col);
}

void
freedc(DC *dc) {
	if(dc->canvas)
		XFreePixmap(dc->dpy, dc->canvas);
	if(dc->gc)
		XFreeGC(dc->dpy, dc->gc);
	if(dc->dpy)
		XCloseDisplay(dc->dpy);
	if(dc)
		free(dc);
}



unsigned long
getcolor(DC *dc, const char *colstr) {
	Colormap cmap = DefaultColormap(dc->dpy, DefaultScreen(dc->dpy));
	XColor color;

	if(!XAllocNamedColor(dc->dpy, cmap, colstr, &color, &color))
		eprintf("cannot allocate color '%s'\n", colstr);
	return color.pixel;
}

ColorSet *
initcolor(DC *dc, const char *foreground, const char *background) {
	ColorSet * col = (ColorSet *)malloc(sizeof(ColorSet));
	if(!col) eprintf("error, cannot allocate memory for color set");
	col->BG = getcolor(dc, background);
	col->FG = getcolor(dc, foreground);
	if (!XftColorAllocName(dc->dpy, DefaultVisual(dc->dpy, DefaultScreen(dc->dpy)),
				DefaultColormap(dc->dpy, DefaultScreen(dc->dpy)), foreground, &col->FG_xft)) {
		eprintf("error, cannot allocate xft font color '%s'\n", foreground);
		free(col); col = NULL;
	}
	return col;
}

DC *
initdc(void) {
	DC *dc;

	if(!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("no locale support\n", stderr);
	if(!(dc = calloc(1, sizeof *dc)))
		eprintf("cannot malloc %u bytes:", sizeof *dc);
	if(!(dc->dpy = XOpenDisplay(NULL)))
		eprintf("cannot open display\n");

	dc->gc = XCreateGC(dc->dpy, DefaultRootWindow(dc->dpy), 0, NULL);
	XSetLineAttributes(dc->dpy, dc->gc, 1, LineSolid, CapButt, JoinMiter);
	return dc;
}

void
initfont(DC *dc, const char *fontstr) {
	PangoFontMetrics *metrics;
	dc->pgc = pango_xft_get_context(dc->dpy, 0);
	dc->pfd = pango_font_description_from_string(fontstr);
	if(pango_font_description_get_size(dc->pfd) == 0)
		pango_font_description_set_size(dc->pfd, 12 * PANGO_SCALE);

	metrics = pango_context_get_metrics(dc->pgc, dc->pfd, pango_language_get_default());
	dc->font.ascent = pango_font_metrics_get_ascent(metrics) / PANGO_SCALE;
	dc->font.descent = pango_font_metrics_get_descent(metrics) / PANGO_SCALE;
	pango_font_metrics_unref(metrics);

	dc->plo = pango_layout_new(dc->pgc);
	pango_layout_set_font_description(dc->plo, dc->pfd);
	dc->font.height = dc->font.ascent + dc->font.descent;
}


void
mapdc(DC *dc, Window win, unsigned int w, unsigned int h) {
	XCopyArea(dc->dpy, dc->canvas, win, dc->gc, 0, 0, w, h, 0, 0);
}

void
resizedc(DC *dc, unsigned int w, unsigned int h) {
	int screen = DefaultScreen(dc->dpy);
	if(dc->canvas)
		XFreePixmap(dc->dpy, dc->canvas);

	dc->w = w;
	dc->h = h;
	dc->canvas = XCreatePixmap(dc->dpy, DefaultRootWindow(dc->dpy), w, h,
	                           DefaultDepth(dc->dpy, screen));

	if(dc->xftdraw) XftDrawDestroy(dc->xftdraw);
	dc->xftdraw = XftDrawCreate(dc->dpy, dc->canvas, DefaultVisual(dc->dpy, screen), DefaultColormap(dc->dpy, screen));
	if (!dc->xftdraw) eprintf("error, cannot create xft drawable\n");
}

int
textnw(DC *dc, const char *text, size_t len) {
	int width, height;
	pango_layout_set_text(dc->plo, text, len);
	pango_layout_get_size(dc->plo, &width, &height);
	return width / PANGO_SCALE;
}

int
textw(DC *dc, const char *text) {
	return textnw(dc, text, strlen(text)) + dc->font.height;
}
