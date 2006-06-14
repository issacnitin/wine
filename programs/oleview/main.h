/*
 * OleView (main.h)
 *
 * Copyright 2006 Piotr Caban
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <windows.h>
#include <winreg.h>
#include <commctrl.h>
#include <unknwn.h>
#include <wine/unicode.h>

#include "resource.h"

#define MAX_LOAD_STRING 256
#define MAX_WINDOW_WIDTH 30000

#define STATUS_WINDOW 2000
#define TREE_WINDOW 2001

/*ItemInfo flags */
#define REGTOP 1
#define REGPATH 2
#define SHOWALL 4
#define INTERFACE 8

typedef struct
{
    HWND hMainWnd;
    HWND hPaneWnd;
    HWND hTree;
    HWND hStatusBar;
    HWND hToolBar;
    HINSTANCE hMainInst;
}GLOBALS;

typedef struct
{
    HWND left;
    HWND right;
    INT pos;
    INT size;
    INT width;
    INT height;
    INT last;
}PANE;

typedef struct
{
    /* Main TreeView entries: */
    HTREEITEM hOC;    /* Object Classes */
    HTREEITEM hGBCC;  /* Grouped by Component Category */
    HTREEITEM hO1O;   /* OLE 1.0 Objects */
    HTREEITEM hCLO;   /* COM Library Objects */
    HTREEITEM hAO;    /* All Objects */
    HTREEITEM hAID;   /* Application IDs */
    HTREEITEM hTL;    /* Type Libraries */
    HTREEITEM hI;     /* Interfaces */
}TREE;

typedef struct
{
    CHAR cFlag;
    WCHAR info[MAX_LOAD_STRING];
    WCHAR clsid[MAX_LOAD_STRING];
    BOOL loaded;
    IUnknown *pU;
}ITEM_INFO;

extern GLOBALS globals;
extern TREE tree;

/* Predefinitions: */
/* pane.c */
BOOL CreatePanedWindow(HWND hWnd, HWND *hWndCreated, HINSTANCE hInst);
void SetLeft(HWND hParent, HWND hWnd);
void SetRight(HWND hParent, HWND hWnd);

/* tree.h */
void EmptyTree(void);
HWND CreateTreeWindow(HINSTANCE hInst);
