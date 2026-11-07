// SPDX-License-Identifier: MIT
/* Minimal key sender for automated testing on Xvfb: xkey [hold_ms] KEY [KEY...]
 * KEY is an X keysym name (Return, Up, Escape, a, ...). "alt+s" style combos are supported.
 * "sleep:N" waits N milliseconds. */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void key(Display *d, const char *name, int holdMs)
{
	char buf[64], *plus;
	KeyCode mod = 0, kc;
	snprintf(buf, sizeof buf, "%s", name);
	plus = strchr(buf, '+');
	if (plus)
	{
		*plus = '\0';
		if (!strcmp(buf, "alt"))
			mod = XKeysymToKeycode(d, XK_Alt_L);
		else if (!strcmp(buf, "ctrl"))
			mod = XKeysymToKeycode(d, XK_Control_L);
		else if (!strcmp(buf, "shift"))
			mod = XKeysymToKeycode(d, XK_Shift_L);
		name = plus + 1;
	}
	kc = XKeysymToKeycode(d, XStringToKeysym(name));
	if (!kc)
	{
		fprintf(stderr, "Unknown key: %s\n", name);
		return;
	}
	if (mod)
		XTestFakeKeyEvent(d, mod, True, 0);
	XTestFakeKeyEvent(d, kc, True, 0);
	XFlush(d);
	usleep(holdMs * 1000);
	XTestFakeKeyEvent(d, kc, False, 0);
	if (mod)
		XTestFakeKeyEvent(d, mod, False, 0);
	XFlush(d);
}

int main(int argc, char **argv)
{
	Display *d = XOpenDisplay(NULL);
	int i = 1, hold = 80;
	if (!d)
		return 1;
	if (argc > 1 && atoi(argv[1]) > 0)
		hold = atoi(argv[i++]);
	for (; i < argc; ++i)
	{
		if (!strncmp(argv[i], "sleep:", 6))
			usleep(atoi(argv[i] + 6) * 1000);
		else
		{
			key(d, argv[i], hold);
			usleep(150000);
		}
	}
	XCloseDisplay(d);
	return 0;
}
