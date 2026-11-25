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
