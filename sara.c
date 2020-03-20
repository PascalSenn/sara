/*                                                                                
 * sara Window Manager
 * ______________________________________________________________________________ 
 *                                                                                
 * Copyright (c) 2020, This Fackin Guy, gitluin on github (no email for you!)     
 *                                                                                
 * Please refer to the complete LICENSE file that should accompany this software.
 * Please refer to the MIT license for details on usage: https://mit-license.org/
 */ 

/* sockets */
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <unistd.h>
/* general */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
/* signal */
#include <signal.h>
#include <sys/wait.h>
/* Xlib */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xft/Xft.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#define BUTTONMASK              	(ButtonPressMask|ButtonReleaseMask)
#define MOUSEMASK               	(BUTTONMASK|PointerMotionMask)
#define EACHCLIENT(_I)			(ic=_I;ic;ic=ic->next) /* ic is a global */
#define EACHMON(_M)			(im=_M;im;im=im->next) /* im is a global */
#define ISOUTSIDE(PX,PY,X,Y,W,H)	((PX > X + W || PX < X || PY > Y + H || PY < Y))
#define ISVISIBLE(C)			((C->desks & C->mon->seldesks))
#define MAX(A,B)               		((A) > (B) ? (A) : (B))
#define POSTOINT(X)			((int)(ceil(log2(X)) == floor(log2(X)) ? ceil(log2(X)) : 0))
#define TABLENGTH(X)    		(sizeof(X)/sizeof(*X))
/* this NEEDS to match with sarasock.c */
#define INPUTSOCK			"/tmp/sara.sock"
#define MAXBUFF				22*sizeof(char) /* longest is youviolatedmymother at 19, +2 for space and "0", +1 for '\0' */

enum { SchNorm,    SchSel };
enum { ColFg,      ColBg };
enum { SymLeft,    SymRight };
enum { AnyVis,     OnlyVis };
enum { NoZoom,     YesZoom };
enum { NoFocus,    YesFocus };
enum { ClkPlcHldr, ClkWin };
enum { WantMove,   WantResize };
enum { NoFloat,    YesFloat };
enum { WantInt,    WantFloat };


/* ---------------------------------------
 * Structs
 * ---------------------------------------
 */

typedef struct client client;
typedef struct desktop desktop;
typedef struct monitor monitor;

typedef union {
	const int i;
	const unsigned int ui;
	const float f;
	const void* v;
	const char* s;
} Arg;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int btn;
	void (*func)(const Arg arg);
	const Arg arg;
} button;

typedef struct {
	const char* symbol;
	void (*arrange)(monitor*);
	const char* name; /* for parsearg */
} layout;

typedef struct {
	const char* class;
	const char* instance;
	const char* title;
	int desks;
	int isfloat, isfull;
	int monitor;
} rule;

struct client {
	int x, y, w, h;
	unsigned int desks;
	unsigned int iscur;
	/* being in monocle is not considered floating */
	int isfloat;
	int isfull;
	/* prior to togglefs */
	int oldfloat;
	Window win;
	client* next;
	monitor* mon;
}; 

struct desktop {
	float msize;
	layout* curlayout;
};

struct monitor {
	/* monitor */
	int mx, my, mh, mw, wy, wh;
	int num;
	monitor* next;
	/* desks */
	float msize;
	unsigned int seldesks;
	unsigned int curdesk;
	client* current;
	client* head;
	desktop* desks;
	layout* curlayout;
};


/* ---------------------------------------
 * Util Functions
 * ---------------------------------------
 */

/* convert 11011110 to "01111011"
 * for this example, len = 8
 * dest must be a calloc'd char* that you free() afterwards
 */
void
uitos(unsigned int ui, int len, char* dest){
	int i, j, res;
	int bytearray[len];
	char bytestr[len + 1];

	/* reverse the array, as tags are printed left to right, not right to left */
	for (i=0;i < len;i++)
		bytearray[i] = ui >> i & 1;

	for (i=0, j=0;
	(i < (len + 1)) && (res = snprintf(bytestr + j, (len + 1) - j, "%d", bytearray[i])) > 0;
	i++)
		j += res;

	snprintf(dest, len + 1, "%s", bytestr);
}

void
die(const char* e, ...){
	fprintf(stdout, "sara: %s\n", e);
	exit(1);
}

void*
ecalloc(size_t nmemb, size_t size){
	void* p;

	if ( !(p = calloc(nmemb, size)) )
		die("ecalloc failed");

	return p;
}

int
slen(const char* str){
	int i = 0;

	while (*str){ str++; i++; }

	return i;
}


/* ---------------------------------------
 * Main Function Declarations
 * ---------------------------------------
 */

/* X Event Processing */
static void buttonpress(XEvent* e);
static void configurenotify(XEvent* e);
static void configurerequest(XEvent* e);
static void destroynotify(XEvent* e);
static void enternotify(XEvent* e);
static void focusin(XEvent* e);
static void maprequest(XEvent* e);
static void motionnotify(XEvent* e);
/* Client & Linked List Manipulation */
static void adjustcoords(client* c);
static void applyrules(client* c);
static void attachaside(client* c);
static void changecurrent(client* c, monitor* m, unsigned int deskmask, int refocused);
static void configure(client* c);
static void detach(client* c);
static void killclient(const Arg arg);
static void manage(Window parent, XWindowAttributes* wa);
static void manipulate(const Arg arg);
static void moveclient(const Arg arg);
static void moveclientup(client* c, int wantzoom);
static void movefocus(const Arg arg);
static void resizeclient(client* c, int x, int y, int w, int h);
static void restack(monitor* m);
static void sendmon(client* c, monitor* m);
static void showhide(monitor* m);
static void todesktop(const Arg arg);
static void toggledesktop(const Arg arg);
static void togglefloat(const Arg arg);
static void togglefs(const Arg arg);
static void tomon(const Arg arg);
static void unmanage(client* c);
static void updatefocus();
static void zoom(const Arg arg);
/* Monitor Manipulation */
static void changemon(monitor* m, int wantfocus);
static void cleanupmon(monitor* m);
static monitor* createmon(int num, int x, int y, int w, int h);
static monitor* dirtomon(int dir);
static monitor* findmon(Window w);
static void focusmon(const Arg arg);
static void updategeom();
/* Client Interfacing */
static client* findclient(Window w);
static client* findcurrent(monitor* m);
static client* findprevclient(client* c, int wantvis, int wantfloat);
static client* findvisclient(client* c, int wantfloat);
/* Desktop Interfacing */
static void arrange(monitor* m);
static void changemsize(const Arg arg);
static void loaddesktop(int i);
static void monocle(monitor* m);
static void setlayout(const Arg arg);
static void tile(monitor* m);
static void toggleview(const Arg arg);
static void view(const Arg arg);
/* Backend */
static void cleanup();
static int getptrcoords(int* x, int* y);
static void grabbuttons(client* c, int focused);
static void outputstats();
static void parsearg(const Arg arg, int wanttype, int* ai, float* af);
static void setup();
static void sigchld(int unused);
static void start();
static int xerror(Display* dis, XErrorEvent* e);
static int xerrordummy(Display* dis, XErrorEvent* e);
static int xsendkill(Window w);
static void youviolatedmymother(const Arg arg);

/* callable functions from outside */
struct {
	void (*func);
	const char* str;
} conversions [] = {
	{view,                "view"},
	{toggledesktop,       "toggledesktop"},
	{toggleview,          "toggleview"},
	{todesktop,           "todesktop"},
	{changemsize,         "changemsize"},
	{setlayout,           "setlayout"},
	{focusmon,            "focusmon"},
	{tomon,               "tomon"},
	{zoom,                "zoom"},
	{togglefs,            "togglefs"},
	{togglefloat,         "togglefloat"},
	{moveclient,          "moveclient"},
	{movefocus,           "movefocus"},
	{killclient,          "killclient"},
	{youviolatedmymother, "youviolatedmymother"},
};

/* thanks to stackoverflow's wallyk for analagous str2enum */
void*
str2func(const char* str){
	int i;
	for (i=0;i < TABLENGTH(conversions);i++)
		if (strcmp(str, conversions[i].str) == 0)
			return conversions[i].func;

	return NULL;
}


/* Make the above known */
#include "config.h"


/* ---------------------------------------
 * Globals
 * ---------------------------------------
 */

/* X Interfacing */
static Cursor cursor;
static Display* dis;
static Window root;
static int screen;
static int sh;
static int sw;
/* Monitor Interfacing */
static monitor* curmon;
static monitor* mhead;
/* Backend */
static int ai; /* for parsearg */
static float af; /* for parsearg */
static const Arg dumbarg; /* for placating function calls like togglefs */
static XEvent dumbev; /* for XCheckMasking */
static int dumbi; /* for placating parsearg */
static float dumbf; /* for placating parsearg */
static client* ic; /* for EACHCLIENT iterating */
static monitor* im; /* for EACHMON iterating */
static int running;

static void (*events[LASTEvent])(XEvent* e) = {
	[ButtonPress] = buttonpress,
	[ConfigureNotify] = configurenotify,
	[ConfigureRequest] = configurerequest,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[FocusIn] = focusin,
	[MapRequest] = maprequest,
	[MotionNotify] = motionnotify
};


/* ---------------------------------------
 * X Event Processing
 * ---------------------------------------
 */

void
buttonpress(XEvent* e){
	int i;
	client* c;
	monitor* m;
	unsigned int click = 0;
	XButtonPressedEvent* ev = &e->xbutton;

	if ( (m = findmon(ev->window)) && m != curmon)
		changemon(m, NoFocus);

	if ( (c = findclient(ev->window)) ){
		if (c != c->mon->current){
			changecurrent(c, c->mon, c->mon->curdesk, 0);
			updatefocus();
		}

		restack(c->mon);
		XAllowEvents(dis, ReplayPointer, CurrentTime);
		click = ClkWin;
	}

	for (i=0;i < TABLENGTH(buttons);i++)
		if (click == buttons[i].click && buttons[i].func
		&& buttons[i].btn == ev->button
		&& buttons[i].mask == ev->state)
			buttons[i].func(buttons[i].arg);
}

void
configurenotify(XEvent* e){
	XConfigureEvent* ev = &e->xconfigure;

	if (ev->window == root){
		sw = ev->width; sh = ev->height;
		updategeom();

		for EACHMON(mhead){
			for EACHCLIENT(im->head)
				if (ic->isfull)
					resizeclient(ic, im->mx, im->my, im->mw, im->mh);

			arrange(im);
		}
	}
}

void
configurerequest(XEvent* e){
	client* c;
	monitor* m;
	XWindowChanges wc;
	XConfigureRequestEvent* ev = &e->xconfigurerequest;

	if ( (c = findclient(ev->window)) ){
		if (c->isfloat){
			m = c->mon;
			if (ev->value_mask & CWX) c->x = m->mx + ev->x;
			if (ev->value_mask & CWY) c->y = (ev->y < barpx) ? barpx : ev->y;
			if (ev->value_mask & CWWidth) c->w = ev->width;
			if (ev->value_mask & CWHeight) c->h = ev->height;
			if ((c->x + c->w) > m->mx + m->mw)
				c->x = m->mx + (m->mw / 2 - c->w / 2);
			if ((c->y + c->h) > m->wy + m->wh)
				c->y = m->wy + (m->wh / 2 - c->h / 2);
			if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dis, c->win, c->x, c->y, c->w, c->h);

		} else {
			configure(c);
		}

	} else {
		wc.x = ev->x; wc.y = ev->y;
		wc.width = ev->width; wc.height = ev->height;
		wc.sibling = ev->above; wc.stack_mode = ev->detail;
		XConfigureWindow(dis, ev->window, ev->value_mask, &wc);
	}

	XSync(dis, False);
}

void
destroynotify(XEvent* e){
	client* c;
	XDestroyWindowEvent* ev = &e->xdestroywindow;

	if ( (c = findclient(ev->window)) )
		unmanage(c);
}

void
enternotify(XEvent* e){
	client* c;
	monitor* m;
	XCrossingEvent* ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;

	if ( !(c = findclient(ev->window)) || c == curmon->current )
		return;

	if ( (m = c->mon) && m != curmon )
		changemon(m, NoFocus);

	changecurrent(c, curmon, curmon->curdesk, 0);
	updatefocus();
}

void
focusin(XEvent* e){
	XFocusChangeEvent* ev = &e->xfocus;

	if (curmon->current && ev->window != curmon->current->win)
		updatefocus();
}

void
maprequest(XEvent* e){
	XWindowAttributes wa;
	XMapRequestEvent* ev = &e->xmaprequest;

	if (!XGetWindowAttributes(dis, ev->window, &wa) || wa.override_redirect)
		return;

	if (!findclient(ev->window))
		manage(ev->window, &wa);
}

void
motionnotify(XEvent* e){
	XMotionEvent* ev = &e->xmotion;

	if (ev->window != root)
		return;

	if (ISOUTSIDE(ev->x_root, ev->y_root, curmon->mx, curmon->my, curmon->mw, curmon->mh)){
		for EACHMON(mhead){
			if (im != curmon){
				changemon(im, YesFocus);
				return;
			}
		}
	}
}


/* ---------------------------------------
 * Client & Linked List Manipulation
 * ---------------------------------------
 */

void
adjustcoords(client* c){
	if (ISOUTSIDE(c->x, c->y, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh)){
		/* find which one it is inside */
		for EACHMON(mhead)
			if (!ISOUTSIDE(c->x, c->y, im->mx, im->my, im->mw, im->mh)){
				c->x += (im->mx < c->mon->mx) ? c->mon->mx : -im->mx;
				c->y += (im->my < c->mon->my) ? c->mon->my : -im->my;
				break;
			}
	}
}

void
applyrules(client* c){
	const char* class, * instance;
	int i;
	const rule* r;
	XTextProperty tp;
	XClassHint ch = { NULL, NULL };

	c->isfloat = c->desks = 0;

	XGetWMName(dis, c->win, &tp);
	XGetClassHint(dis, c->win, &ch);
	class = ch.res_class ? ch.res_class : "broken";
	instance = ch.res_name  ? ch.res_name  : "broken";

	for (i=0;i < TABLENGTH(rules);i++){
		r = &rules[i];
		if ((!r->title || (tp.value && strstr(r->title, (const char*) tp.value)))
		&& (!r->class || strstr(class, r->class))
		&& (!r->instance || strstr(instance, r->instance))){
			c->isfloat = r->isfloat;
			c->isfull = r->isfull;
			c->desks |= r->desks;
			for EACHMON(mhead) if (im->num == r->monitor) break;
			if (im) c->mon = im;
		}
	}
	if (ch.res_class) XFree(ch.res_class);
	if (ch.res_name) XFree(ch.res_name);

	c->desks = c->desks ? c->desks : c->mon->seldesks;
}

void
attachaside(client* c){
	client* l;

	if (!c->mon->head){
		c->mon->head = c;

	} else {
		/* If not the first on this desktop */
		if (c->mon->current){
			c->next = c->mon->current->next;
			c->mon->current->next = c;

		} else {
			for (l=c->mon->head;l->next;l=l->next);
			l->next = c;
		}
	}
}

void
changecurrent(client* c, monitor* m, unsigned int deskmask, int refocused){
	client* vis;

	if (c){
		c->iscur ^= deskmask;
		grabbuttons(c, 1);
	}
	
	for EACHCLIENT(m->head) if (ic != c && (ic->iscur & deskmask)){
			ic->iscur ^= deskmask;
			grabbuttons(ic, 0);
		}

	m->current = c;

	if (m->current && refocused){
		vis = (vis = findvisclient(c->next, YesFloat)) ?
			vis : findprevclient(c, OnlyVis, YesFloat);
		changecurrent(vis, m, m->curdesk, 0);
	}
}

void
configure(client* c){
	XConfigureEvent ce = {
		.type = ConfigureNotify,
		.display = dis,
		.event = c->win,
		.window = c->win,
		.x = c->x,
		.y = c->y,
		.width = c->w,
		.height = c->h,
		.above = None,
		.override_redirect = False
	};

	XSendEvent(dis, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void
detach(client* c){
	client* p;
	/* Move the window out of the way first to hide it while it hangs around :) */
	XMoveWindow(dis, c->win, 2*sw, 0);

	changecurrent(c, c->mon, c->mon->curdesk, (c->desks & c->mon->seldesks) ? 1 : 0);

	/* For both, if NULL, then we're still okay */
	if ( (p = findprevclient(c, AnyVis, YesFloat)) )
		p->next = c->next;
	else
		c->mon->head = c->next;
}

void
killclient(const Arg arg){
	if (!curmon->current)
		return;

	if (!xsendkill(curmon->current->win)){
		XGrabServer(dis);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dis, DestroyAll);
		XKillClient(dis, curmon->current->win);
		XSync(dis, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dis);
	}
}

void
manage(Window parent, XWindowAttributes* wa){
	client* c, * t;
	Window trans = None;

	if ( !(c = ecalloc(1, sizeof(client))) )
		die("Error while callocing new client!");

	c->win = parent;
	c->isfloat = c->oldfloat = c->isfull = c->iscur = 0;
	c->x = wa->x; c->y = wa->y;
	c->w = wa->width; c->h = wa->height;

	if (XGetTransientForHint(dis, parent, &trans) && (t = findclient(trans))){
		c->desks = t->desks;
		c->mon = t->mon;

	} else {
		c->mon = curmon;
		applyrules(c);
	}
	if (!c->isfloat) c->isfloat = c->oldfloat = (trans != None);

	configure(c);
	XSelectInput(dis, c->win, EnterWindowMask|FocusChangeMask|PropertyChangeMask
		|StructureNotifyMask);
	grabbuttons(c, 0);

	attachaside(c);
	adjustcoords(c);
	c->y = (c->y < c->mon->wy) ? c->mon->wy : c->y;

	/* move out of the way until told otherwise */
	XMoveResizeWindow(dis, c->win, c->x + 2*sw, c->y, c->w, c->h);
	XMapWindow(dis, c->win);

	arrange(c->mon);
	if (c->desks & c->mon->seldesks){
		changecurrent(c, c->mon, c->mon->curdesk, 0);
		updatefocus();

		/* applyrules */
		if (c->isfull){
			c->isfull = !c->isfull;
			togglefs(dumbarg);
		}
	}
	outputstats();
}

void
moveclient(const Arg arg){
	client* c;

	if (!curmon->current || curmon->current->isfull)
		return;

	parsearg(arg, WantInt, &ai, &dumbf);

	/* Up stack */
	if (ai > 0)
		moveclientup(curmon->current, NoZoom);
	/* Down stack - equivalent to moving next tiled client up */
	else if ( ai < 0 && (c = findvisclient(curmon->current->next, NoFloat)) )
		moveclientup(c, NoZoom);

	arrange(curmon);
	updatefocus();
}

void
moveclientup(client* c, int wantzoom){
	client* p, * target, * ptarget;

	if (!c)
		return;

	/* Go up only if not highest visible */
	if (wantzoom){
		if ( !(target = findvisclient(curmon->head, NoFloat)) || target == c )
			return;
	} else {
		if ( !(target = findprevclient(c, 0, NoFloat)) )
			return;
	}

	p = findprevclient(c, AnyVis, YesFloat);
	ptarget = findprevclient(target, AnyVis, YesFloat);

	/* if p == target, then we're still okay */
	p->next = c->next;
	c->next = target;
	if (ptarget) ptarget->next = c;
	else curmon->head = c;
}

void
movefocus(const Arg arg){
	client* j, * c = NULL;

	if (!curmon->current || curmon->current->isfull)
		return;

	parsearg(arg, WantInt, &ai, &dumbf);

	/* up stack */
	if (ai > 0){
		for (j=curmon->head;j != curmon->current;j=j->next)
			if ISVISIBLE(j) c = j;

		/* if curmon->current was highest, go to the bottom */
		if (!c) for (;j;j=j->next) if ISVISIBLE(j) c = j;

	/* down stack */
	} else {
		if ( !(c = findvisclient(curmon->current->next, YesFloat)) )
			for (c=curmon->head;c && !ISVISIBLE(c);c=c->next);
	}

	if (c && c != curmon->current){
		changecurrent(c, curmon, curmon->curdesk, 0);
		restack(curmon);
		updatefocus();
	}
}

void
manipulate(const Arg arg){
	int x, y, ocx, ocy, nx, ny, nw, nh;
	client* c;
	monitor* m;
	XEvent ev;
	int trytoggle = 0, manipulate = 1;
	Time lasttime = 0;

	if ( !(c = curmon->current) || c->isfull )
		return;

	restack(curmon);

	if (XGrabPointer(dis, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor, CurrentTime) != GrabSuccess)
		return;

	if (!getptrcoords(&x, &y))
		return;

	if (arg.i == WantResize){
		if ( !(m = c->mon) )
			return;
		XWarpPointer(dis, None, c->win, 0, 0, 0, 0, c->w + 1, c->h + 1);
	}

	ocx = c->x; ocy = c->y;
	do {
		XMaskEvent(dis, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type){
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			events[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

			if (arg.i == WantResize){
				nw = MAX(ev.xmotion.x - ocx + 1, 1);
				nh = MAX(ev.xmotion.y - ocy + 1, 1);
				/* if c extends beyond the boundaries of its monitor, make it a float */
				if (m->mx + nw >= curmon->mx && m->mx + nw <= curmon->mx + curmon->mw
				&& m->wy + nh >= curmon->wy && m->wy + nh <= curmon->wy + curmon->wh)
					trytoggle = 1;
				nx = c->x; ny = c->y;

			} else {
				nx = ocx + (ev.xmotion.x - x);
				ny = ocy + (ev.xmotion.y - y);
				/* if c is within snap pixel of the monitor borders, then snap */
				if (abs(curmon->mx - nx) < snap)
					nx = curmon->mx;
				else if (abs((curmon->mx + curmon->mw) - (nx + c->w)) < snap)
					nx = curmon->mx + curmon->mw - c->w;
				if (abs(curmon->wy - ny) < snap)
					ny = curmon->wy;
				else if (abs((curmon->wy + curmon->wh) - (ny + c->h)) < snap)
					ny = curmon->wy + curmon->wh - c->h;
				trytoggle = 1;
				nw = c->w; nh = c->h;
			}
			if (trytoggle)
				if (!c->isfloat && ((abs(nw - c->w) > snap || abs(nh - c->h) > snap)
				|| (abs(nx - c->x) > snap || abs(ny - c->y) > snap)))
					togglefloat(dumbarg);
			if (c->isfloat)
				resizeclient(c, nx, ny, nw, nh);
			XFlush(dis);
			break;
		case ButtonRelease:
			manipulate = 0;
			break;
		}
	} while (manipulate);
	if (arg.i == WantResize)
		XWarpPointer(dis, None, c->win, 0, 0, 0, 0, c->w + 1, c->h + 1);
	XUngrabPointer(dis, CurrentTime);

	if (arg.i == WantResize)
		while (XCheckMaskEvent(dis, EnterWindowMask, &ev));

	if (ISOUTSIDE(c->x, c->y, curmon->mx, curmon->my, curmon->mw, curmon->mh)){
		for EACHMON(mhead){
			if (im != curmon && !ISOUTSIDE(c->x, c->y, im->mx, im->my, im->mw, im->mh)){
				m = im;
				/* this also calls EACHMON - need m, because im - whoops */
				sendmon(c, m);
				changemon(m, YesFocus);
				return;
			}
		}
		outputstats();
	}
}

void
resizeclient(client* c, int x, int y, int w, int h){
	XWindowChanges wc;

	c->x = wc.x = x;
	c->y = wc.y = y;
	c->w = wc.width = w;
	c->h = wc.height = h;
	XConfigureWindow(dis, c->win, CWX|CWY|CWWidth|CWHeight, &wc);
	XSync(dis, False);
}

void
restack(monitor* m){
	XWindowChanges wc;

	if (!m->current)
		return;

	wc.stack_mode = Below;
	wc.sibling = m->current->win;

	for EACHCLIENT(m->head){
		if (ic != m->current && !ic->isfloat && ISVISIBLE(ic)){
			XConfigureWindow(dis, ic->win, CWSibling|CWStackMode, &wc);
			wc.sibling = ic->win;
		}
	}

	XSync(dis, False);
	while (XCheckMaskEvent(dis, EnterWindowMask, &dumbev));
}

void
showhide(monitor* m){
	for EACHCLIENT(m->head){
		if ISVISIBLE(ic){
			XMoveWindow(dis, ic->win, ic->x, ic->y);
			if (ic->isfloat && !ic->isfull){
				resizeclient(ic, ic->x, ic->y, ic->w, ic->h);
				XRaiseWindow(dis, ic->win);
			}
		} else {
			XMoveWindow(dis, ic->win, -2*ic->w, ic->y);
		}
	}
}

void
sendmon(client* c, monitor* m){
	if (c->mon == m || c->isfull)
		return;

	detach(c);
	c->mon = m;
	c->desks = m->seldesks;

	c->next = NULL;
	attachaside(c);
	c->iscur = 0;
	changecurrent(c, c->mon, c->mon->curdesk, 0);
	if (c->isfloat) adjustcoords(c);

	curmon->current = findcurrent(curmon);
	for EACHMON(mhead)
		arrange(im);
	changemon(c->mon, YesFocus);
}

void
todesktop(const Arg arg){
	if (!curmon->current)
		return;

	parsearg(arg, WantInt, &ai, &dumbf);

	if (curmon->curdesk & 1 << ai)
		return;

	curmon->current->desks = 1 << ai;
	curmon->current->iscur = 0;
	changecurrent(curmon->current, curmon, 1 << ai, 1);

	arrange(curmon);
	updatefocus();
	outputstats();
}

void
toggledesktop(const Arg arg){
	unsigned int newdesks;

	if (!curmon->current)
		return;

	parsearg(arg, WantInt, &ai, &dumbf);

	if ( (newdesks = curmon->current->desks ^ (1 << ai)) ){
		curmon->current->desks = newdesks;
		/* set current to be current on new desktop
		 * if it will no longer be visible, adjust current
		 */
		changecurrent(curmon->current, curmon, 1 << ai,
			(curmon->current->desks & curmon->seldesks) ? 0 : 1);

		arrange(curmon);
		updatefocus();
		outputstats();
	}
}

void
togglefloat(const Arg arg){
	if (!curmon->current || curmon->current->isfull)
		return;

	curmon->current->isfloat = !curmon->current->isfloat;
	arrange(curmon);
}

void
togglefs(const Arg arg){
	if (!curmon->current)
		return;

	if ( (curmon->current->isfull = !curmon->current->isfull) ){
		curmon->current->oldfloat = curmon->current->isfloat;
		curmon->current->isfloat = 0;

		XMoveResizeWindow(dis, curmon->current->win, curmon->mx, curmon->my,
				curmon->mw, curmon->mh);
		XRaiseWindow(dis, curmon->current->win);

	} else {
		curmon->current->isfloat = curmon->current->oldfloat;
		arrange(curmon);
	}
}

void
tomon(const Arg arg){
	if (!curmon->current || !mhead->next)
		return;
	
	parsearg(arg, WantInt, &ai, &dumbf);

	sendmon(curmon->current, dirtomon(ai));
	outputstats();
}

void
unmanage(client *c){
	monitor* m = c->mon;

	detach(c);
	free(c);
	arrange(m);
	updatefocus();
	outputstats();
}

void
updatefocus(){
	if (curmon->current)
		XSetInputFocus(dis, curmon->current->win, RevertToPointerRoot, CurrentTime);
	else
		XSetInputFocus(dis, root, RevertToPointerRoot, CurrentTime);

	XSync(dis, False);
}

void
zoom(const Arg arg){
	moveclientup(curmon->current, YesZoom);
	arrange(curmon);
	updatefocus();
}


/* ---------------------------------------
 * Monitor Manipulation
 * ---------------------------------------
 */

void
changemon(monitor* m, int wantfocus){
	if (curmon && curmon->current) grabbuttons(curmon->current, 0);
	curmon = m;
	if (wantfocus) updatefocus();
}

void
cleanupmon(monitor* m){
	free(m->desks);
	free(m);
}

monitor*
createmon(int num, int x, int y, int w, int h){
	int i;
	monitor* m = ecalloc(1, sizeof(monitor));

	m->num = num;
	m->mx = x; m->my = y;
	m->mw = w; m->mh = h;

	m->wy = m->my + (bottombar ? 0 : barpx );
	m->wh = m->mh - barpx;

	/* Default to first layout */
	m->curlayout = (layout*) &layouts[0];
	m->msize = m->mw * MASTER_SIZE;

	m->desks = ecalloc(NUMTAGS, sizeof(desktop));
	for (i=0;i < NUMTAGS;i++){
		m->desks[i].curlayout = m->curlayout;
		m->desks[i].msize = m->msize;
	}

	/* Default to first desktop */
	m->seldesks = m->curdesk = 1 << 0;
	m->head = NULL;
	m->current = NULL;

	return m;
}

monitor*
dirtomon(int dir){
	monitor* m;

	if (dir > 0){
		if ( !(m = curmon->next) ) m = mhead;

	} else if (curmon == mhead){
		for (m=mhead;m->next;m=m->next);

	} else {
		for (m=mhead;m->next != curmon;m=m->next);
	}

	return m;
}

monitor*
findmon(Window w){
	for EACHMON(mhead)
		for EACHCLIENT(im->head)
			if (ic->win == w)
				return im;

	return curmon;
}

void
focusmon(const Arg arg){
	monitor* m;

	if (!mhead->next)
		return;

	parsearg(arg, WantInt, &ai, &dumbf);

	if ((m = dirtomon(ai)) == curmon)
		return;
	changemon(m, YesFocus);
}

#ifdef XINERAMA
static int isuniquegeom(XineramaScreenInfo* unique, size_t n, XineramaScreenInfo* info){
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org)
			return 0;
	return 1;
}
#endif

/* a la dwm 6.1 */
void
updategeom(){
	int x, y;
	client* c;
	monitor* m, * oldmhead = mhead;

#ifdef XINERAMA
	if (XineramaIsActive(dis)){
		int i, j, ns;

		XineramaScreenInfo* info = XineramaQueryScreens(dis, &ns);
		XineramaScreenInfo* unique;

      		/* only consider unique geometries as separate screens */
		unique = ecalloc(ns, sizeof(XineramaScreenInfo));
		for (i=0, j=0;i < ns;i++)
			if (isuniquegeom(unique, j, &info[i]))
				memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
		XFree(info);
		
		mhead = m = createmon(0, unique[0].x_org, unique[0].y_org,
				unique[0].width, unique[0].height);
		for (i=1;i < j;i++){
			m->next = createmon(i, unique[i].x_org, unique[i].y_org,
					unique[i].width, unique[i].height);
			m = m->next;
		}

		free(unique);

	} else
#endif
	{
		mhead = createmon(0, 0, 0, sw, sh);
	}

	/* if updating, reattach any old clients to the new mhead */
	while ( (m = oldmhead) ){
		while ( (c = m->head) ){
			m->head = c->next;
			detach(c);
			c->mon = mhead;
			c->next = NULL;
			attachaside(c);
		}
		mhead->seldesks |= m->seldesks;
		oldmhead = m->next;
		cleanupmon(m);
	}

	/* focus monitor that has the pointer inside it */
	for EACHMON(mhead)
		if (getptrcoords(&x, &y) && !ISOUTSIDE(x, y, im->mx, im->my, im->mw, im->mh)){
			changemon(im, YesFocus);
			break;
		}

	if (!curmon)
		changemon(mhead, YesFocus);
	outputstats();
}


/* ---------------------------------------
 * Client Interfacing
 * ---------------------------------------
 */

client*
findclient(Window w){
	for EACHMON(mhead)
		for EACHCLIENT(im->head)
			if (ic->win == w)
				return ic;

	return NULL;
}

client*
findcurrent(monitor* m){
	for EACHCLIENT(m->head)
		if (ISVISIBLE(ic) && (ic->iscur & m->curdesk))
			return ic;

	return NULL;
}

client*
findprevclient(client* c, int onlyvis, int wantfloat){
	client* ret = NULL;

	for EACHCLIENT(c->mon->head){
		if (ic == c) break;
		if (onlyvis){
			if (ISVISIBLE(ic)) ret = ic;
		} else if (!wantfloat){
			if (ISVISIBLE(ic) && !ic->isfloat) ret = ic;
		}
		if (ic->next == c)
			return (onlyvis || !wantfloat) ? ret : ic;
	}

	return NULL;
}

client*
findvisclient(client* c, int wantfloat){
	for EACHCLIENT(c){
		if ISVISIBLE(ic){
			if (!wantfloat){
				if (!ic->isfloat)
					return ic;
			} else {
				return ic;
			}
		}
	}
	
	return NULL;
}


/* ---------------------------------------
 * Desktop Interfacing
 * ---------------------------------------
 */

void
arrange(monitor* m){
	showhide(m);
	m->curlayout->arrange(m);
	restack(m);
};

void
changemsize(const Arg arg){
	parsearg(arg, WantFloat, &dumbi, &af);
	curmon->msize += ( ((curmon->msize < 0.95 * curmon->mw) && (af > 0))
			|| ((curmon->msize > 0.05 * curmon->mw) && (af < 0))  ) ? af * curmon->mw : 0;

	arrange(curmon);
}

void
loaddesktop(int i){
	curmon->desks[POSTOINT(curmon->curdesk)].msize = curmon->msize;
	curmon->desks[POSTOINT(curmon->curdesk)].curlayout = curmon->curlayout;

	curmon->msize = curmon->desks[i].msize;
	curmon->curlayout = curmon->desks[i].curlayout;
}

void
monocle(monitor* m){
	for EACHCLIENT(m->head)
		if (ISVISIBLE(ic) && !ic->isfloat && !ic->isfull)
			resizeclient(ic, m->mx, m->wy, m->mw, m->wh);
}

void
setlayout(const Arg arg){
	int i;
	for (i=0;i < TABLENGTH(layouts);i++)
		if (strcmp(arg.s, layouts[i].name) == 0)
			curmon->curlayout = (layout*) &layouts[i];

	arrange(curmon);
	outputstats();
}

void
tile(monitor* m){
	client* nf = NULL;
	int n = 0, x = m->mx, y = m->wy;

	/* Find the first non-floating, visible window and tally non-floating, visible windows */
	for EACHCLIENT(m->head) if (!ic->isfloat && ISVISIBLE(ic)){
			nf = (!nf) ? ic : nf;
			n++;
		}

	if (nf && n == 1){
		if (!nf->isfull) resizeclient(nf, x, y, m->mw, m->wh);

	} else if (nf){
		/* so having a master doesn't affect stack splitting */
		n--;

		/* Master window */
		if (!nf->isfull) resizeclient(nf, x, y, m->msize, m->wh);

		/* Stack */
		for EACHCLIENT(nf->next){
			if (ISVISIBLE(ic) && !ic->isfloat && !ic->isfull){
				resizeclient(ic, x + m->msize, y, m->mw - m->msize, m->wh / n);

				y += m->wh / n;
			}
		}
	}
}

void
toggleview(const Arg arg){
	int i;
	unsigned int tagmask;

	parsearg(arg, WantInt, &ai, &dumbf);

	if (ai < 0) tagmask = ~(curmon->seldesks);
	else tagmask = 1 << ai;

	/* if this would leave nothing visible */
	if ((curmon->seldesks ^ tagmask) == 0)
		return;

	curmon->seldesks ^= tagmask;

	if (!(curmon->curdesk & curmon->seldesks)){
		for (i=0;i < NUMTAGS;i++){
			if (curmon->seldesks & 1 << i){
				loaddesktop(i);
				curmon->curdesk = 1 << i;
				break;
			}
		}
	}

	/* refocuses, toggles off curmon->current's currentness */
	if (curmon->current && !ISVISIBLE(curmon->current))
		changecurrent(curmon->current, curmon, curmon->curdesk, 1);

	arrange(curmon);
	updatefocus();
	outputstats();
}

void
view(const Arg arg){
	client* c;

	parsearg(arg, WantInt, &ai, &dumbf);

	loaddesktop(ai);
	curmon->seldesks = curmon->curdesk = 1 << ai;

	if ( (c = findcurrent(curmon)) )
		/* rezero, so it can be set and everyone else unset */
		c->iscur ^= curmon->curdesk;
	else
		c = findvisclient(curmon->head, YesFloat);
		
	changecurrent(c, curmon, curmon->curdesk, 0);

	arrange(curmon);
	updatefocus();
	outputstats();
}


/* ---------------------------------------
 * Backend
 * ---------------------------------------
 */

/* Kill off any remaining clients
 * Free all the things
 */
void
cleanup(){
	int i = 0;
	monitor* m, * tm = mhead;
	const Arg arg = {.s = "-1"};

	/* for unknown reason, using EACHMON segfaults this */
	for (m=mhead;m;m=m->next){
		changemon(m, NoFocus);
		/* make everything visible */
		toggleview(arg);
		while (curmon->current)
			unmanage(curmon->current);
		i++;
	}

	XUngrabKey(dis, AnyKey, AnyModifier, root);

	while ( (m = tm) ){
		tm = m->next;
		cleanupmon(m);
	}

	XFreeCursor(dis, cursor);

	XSync(dis, False);
	XSetInputFocus(dis, PointerRoot, RevertToPointerRoot, CurrentTime);
}

int
getptrcoords(int* x, int* y){
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dis, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

void
grabbuttons(client* c, int focused){
	int i, j;
	unsigned int modifiers[] = { 0, LockMask };

	XUngrabButton(dis, AnyButton, AnyModifier, c->win);
	if (!focused)
		XGrabButton(dis, AnyButton, AnyModifier, c->win, False,
			BUTTONMASK, GrabModeSync, GrabModeSync, None, None);

	for (i=0;i < TABLENGTH(buttons);i++)
		if (buttons[i].click == ClkWin)
			for (j=0;j < TABLENGTH(modifiers);j++)
				XGrabButton(dis, buttons[i].btn,
						buttons[i].mask | modifiers[j],
						c->win, False, BUTTONMASK,
						GrabModeAsync, GrabModeSync, None, None);
}

void
outputstats(){
	char* isdeskocc, * isdesksel;
	unsigned int occ, sel;

	for EACHMON(mhead){
		occ = sel = 0;
		isdeskocc = ecalloc(NUMTAGS, sizeof(char));
		isdesksel = ecalloc(NUMTAGS, sizeof(char));
		sel = im->seldesks;

		for EACHCLIENT(im->head)
			occ |= ic->desks;

		/* uis get reordered in the dest string so they are left-to-right */
		uitos(occ, NUMTAGS-1, isdeskocc);
		uitos(sel, NUMTAGS-1, isdesksel);

		/* output:
		 * "0:00000000:00000000:[]="
		 * im->num:isdeskocc:isdesksel:curlayout->symbol
		 */
		printf("%d:%s:%s:%s%c", im->num, isdeskocc, isdesksel, im->curlayout->symbol, im->next ? ' ' : '\n');

		free(isdeskocc);
		free(isdesksel);
	}
	fflush(stdout);
}

void
parsearg(const Arg arg, int wanttype, int* ai, float* af){
	switch (wanttype){
	case WantInt:
		*ai = atoi(arg.s);
		break;
	case WantFloat:
		*af = atof(arg.s);
		break;
	default:
		break;
	}
}

void
setup(){
	sigchld(0);

	XSetWindowAttributes wa;

	screen = DefaultScreen(dis);
	root = RootWindow(dis, screen);

	sw = XDisplayWidth(dis, screen);
	sh = XDisplayHeight(dis, screen);

	cursor = XCreateFontCursor(dis, 68);

	running = 1;

	mhead = NULL;
	curmon = NULL;

	updategeom();
	loaddesktop(0);
	outputstats();

	wa.cursor = cursor;
	wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask
		|ButtonPressMask|PointerMotionMask|EnterWindowMask
		|StructureNotifyMask|PropertyChangeMask;
	XChangeWindowAttributes(dis, root, CWEventMask|CWCursor, &wa);
	XSelectInput(dis, root, wa.event_mask);
}

void
sigchld(int unused){
	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		die("Can't install SIGCHLD handler");
	
	while (0 < waitpid(-1, NULL, WNOHANG));
}

/* many thanks to bspwm, geeksforgeeks, Beej for sockets */
void
start(){
	XEvent ev;
	fd_set desc;
	char msg[MAXBUFF];
	char* funcstr, * argstr;
	void (*func)(const Arg);
	int sfd, cfd, nbytes, max_fd;
	int xfd = ConnectionNumber(dis);
	struct sockaddr saddress = {AF_UNIX, INPUTSOCK};

	if ( (sfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 )
		die("couldn't create socket!");

	unlink(INPUTSOCK);

	if (bind(sfd, &saddress, sizeof(saddress)) < 0)
		die("couldn't bind socket!");

	if (listen(sfd, SOMAXCONN) < 0)
		die("couldn't listen to socket!");

	while (running){
		XFlush(dis);

		FD_ZERO(&desc);
		FD_SET(sfd, &desc);
		FD_SET(xfd, &desc);
		max_fd = MAX(sfd, xfd);

		if (select(max_fd + 1, &desc, NULL, NULL, NULL) > 0){
			/* Check for socket connections */
			if (FD_ISSET(sfd, &desc)){
				cfd = accept(sfd, NULL, NULL);
				if (cfd > 0 && (nbytes = recv(cfd, msg, sizeof(msg)-1, 0)) > 0){
					msg[nbytes] = '\0';
					funcstr = strtok(msg, " ");
					argstr = strtok(NULL, " ");
					
					/* all functions only have one argument */
					if (argstr){
						const Arg arg = {.s = argstr};
						if ( (func = str2func(funcstr)) )
							func(arg);
					}
					close(cfd);
				}
			}

			/* Check for an X event manually - XNextEvent blocks until an event occurs */
			if (FD_ISSET(xfd, &desc)){
				while (XCheckMaskEvent(dis, ~0, &ev))
					if (events[ev.type])
						events[ev.type](&ev);
			}
		}
	}

	close(sfd);
	unlink(INPUTSOCK);
}

int
xerror(Display* dis, XErrorEvent* e){
	if (e->error_code == BadWindow
	|| (e->request_code == X_SetInputFocus && e->error_code == BadMatch)
	|| (e->request_code == X_PolyText8 && e->error_code == BadDrawable)
	|| (e->request_code == X_PolyFillRectangle && e->error_code == BadDrawable)
	|| (e->request_code == X_PolySegment && e->error_code == BadDrawable)
	|| (e->request_code == X_ConfigureWindow && e->error_code == BadMatch)
	|| (e->request_code == X_GrabButton && e->error_code == BadAccess)
	|| (e->request_code == X_GrabKey && e->error_code == BadAccess)
	|| (e->request_code == X_CopyArea && e->error_code == BadDrawable))
		return 0;

	die("xerror handler had trouble! I'm too lazy to tell you what went wrong.");
	return -1;
}

int
xerrordummy(Display* dis, XErrorEvent* e){
	return 0;
}

int
xsendkill(Window w){
	int n;
	XEvent ev;
	Atom* protocols;
	int exists = 0;
	Atom destproto = XInternAtom(dis, "WM_DELETE_WINDOW", False);

	if (XGetWMProtocols(dis, w, &protocols, &n)){
		while (!exists && n--)
			exists = (protocols[n] == destproto);
		XFree(protocols);
	}

	if (exists){
		ev.type = ClientMessage;
		ev.xclient.window = w;
		ev.xclient.message_type = XInternAtom(dis, "WM_PROTOCOLS", True);
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = destproto;
		ev.xclient.data.l[1] = CurrentTime;
		XSendEvent(dis, w, False, NoEventMask, &ev);
	}

	return exists;
}

void
youviolatedmymother(const Arg arg){
	running = 0;
}

int
main(){
	if ( !(dis = XOpenDisplay(NULL)) )
		die("Cannot open display!");
	XSetErrorHandler(xerror);
	setup();

	start();

	cleanup();
        XCloseDisplay(dis);

	return 0;
}
