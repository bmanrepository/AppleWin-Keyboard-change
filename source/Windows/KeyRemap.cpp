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

static std::map<BYTE, BYTE> g_dlgMap;	// working copy while the dialog is open

static void FillKeyCombo(HWND hCombo)
{
	std::vector<BYTE> vks;
	for (BYTE c = 'A'; c <= 'Z'; c++) vks.push_back(c);
	for (BYTE c = '0'; c <= '9'; c++) vks.push_back(c);
	for (size_t i = 0; i < sizeof(g_keyNames) / sizeof(g_keyNames[0]); i++)
	{
		BYTE vk = g_keyNames[i].vk;
		if (std::find(vks.begin(), vks.end(), vk) == vks.end())	// skip alias names
			vks.push_back(vk);
	}

	for (size_t i = 0; i < vks.size(); i++)
	{
		LRESULT idx = SendMessageA(hCombo, CB_ADDSTRING, 0, (LPARAM)KeyNameFromVK(vks[i]).c_str());
		SendMessageA(hCombo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)vks[i]);
	}
	SendMessageA(hCombo, CB_SETCURSEL, 0, 0);
}

static void RefreshList(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_KEYREMAP_LIST);
	SendMessageA(hList, LB_RESETCONTENT, 0, 0);

	for (std::map<BYTE, BYTE>::const_iterator it = g_dlgMap.begin(); it != g_dlgMap.end(); ++it)
	{
		std::string text = KeyNameFromVK(it->first) + "   ->   " + KeyNameFromVK(it->second);
		LRESULT idx = SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)text.c_str());
		SendMessageA(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)it->first);
	}
}

static BYTE GetComboVK(HWND hDlg, int id)
{
	LRESULT sel = SendDlgItemMessageA(hDlg, id, CB_GETCURSEL, 0, 0);
	if (sel == CB_ERR)
		return 0;
	return (BYTE)SendDlgItemMessageA(hDlg, id, CB_GETITEMDATA, (WPARAM)sel, 0);
}

static INT_PTR CALLBACK KeyRemapDlgProc(HWND hDlg, UINT message, WPARAM wparam, LPARAM /*lparam*/)
{
	switch (message)
	{
	case WM_INITDIALOG:
		CheckForChanges();		// pick up any hand edits to keymap.txt
		g_dlgMap.clear();
		for (int i = 0; i < 256; i++)
			if (g_keyMap[i])
				g_dlgMap[(BYTE)i] = g_keyMap[i];
		FillKeyCombo(GetDlgItem(hDlg, IDC_KEYREMAP_FROM));
		FillKeyCombo(GetDlgItem(hDlg, IDC_KEYREMAP_TO));
		RefreshList(hDlg);
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wparam))
		{
		case IDC_KEYREMAP_ADD:
		{
			BYTE src = GetComboVK(hDlg, IDC_KEYREMAP_FROM);
			BYTE dst = GetComboVK(hDlg, IDC_KEYREMAP_TO);
			if (src && dst)
			{
				if (src == dst)
					g_dlgMap.erase(src);	// mapping a key to itself = no mapping
				else
					g_dlgMap[src] = dst;	// replaces any existing binding for this key
				RefreshList(hDlg);
			}
			return TRUE;
		}

		case IDC_KEYREMAP_REMOVE:
		{
			LRESULT sel = SendDlgItemMessageA(hDlg, IDC_KEYREMAP_LIST, LB_GETCURSEL, 0, 0);
			if (sel != LB_ERR)
			{
				BYTE src = (BYTE)SendDlgItemMessageA(hDlg, IDC_KEYREMAP_LIST, LB_GETITEMDATA, (WPARAM)sel, 0);
				g_dlgMap.erase(src);
				RefreshList(hDlg);
			}
			return TRUE;
		}

		case IDC_KEYREMAP_CLEAR:
			g_dlgMap.clear();
			RefreshList(hDlg);
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
	}

	return FALSE;
}

void KeyRemap_ShowDialog(HINSTANCE hInstance, HWND hParent)
{
	DialogBoxA(hInstance, MAKEINTRESOURCEA(IDD_KEYREMAP), hParent, KeyRemapDlgProc);
}
