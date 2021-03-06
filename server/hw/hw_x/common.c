/* Functions that are common between hw_x11 and hw_xft.  Currently does not cover hw_gfx. */

/* this can stay static, X11_FlushHW() is not reentrant */
static hwcol _col;

INLINE void X11_Mogrify(dat x, dat y, uldat len) {
    hwattr *V, *oV;
    hwcol col;
    udat buflen = 0;
    hwfont f;
    XChar16 buf[TW_SMALLBUFF];
    int xbegin, ybegin;
    
    if (xhw_view) {
	if (x >= xhw_endx || x + len < xhw_startx ||
	    y < xhw_starty || y >= xhw_endy)
	    return;
    
	if (x + len >= xhw_endx)
	    len = xhw_endx - x;
    
	if (x < xhw_startx) {
	    len -= xhw_startx - x;
	    x = xhw_startx;
	}
    }

    xbegin = (x - xhw_startx) * (ldat)xwfont;
    ybegin = (y - xhw_starty) * (ldat)xhfont;
    
    V = Video + x + y * (ldat)DisplayWidth;
    oV = OldVideo + x + y * (ldat)DisplayWidth;
    
    for (_col = ~HWCOL(*V); len; x++, V++, oV++, len--) {
	col = HWCOL(*V);
	if (buflen && (col != _col || (ValidOldVideo && *V == *oV) || buflen == TW_SMALLBUFF)) {
	    XDRAW(_col, buf, buflen);
	    buflen = 0;
	}
	if (!ValidOldVideo || *V != *oV) {
	    if (!buflen) {
		xbegin = (x - xhw_startx) * (ldat)xwfont;
		_col = col;
	    }
	    f = xUTF_32_to_charset(HWFONT(*V));
            XChar16FromRaw(f, &buf[buflen++]);
	}
    }
    if (buflen) {
	XDRAW(_col, buf, buflen);
	buflen = 0;
    }
}

INLINE ldat diff(ldat x, ldat y) {
    return x >= y ? x - y : y - x;
}

enum { MAX_FONT_SCORE = 100 };

static ldat calcFontScore(udat fontwidth, udat fontheight, ldat width, ldat height) {
    /*
     * TODO xft: since xft fonts are scalable, we could just consider the aspect ratio
     * compared to fontwidth X fontheight.
     */
    ldat prod1 = (ldat)height * fontwidth;
    ldat prod2 = (ldat)width * fontheight;
    ldat score = MAX_FONT_SCORE - diff(height, fontheight) - 2 * diff(width, fontwidth)
        /* compare height/width ratio with requested one */
        - diff(prod1, prod2) / Max3(1, fontwidth, width);
    return score;
}

static byte X11_LoadFont(CONST char * fontname, udat fontwidth, udat fontheight) {
    char * alloc_fontname = 0;
    byte loaded = tfalse;

    if (!fontname)
        fontname = alloc_fontname = X11_AutodetectFont(fontwidth, fontheight);
    
#if HW_X_DRIVER == HW_X11
    if ((fontname && (xsfont = XLoadQueryFont(xdisplay, fontname)))
        || (xsfont = XLoadQueryFont(xdisplay, fontname = "fixed")))
#elif HW_X_DRIVER == HW_XFT
    if (fontname && (xsfont = XftFontOpenName(xdisplay, DefaultScreen(xdisplay), fontname)))
#endif
    {
        loaded = ttrue;

#if HW_X_DRIVER == HW_X11
        xwfont = xsfont->min_bounds.width;
#elif HW_X_DRIVER == HW_XFT
        xwfont = xsfont->max_advance_width;
#endif
        xwidth = xwfont * (unsigned)(HW->X = GetDisplayWidth());
        xhfont = (xupfont = xsfont->ascent) + xsfont->descent;
        xheight = xhfont * (unsigned)(HW->Y = GetDisplayHeight());
        
        printk("      selected %ux%u font `%."STR(TW_SMALLBUFF)"s'\n", (unsigned)xwfont, (unsigned)xhfont, fontname);
    }
    if (alloc_fontname)
        FreeMem(alloc_fontname);

    return loaded;
}

static void X11_QuitHW(void) {
#ifdef TW_FEATURE_X11_XIM_XIC
    if (xic)    XDestroyIC(xic);
    if (xim)    XCloseIM(xim);
#endif
    X11_FlavorQuitHW();
    if (xgc != None) XFreeGC(xdisplay, xgc);
    if (xwindow != None) {
	XUnmapWindow(xdisplay, xwindow);
	XDestroyWindow(xdisplay, xwindow);
    }
    XCloseDisplay(xdisplay);
    xdisplay = NULL;
    
    if (HW->keyboard_slot != NOSLOT)
	UnRegisterRemote(HW->keyboard_slot);
    HW->keyboard_slot = NOSLOT;
    HW->KeyboardEvent = (void *)NoOp;
    
    HW->QuitHW = NoOp;

    FreeMem(HW->Private);
    HW->Private = NULL;
}

static byte X11_InitHW(void) {
    char *arg = HW->Name;
    int xscreen;
    unsigned int xdepth;
    XSetWindowAttributes xattr;
    XColor xcolor;
    XSizeHints *xhints;
    XEvent event;
    Visual *xvisual;
    Colormap colormap;
    byte *s, *xdisplay_ = NULL, *xdisplay0 = NULL,
        *fontname = NULL, *fontname0 = NULL,
        *charset = NULL, *charset0 = NULL,
        title[X11_TITLE_MAXLEN];
    int i, nskip;
    udat fontwidth = 8, fontheight = 16;
    byte drag = tfalse, noinput = tfalse;
    unsigned long xcreategc_mask = GCForeground|GCBackground|GCGraphicsExposures;
    
    if (!(HW->Private = (struct x11_data *)AllocMem(sizeof(struct x11_data)))) {
	printk("      X11_InitHW(): Out of memory!\n");
	return tfalse;
    }
    WriteMem(HW->Private, 0, sizeof(struct x11_data));

    /* default: show the whole screen */
    xhw_view = xhw_startx = xhw_starty = xhw_endx = xhw_endy = 0;

    /* not yet opened */
    xdisplay = NULL;
    
    if (arg && ((nskip = check_hw_name(arg)) > 0)) {
        arg += nskip;

	if (*arg == '@') {
	    if ((s = strchr(xdisplay_ = ++arg, ','))) {
		*(xdisplay0 = s) = '\0';
		arg = s + 1;
	    } else
		arg = NULL;
	}

	while (arg && *arg) {
	    /* parse options */
	    if (*arg == ',') {
		arg++;
		continue;
	    }
	    if (!strncmp(arg, "font=", 5)) {
		fontname = arg += 5;
		s = strchr(arg, ',');
		if (s) *(fontname0 = s++) = '\0';
		arg = s;
            } else if (!strncmp(arg, "fontsize=", 9)) {
                int n1 = atoi(arg += 9), n2 = 0;
                byte ch;
                if (n1 > 0) {
                    while ((ch = (byte)*++arg) && ch != ',') {
                        if (ch == 'x') {
                            n2 = atoi(arg+1);
                            break;
                        }
                    }
                    fontwidth  = Min2(TW_MAXUDAT, n2 > 0 ? n1 : n1 / 2);
                    fontheight = Min2(TW_MAXUDAT, n2 > 0 ? n2 : n1);
                }
	    } else if (!strncmp(arg, "charset=", 8)) {
		charset = arg += 8;
		s = strchr(arg, ',');
		if (s) *(charset0 = s++) = '\0';
		arg = s;
	    } else if (!strncmp(arg, "view=", 5)) {
		xhw_view = 1;
		xhw_endx = strtol(arg+5, &arg, 0);
		xhw_endy = strtol(arg+1, &arg, 0);
		xhw_startx = strtol(arg+1, &arg, 0);
		xhw_starty = strtol(arg+1, &arg, 0);
		xhw_endx += xhw_startx;
		xhw_endy += xhw_starty;
	    } else if (!strncmp(arg, "drag", 4)) {
		arg += 4;
		drag = ttrue;
	    } else if (!strncmp(arg, "noinput", 7)) {
		arg += 7;
		noinput = ttrue;
	    } else
		arg = strchr(arg, ',');
	}
    }

    xsfont = NULL; xhints = NULL;
    xwindow = None; xgc = None;
    xReqCount = XReqCount = 0;
    HW->keyboard_slot = NOSLOT;
    
    if ((xdisplay = XOpenDisplay(xdisplay_))) do {
	
	(void)XSetIOErrorHandler(X11_Die);

	if (!X11_CheckRemapKeys())
	    break;

	xscreen = DefaultScreen(xdisplay);
	xdepth  = DefaultDepth(xdisplay, xscreen);
        xvisual = DefaultVisual(xdisplay, xscreen);
        colormap = DefaultColormap(xdisplay, xscreen);
	
	for (i = 0; i <= MAXCOL; i++) {
	    xcolor.red   = 257 * (udat)Palette[i].Red;
	    xcolor.green = 257 * (udat)Palette[i].Green;
	    xcolor.blue  = 257 * (udat)Palette[i].Blue;
            if (!X11_AllocColor(xdisplay, xvisual, colormap, &xcolor, &xcol[i], i)) {
                printk("      X11_InitHW() failed to allocate colors\n");
                break;
            }
	}
	if (i <= MAXCOL)
	    break;
	
	xattr.background_pixel = xcol[0];
	xattr.event_mask = ExposureMask | VisibilityChangeMask |
	    StructureNotifyMask | SubstructureNotifyMask |
	    KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

	if (!X11_LoadFont(fontname, fontwidth, fontheight))
	    break;
	
	if (xhw_view && xhw_startx >= 0 && xhw_starty >= 0 && xhw_endx > xhw_startx && xhw_endy > xhw_starty) {
	    /* a valid view was specified */
	    
	    xwidth  = xwfont * (ldat)(xhw_endx - xhw_startx);
	    xheight = xhfont * (ldat)(xhw_endy - xhw_starty);
	} else {
	    xhw_view = xhw_startx = xhw_starty = 0;
	    xhw_endx = HW->X;
	    xhw_endy = HW->Y;
	}
	
	if ((xwindow = XCreateWindow(xdisplay, DefaultRootWindow(xdisplay), 0, 0,
				     xwidth, xheight, 0, xdepth, InputOutput,
				     xvisual, CWBackPixel | CWEventMask, &xattr)) &&

	    (xsgc.foreground = xsgc.background = xcol[0],
	     xsgc.graphics_exposures = False,
#if HW_X_DRIVER == HW_X11
	     xsgc.font = xsfont->fid,
             xcreategc_mask = xcreategc_mask|GCFont,
#elif HW_X_DRIVER == HW_XFT
             xforeground = xbackground = xftcolors[0],
#endif
	     xgc = XCreateGC(xdisplay, xwindow, xcreategc_mask, &xsgc)) &&

	    (xhints = XAllocSizeHints()))
        {
	    
            static XComposeStatus static_xcompose;
            xcompose = static_xcompose;

#if HW_X_DRIVER == HW_XFT
            xftdraw = XftDrawCreate(xdisplay,xwindow,xvisual,colormap);
#endif

#ifdef TW_FEATURE_X11_XIM_XIC
            xim = XOpenIM(xdisplay, NULL, NULL, NULL);
            if (xim != NULL) {
                xic = XCreateIC(xim, XNInputStyle, XIMStatusNothing|XIMPreeditNothing,
                                XNClientWindow, xwindow, XNFocusWindow, xwindow, NULL);
                if (xic == NULL) {
                    XCloseIM(xim);
                    xim = NULL;
                }
            } else
                xic = NULL;
#endif
            X11_FillWindowTitle(title, sizeof(title));
	    XStoreName(xdisplay, xwindow, title);


	    if (!(xUTF_32_to_charset = X11_UTF_32_to_charset_function(charset)))
		xUTF_32_to_charset = X11_UTF_32_to_UCS_2;
	    /*
	     * ask ICCCM-compliant window manager to tell us when close window
	     * has been chosen, rather than just killing us
	     */
	    xWM_PROTOCOLS = XInternAtom(xdisplay, "WM_PROTOCOLS", False);
	    xWM_DELETE_WINDOW = XInternAtom(xdisplay, "WM_DELETE_WINDOW", False);
	    xTARGETS = XInternAtom(xdisplay, "TARGETS", False);

	    XChangeProperty(xdisplay, xwindow, xWM_PROTOCOLS, XA_ATOM, 32, PropModeReplace,
			    (unsigned char *) &xWM_DELETE_WINDOW, 1);

	    if (xhw_view) {
		xhints->flags = PMinSize|PMaxSize;
		xhints->min_width = xhints->max_width = xwidth;
		xhints->min_height = xhints->max_height = xheight;
	    } else {
		xhints->flags = PResizeInc;
		xhints->width_inc  = xwfont;
		xhints->height_inc = xhfont;
	    }
	    XSetWMNormalHints(xdisplay, xwindow, xhints);
	    
	    XMapWindow(xdisplay, xwindow);
	    
	    do {
		XNextEvent(xdisplay, &event);
	    } while (event.type != MapNotify);
	    
	    XFree(xhints); xhints = NULL;
	    
	    HW->mouse_slot = NOSLOT;
	    HW->keyboard_slot = RegisterRemote(i = XConnectionNumber(xdisplay), (obj)HW,
					       X11_KeyboardEvent);
	    if (HW->keyboard_slot == NOSLOT)
		break;
	    fcntl(i, F_SETFD, FD_CLOEXEC);
	    
	    HW->FlushVideo = X11_FlushVideo;
	    HW->FlushHW = X11_FlushHW;
	    
	    HW->KeyboardEvent = X11_KeyboardEvent;
	    HW->MouseEvent = (void *)NoOp; /* mouse events handled by X11_KeyboardEvent */
	    
	    HW->XY[0] = HW->XY[1] = 0;
	    HW->TT = NOCURSOR;
	    
	    HW->ShowMouse = NoOp;
	    HW->HideMouse = NoOp;
	    HW->UpdateMouseAndCursor = NoOp;
	    HW->MouseState.x = HW->MouseState.y = HW->MouseState.keys = 0;
	    
	    HW->DetectSize  = X11_DetectSize;
	    HW->CheckResize = X11_CheckResize;
	    HW->Resize      = X11_Resize;
	    
	    HW->HWSelectionImport  = X11_SelectionImport_X11;
	    HW->HWSelectionExport  = X11_SelectionExport_X11;
	    HW->HWSelectionRequest = X11_SelectionRequest_X11;
	    HW->HWSelectionNotify  = X11_SelectionNotify_X11;
	    HW->HWSelectionPrivate = 0;
	    
	    if (drag) {
		HW->CanDragArea = X11_CanDragArea;
		HW->DragArea    = X11_DragArea;
	    } else
		HW->CanDragArea = NULL;
	    
	    HW->Beep = X11_Beep;
	    HW->Configure = X11_Configure;
	    HW->SetPalette = (void *)NoOp;
	    HW->ResetPalette = NoOp;
	    
	    HW->QuitHW = X11_QuitHW;
	    HW->QuitKeyboard  = NoOp;
	    HW->QuitMouse = NoOp;
	    HW->QuitVideo = NoOp;
	    
	    HW->DisplayIsCTTY = tfalse;
	    HW->FlagsHW &= ~FlHWSoftMouse; /* mouse pointer handled by X11 server */
	    
	    HW->FlagsHW |= FlHWNeedOldVideo;
	    HW->FlagsHW |= FlHWExpensiveFlushVideo;
	    if (noinput)
		HW->FlagsHW |= FlHWNoInput;
	    
	    HW->NeedHW = 0;
	    HW->CanResize = ttrue;
	    HW->merge_Threshold = 0;
	    
	    /*
	     * we must draw everything on our new shiny window
	     * without forcing all other displays
	     * to redraw everything too.
	     */
	    HW->RedrawVideo = tfalse;
	    NeedRedrawVideo(0, 0, HW->X - 1, HW->Y - 1);
	    
	    if (xdisplay0) *xdisplay0 = ',';
	    if (fontname0) *fontname0 = ',';
	    if (charset0) *charset0 = ',';
	    
	    return ttrue;
	}
    } while (0); else {
	if (xdisplay_ || (xdisplay_ = getenv("DISPLAY")))
	    printk("      X11_InitHW() failed to open display %."STR(TW_SMALLBUFF)"s\n", HW->Name);
	else
	    printk("      X11_InitHW() failed: DISPLAY is not set\n");
    }

fail:
    if (xdisplay0) *xdisplay0 = ',';
    if (fontname0) *fontname0 = ',';
    if (charset0) *charset0 = ',';
	
    if (xdisplay)
	X11_QuitHW();

    FreeMem(HW->Private);
    HW->Private = NULL;
    
    return tfalse;
}

byte InitModule(module Module) {
    Module->Private = X11_InitHW;
    return ttrue;
}

/* this MUST be included, or it seems that a bug in dlsym() gets triggered */
void QuitModule(module Module) {
}
