/*
AppleWin : An Apple //e emulator for Windows

This file is part of AppleWin.

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/* Description: User key remapping via keymap.txt
 *
 * File format (one mapping per line, '#' starts a comment):
 *     <key you press> = <key the Apple II sees>
 * Examples:
 *     Q = 5
 *     Space = Return
 *     Numpad8 = Up
 *     A = D
 *     D = A        (swaps work: each key is only remapped once)
 *
 * The file is re-read automatically when it changes, so it can be
 * edited while AppleWin is running.
 */

#include "StdAfx.h"

#include "KeyRemap.h"
#include "Core.h"
#include "Log.h"
#include "../resource/resource.h"

#include <algorithm>
#include <map>
#include <vector>

static const char* const kKeymapFilename = "keymap.txt";

static BYTE g_keyMap[256];			// g_keyMap[src VK] = dst VK (0 = not remapped)
static bool g_keyMapActive = false;
static std::string g_keyMapPath;
static FILETIME g_keyMapTime = {0, 0};

//===========================================================================

struct KeyName
{
	const char* name;
	BYTE vk;
};

static const KeyName g_keyNames[] =
{
	{"Space", VK_SPACE},
	{"Return", VK_RETURN},		{"Enter", VK_RETURN},
	{"Esc", VK_ESCAPE},			{"Escape", VK_ESCAPE},
	{"Tab", VK_TAB},
	{"Backspace", VK_BACK},
	{"Delete", VK_DELETE},		{"Del", VK_DELETE},
	{"Insert", VK_INSERT},		{"Ins", VK_INSERT},
	{"Left", VK_LEFT},			{"Right", VK_RIGHT},
	{"Up", VK_UP},				{"Down", VK_DOWN},
	{"Home", VK_HOME},			{"End", VK_END},
	{"PageUp", VK_PRIOR},		{"PageDown", VK_NEXT},
	{"Numpad0", VK_NUMPAD0},	{"Numpad1", VK_NUMPAD1},	{"Numpad2", VK_NUMPAD2},
	{"Numpad3", VK_NUMPAD3},	{"Numpad4", VK_NUMPAD4},	{"Numpad5", VK_NUMPAD5},
	{"Numpad6", VK_NUMPAD6},	{"Numpad7", VK_NUMPAD7},	{"Numpad8", VK_NUMPAD8},
	{"Numpad9", VK_NUMPAD9},
	{"NumpadMultiply", VK_MULTIPLY},	{"NumpadAdd", VK_ADD},
	{"NumpadSubtract", VK_SUBTRACT},	{"NumpadDecimal", VK_DECIMAL},
	{"NumpadDivide", VK_DIVIDE},
	{"Minus", VK_OEM_MINUS},	{"Equals", VK_OEM_PLUS},
	{"Comma", VK_OEM_COMMA},	{"Period", VK_OEM_PERIOD},
	{"Slash", VK_OEM_2},		{"Semicolon", VK_OEM_1},
	{"Quote", VK_OEM_7},		{"Backquote", VK_OEM_3},
	{"LBracket", VK_OEM_4},		{"RBracket", VK_OEM_6},
	{"Backslash", VK_OEM_5},
	{"LAlt", VK_LMENU},			{"RAlt", VK_RMENU},		// Open-Apple / Closed-Apple (joystick buttons)
	{"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
	{"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
	{"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
};

static std::string Trim(const std::string& s)
{
	const char* ws = " \t\r\n";
	size_t b = s.find_first_not_of(ws);
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(ws);
	return s.substr(b, e - b + 1);
}

// Returns VK code, or 0 if not recognised
static BYTE ParseKeyName(const std::string& name)
{
	if (name.empty())
		return 0;

	// Single letter / digit
	if (name.size() == 1)
	{
		char c = (char)toupper((unsigned char)name[0]);
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
			return (BYTE)c;

		// Other single printable char: ask the current keyboard layout
		SHORT r = VkKeyScanA(name[0]);
		if (r != -1)
			return (BYTE)(r & 0xFF);
		return 0;
	}

	for (size_t i = 0; i < sizeof(g_keyNames) / sizeof(g_keyNames[0]); i++)
	{
		if (_stricmp(name.c_str(), g_keyNames[i].name) == 0)
			return g_keyNames[i].vk;
	}

	// Raw virtual-key code, eg. 0x41
	if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
	{
		unsigned long v = strtoul(name.c_str() + 2, NULL, 16);
		if (v > 0 && v < 256)
			return (BYTE)v;
	}

	return 0;
}

static bool GetFileTime(const std::string& path, FILETIME& ft)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
		return false;
	ft = data.ftLastWriteTime;
	return true;
}

static void LoadKeymap()
{
	memset(g_keyMap, 0, sizeof(g_keyMap));
	g_keyMapActive = false;

	FILE* f = fopen(g_keyMapPath.c_str(), "rt");
	if (!f)
		return;

	char buf[256];
	int lineNum = 0;
	while (fgets(buf, sizeof(buf), f))
	{
		lineNum++;
		std::string line = Trim(buf);
		if (line.empty() || line[0] == '#')
			continue;

		// Allow '=' itself as the source key: "= = X"
		size_t sep = line.find('=', line[0] == '=' ? 1 : 0);
		if (sep == std::string::npos)
		{
			LogFileOutput("KeyRemap: line %d: missing '='\n", lineNum);
			continue;
		}

		std::string srcName = Trim(line.substr(0, sep));
		std::string dstName = Trim(line.substr(sep + 1));

		// Strip trailing comment on the target (but keep '#' as a key name)
		size_t hash = dstName.find(" #");
		if (hash != std::string::npos)
			dstName = Trim(dstName.substr(0, hash));

		BYTE src = ParseKeyName(srcName);
		BYTE dst = ParseKeyName(dstName);
		if (!src || !dst)
		{
			LogFileOutput("KeyRemap: line %d: unknown key '%s'\n", lineNum, !src ? srcName.c_str() : dstName.c_str());
			continue;
		}

		g_keyMap[src] = dst;
		g_keyMapActive = true;
	}

	fclose(f);
	LogFileOutput("KeyRemap: loaded %s\n", g_keyMapPath.c_str());
}

// Reload if the file was created, edited or deleted since last load
static void CheckForChanges()
{
	FILETIME ft = {0, 0};
	GetFileTime(g_keyMapPath, ft);
	if (CompareFileTime(&ft, &g_keyMapTime) != 0)
	{
		g_keyMapTime = ft;
		LoadKeymap();
	}
}

//===========================================================================

void KeyRemap_Init()
{
	g_keyMapPath = g_sProgramDir + kKeymapFilename;
	CheckForChanges();
}

static bool IsExtendedKey(BYTE vk)
{
	switch (vk)
	{
	case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
	case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
	case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
	case VK_RMENU: case VK_RCONTROL:
		return true;
	}
	return false;
}

void KeyRemap_Apply(MSG& msg)
{
	if (msg.message != WM_KEYDOWN && msg.message != WM_KEYUP &&
		msg.message != WM_SYSKEYDOWN && msg.message != WM_SYSKEYUP)
		return;

	// Only while the emulated machine is running (not in the debugger)
	if (g_nAppMode != MODE_RUNNING && g_nAppMode != MODE_STEPPING)
		return;

	if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)
		CheckForChanges();

	if (!g_keyMapActive || msg.wParam > 255)
		return;

	BYTE vk = (BYTE)msg.wParam;

	// Windows reports generic Alt; resolve to left/right so LAlt/RAlt mappings work
	if (vk == VK_MENU)
		vk = (HIWORD(msg.lParam) & KF_EXTENDED) ? VK_RMENU : VK_LMENU;

	BYTE dst = g_keyMap[vk];
	if (!dst)
		return;

	// Rewrite the virtual key, and the scan code + extended flag to match,
	// so TranslateMessage() produces the correct WM_CHAR for the new key.
	WPARAM newVK = dst;
	if (dst == VK_LMENU || dst == VK_RMENU)
		newVK = VK_MENU;

	UINT scan = MapVirtualKeyA(dst, MAPVK_VK_TO_VSC) & 0xFF;
	LPARAM lp = msg.lParam & ~(LPARAM)0x01FF0000;	// clear scan code (bits 16-23) + extended (bit 24)
	lp |= (LPARAM)scan << 16;
	if (IsExtendedKey(dst))
		lp |= (LPARAM)1 << 24;

	// Always deliver as a plain WM_KEYDOWN/WM_KEYUP. (WM_SYSKEYx would be re-posted by
	// the frame window as WM_KEYx and come back through here, getting remapped twice.)
	if (msg.message == WM_SYSKEYDOWN) msg.message = WM_KEYDOWN;
	if (msg.message == WM_SYSKEYUP)   msg.message = WM_KEYUP;

	msg.wParam = newVK;
	msg.lParam = lp;
}

//===========================================================================
// Settings dialog

static std::string KeyNameFromVK(BYTE vk)
{
	if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
		return std::string(1, (char)vk);

	for (size_t i = 0; i < sizeof(g_keyNames) / sizeof(g_keyNames[0]); i++)
	{
		if (g_keyNames[i].vk == vk)
			return g_keyNames[i].name;	// first name in table is the preferred one
	}

	char buf[8];
	sprintf_s(buf, sizeof(buf), "0x%02X", vk);
	return buf;
}

static bool SaveKeymap(const std::map<BYTE, BYTE>& map)
{
	FILE* f = fopen(g_keyMapPath.c_str(), "wt");
	if (!f)
		return false;

	fputs("# AppleWin key remapping\n"
		  "# Format:   <key you press> = <key the Apple II sees>\n"
		  "# Edit here or via Configuration > Input > Key Remapping.\n\n", f);

	for (std::map<BYTE, BYTE>::const_iterator it = map.begin(); it != map.end(); ++it)
		fprintf(f, "%s = %s\n", KeyNameFromVK(it->first).c_str(), KeyNameFromVK(it->second).c_str());

	fclose(f);

	// Apply immediately
	GetFileTime(g_keyMapPath, g_keyMapTime);
	LoadKeymap();
	return true;
}

static std::map<BYTE, BYTE> g_dlgMap;	// working copy while the dialog is open (PC key -> Apple key)

//---------------------------------------------------------------------------
// Apple IIe keyboard picture

struct AppleKey
{
	const char* label;
	BYTE vk;		// key the Apple II receives (0 = can't be remapped to)
	float w;		// width in key units (each row = 15 units)
};

static const AppleKey g_row0[] = {
	{"ESC",VK_ESCAPE,1}, {"1",'1',1}, {"2",'2',1}, {"3",'3',1}, {"4",'4',1}, {"5",'5',1},
	{"6",'6',1}, {"7",'7',1}, {"8",'8',1}, {"9",'9',1}, {"0",'0',1},
	{"-",VK_OEM_MINUS,1}, {"=",VK_OEM_PLUS,1}, {"DELETE",VK_DELETE,2} };
static const AppleKey g_row1[] = {
	{"TAB",VK_TAB,1.5f}, {"Q",'Q',1}, {"W",'W',1}, {"E",'E',1}, {"R",'R',1}, {"T",'T',1},
	{"Y",'Y',1}, {"U",'U',1}, {"I",'I',1}, {"O",'O',1}, {"P",'P',1},
	{"[",VK_OEM_4,1}, {"]",VK_OEM_6,1}, {"\\",VK_OEM_5,1.5f} };
static const AppleKey g_row2[] = {
	{"CONTROL",0,1.75f}, {"A",'A',1}, {"S",'S',1}, {"D",'D',1}, {"F",'F',1}, {"G",'G',1},
	{"H",'H',1}, {"J",'J',1}, {"K",'K',1}, {"L",'L',1},
	{";",VK_OEM_1,1}, {"'",VK_OEM_7,1}, {"RETURN",VK_RETURN,2.25f} };
static const AppleKey g_row3[] = {
	{"SHIFT",0,2.25f}, {"Z",'Z',1}, {"X",'X',1}, {"C",'C',1}, {"V",'V',1}, {"B",'B',1},
	{"N",'N',1}, {"M",'M',1}, {",",VK_OEM_COMMA,1}, {".",VK_OEM_PERIOD,1}, {"/",VK_OEM_2,1},
	{"SHIFT",0,2.75f} };
static const AppleKey g_row4[] = {
	{"CAPS LOCK",0,1.5f}, {"Open Apple",VK_LMENU,1}, {"`",VK_OEM_3,1}, {"",VK_SPACE,6.5f},
	{"Solid Apple",VK_RMENU,1}, {"\x11",VK_LEFT,1}, {"\x10",VK_RIGHT,1}, {"\x1F",VK_DOWN,1}, {"\x1E",VK_UP,1} };

struct KeyRow { const AppleKey* keys; int count; };
static const KeyRow g_rows[] = {
	{g_row0, sizeof(g_row0)/sizeof(g_row0[0])}, {g_row1, sizeof(g_row1)/sizeof(g_row1[0])},
	{g_row2, sizeof(g_row2)/sizeof(g_row2[0])}, {g_row3, sizeof(g_row3)/sizeof(g_row3[0])},
	{g_row4, sizeof(g_row4)/sizeof(g_row4[0])} };
static const int kNumRows = sizeof(g_rows)/sizeof(g_rows[0]);

static const char* const kKbdClass = "AppleWinKeyRemapKeyboard";
static const AppleKey* g_selectedKey = NULL;	// Apple key waiting for a PC key press
static HWND g_hDlg = NULL;

static bool GetKeyRect(const RECT& client, int row, int col, RECT& r)
{
	const int margin = 6, gap = 3;
	float unit = (float)(client.right - client.left - 2*margin) / 15.0f;
	int rowH = (client.bottom - client.top - 2*margin) / kNumRows;

	float x = (float)margin;
	for (int i = 0; i < col; i++)
		x += g_rows[row].keys[i].w * unit;

	r.left   = (int)x + gap/2;
	r.right  = (int)(x + g_rows[row].keys[col].w * unit) - gap/2;
	r.top    = margin + row * rowH + gap/2;
	r.bottom = margin + (row + 1) * rowH - gap/2;
	return true;
}

static const AppleKey* HitTest(HWND hWnd, int x, int y)
{
	RECT client;
	GetClientRect(hWnd, &client);
	POINT pt = {x, y};
	for (int row = 0; row < kNumRows; row++)
		for (int col = 0; col < g_rows[row].count; col++)
		{
			RECT r;
			GetKeyRect(client, row, col, r);
			if (PtInRect(&r, pt))
				return &g_rows[row].keys[col];
		}
	return NULL;
}

// PC keys currently bound to this Apple key, eg. "Q, F5"
static std::string BoundKeysText(BYTE appleVK)
{
	std::string s;
	for (std::map<BYTE, BYTE>::const_iterator it = g_dlgMap.begin(); it != g_dlgMap.end(); ++it)
		if (it->second == appleVK)
		{
			if (!s.empty()) s += ", ";
			s += KeyNameFromVK(it->first);
		}
	return s;
}

static bool IsOnPicture(BYTE vk)
{
	for (int row = 0; row < kNumRows; row++)
		for (int col = 0; col < g_rows[row].count; col++)
			if (g_rows[row].keys[col].vk == vk)
				return true;
	return false;
}

static void SetStatus(const std::string& text)
{
	if (g_hDlg)
		SetDlgItemTextA(g_hDlg, IDC_KEYREMAP_STATUS, text.c_str());
}

static void ShowIdleStatus()
{
	int other = 0;
	for (std::map<BYTE, BYTE>::const_iterator it = g_dlgMap.begin(); it != g_dlgMap.end(); ++it)
		if (!IsOnPicture(it->second))
			other++;

	char buf[160];
	if (other)
		sprintf_s(buf, sizeof(buf), "%d binding(s): %d not shown on the picture (eg. numpad keys from keymap.txt) - these are kept.", (int)g_dlgMap.size(), other);
	else
		sprintf_s(buf, sizeof(buf), "%d binding(s). Green keys have a PC key assigned (shown in blue).", (int)g_dlgMap.size());
	SetStatus(buf);
}

static void PaintKeyboard(HWND hWnd, HDC hdcWin)
{
	RECT client;
	GetClientRect(hWnd, &client);

	// Double-buffer to avoid flicker
	HDC hdc = CreateCompatibleDC(hdcWin);
	HBITMAP bmp = CreateCompatibleBitmap(hdcWin, client.right, client.bottom);
	HGDIOBJ oldBmp = SelectObject(hdc, bmp);

	HBRUSH bgBrush = CreateSolidBrush(RGB(196, 180, 150));		// beige Apple IIe case
	FillRect(hdc, &client, bgBrush);
	DeleteObject(bgBrush);

	HFONT dlgFont = (HFONT)SendMessage(GetParent(hWnd), WM_GETFONT, 0, 0);
	LOGFONTA lf;
	GetObjectA(dlgFont, sizeof(lf), &lf);
	lf.lfWeight = FW_BOLD;
	HFONT boldFont = CreateFontIndirectA(&lf);
	lf.lfWeight = FW_NORMAL;
	lf.lfHeight = lf.lfHeight * 4 / 5;
	HFONT smallFont = CreateFontIndirectA(&lf);
	lf.lfFaceName[0] = 0;
	strcpy_s(lf.lfFaceName, sizeof(lf.lfFaceName), "Marlett");	// arrow glyphs
	lf.lfCharSet = SYMBOL_CHARSET;
	lf.lfHeight = lf.lfHeight * 3 / 2;
	HFONT arrowFont = CreateFontIndirectA(&lf);

	SetBkMode(hdc, TRANSPARENT);
	HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 80, 70));
	HGDIOBJ oldPen = SelectObject(hdc, pen);

	for (int row = 0; row < kNumRows; row++)
	{
		for (int col = 0; col < g_rows[row].count; col++)
		{
			const AppleKey& key = g_rows[row].keys[col];
			RECT r;
			GetKeyRect(client, row, col, r);

			std::string bound = key.vk ? BoundKeysText(key.vk) : "";
			COLORREF face;
			if (&key == g_selectedKey)	face = RGB(255, 220, 80);		// waiting for PC key
			else if (!key.vk)			face = RGB(170, 160, 145);		// not remappable
			else if (!bound.empty())	face = RGB(150, 215, 140);		// has a binding
			else						face = RGB(240, 234, 220);

			HBRUSH b = CreateSolidBrush(face);
			HGDIOBJ oldBrush = SelectObject(hdc, b);
			RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
			SelectObject(hdc, oldBrush);
			DeleteObject(b);

			// Key label (top part)
			RECT tr = r;
			InflateRect(&tr, -2, -2);
			bool isArrow = (key.vk >= VK_LEFT && key.vk <= VK_DOWN);
			const char* label = key.label;
			if (isArrow)	// Marlett: 3=left 4=right 5=up 6=down
				label = key.vk == VK_LEFT ? "3" : key.vk == VK_RIGHT ? "4" : key.vk == VK_UP ? "5" : "6";

			SelectObject(hdc, isArrow ? arrowFont : boldFont);
			SetTextColor(hdc, key.vk ? RGB(40, 35, 30) : RGB(100, 95, 90));
			if (bound.empty())
				DrawTextA(hdc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_WORDBREAK | (strchr(label, ' ') ? 0 : DT_SINGLELINE));
			else
			{
				RECT top = tr; top.bottom = tr.top + (tr.bottom - tr.top) / 2;
				DrawTextA(hdc, label, -1, &top, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

				// Bound PC key(s) (bottom part, blue)
				RECT bot = tr; bot.top = top.bottom;
				SelectObject(hdc, smallFont);
				SetTextColor(hdc, RGB(0, 60, 200));
				DrawTextA(hdc, bound.c_str(), -1, &bot, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
			}
		}
	}

	SelectObject(hdc, oldPen);
	DeleteObject(pen);

	BitBlt(hdcWin, 0, 0, client.right, client.bottom, hdc, 0, 0, SRCCOPY);
	SelectObject(hdc, oldBmp);
	DeleteObject(bmp);
	DeleteDC(hdc);
	DeleteObject(boldFont);
	DeleteObject(smallFont);
	DeleteObject(arrowFont);
}

static void CancelSelection(HWND hWnd)
{
	g_selectedKey = NULL;
	InvalidateRect(hWnd, NULL, FALSE);
	ShowIdleStatus();
}

static void OnPCKeyPressed(HWND hWnd, WPARAM wparam, LPARAM lparam)
{
	BYTE vk = (BYTE)wparam;
	if (vk == VK_MENU)
		vk = (HIWORD(lparam) & KF_EXTENDED) ? VK_RMENU : VK_LMENU;

	if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_CAPITAL || vk == VK_LWIN || vk == VK_RWIN || vk == VK_NUMLOCK)
	{
		SetStatus("That PC key can't be used (Shift, Ctrl, Caps Lock, Win, Num Lock). Press another key, or click the yellow key to cancel.");
		return;
	}

	BYTE appleVK = g_selectedKey->vk;
	std::string apple = g_selectedKey->label[0] ? g_selectedKey->label : "SPACE";
	if (appleVK >= VK_LEFT && appleVK <= VK_DOWN)
		apple = KeyNameFromVK(appleVK);

	std::string msg;
	if (vk == appleVK)
	{
		g_dlgMap.erase(vk);		// a key mapped to itself = normal behaviour
		msg = "PC key " + KeyNameFromVK(vk) + " is back to normal.";
	}
	else
	{
		g_dlgMap[vk] = appleVK;	// replaces whatever this PC key did before
		msg = "PC key " + KeyNameFromVK(vk) + " now sends Apple " + apple + ".";
	}

	g_selectedKey = NULL;
	InvalidateRect(hWnd, NULL, FALSE);
	SetStatus(msg + " Click another key, or OK to save.");
}

static LRESULT CALLBACK KeyboardWndProc(HWND hWnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message)
	{
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		PaintKeyboard(hWnd, hdc);
		EndPaint(hWnd, &ps);
		return 0;
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_LBUTTONDOWN:
	{
		SetFocus(hWnd);
		const AppleKey* key = HitTest(hWnd, (short)LOWORD(lparam), (short)HIWORD(lparam));
		if (!key || key == g_selectedKey)
		{
			CancelSelection(hWnd);
		}
		else if (!key->vk)
		{
			g_selectedKey = NULL;
			InvalidateRect(hWnd, NULL, FALSE);
			SetStatus(std::string(key->label) + " can't be remapped.");
		}
		else
		{
			g_selectedKey = key;
			InvalidateRect(hWnd, NULL, FALSE);
			std::string name = key->label[0] ? key->label : "SPACE";
			if (key->vk >= VK_LEFT && key->vk <= VK_DOWN)
				name = KeyNameFromVK(key->vk);
			SetStatus("Now press the PC key that should send Apple " + name + "...  (click the yellow key again to cancel)");
		}
		return 0;
	}

	case WM_RBUTTONDOWN:	// clear all PC keys bound to this Apple key
	{
		const AppleKey* key = HitTest(hWnd, (short)LOWORD(lparam), (short)HIWORD(lparam));
		if (key && key->vk)
		{
			for (std::map<BYTE, BYTE>::iterator it = g_dlgMap.begin(); it != g_dlgMap.end(); )
			{
				if (it->second == key->vk) it = g_dlgMap.erase(it);
				else ++it;
			}
		}
		CancelSelection(hWnd);
		return 0;
	}

	case WM_GETDLGCODE:
		// While waiting for a key, take every key (Tab, Enter, Esc, arrows...) instead of the dialog
		if (g_selectedKey)
			return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTTAB | DLGC_WANTCHARS | DLGC_WANTMESSAGE;
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		if (g_selectedKey)
		{
			if (!(HIWORD(lparam) & KF_REPEAT))
				OnPCKeyPressed(hWnd, wparam, lparam);
			return 0;
		}
		break;

	case WM_SYSKEYUP:
	case WM_SYSCHAR:
	case WM_CHAR:
		if (g_selectedKey || message != WM_CHAR)
			return 0;	// swallow, so Alt doesn't beep / open the system menu
		break;

	case WM_KILLFOCUS:
		if (g_selectedKey)
			CancelSelection(hWnd);
		break;
	}

	return DefWindowProcA(hWnd, message, wparam, lparam);
}

static INT_PTR CALLBACK KeyRemapDlgProc(HWND hDlg, UINT message, WPARAM wparam, LPARAM /*lparam*/)
{
	switch (message)
	{
	case WM_INITDIALOG:
		g_hDlg = hDlg;
		g_selectedKey = NULL;
		CheckForChanges();		// pick up any hand edits to keymap.txt
		g_dlgMap.clear();
		for (int i = 0; i < 256; i++)
			if (g_keyMap[i])
				g_dlgMap[(BYTE)i] = g_keyMap[i];
		ShowIdleStatus();
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case IDC_KEYREMAP_CLEAR:
			g_dlgMap.clear();
			g_selectedKey = NULL;
			InvalidateRect(GetDlgItem(hDlg, IDC_KEYREMAP_KB), NULL, FALSE);
			ShowIdleStatus();
			return TRUE;

		case IDOK:
			if (!SaveKeymap(g_dlgMap))
			{
				std::string msg = "Could not save:\n" + g_keyMapPath;
				MessageBoxA(hDlg, msg.c_str(), "Key Remapping", MB_ICONEXCLAMATION | MB_OK);
				return TRUE;
			}
			EndDialog(hDlg, IDOK);
			return TRUE;

		case IDCANCEL:
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		}
		break;

	case WM_DESTROY:
		g_hDlg = NULL;
		g_selectedKey = NULL;
		break;
	}

	return FALSE;
}

void KeyRemap_ShowDialog(HINSTANCE hInstance, HWND hParent)
{
	static bool registered = false;
	if (!registered)
	{
		WNDCLASSA wc = {};
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = KeyboardWndProc;
		wc.hInstance = hInstance;
		wc.hCursor = LoadCursor(NULL, IDC_HAND);
		wc.lpszClassName = kKbdClass;
		registered = RegisterClassA(&wc) != 0;
	}

	DialogBoxA(hInstance, MAKEINTRESOURCEA(IDD_KEYREMAP), hParent, KeyRemapDlgProc);
}
