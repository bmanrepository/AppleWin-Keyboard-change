#pragma once

// Simple user key remapping.
// Reads "keymap.txt" from the AppleWin folder and rewrites keyboard
// messages before TranslateMessage(), so every part of the emulator
// (Apple keyboard, any-key-down, joystick keys) sees the remapped key.

void KeyRemap_Init();						// Load keymap.txt (call once at startup)
void KeyRemap_Apply(MSG& msg);				// Call just before TranslateMessage()
