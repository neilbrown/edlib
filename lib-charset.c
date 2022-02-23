/*
 * Copyright Neil Brown ©2017-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Filter a view on a document to convert 8-bit chars in various
 * charsets to the relevant unicode characters.
 *
 * Include tables transformed from
 *    https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WindowsBestFit/bestfit1251.txt
 *    https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WindowsBestFit/bestfit1252.txt
 *    https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-1.TXT
 *    https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-2.TXT
 *    https://www.unicode.org/Public/MAPPINGS/ISO8859/8859-15.TXT
 */

#include <unistd.h>
#include <stdlib.h>

#include "core.h"

static const wchar_t WIN1251_UNICODE_TABLE[] = {
	[0x00] = 0x0000,	// Null
	[0x01] = 0x0001,	// Start Of Heading
	[0x02] = 0x0002,	// Start Of Text
	[0x03] = 0x0003,	// End Of Text
	[0x04] = 0x0004,	// End Of Transmission
	[0x05] = 0x0005,	// Enquiry
	[0x06] = 0x0006,	// Acknowledge
	[0x07] = 0x0007,	// Bell
	[0x08] = 0x0008,	// Backspace
	[0x09] = 0x0009,	// Horizontal Tabulation
	[0x0a] = 0x000a,	// Line Feed
	[0x0b] = 0x000b,	// Vertical Tabulation
	[0x0c] = 0x000c,	// Form Feed
	[0x0d] = 0x000d,	// Carriage Return
	[0x0e] = 0x000e,	// Shift Out
	[0x0f] = 0x000f,	// Shift In
	[0x10] = 0x0010,	// Data Link Escape
	[0x11] = 0x0011,	// Device Control One
	[0x12] = 0x0012,	// Device Control Two
	[0x13] = 0x0013,	// Device Control Three
	[0x14] = 0x0014,	// Device Control Four
	[0x15] = 0x0015,	// Negative Acknowledge
	[0x16] = 0x0016,	// Synchronous Idle
	[0x17] = 0x0017,	// End Of Transmission Block
	[0x18] = 0x0018,	// Cancel
	[0x19] = 0x0019,	// End Of Medium
	[0x1a] = 0x001a,	// Substitute
	[0x1b] = 0x001b,	// Escape
	[0x1c] = 0x001c,	// File Separator
	[0x1d] = 0x001d,	// Group Separator
	[0x1e] = 0x001e,	// Record Separator
	[0x1f] = 0x001f,	// Unit Separator
	[0x20] = 0x0020,	// Space
	[0x21] = 0x0021,	// Exclamation Mark
	[0x22] = 0x0022,	// Quotation Mark
	[0x23] = 0x0023,	// Number Sign
	[0x24] = 0x0024,	// Dollar Sign
	[0x25] = 0x0025,	// Percent Sign
	[0x26] = 0x0026,	// Ampersand
	[0x27] = 0x0027,	// Apostrophe
	[0x28] = 0x0028,	// Left Parenthesis
	[0x29] = 0x0029,	// Right Parenthesis
	[0x2a] = 0x002a,	// Asterisk
	[0x2b] = 0x002b,	// Plus Sign
	[0x2c] = 0x002c,	// Comma
	[0x2d] = 0x002d,	// Hyphen-Minus
	[0x2e] = 0x002e,	// Full Stop
	[0x2f] = 0x002f,	// Solidus
	[0x30] = 0x0030,	// Digit Zero
	[0x31] = 0x0031,	// Digit One
	[0x32] = 0x0032,	// Digit Two
	[0x33] = 0x0033,	// Digit Three
	[0x34] = 0x0034,	// Digit Four
	[0x35] = 0x0035,	// Digit Five
	[0x36] = 0x0036,	// Digit Six
	[0x37] = 0x0037,	// Digit Seven
	[0x38] = 0x0038,	// Digit Eight
	[0x39] = 0x0039,	// Digit Nine
	[0x3a] = 0x003a,	// Colon
	[0x3b] = 0x003b,	// Semicolon
	[0x3c] = 0x003c,	// Less-Than Sign
	[0x3d] = 0x003d,	// Equals Sign
	[0x3e] = 0x003e,	// Greater-Than Sign
	[0x3f] = 0x003f,	// Question Mark
	[0x40] = 0x0040,	// Commercial At
	[0x41] = 0x0041,	// Latin Capital Letter A
	[0x42] = 0x0042,	// Latin Capital Letter B
	[0x43] = 0x0043,	// Latin Capital Letter C
	[0x44] = 0x0044,	// Latin Capital Letter D
	[0x45] = 0x0045,	// Latin Capital Letter E
	[0x46] = 0x0046,	// Latin Capital Letter F
	[0x47] = 0x0047,	// Latin Capital Letter G
	[0x48] = 0x0048,	// Latin Capital Letter H
	[0x49] = 0x0049,	// Latin Capital Letter I
	[0x4a] = 0x004a,	// Latin Capital Letter J
	[0x4b] = 0x004b,	// Latin Capital Letter K
	[0x4c] = 0x004c,	// Latin Capital Letter L
	[0x4d] = 0x004d,	// Latin Capital Letter M
	[0x4e] = 0x004e,	// Latin Capital Letter N
	[0x4f] = 0x004f,	// Latin Capital Letter O
	[0x50] = 0x0050,	// Latin Capital Letter P
	[0x51] = 0x0051,	// Latin Capital Letter Q
	[0x52] = 0x0052,	// Latin Capital Letter R
	[0x53] = 0x0053,	// Latin Capital Letter S
	[0x54] = 0x0054,	// Latin Capital Letter T
	[0x55] = 0x0055,	// Latin Capital Letter U
	[0x56] = 0x0056,	// Latin Capital Letter V
	[0x57] = 0x0057,	// Latin Capital Letter W
	[0x58] = 0x0058,	// Latin Capital Letter X
	[0x59] = 0x0059,	// Latin Capital Letter Y
	[0x5a] = 0x005a,	// Latin Capital Letter Z
	[0x5b] = 0x005b,	// Left Square Bracket
	[0x5c] = 0x005c,	// Reverse Solidus
	[0x5d] = 0x005d,	// Right Square Bracket
	[0x5e] = 0x005e,	// Circumflex Accent
	[0x5f] = 0x005f,	// Low Line
	[0x60] = 0x0060,	// Grave Accent
	[0x61] = 0x0061,	// Latin Small Letter A
	[0x62] = 0x0062,	// Latin Small Letter B
	[0x63] = 0x0063,	// Latin Small Letter C
	[0x64] = 0x0064,	// Latin Small Letter D
	[0x65] = 0x0065,	// Latin Small Letter E
	[0x66] = 0x0066,	// Latin Small Letter F
	[0x67] = 0x0067,	// Latin Small Letter G
	[0x68] = 0x0068,	// Latin Small Letter H
	[0x69] = 0x0069,	// Latin Small Letter I
	[0x6a] = 0x006a,	// Latin Small Letter J
	[0x6b] = 0x006b,	// Latin Small Letter K
	[0x6c] = 0x006c,	// Latin Small Letter L
	[0x6d] = 0x006d,	// Latin Small Letter M
	[0x6e] = 0x006e,	// Latin Small Letter N
	[0x6f] = 0x006f,	// Latin Small Letter O
	[0x70] = 0x0070,	// Latin Small Letter P
	[0x71] = 0x0071,	// Latin Small Letter Q
	[0x72] = 0x0072,	// Latin Small Letter R
	[0x73] = 0x0073,	// Latin Small Letter S
	[0x74] = 0x0074,	// Latin Small Letter T
	[0x75] = 0x0075,	// Latin Small Letter U
	[0x76] = 0x0076,	// Latin Small Letter V
	[0x77] = 0x0077,	// Latin Small Letter W
	[0x78] = 0x0078,	// Latin Small Letter X
	[0x79] = 0x0079,	// Latin Small Letter Y
	[0x7a] = 0x007a,	// Latin Small Letter Z
	[0x7b] = 0x007b,	// Left Curly Bracket
	[0x7c] = 0x007c,	// Vertical Line
	[0x7d] = 0x007d,	// Right Curly Bracket
	[0x7e] = 0x007e,	// Tilde
	[0x7f] = 0x007f,	// Delete
	[0x80] = 0x0402,	// Cyrillic Capital Letter Dje
	[0x81] = 0x0403,	// Cyrillic Capital Letter Gje
	[0x82] = 0x201a,	// Single Low-9 Quotation Mark
	[0x83] = 0x0453,	// Cyrillic Small Letter Gje
	[0x84] = 0x201e,	// Double Low-9 Quotation Mark
	[0x85] = 0x2026,	// Horizontal Ellipsis
	[0x86] = 0x2020,	// Dagger
	[0x87] = 0x2021,	// Double Dagger
	[0x88] = 0x20ac,	// Euro Sign
	[0x89] = 0x2030,	// Per Mille Sign
	[0x8a] = 0x0409,	// Cyrillic Capital Letter Lje
	[0x8b] = 0x2039,	// Single Left-Pointing Angle Quotation Mark
	[0x8c] = 0x040a,	// Cyrillic Capital Letter Nje
	[0x8d] = 0x040c,	// Cyrillic Capital Letter Kje
	[0x8e] = 0x040b,	// Cyrillic Capital Letter Tshe
	[0x8f] = 0x040f,	// Cyrillic Capital Letter Dzhe
	[0x90] = 0x0452,	// Cyrillic Small Letter Dje
	[0x91] = 0x2018,	// Left Single Quotation Mark
	[0x92] = 0x2019,	// Right Single Quotation Mark
	[0x93] = 0x201c,	// Left Double Quotation Mark
	[0x94] = 0x201d,	// Right Double Quotation Mark
	[0x95] = 0x2022,	// Bullet
	[0x96] = 0x2013,	// En Dash
	[0x97] = 0x2014,	// Em Dash
	[0x98] = 0x0098,	// ??
	[0x99] = 0x2122,	// Trade Mark Sign
	[0x9a] = 0x0459,	// Cyrillic Small Letter Lje
	[0x9b] = 0x203a,	// Single Right-Pointing Angle Quotation Mark
	[0x9c] = 0x045a,	// Cyrillic Small Letter Nje
	[0x9d] = 0x045c,	// Cyrillic Small Letter Kje
	[0x9e] = 0x045b,	// Cyrillic Small Letter Tshe
	[0x9f] = 0x045f,	// Cyrillic Small Letter Dzhe
	[0xa0] = 0x00a0,	// No-Break Space
	[0xa1] = 0x040e,	// Cyrillic Capital Letter Short U
	[0xa2] = 0x045e,	// Cyrillic Small Letter Short U
	[0xa3] = 0x0408,	// Cyrillic Capital Letter Je
	[0xa4] = 0x00a4,	// Currency Sign
	[0xa5] = 0x0490,	// Cyrillic Capital Letter Ghe With Upturn
	[0xa6] = 0x00a6,	// Broken Bar
	[0xa7] = 0x00a7,	// Section Sign
	[0xa8] = 0x0401,	// Cyrillic Capital Letter Io
	[0xa9] = 0x00a9,	// Copyright Sign
	[0xaa] = 0x0404,	// Cyrillic Capital Letter Ukrainian Ie
	[0xab] = 0x00ab,	// Left-Pointing Double Angle Quotation Mark
	[0xac] = 0x00ac,	// Not Sign
	[0xad] = 0x00ad,	// Soft Hyphen
	[0xae] = 0x00ae,	// Registered Sign
	[0xaf] = 0x0407,	// Cyrillic Capital Letter Yi
	[0xb0] = 0x00b0,	// Degree Sign
	[0xb1] = 0x00b1,	// Plus-Minus Sign
	[0xb2] = 0x0406,	// Cyrillic Capital Letter Byelorussian-Ukrainian I
	[0xb3] = 0x0456,	// Cyrillic Small Letter Byelorussian-Ukrainian I
	[0xb4] = 0x0491,	// Cyrillic Small Letter Ghe With Upturn
	[0xb5] = 0x00b5,	// Micro Sign
	[0xb6] = 0x00b6,	// Pilcrow Sign
	[0xb7] = 0x00b7,	// Middle Dot
	[0xb8] = 0x0451,	// Cyrillic Small Letter Io
	[0xb9] = 0x2116,	// Numero Sign
	[0xba] = 0x0454,	// Cyrillic Small Letter Ukrainian Ie
	[0xbb] = 0x00bb,	// Right-Pointing Double Angle Quotation Mark
	[0xbc] = 0x0458,	// Cyrillic Small Letter Je
	[0xbd] = 0x0405,	// Cyrillic Capital Letter Dze
	[0xbe] = 0x0455,	// Cyrillic Small Letter Dze
	[0xbf] = 0x0457,	// Cyrillic Small Letter Yi
	[0xc0] = 0x0410,	// Cyrillic Capital Letter A
	[0xc1] = 0x0411,	// Cyrillic Capital Letter Be
	[0xc2] = 0x0412,	// Cyrillic Capital Letter Ve
	[0xc3] = 0x0413,	// Cyrillic Capital Letter Ghe
	[0xc4] = 0x0414,	// Cyrillic Capital Letter De
	[0xc5] = 0x0415,	// Cyrillic Capital Letter Ie
	[0xc6] = 0x0416,	// Cyrillic Capital Letter Zhe
	[0xc7] = 0x0417,	// Cyrillic Capital Letter Ze
	[0xc8] = 0x0418,	// Cyrillic Capital Letter I
	[0xc9] = 0x0419,	// Cyrillic Capital Letter Short I
	[0xca] = 0x041a,	// Cyrillic Capital Letter Ka
	[0xcb] = 0x041b,	// Cyrillic Capital Letter El
	[0xcc] = 0x041c,	// Cyrillic Capital Letter Em
	[0xcd] = 0x041d,	// Cyrillic Capital Letter En
	[0xce] = 0x041e,	// Cyrillic Capital Letter O
	[0xcf] = 0x041f,	// Cyrillic Capital Letter Pe
	[0xd0] = 0x0420,	// Cyrillic Capital Letter Er
	[0xd1] = 0x0421,	// Cyrillic Capital Letter Es
	[0xd2] = 0x0422,	// Cyrillic Capital Letter Te
	[0xd3] = 0x0423,	// Cyrillic Capital Letter U
	[0xd4] = 0x0424,	// Cyrillic Capital Letter Ef
	[0xd5] = 0x0425,	// Cyrillic Capital Letter Ha
	[0xd6] = 0x0426,	// Cyrillic Capital Letter Tse
	[0xd7] = 0x0427,	// Cyrillic Capital Letter Che
	[0xd8] = 0x0428,	// Cyrillic Capital Letter Sha
	[0xd9] = 0x0429,	// Cyrillic Capital Letter Shcha
	[0xda] = 0x042a,	// Cyrillic Capital Letter Hard Sign
	[0xdb] = 0x042b,	// Cyrillic Capital Letter Yeru
	[0xdc] = 0x042c,	// Cyrillic Capital Letter Soft Sign
	[0xdd] = 0x042d,	// Cyrillic Capital Letter E
	[0xde] = 0x042e,	// Cyrillic Capital Letter Yu
	[0xdf] = 0x042f,	// Cyrillic Capital Letter Ya
	[0xe0] = 0x0430,	// Cyrillic Small Letter A
	[0xe1] = 0x0431,	// Cyrillic Small Letter Be
	[0xe2] = 0x0432,	// Cyrillic Small Letter Ve
	[0xe3] = 0x0433,	// Cyrillic Small Letter Ghe
	[0xe4] = 0x0434,	// Cyrillic Small Letter De
	[0xe5] = 0x0435,	// Cyrillic Small Letter Ie
	[0xe6] = 0x0436,	// Cyrillic Small Letter Zhe
	[0xe7] = 0x0437,	// Cyrillic Small Letter Ze
	[0xe8] = 0x0438,	// Cyrillic Small Letter I
	[0xe9] = 0x0439,	// Cyrillic Small Letter Short I
	[0xea] = 0x043a,	// Cyrillic Small Letter Ka
	[0xeb] = 0x043b,	// Cyrillic Small Letter El
	[0xec] = 0x043c,	// Cyrillic Small Letter Em
	[0xed] = 0x043d,	// Cyrillic Small Letter En
	[0xee] = 0x043e,	// Cyrillic Small Letter O
	[0xef] = 0x043f,	// Cyrillic Small Letter Pe
	[0xf0] = 0x0440,	// Cyrillic Small Letter Er
	[0xf1] = 0x0441,	// Cyrillic Small Letter Es
	[0xf2] = 0x0442,	// Cyrillic Small Letter Te
	[0xf3] = 0x0443,	// Cyrillic Small Letter U
	[0xf4] = 0x0444,	// Cyrillic Small Letter Ef
	[0xf5] = 0x0445,	// Cyrillic Small Letter Ha
	[0xf6] = 0x0446,	// Cyrillic Small Letter Tse
	[0xf7] = 0x0447,	// Cyrillic Small Letter Che
	[0xf8] = 0x0448,	// Cyrillic Small Letter Sha
	[0xf9] = 0x0449,	// Cyrillic Small Letter Shcha
	[0xfa] = 0x044a,	// Cyrillic Small Letter Hard Sign
	[0xfb] = 0x044b,	// Cyrillic Small Letter Yeru
	[0xfc] = 0x044c,	// Cyrillic Small Letter Soft Sign
	[0xfd] = 0x044d,	// Cyrillic Small Letter E
	[0xfe] = 0x044e,	// Cyrillic Small Letter Yu
	[0xff] = 0x044f,	// Cyrillic Small Letter Ya
};

static const wchar_t WIN1252_UNICODE_TABLE[] = {
	[0x00] = 0x0000,	// Null
	[0x01] = 0x0001,	// Start Of Heading
	[0x02] = 0x0002,	// Start Of Text
	[0x03] = 0x0003,	// End Of Text
	[0x04] = 0x0004,	// End Of Transmission
	[0x05] = 0x0005,	// Enquiry
	[0x06] = 0x0006,	// Acknowledge
	[0x07] = 0x0007,	// Bell
	[0x08] = 0x0008,	// Backspace
	[0x09] = 0x0009,	// Horizontal Tabulation
	[0x0a] = 0x000a,	// Line Feed
	[0x0b] = 0x000b,	// Vertical Tabulation
	[0x0c] = 0x000c,	// Form Feed
	[0x0d] = 0x000d,	// Carriage Return
	[0x0e] = 0x000e,	// Shift Out
	[0x0f] = 0x000f,	// Shift In
	[0x10] = 0x0010,	// Data Link Escape
	[0x11] = 0x0011,	// Device Control One
	[0x12] = 0x0012,	// Device Control Two
	[0x13] = 0x0013,	// Device Control Three
	[0x14] = 0x0014,	// Device Control Four
	[0x15] = 0x0015,	// Negative Acknowledge
	[0x16] = 0x0016,	// Synchronous Idle
	[0x17] = 0x0017,	// End Of Transmission Block
	[0x18] = 0x0018,	// Cancel
	[0x19] = 0x0019,	// End Of Medium
	[0x1a] = 0x001a,	// Substitute
	[0x1b] = 0x001b,	// Escape
	[0x1c] = 0x001c,	// File Separator
	[0x1d] = 0x001d,	// Group Separator
	[0x1e] = 0x001e,	// Record Separator
	[0x1f] = 0x001f,	// Unit Separator
	[0x20] = 0x0020,	// Space
	[0x21] = 0x0021,	// Exclamation Mark
	[0x22] = 0x0022,	// Quotation Mark
	[0x23] = 0x0023,	// Number Sign
	[0x24] = 0x0024,	// Dollar Sign
	[0x25] = 0x0025,	// Percent Sign
	[0x26] = 0x0026,	// Ampersand
	[0x27] = 0x0027,	// Apostrophe
	[0x28] = 0x0028,	// Left Parenthesis
	[0x29] = 0x0029,	// Right Parenthesis
	[0x2a] = 0x002a,	// Asterisk
	[0x2b] = 0x002b,	// Plus Sign
	[0x2c] = 0x002c,	// Comma
	[0x2d] = 0x002d,	// Hyphen-Minus
	[0x2e] = 0x002e,	// Full Stop
	[0x2f] = 0x002f,	// Solidus
	[0x30] = 0x0030,	// Digit Zero
	[0x31] = 0x0031,	// Digit One
	[0x32] = 0x0032,	// Digit Two
	[0x33] = 0x0033,	// Digit Three
	[0x34] = 0x0034,	// Digit Four
	[0x35] = 0x0035,	// Digit Five
	[0x36] = 0x0036,	// Digit Six
	[0x37] = 0x0037,	// Digit Seven
	[0x38] = 0x0038,	// Digit Eight
	[0x39] = 0x0039,	// Digit Nine
	[0x3a] = 0x003a,	// Colon
	[0x3b] = 0x003b,	// Semicolon
	[0x3c] = 0x003c,	// Less-Than Sign
	[0x3d] = 0x003d,	// Equals Sign
	[0x3e] = 0x003e,	// Greater-Than Sign
	[0x3f] = 0x003f,	// Question Mark
	[0x40] = 0x0040,	// Commercial At
	[0x41] = 0x0041,	// Latin Capital Letter A
	[0x42] = 0x0042,	// Latin Capital Letter B
	[0x43] = 0x0043,	// Latin Capital Letter C
	[0x44] = 0x0044,	// Latin Capital Letter D
	[0x45] = 0x0045,	// Latin Capital Letter E
	[0x46] = 0x0046,	// Latin Capital Letter F
	[0x47] = 0x0047,	// Latin Capital Letter G
	[0x48] = 0x0048,	// Latin Capital Letter H
	[0x49] = 0x0049,	// Latin Capital Letter I
	[0x4a] = 0x004a,	// Latin Capital Letter J
	[0x4b] = 0x004b,	// Latin Capital Letter K
	[0x4c] = 0x004c,	// Latin Capital Letter L
	[0x4d] = 0x004d,	// Latin Capital Letter M
	[0x4e] = 0x004e,	// Latin Capital Letter N
	[0x4f] = 0x004f,	// Latin Capital Letter O
	[0x50] = 0x0050,	// Latin Capital Letter P
	[0x51] = 0x0051,	// Latin Capital Letter Q
	[0x52] = 0x0052,	// Latin Capital Letter R
	[0x53] = 0x0053,	// Latin Capital Letter S
	[0x54] = 0x0054,	// Latin Capital Letter T
	[0x55] = 0x0055,	// Latin Capital Letter U
	[0x56] = 0x0056,	// Latin Capital Letter V
	[0x57] = 0x0057,	// Latin Capital Letter W
	[0x58] = 0x0058,	// Latin Capital Letter X
	[0x59] = 0x0059,	// Latin Capital Letter Y
	[0x5a] = 0x005a,	// Latin Capital Letter Z
	[0x5b] = 0x005b,	// Left Square Bracket
	[0x5c] = 0x005c,	// Reverse Solidus
	[0x5d] = 0x005d,	// Right Square Bracket
	[0x5e] = 0x005e,	// Circumflex Accent
	[0x5f] = 0x005f,	// Low Line
	[0x60] = 0x0060,	// Grave Accent
	[0x61] = 0x0061,	// Latin Small Letter A
	[0x62] = 0x0062,	// Latin Small Letter B
	[0x63] = 0x0063,	// Latin Small Letter C
	[0x64] = 0x0064,	// Latin Small Letter D
	[0x65] = 0x0065,	// Latin Small Letter E
	[0x66] = 0x0066,	// Latin Small Letter F
	[0x67] = 0x0067,	// Latin Small Letter G
	[0x68] = 0x0068,	// Latin Small Letter H
	[0x69] = 0x0069,	// Latin Small Letter I
	[0x6a] = 0x006a,	// Latin Small Letter J
	[0x6b] = 0x006b,	// Latin Small Letter K
	[0x6c] = 0x006c,	// Latin Small Letter L
	[0x6d] = 0x006d,	// Latin Small Letter M
	[0x6e] = 0x006e,	// Latin Small Letter N
	[0x6f] = 0x006f,	// Latin Small Letter O
	[0x70] = 0x0070,	// Latin Small Letter P
	[0x71] = 0x0071,	// Latin Small Letter Q
	[0x72] = 0x0072,	// Latin Small Letter R
	[0x73] = 0x0073,	// Latin Small Letter S
	[0x74] = 0x0074,	// Latin Small Letter T
	[0x75] = 0x0075,	// Latin Small Letter U
	[0x76] = 0x0076,	// Latin Small Letter V
	[0x77] = 0x0077,	// Latin Small Letter W
	[0x78] = 0x0078,	// Latin Small Letter X
	[0x79] = 0x0079,	// Latin Small Letter Y
	[0x7a] = 0x007a,	// Latin Small Letter Z
	[0x7b] = 0x007b,	// Left Curly Bracket
	[0x7c] = 0x007c,	// Vertical Line
	[0x7d] = 0x007d,	// Right Curly Bracket
	[0x7e] = 0x007e,	// Tilde
	[0x7f] = 0x007f,	// Delete
	[0x80] = 0x20ac,	// Euro Sign
	[0x81] = 0x0081,	// ??
	[0x82] = 0x201a,	// Single Low-9 Quotation Mark
	[0x83] = 0x0192,	// Latin Small Letter F With Hook
	[0x84] = 0x201e,	// Double Low-9 Quotation Mark
	[0x85] = 0x2026,	// Horizontal Ellipsis
	[0x86] = 0x2020,	// Dagger
	[0x87] = 0x2021,	// Double Dagger
	[0x88] = 0x02c6,	// Modifier Letter Circumflex Accent
	[0x89] = 0x2030,	// Per Mille Sign
	[0x8a] = 0x0160,	// Latin Capital Letter S With Caron
	[0x8b] = 0x2039,	// Single Left-Pointing Angle Quotation Mark
	[0x8c] = 0x0152,	// Latin Capital Ligature Oe
	[0x8d] = 0x008d,	// ??
	[0x8e] = 0x017d,	// Latin Capital Letter Z With Caron
	[0x8f] = 0x008f,	// ??
	[0x90] = 0x0090,	// ??
	[0x91] = 0x2018,	// Left Single Quotation Mark
	[0x92] = 0x2019,	// Right Single Quotation Mark
	[0x93] = 0x201c,	// Left Double Quotation Mark
	[0x94] = 0x201d,	// Right Double Quotation Mark
	[0x95] = 0x2022,	// Bullet
	[0x96] = 0x2013,	// En Dash
	[0x97] = 0x2014,	// Em Dash
	[0x98] = 0x02dc,	// Small Tilde
	[0x99] = 0x2122,	// Trade Mark Sign
	[0x9a] = 0x0161,	// Latin Small Letter S With Caron
	[0x9b] = 0x203a,	// Single Right-Pointing Angle Quotation Mark
	[0x9c] = 0x0153,	// Latin Small Ligature Oe
	[0x9d] = 0x009d,	//  ??
	[0x9e] = 0x017e,	// Latin Small Letter Z With Caron
	[0x9f] = 0x0178,	// Latin Capital Letter Y With Diaeresis
	[0xa0] = 0x00a0,	// No-Break Space
	[0xa1] = 0x00a1,	// Inverted Exclamation Mark
	[0xa2] = 0x00a2,	// Cent Sign
	[0xa3] = 0x00a3,	// Pound Sign
	[0xa4] = 0x00a4,	// Currency Sign
	[0xa5] = 0x00a5,	// Yen Sign
	[0xa6] = 0x00a6,	// Broken Bar
	[0xa7] = 0x00a7,	// Section Sign
	[0xa8] = 0x00a8,	// Diaeresis
	[0xa9] = 0x00a9,	// Copyright Sign
	[0xaa] = 0x00aa,	// Feminine Ordinal Indicator
	[0xab] = 0x00ab,	// Left-Pointing Double Angle Quotation Mark
	[0xac] = 0x00ac,	// Not Sign
	[0xad] = 0x00ad,	// Soft Hyphen
	[0xae] = 0x00ae,	// Registered Sign
	[0xaf] = 0x00af,	// Macron
	[0xb0] = 0x00b0,	// Degree Sign
	[0xb1] = 0x00b1,	// Plus-Minus Sign
	[0xb2] = 0x00b2,	// Superscript Two
	[0xb3] = 0x00b3,	// Superscript Three
	[0xb4] = 0x00b4,	// Acute Accent
	[0xb5] = 0x00b5,	// Micro Sign
	[0xb6] = 0x00b6,	// Pilcrow Sign
	[0xb7] = 0x00b7,	// Middle Dot
	[0xb8] = 0x00b8,	// Cedilla
	[0xb9] = 0x00b9,	// Superscript One
	[0xba] = 0x00ba,	// Masculine Ordinal Indicator
	[0xbb] = 0x00bb,	// Right-Pointing Double Angle Quotation Mark
	[0xbc] = 0x00bc,	// Vulgar Fraction One Quarter
	[0xbd] = 0x00bd,	// Vulgar Fraction One Half
	[0xbe] = 0x00be,	// Vulgar Fraction Three Quarters
	[0xbf] = 0x00bf,	// Inverted Question Mark
	[0xc0] = 0x00c0,	// Latin Capital Letter A With Grave
	[0xc1] = 0x00c1,	// Latin Capital Letter A With Acute
	[0xc2] = 0x00c2,	// Latin Capital Letter A With Circumflex
	[0xc3] = 0x00c3,	// Latin Capital Letter A With Tilde
	[0xc4] = 0x00c4,	// Latin Capital Letter A With Diaeresis
	[0xc5] = 0x00c5,	// Latin Capital Letter A With Ring Above
	[0xc6] = 0x00c6,	// Latin Capital Ligature Ae
	[0xc7] = 0x00c7,	// Latin Capital Letter C With Cedilla
	[0xc8] = 0x00c8,	// Latin Capital Letter E With Grave
	[0xc9] = 0x00c9,	// Latin Capital Letter E With Acute
	[0xca] = 0x00ca,	// Latin Capital Letter E With Circumflex
	[0xcb] = 0x00cb,	// Latin Capital Letter E With Diaeresis
	[0xcc] = 0x00cc,	// Latin Capital Letter I With Grave
	[0xcd] = 0x00cd,	// Latin Capital Letter I With Acute
	[0xce] = 0x00ce,	// Latin Capital Letter I With Circumflex
	[0xcf] = 0x00cf,	// Latin Capital Letter I With Diaeresis
	[0xd0] = 0x00d0,	// Latin Capital Letter Eth
	[0xd1] = 0x00d1,	// Latin Capital Letter N With Tilde
	[0xd2] = 0x00d2,	// Latin Capital Letter O With Grave
	[0xd3] = 0x00d3,	// Latin Capital Letter O With Acute
	[0xd4] = 0x00d4,	// Latin Capital Letter O With Circumflex
	[0xd5] = 0x00d5,	// Latin Capital Letter O With Tilde
	[0xd6] = 0x00d6,	// Latin Capital Letter O With Diaeresis
	[0xd7] = 0x00d7,	// Multiplication Sign
	[0xd8] = 0x00d8,	// Latin Capital Letter O With Stroke
	[0xd9] = 0x00d9,	// Latin Capital Letter U With Grave
	[0xda] = 0x00da,	// Latin Capital Letter U With Acute
	[0xdb] = 0x00db,	// Latin Capital Letter U With Circumflex
	[0xdc] = 0x00dc,	// Latin Capital Letter U With Diaeresis
	[0xdd] = 0x00dd,	// Latin Capital Letter Y With Acute
	[0xde] = 0x00de,	// Latin Capital Letter Thorn
	[0xdf] = 0x00df,	// Latin Small Letter Sharp S
	[0xe0] = 0x00e0,	// Latin Small Letter A With Grave
	[0xe1] = 0x00e1,	// Latin Small Letter A With Acute
	[0xe2] = 0x00e2,	// Latin Small Letter A With Circumflex
	[0xe3] = 0x00e3,	// Latin Small Letter A With Tilde
	[0xe4] = 0x00e4,	// Latin Small Letter A With Diaeresis
	[0xe5] = 0x00e5,	// Latin Small Letter A With Ring Above
	[0xe6] = 0x00e6,	// Latin Small Ligature Ae
	[0xe7] = 0x00e7,	// Latin Small Letter C With Cedilla
	[0xe8] = 0x00e8,	// Latin Small Letter E With Grave
	[0xe9] = 0x00e9,	// Latin Small Letter E With Acute
	[0xea] = 0x00ea,	// Latin Small Letter E With Circumflex
	[0xeb] = 0x00eb,	// Latin Small Letter E With Diaeresis
	[0xec] = 0x00ec,	// Latin Small Letter I With Grave
	[0xed] = 0x00ed,	// Latin Small Letter I With Acute
	[0xee] = 0x00ee,	// Latin Small Letter I With Circumflex
	[0xef] = 0x00ef,	// Latin Small Letter I With Diaeresis
	[0xf0] = 0x00f0,	// Latin Small Letter Eth
	[0xf1] = 0x00f1,	// Latin Small Letter N With Tilde
	[0xf2] = 0x00f2,	// Latin Small Letter O With Grave
	[0xf3] = 0x00f3,	// Latin Small Letter O With Acute
	[0xf4] = 0x00f4,	// Latin Small Letter O With Circumflex
	[0xf5] = 0x00f5,	// Latin Small Letter O With Tilde
	[0xf6] = 0x00f6,	// Latin Small Letter O With Diaeresis
	[0xf7] = 0x00f7,	// Division Sign
	[0xf8] = 0x00f8,	// Latin Small Letter O With Stroke
	[0xf9] = 0x00f9,	// Latin Small Letter U With Grave
	[0xfa] = 0x00fa,	// Latin Small Letter U With Acute
	[0xfb] = 0x00fb,	// Latin Small Letter U With Circumflex
	[0xfc] = 0x00fc,	// Latin Small Letter U With Diaeresis
	[0xfd] = 0x00fd,	// Latin Small Letter Y With Acute
	[0xfe] = 0x00fe,	// Latin Small Letter Thorn
	[0xff] = 0x00ff,	// Latin Small Letter Y With Diaeresis
};

static const wchar_t ISO_8859_1_UNICODE_TABLE[] = {
	[0x00] = 0x0000,	// NULL
	[0x01] = 0x0001,	// START OF HEADING
	[0x02] = 0x0002,	// START OF TEXT
	[0x03] = 0x0003,	// END OF TEXT
	[0x04] = 0x0004,	// END OF TRANSMISSION
	[0x05] = 0x0005,	// ENQUIRY
	[0x06] = 0x0006,	// ACKNOWLEDGE
	[0x07] = 0x0007,	// BELL
	[0x08] = 0x0008,	// BACKSPACE
	[0x09] = 0x0009,	// HORIZONTAL TABULATION
	[0x0A] = 0x000A,	// LINE FEED
	[0x0B] = 0x000B,	// VERTICAL TABULATION
	[0x0C] = 0x000C,	// FORM FEED
	[0x0D] = 0x000D,	// CARRIAGE RETURN
	[0x0E] = 0x000E,	// SHIFT OUT
	[0x0F] = 0x000F,	// SHIFT IN
	[0x10] = 0x0010,	// DATA LINK ESCAPE
	[0x11] = 0x0011,	// DEVICE CONTROL ONE
	[0x12] = 0x0012,	// DEVICE CONTROL TWO
	[0x13] = 0x0013,	// DEVICE CONTROL THREE
	[0x14] = 0x0014,	// DEVICE CONTROL FOUR
	[0x15] = 0x0015,	// NEGATIVE ACKNOWLEDGE
	[0x16] = 0x0016,	// SYNCHRONOUS IDLE
	[0x17] = 0x0017,	// END OF TRANSMISSION BLOCK
	[0x18] = 0x0018,	// CANCEL
	[0x19] = 0x0019,	// END OF MEDIUM
	[0x1A] = 0x001A,	// SUBSTITUTE
	[0x1B] = 0x001B,	// ESCAPE
	[0x1C] = 0x001C,	// FILE SEPARATOR
	[0x1D] = 0x001D,	// GROUP SEPARATOR
	[0x1E] = 0x001E,	// RECORD SEPARATOR
	[0x1F] = 0x001F,	// UNIT SEPARATOR
	[0x20] = 0x0020,	// SPACE
	[0x21] = 0x0021,	// EXCLAMATION MARK
	[0x22] = 0x0022,	// QUOTATION MARK
	[0x23] = 0x0023,	// NUMBER SIGN
	[0x24] = 0x0024,	// DOLLAR SIGN
	[0x25] = 0x0025,	// PERCENT SIGN
	[0x26] = 0x0026,	// AMPERSAND
	[0x27] = 0x0027,	// APOSTROPHE
	[0x28] = 0x0028,	// LEFT PARENTHESIS
	[0x29] = 0x0029,	// RIGHT PARENTHESIS
	[0x2A] = 0x002A,	// ASTERISK
	[0x2B] = 0x002B,	// PLUS SIGN
	[0x2C] = 0x002C,	// COMMA
	[0x2D] = 0x002D,	// HYPHEN-MINUS
	[0x2E] = 0x002E,	// FULL STOP
	[0x2F] = 0x002F,	// SOLIDUS
	[0x30] = 0x0030,	// DIGIT ZERO
	[0x31] = 0x0031,	// DIGIT ONE
	[0x32] = 0x0032,	// DIGIT TWO
	[0x33] = 0x0033,	// DIGIT THREE
	[0x34] = 0x0034,	// DIGIT FOUR
	[0x35] = 0x0035,	// DIGIT FIVE
	[0x36] = 0x0036,	// DIGIT SIX
	[0x37] = 0x0037,	// DIGIT SEVEN
	[0x38] = 0x0038,	// DIGIT EIGHT
	[0x39] = 0x0039,	// DIGIT NINE
	[0x3A] = 0x003A,	// COLON
	[0x3B] = 0x003B,	// SEMICOLON
	[0x3C] = 0x003C,	// LESS-THAN SIGN
	[0x3D] = 0x003D,	// EQUALS SIGN
	[0x3E] = 0x003E,	// GREATER-THAN SIGN
	[0x3F] = 0x003F,	// QUESTION MARK
	[0x40] = 0x0040,	// COMMERCIAL AT
	[0x41] = 0x0041,	// LATIN CAPITAL LETTER A
	[0x42] = 0x0042,	// LATIN CAPITAL LETTER B
	[0x43] = 0x0043,	// LATIN CAPITAL LETTER C
	[0x44] = 0x0044,	// LATIN CAPITAL LETTER D
	[0x45] = 0x0045,	// LATIN CAPITAL LETTER E
	[0x46] = 0x0046,	// LATIN CAPITAL LETTER F
	[0x47] = 0x0047,	// LATIN CAPITAL LETTER G
	[0x48] = 0x0048,	// LATIN CAPITAL LETTER H
	[0x49] = 0x0049,	// LATIN CAPITAL LETTER I
	[0x4A] = 0x004A,	// LATIN CAPITAL LETTER J
	[0x4B] = 0x004B,	// LATIN CAPITAL LETTER K
	[0x4C] = 0x004C,	// LATIN CAPITAL LETTER L
	[0x4D] = 0x004D,	// LATIN CAPITAL LETTER M
	[0x4E] = 0x004E,	// LATIN CAPITAL LETTER N
	[0x4F] = 0x004F,	// LATIN CAPITAL LETTER O
	[0x50] = 0x0050,	// LATIN CAPITAL LETTER P
	[0x51] = 0x0051,	// LATIN CAPITAL LETTER Q
	[0x52] = 0x0052,	// LATIN CAPITAL LETTER R
	[0x53] = 0x0053,	// LATIN CAPITAL LETTER S
	[0x54] = 0x0054,	// LATIN CAPITAL LETTER T
	[0x55] = 0x0055,	// LATIN CAPITAL LETTER U
	[0x56] = 0x0056,	// LATIN CAPITAL LETTER V
	[0x57] = 0x0057,	// LATIN CAPITAL LETTER W
	[0x58] = 0x0058,	// LATIN CAPITAL LETTER X
	[0x59] = 0x0059,	// LATIN CAPITAL LETTER Y
	[0x5A] = 0x005A,	// LATIN CAPITAL LETTER Z
	[0x5B] = 0x005B,	// LEFT SQUARE BRACKET
	[0x5C] = 0x005C,	// REVERSE SOLIDUS
	[0x5D] = 0x005D,	// RIGHT SQUARE BRACKET
	[0x5E] = 0x005E,	// CIRCUMFLEX ACCENT
	[0x5F] = 0x005F,	// LOW LINE
	[0x60] = 0x0060,	// GRAVE ACCENT
	[0x61] = 0x0061,	// LATIN SMALL LETTER A
	[0x62] = 0x0062,	// LATIN SMALL LETTER B
	[0x63] = 0x0063,	// LATIN SMALL LETTER C
	[0x64] = 0x0064,	// LATIN SMALL LETTER D
	[0x65] = 0x0065,	// LATIN SMALL LETTER E
	[0x66] = 0x0066,	// LATIN SMALL LETTER F
	[0x67] = 0x0067,	// LATIN SMALL LETTER G
	[0x68] = 0x0068,	// LATIN SMALL LETTER H
	[0x69] = 0x0069,	// LATIN SMALL LETTER I
	[0x6A] = 0x006A,	// LATIN SMALL LETTER J
	[0x6B] = 0x006B,	// LATIN SMALL LETTER K
	[0x6C] = 0x006C,	// LATIN SMALL LETTER L
	[0x6D] = 0x006D,	// LATIN SMALL LETTER M
	[0x6E] = 0x006E,	// LATIN SMALL LETTER N
	[0x6F] = 0x006F,	// LATIN SMALL LETTER O
	[0x70] = 0x0070,	// LATIN SMALL LETTER P
	[0x71] = 0x0071,	// LATIN SMALL LETTER Q
	[0x72] = 0x0072,	// LATIN SMALL LETTER R
	[0x73] = 0x0073,	// LATIN SMALL LETTER S
	[0x74] = 0x0074,	// LATIN SMALL LETTER T
	[0x75] = 0x0075,	// LATIN SMALL LETTER U
	[0x76] = 0x0076,	// LATIN SMALL LETTER V
	[0x77] = 0x0077,	// LATIN SMALL LETTER W
	[0x78] = 0x0078,	// LATIN SMALL LETTER X
	[0x79] = 0x0079,	// LATIN SMALL LETTER Y
	[0x7A] = 0x007A,	// LATIN SMALL LETTER Z
	[0x7B] = 0x007B,	// LEFT CURLY BRACKET
	[0x7C] = 0x007C,	// VERTICAL LINE
	[0x7D] = 0x007D,	// RIGHT CURLY BRACKET
	[0x7E] = 0x007E,	// TILDE
	[0x7F] = 0x007F,	// DELETE
	[0x80] = 0x0080,	// <control>
	[0x81] = 0x0081,	// <control>
	[0x82] = 0x0082,	// <control>
	[0x83] = 0x0083,	// <control>
	[0x84] = 0x0084,	// <control>
	[0x85] = 0x0085,	// <control>
	[0x86] = 0x0086,	// <control>
	[0x87] = 0x0087,	// <control>
	[0x88] = 0x0088,	// <control>
	[0x89] = 0x0089,	// <control>
	[0x8A] = 0x008A,	// <control>
	[0x8B] = 0x008B,	// <control>
	[0x8C] = 0x008C,	// <control>
	[0x8D] = 0x008D,	// <control>
	[0x8E] = 0x008E,	// <control>
	[0x8F] = 0x008F,	// <control>
	[0x90] = 0x0090,	// <control>
	[0x91] = 0x0091,	// <control>
	[0x92] = 0x0092,	// <control>
	[0x93] = 0x0093,	// <control>
	[0x94] = 0x0094,	// <control>
	[0x95] = 0x0095,	// <control>
	[0x96] = 0x0096,	// <control>
	[0x97] = 0x0097,	// <control>
	[0x98] = 0x0098,	// <control>
	[0x99] = 0x0099,	// <control>
	[0x9A] = 0x009A,	// <control>
	[0x9B] = 0x009B,	// <control>
	[0x9C] = 0x009C,	// <control>
	[0x9D] = 0x009D,	// <control>
	[0x9E] = 0x009E,	// <control>
	[0x9F] = 0x009F,	// <control>
	[0xA0] = 0x00A0,	// NO-BREAK SPACE
	[0xA1] = 0x00A1,	// INVERTED EXCLAMATION MARK
	[0xA2] = 0x00A2,	// CENT SIGN
	[0xA3] = 0x00A3,	// POUND SIGN
	[0xA4] = 0x00A4,	// CURRENCY SIGN
	[0xA5] = 0x00A5,	// YEN SIGN
	[0xA6] = 0x00A6,	// BROKEN BAR
	[0xA7] = 0x00A7,	// SECTION SIGN
	[0xA8] = 0x00A8,	// DIAERESIS
	[0xA9] = 0x00A9,	// COPYRIGHT SIGN
	[0xAA] = 0x00AA,	// FEMININE ORDINAL INDICATOR
	[0xAB] = 0x00AB,	// LEFT-POINTING DOUBLE ANGLE QUOTATION MARK
	[0xAC] = 0x00AC,	// NOT SIGN
	[0xAD] = 0x00AD,	// SOFT HYPHEN
	[0xAE] = 0x00AE,	// REGISTERED SIGN
	[0xAF] = 0x00AF,	// MACRON
	[0xB0] = 0x00B0,	// DEGREE SIGN
	[0xB1] = 0x00B1,	// PLUS-MINUS SIGN
	[0xB2] = 0x00B2,	// SUPERSCRIPT TWO
	[0xB3] = 0x00B3,	// SUPERSCRIPT THREE
	[0xB4] = 0x00B4,	// ACUTE ACCENT
	[0xB5] = 0x00B5,	// MICRO SIGN
	[0xB6] = 0x00B6,	// PILCROW SIGN
	[0xB7] = 0x00B7,	// MIDDLE DOT
	[0xB8] = 0x00B8,	// CEDILLA
	[0xB9] = 0x00B9,	// SUPERSCRIPT ONE
	[0xBA] = 0x00BA,	// MASCULINE ORDINAL INDICATOR
	[0xBB] = 0x00BB,	// RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK
	[0xBC] = 0x00BC,	// VULGAR FRACTION ONE QUARTER
	[0xBD] = 0x00BD,	// VULGAR FRACTION ONE HALF
	[0xBE] = 0x00BE,	// VULGAR FRACTION THREE QUARTERS
	[0xBF] = 0x00BF,	// INVERTED QUESTION MARK
	[0xC0] = 0x00C0,	// LATIN CAPITAL LETTER A WITH GRAVE
	[0xC1] = 0x00C1,	// LATIN CAPITAL LETTER A WITH ACUTE
	[0xC2] = 0x00C2,	// LATIN CAPITAL LETTER A WITH CIRCUMFLEX
	[0xC3] = 0x00C3,	// LATIN CAPITAL LETTER A WITH TILDE
	[0xC4] = 0x00C4,	// LATIN CAPITAL LETTER A WITH DIAERESIS
	[0xC5] = 0x00C5,	// LATIN CAPITAL LETTER A WITH RING ABOVE
	[0xC6] = 0x00C6,	// LATIN CAPITAL LETTER AE
	[0xC7] = 0x00C7,	// LATIN CAPITAL LETTER C WITH CEDILLA
	[0xC8] = 0x00C8,	// LATIN CAPITAL LETTER E WITH GRAVE
	[0xC9] = 0x00C9,	// LATIN CAPITAL LETTER E WITH ACUTE
	[0xCA] = 0x00CA,	// LATIN CAPITAL LETTER E WITH CIRCUMFLEX
	[0xCB] = 0x00CB,	// LATIN CAPITAL LETTER E WITH DIAERESIS
	[0xCC] = 0x00CC,	// LATIN CAPITAL LETTER I WITH GRAVE
	[0xCD] = 0x00CD,	// LATIN CAPITAL LETTER I WITH ACUTE
	[0xCE] = 0x00CE,	// LATIN CAPITAL LETTER I WITH CIRCUMFLEX
	[0xCF] = 0x00CF,	// LATIN CAPITAL LETTER I WITH DIAERESIS
	[0xD0] = 0x00D0,	// LATIN CAPITAL LETTER ETH (Icelandic)
	[0xD1] = 0x00D1,	// LATIN CAPITAL LETTER N WITH TILDE
	[0xD2] = 0x00D2,	// LATIN CAPITAL LETTER O WITH GRAVE
	[0xD3] = 0x00D3,	// LATIN CAPITAL LETTER O WITH ACUTE
	[0xD4] = 0x00D4,	// LATIN CAPITAL LETTER O WITH CIRCUMFLEX
	[0xD5] = 0x00D5,	// LATIN CAPITAL LETTER O WITH TILDE
	[0xD6] = 0x00D6,	// LATIN CAPITAL LETTER O WITH DIAERESIS
	[0xD7] = 0x00D7,	// MULTIPLICATION SIGN
	[0xD8] = 0x00D8,	// LATIN CAPITAL LETTER O WITH STROKE
	[0xD9] = 0x00D9,	// LATIN CAPITAL LETTER U WITH GRAVE
	[0xDA] = 0x00DA,	// LATIN CAPITAL LETTER U WITH ACUTE
	[0xDB] = 0x00DB,	// LATIN CAPITAL LETTER U WITH CIRCUMFLEX
	[0xDC] = 0x00DC,	// LATIN CAPITAL LETTER U WITH DIAERESIS
	[0xDD] = 0x00DD,	// LATIN CAPITAL LETTER Y WITH ACUTE
	[0xDE] = 0x00DE,	// LATIN CAPITAL LETTER THORN (Icelandic)
	[0xDF] = 0x00DF,	// LATIN SMALL LETTER SHARP S (German)
	[0xE0] = 0x00E0,	// LATIN SMALL LETTER A WITH GRAVE
	[0xE1] = 0x00E1,	// LATIN SMALL LETTER A WITH ACUTE
	[0xE2] = 0x00E2,	// LATIN SMALL LETTER A WITH CIRCUMFLEX
	[0xE3] = 0x00E3,	// LATIN SMALL LETTER A WITH TILDE
	[0xE4] = 0x00E4,	// LATIN SMALL LETTER A WITH DIAERESIS
	[0xE5] = 0x00E5,	// LATIN SMALL LETTER A WITH RING ABOVE
	[0xE6] = 0x00E6,	// LATIN SMALL LETTER AE
	[0xE7] = 0x00E7,	// LATIN SMALL LETTER C WITH CEDILLA
	[0xE8] = 0x00E8,	// LATIN SMALL LETTER E WITH GRAVE
	[0xE9] = 0x00E9,	// LATIN SMALL LETTER E WITH ACUTE
	[0xEA] = 0x00EA,	// LATIN SMALL LETTER E WITH CIRCUMFLEX
	[0xEB] = 0x00EB,	// LATIN SMALL LETTER E WITH DIAERESIS
	[0xEC] = 0x00EC,	// LATIN SMALL LETTER I WITH GRAVE
	[0xED] = 0x00ED,	// LATIN SMALL LETTER I WITH ACUTE
	[0xEE] = 0x00EE,	// LATIN SMALL LETTER I WITH CIRCUMFLEX
	[0xEF] = 0x00EF,	// LATIN SMALL LETTER I WITH DIAERESIS
	[0xF0] = 0x00F0,	// LATIN SMALL LETTER ETH (Icelandic)
	[0xF1] = 0x00F1,	// LATIN SMALL LETTER N WITH TILDE
	[0xF2] = 0x00F2,	// LATIN SMALL LETTER O WITH GRAVE
	[0xF3] = 0x00F3,	// LATIN SMALL LETTER O WITH ACUTE
	[0xF4] = 0x00F4,	// LATIN SMALL LETTER O WITH CIRCUMFLEX
	[0xF5] = 0x00F5,	// LATIN SMALL LETTER O WITH TILDE
	[0xF6] = 0x00F6,	// LATIN SMALL LETTER O WITH DIAERESIS
	[0xF7] = 0x00F7,	// DIVISION SIGN
	[0xF8] = 0x00F8,	// LATIN SMALL LETTER O WITH STROKE
	[0xF9] = 0x00F9,	// LATIN SMALL LETTER U WITH GRAVE
	[0xFA] = 0x00FA,	// LATIN SMALL LETTER U WITH ACUTE
	[0xFB] = 0x00FB,	// LATIN SMALL LETTER U WITH CIRCUMFLEX
	[0xFC] = 0x00FC,	// LATIN SMALL LETTER U WITH DIAERESIS
	[0xFD] = 0x00FD,	// LATIN SMALL LETTER Y WITH ACUTE
	[0xFE] = 0x00FE,	// LATIN SMALL LETTER THORN (Icelandic)
	[0xFF] = 0x00FF,	// LATIN SMALL LETTER Y WITH DIAERESIS
};

static const wchar_t ISO_8859_2_UNICODE_TABLE[] = {
	[0x00] = 0x0000,	// NULL
	[0x01] = 0x0001,	// START OF HEADING
	[0x02] = 0x0002,	// START OF TEXT
	[0x03] = 0x0003,	// END OF TEXT
	[0x04] = 0x0004,	// END OF TRANSMISSION
	[0x05] = 0x0005,	// ENQUIRY
	[0x06] = 0x0006,	// ACKNOWLEDGE
	[0x07] = 0x0007,	// BELL
	[0x08] = 0x0008,	// BACKSPACE
	[0x09] = 0x0009,	// HORIZONTAL TABULATION
	[0x0A] = 0x000A,	// LINE FEED
	[0x0B] = 0x000B,	// VERTICAL TABULATION
	[0x0C] = 0x000C,	// FORM FEED
	[0x0D] = 0x000D,	// CARRIAGE RETURN
	[0x0E] = 0x000E,	// SHIFT OUT
	[0x0F] = 0x000F,	// SHIFT IN
	[0x10] = 0x0010,	// DATA LINK ESCAPE
	[0x11] = 0x0011,	// DEVICE CONTROL ONE
	[0x12] = 0x0012,	// DEVICE CONTROL TWO
	[0x13] = 0x0013,	// DEVICE CONTROL THREE
	[0x14] = 0x0014,	// DEVICE CONTROL FOUR
	[0x15] = 0x0015,	// NEGATIVE ACKNOWLEDGE
	[0x16] = 0x0016,	// SYNCHRONOUS IDLE
	[0x17] = 0x0017,	// END OF TRANSMISSION BLOCK
	[0x18] = 0x0018,	// CANCEL
	[0x19] = 0x0019,	// END OF MEDIUM
	[0x1A] = 0x001A,	// SUBSTITUTE
	[0x1B] = 0x001B,	// ESCAPE
	[0x1C] = 0x001C,	// FILE SEPARATOR
	[0x1D] = 0x001D,	// GROUP SEPARATOR
	[0x1E] = 0x001E,	// RECORD SEPARATOR
	[0x1F] = 0x001F,	// UNIT SEPARATOR
	[0x20] = 0x0020,	// SPACE
	[0x21] = 0x0021,	// EXCLAMATION MARK
	[0x22] = 0x0022,	// QUOTATION MARK
	[0x23] = 0x0023,	// NUMBER SIGN
	[0x24] = 0x0024,	// DOLLAR SIGN
	[0x25] = 0x0025,	// PERCENT SIGN
	[0x26] = 0x0026,	// AMPERSAND
	[0x27] = 0x0027,	// APOSTROPHE
	[0x28] = 0x0028,	// LEFT PARENTHESIS
	[0x29] = 0x0029,	// RIGHT PARENTHESIS
	[0x2A] = 0x002A,	// ASTERISK
	[0x2B] = 0x002B,	// PLUS SIGN
	[0x2C] = 0x002C,	// COMMA
	[0x2D] = 0x002D,	// HYPHEN-MINUS
	[0x2E] = 0x002E,	// FULL STOP
	[0x2F] = 0x002F,	// SOLIDUS
	[0x30] = 0x0030,	// DIGIT ZERO
	[0x31] = 0x0031,	// DIGIT ONE
	[0x32] = 0x0032,	// DIGIT TWO
	[0x33] = 0x0033,	// DIGIT THREE
	[0x34] = 0x0034,	// DIGIT FOUR
	[0x35] = 0x0035,	// DIGIT FIVE
	[0x36] = 0x0036,	// DIGIT SIX
	[0x37] = 0x0037,	// DIGIT SEVEN
	[0x38] = 0x0038,	// DIGIT EIGHT
	[0x39] = 0x0039,	// DIGIT NINE
	[0x3A] = 0x003A,	// COLON
	[0x3B] = 0x003B,	// SEMICOLON
	[0x3C] = 0x003C,	// LESS-THAN SIGN
	[0x3D] = 0x003D,	// EQUALS SIGN
	[0x3E] = 0x003E,	// GREATER-THAN SIGN
	[0x3F] = 0x003F,	// QUESTION MARK
	[0x40] = 0x0040,	// COMMERCIAL AT
	[0x41] = 0x0041,	// LATIN CAPITAL LETTER A
	[0x42] = 0x0042,	// LATIN CAPITAL LETTER B
	[0x43] = 0x0043,	// LATIN CAPITAL LETTER C
	[0x44] = 0x0044,	// LATIN CAPITAL LETTER D
	[0x45] = 0x0045,	// LATIN CAPITAL LETTER E
	[0x46] = 0x0046,	// LATIN CAPITAL LETTER F
	[0x47] = 0x0047,	// LATIN CAPITAL LETTER G
	[0x48] = 0x0048,	// LATIN CAPITAL LETTER H
	[0x49] = 0x0049,	// LATIN CAPITAL LETTER I
	[0x4A] = 0x004A,	// LATIN CAPITAL LETTER J
	[0x4B] = 0x004B,	// LATIN CAPITAL LETTER K
	[0x4C] = 0x004C,	// LATIN CAPITAL LETTER L
	[0x4D] = 0x004D,	// LATIN CAPITAL LETTER M
	[0x4E] = 0x004E,	// LATIN CAPITAL LETTER N
	[0x4F] = 0x004F,	// LATIN CAPITAL LETTER O
	[0x50] = 0x0050,	// LATIN CAPITAL LETTER P
	[0x51] = 0x0051,	// LATIN CAPITAL LETTER Q
	[0x52] = 0x0052,	// LATIN CAPITAL LETTER R
	[0x53] = 0x0053,	// LATIN CAPITAL LETTER S
	[0x54] = 0x0054,	// LATIN CAPITAL LETTER T
	[0x55] = 0x0055,	// LATIN CAPITAL LETTER U
	[0x56] = 0x0056,	// LATIN CAPITAL LETTER V
	[0x57] = 0x0057,	// LATIN CAPITAL LETTER W
	[0x58] = 0x0058,	// LATIN CAPITAL LETTER X
	[0x59] = 0x0059,	// LATIN CAPITAL LETTER Y
	[0x5A] = 0x005A,	// LATIN CAPITAL LETTER Z
	[0x5B] = 0x005B,	// LEFT SQUARE BRACKET
	[0x5C] = 0x005C,	// REVERSE SOLIDUS
	[0x5D] = 0x005D,	// RIGHT SQUARE BRACKET
	[0x5E] = 0x005E,	// CIRCUMFLEX ACCENT
	[0x5F] = 0x005F,	// LOW LINE
	[0x60] = 0x0060,	// GRAVE ACCENT
	[0x61] = 0x0061,	// LATIN SMALL LETTER A
	[0x62] = 0x0062,	// LATIN SMALL LETTER B
	[0x63] = 0x0063,	// LATIN SMALL LETTER C
	[0x64] = 0x0064,	// LATIN SMALL LETTER D
	[0x65] = 0x0065,	// LATIN SMALL LETTER E
	[0x66] = 0x0066,	// LATIN SMALL LETTER F
	[0x67] = 0x0067,	// LATIN SMALL LETTER G
	[0x68] = 0x0068,	// LATIN SMALL LETTER H
	[0x69] = 0x0069,	// LATIN SMALL LETTER I
	[0x6A] = 0x006A,	// LATIN SMALL LETTER J
	[0x6B] = 0x006B,	// LATIN SMALL LETTER K
	[0x6C] = 0x006C,	// LATIN SMALL LETTER L
	[0x6D] = 0x006D,	// LATIN SMALL LETTER M
	[0x6E] = 0x006E,	// LATIN SMALL LETTER N
	[0x6F] = 0x006F,	// LATIN SMALL LETTER O
	[0x70] = 0x0070,	// LATIN SMALL LETTER P
	[0x71] = 0x0071,	// LATIN SMALL LETTER Q
	[0x72] = 0x0072,	// LATIN SMALL LETTER R
	[0x73] = 0x0073,	// LATIN SMALL LETTER S
	[0x74] = 0x0074,	// LATIN SMALL LETTER T
	[0x75] = 0x0075,	// LATIN SMALL LETTER U
	[0x76] = 0x0076,	// LATIN SMALL LETTER V
	[0x77] = 0x0077,	// LATIN SMALL LETTER W
	[0x78] = 0x0078,	// LATIN SMALL LETTER X
	[0x79] = 0x0079,	// LATIN SMALL LETTER Y
	[0x7A] = 0x007A,	// LATIN SMALL LETTER Z
	[0x7B] = 0x007B,	// LEFT CURLY BRACKET
	[0x7C] = 0x007C,	// VERTICAL LINE
	[0x7D] = 0x007D,	// RIGHT CURLY BRACKET
	[0x7E] = 0x007E,	// TILDE
	[0x7F] = 0x007F,	// DELETE
	[0x80] = 0x0080,	// <control>
	[0x81] = 0x0081,	// <control>
	[0x82] = 0x0082,	// <control>
	[0x83] = 0x0083,	// <control>
	[0x84] = 0x0084,	// <control>
	[0x85] = 0x0085,	// <control>
	[0x86] = 0x0086,	// <control>
	[0x87] = 0x0087,	// <control>
	[0x88] = 0x0088,	// <control>
	[0x89] = 0x0089,	// <control>
	[0x8A] = 0x008A,	// <control>
	[0x8B] = 0x008B,	// <control>
	[0x8C] = 0x008C,	// <control>
	[0x8D] = 0x008D,	// <control>
	[0x8E] = 0x008E,	// <control>
	[0x8F] = 0x008F,	// <control>
	[0x90] = 0x0090,	// <control>
	[0x91] = 0x0091,	// <control>
	[0x92] = 0x0092,	// <control>
	[0x93] = 0x0093,	// <control>
	[0x94] = 0x0094,	// <control>
	[0x95] = 0x0095,	// <control>
	[0x96] = 0x0096,	// <control>
	[0x97] = 0x0097,	// <control>
	[0x98] = 0x0098,	// <control>
	[0x99] = 0x0099,	// <control>
	[0x9A] = 0x009A,	// <control>
	[0x9B] = 0x009B,	// <control>
	[0x9C] = 0x009C,	// <control>
	[0x9D] = 0x009D,	// <control>
	[0x9E] = 0x009E,	// <control>
	[0x9F] = 0x009F,	// <control>
	[0xA0] = 0x00A0,	// NO-BREAK SPACE
	[0xA1] = 0x0104,	// LATIN CAPITAL LETTER A WITH OGONEK
	[0xA2] = 0x02D8,	// BREVE
	[0xA3] = 0x0141,	// LATIN CAPITAL LETTER L WITH STROKE
	[0xA4] = 0x00A4,	// CURRENCY SIGN
	[0xA5] = 0x013D,	// LATIN CAPITAL LETTER L WITH CARON
	[0xA6] = 0x015A,	// LATIN CAPITAL LETTER S WITH ACUTE
	[0xA7] = 0x00A7,	// SECTION SIGN
	[0xA8] = 0x00A8,	// DIAERESIS
	[0xA9] = 0x0160,	// LATIN CAPITAL LETTER S WITH CARON
	[0xAA] = 0x015E,	// LATIN CAPITAL LETTER S WITH CEDILLA
	[0xAB] = 0x0164,	// LATIN CAPITAL LETTER T WITH CARON
	[0xAC] = 0x0179,	// LATIN CAPITAL LETTER Z WITH ACUTE
	[0xAD] = 0x00AD,	// SOFT HYPHEN
	[0xAE] = 0x017D,	// LATIN CAPITAL LETTER Z WITH CARON
	[0xAF] = 0x017B,	// LATIN CAPITAL LETTER Z WITH DOT ABOVE
	[0xB0] = 0x00B0,	// DEGREE SIGN
	[0xB1] = 0x0105,	// LATIN SMALL LETTER A WITH OGONEK
	[0xB2] = 0x02DB,	// OGONEK
	[0xB3] = 0x0142,	// LATIN SMALL LETTER L WITH STROKE
	[0xB4] = 0x00B4,	// ACUTE ACCENT
	[0xB5] = 0x013E,	// LATIN SMALL LETTER L WITH CARON
	[0xB6] = 0x015B,	// LATIN SMALL LETTER S WITH ACUTE
	[0xB7] = 0x02C7,	// CARON
	[0xB8] = 0x00B8,	// CEDILLA
	[0xB9] = 0x0161,	// LATIN SMALL LETTER S WITH CARON
	[0xBA] = 0x015F,	// LATIN SMALL LETTER S WITH CEDILLA
	[0xBB] = 0x0165,	// LATIN SMALL LETTER T WITH CARON
	[0xBC] = 0x017A,	// LATIN SMALL LETTER Z WITH ACUTE
	[0xBD] = 0x02DD,	// DOUBLE ACUTE ACCENT
	[0xBE] = 0x017E,	// LATIN SMALL LETTER Z WITH CARON
	[0xBF] = 0x017C,	// LATIN SMALL LETTER Z WITH DOT ABOVE
	[0xC0] = 0x0154,	// LATIN CAPITAL LETTER R WITH ACUTE
	[0xC1] = 0x00C1,	// LATIN CAPITAL LETTER A WITH ACUTE
	[0xC2] = 0x00C2,	// LATIN CAPITAL LETTER A WITH CIRCUMFLEX
	[0xC3] = 0x0102,	// LATIN CAPITAL LETTER A WITH BREVE
	[0xC4] = 0x00C4,	// LATIN CAPITAL LETTER A WITH DIAERESIS
	[0xC5] = 0x0139,	// LATIN CAPITAL LETTER L WITH ACUTE
	[0xC6] = 0x0106,	// LATIN CAPITAL LETTER C WITH ACUTE
	[0xC7] = 0x00C7,	// LATIN CAPITAL LETTER C WITH CEDILLA
	[0xC8] = 0x010C,	// LATIN CAPITAL LETTER C WITH CARON
	[0xC9] = 0x00C9,	// LATIN CAPITAL LETTER E WITH ACUTE
	[0xCA] = 0x0118,	// LATIN CAPITAL LETTER E WITH OGONEK
	[0xCB] = 0x00CB,	// LATIN CAPITAL LETTER E WITH DIAERESIS
	[0xCC] = 0x011A,	// LATIN CAPITAL LETTER E WITH CARON
	[0xCD] = 0x00CD,	// LATIN CAPITAL LETTER I WITH ACUTE
	[0xCE] = 0x00CE,	// LATIN CAPITAL LETTER I WITH CIRCUMFLEX
	[0xCF] = 0x010E,	// LATIN CAPITAL LETTER D WITH CARON
	[0xD0] = 0x0110,	// LATIN CAPITAL LETTER D WITH STROKE
	[0xD1] = 0x0143,	// LATIN CAPITAL LETTER N WITH ACUTE
	[0xD2] = 0x0147,	// LATIN CAPITAL LETTER N WITH CARON
	[0xD3] = 0x00D3,	// LATIN CAPITAL LETTER O WITH ACUTE
	[0xD4] = 0x00D4,	// LATIN CAPITAL LETTER O WITH CIRCUMFLEX
	[0xD5] = 0x0150,	// LATIN CAPITAL LETTER O WITH DOUBLE ACUTE
	[0xD6] = 0x00D6,	// LATIN CAPITAL LETTER O WITH DIAERESIS
	[0xD7] = 0x00D7,	// MULTIPLICATION SIGN
	[0xD8] = 0x0158,	// LATIN CAPITAL LETTER R WITH CARON
	[0xD9] = 0x016E,	// LATIN CAPITAL LETTER U WITH RING ABOVE
	[0xDA] = 0x00DA,	// LATIN CAPITAL LETTER U WITH ACUTE
	[0xDB] = 0x0170,	// LATIN CAPITAL LETTER U WITH DOUBLE ACUTE
	[0xDC] = 0x00DC,	// LATIN CAPITAL LETTER U WITH DIAERESIS
	[0xDD] = 0x00DD,	// LATIN CAPITAL LETTER Y WITH ACUTE
	[0xDE] = 0x0162,	// LATIN CAPITAL LETTER T WITH CEDILLA
	[0xDF] = 0x00DF,	// LATIN SMALL LETTER SHARP S
	[0xE0] = 0x0155,	// LATIN SMALL LETTER R WITH ACUTE
	[0xE1] = 0x00E1,	// LATIN SMALL LETTER A WITH ACUTE
	[0xE2] = 0x00E2,	// LATIN SMALL LETTER A WITH CIRCUMFLEX
	[0xE3] = 0x0103,	// LATIN SMALL LETTER A WITH BREVE
	[0xE4] = 0x00E4,	// LATIN SMALL LETTER A WITH DIAERESIS
	[0xE5] = 0x013A,	// LATIN SMALL LETTER L WITH ACUTE
	[0xE6] = 0x0107,	// LATIN SMALL LETTER C WITH ACUTE
	[0xE7] = 0x00E7,	// LATIN SMALL LETTER C WITH CEDILLA
	[0xE8] = 0x010D,	// LATIN SMALL LETTER C WITH CARON
	[0xE9] = 0x00E9,	// LATIN SMALL LETTER E WITH ACUTE
	[0xEA] = 0x0119,	// LATIN SMALL LETTER E WITH OGONEK
	[0xEB] = 0x00EB,	// LATIN SMALL LETTER E WITH DIAERESIS
	[0xEC] = 0x011B,	// LATIN SMALL LETTER E WITH CARON
	[0xED] = 0x00ED,	// LATIN SMALL LETTER I WITH ACUTE
	[0xEE] = 0x00EE,	// LATIN SMALL LETTER I WITH CIRCUMFLEX
	[0xEF] = 0x010F,	// LATIN SMALL LETTER D WITH CARON
	[0xF0] = 0x0111,	// LATIN SMALL LETTER D WITH STROKE
	[0xF1] = 0x0144,	// LATIN SMALL LETTER N WITH ACUTE
	[0xF2] = 0x0148,	// LATIN SMALL LETTER N WITH CARON
	[0xF3] = 0x00F3,	// LATIN SMALL LETTER O WITH ACUTE
	[0xF4] = 0x00F4,	// LATIN SMALL LETTER O WITH CIRCUMFLEX
	[0xF5] = 0x0151,	// LATIN SMALL LETTER O WITH DOUBLE ACUTE
	[0xF6] = 0x00F6,	// LATIN SMALL LETTER O WITH DIAERESIS
	[0xF7] = 0x00F7,	// DIVISION SIGN
	[0xF8] = 0x0159,	// LATIN SMALL LETTER R WITH CARON
	[0xF9] = 0x016F,	// LATIN SMALL LETTER U WITH RING ABOVE
	[0xFA] = 0x00FA,	// LATIN SMALL LETTER U WITH ACUTE
	[0xFB] = 0x0171,	// LATIN SMALL LETTER U WITH DOUBLE ACUTE
	[0xFC] = 0x00FC,	// LATIN SMALL LETTER U WITH DIAERESIS
	[0xFD] = 0x00FD,	// LATIN SMALL LETTER Y WITH ACUTE
	[0xFE] = 0x0163,	// LATIN SMALL LETTER T WITH CEDILLA
	[0xFF] = 0x02D9,	// DOT ABOVE
};

static const wchar_t ISO_8859_15_UNICODE_TABLE[] = {
	[0x00] = 0x0000,	// NULL
	[0x01] = 0x0001,	// START OF HEADING
	[0x02] = 0x0002,	// START OF TEXT
	[0x03] = 0x0003,	// END OF TEXT
	[0x04] = 0x0004,	// END OF TRANSMISSION
	[0x05] = 0x0005,	// ENQUIRY
	[0x06] = 0x0006,	// ACKNOWLEDGE
	[0x07] = 0x0007,	// BELL
	[0x08] = 0x0008,	// BACKSPACE
	[0x09] = 0x0009,	// HORIZONTAL TABULATION
	[0x0A] = 0x000A,	// LINE FEED
	[0x0B] = 0x000B,	// VERTICAL TABULATION
	[0x0C] = 0x000C,	// FORM FEED
	[0x0D] = 0x000D,	// CARRIAGE RETURN
	[0x0E] = 0x000E,	// SHIFT OUT
	[0x0F] = 0x000F,	// SHIFT IN
	[0x10] = 0x0010,	// DATA LINK ESCAPE
	[0x11] = 0x0011,	// DEVICE CONTROL ONE
	[0x12] = 0x0012,	// DEVICE CONTROL TWO
	[0x13] = 0x0013,	// DEVICE CONTROL THREE
	[0x14] = 0x0014,	// DEVICE CONTROL FOUR
	[0x15] = 0x0015,	// NEGATIVE ACKNOWLEDGE
	[0x16] = 0x0016,	// SYNCHRONOUS IDLE
	[0x17] = 0x0017,	// END OF TRANSMISSION BLOCK
	[0x18] = 0x0018,	// CANCEL
	[0x19] = 0x0019,	// END OF MEDIUM
	[0x1A] = 0x001A,	// SUBSTITUTE
	[0x1B] = 0x001B,	// ESCAPE
	[0x1C] = 0x001C,	// FILE SEPARATOR
	[0x1D] = 0x001D,	// GROUP SEPARATOR
	[0x1E] = 0x001E,	// RECORD SEPARATOR
	[0x1F] = 0x001F,	// UNIT SEPARATOR
	[0x20] = 0x0020,	// SPACE
	[0x21] = 0x0021,	// EXCLAMATION MARK
	[0x22] = 0x0022,	// QUOTATION MARK
	[0x23] = 0x0023,	// NUMBER SIGN
	[0x24] = 0x0024,	// DOLLAR SIGN
	[0x25] = 0x0025,	// PERCENT SIGN
	[0x26] = 0x0026,	// AMPERSAND
	[0x27] = 0x0027,	// APOSTROPHE
	[0x28] = 0x0028,	// LEFT PARENTHESIS
	[0x29] = 0x0029,	// RIGHT PARENTHESIS
	[0x2A] = 0x002A,	// ASTERISK
	[0x2B] = 0x002B,	// PLUS SIGN
	[0x2C] = 0x002C,	// COMMA
	[0x2D] = 0x002D,	// HYPHEN-MINUS
	[0x2E] = 0x002E,	// FULL STOP
	[0x2F] = 0x002F,	// SOLIDUS
	[0x30] = 0x0030,	// DIGIT ZERO
	[0x31] = 0x0031,	// DIGIT ONE
	[0x32] = 0x0032,	// DIGIT TWO
	[0x33] = 0x0033,	// DIGIT THREE
	[0x34] = 0x0034,	// DIGIT FOUR
	[0x35] = 0x0035,	// DIGIT FIVE
	[0x36] = 0x0036,	// DIGIT SIX
	[0x37] = 0x0037,	// DIGIT SEVEN
	[0x38] = 0x0038,	// DIGIT EIGHT
	[0x39] = 0x0039,	// DIGIT NINE
	[0x3A] = 0x003A,	// COLON
	[0x3B] = 0x003B,	// SEMICOLON
	[0x3C] = 0x003C,	// LESS-THAN SIGN
	[0x3D] = 0x003D,	// EQUALS SIGN
	[0x3E] = 0x003E,	// GREATER-THAN SIGN
	[0x3F] = 0x003F,	// QUESTION MARK
	[0x40] = 0x0040,	// COMMERCIAL AT
	[0x41] = 0x0041,	// LATIN CAPITAL LETTER A
	[0x42] = 0x0042,	// LATIN CAPITAL LETTER B
	[0x43] = 0x0043,	// LATIN CAPITAL LETTER C
	[0x44] = 0x0044,	// LATIN CAPITAL LETTER D
	[0x45] = 0x0045,	// LATIN CAPITAL LETTER E
	[0x46] = 0x0046,	// LATIN CAPITAL LETTER F
	[0x47] = 0x0047,	// LATIN CAPITAL LETTER G
	[0x48] = 0x0048,	// LATIN CAPITAL LETTER H
	[0x49] = 0x0049,	// LATIN CAPITAL LETTER I
	[0x4A] = 0x004A,	// LATIN CAPITAL LETTER J
	[0x4B] = 0x004B,	// LATIN CAPITAL LETTER K
	[0x4C] = 0x004C,	// LATIN CAPITAL LETTER L
	[0x4D] = 0x004D,	// LATIN CAPITAL LETTER M
	[0x4E] = 0x004E,	// LATIN CAPITAL LETTER N
	[0x4F] = 0x004F,	// LATIN CAPITAL LETTER O
	[0x50] = 0x0050,	// LATIN CAPITAL LETTER P
	[0x51] = 0x0051,	// LATIN CAPITAL LETTER Q
	[0x52] = 0x0052,	// LATIN CAPITAL LETTER R
	[0x53] = 0x0053,	// LATIN CAPITAL LETTER S
	[0x54] = 0x0054,	// LATIN CAPITAL LETTER T
	[0x55] = 0x0055,	// LATIN CAPITAL LETTER U
	[0x56] = 0x0056,	// LATIN CAPITAL LETTER V
	[0x57] = 0x0057,	// LATIN CAPITAL LETTER W
	[0x58] = 0x0058,	// LATIN CAPITAL LETTER X
	[0x59] = 0x0059,	// LATIN CAPITAL LETTER Y
	[0x5A] = 0x005A,	// LATIN CAPITAL LETTER Z
	[0x5B] = 0x005B,	// LEFT SQUARE BRACKET
	[0x5C] = 0x005C,	// REVERSE SOLIDUS
	[0x5D] = 0x005D,	// RIGHT SQUARE BRACKET
	[0x5E] = 0x005E,	// CIRCUMFLEX ACCENT
	[0x5F] = 0x005F,	// LOW LINE
	[0x60] = 0x0060,	// GRAVE ACCENT
	[0x61] = 0x0061,	// LATIN SMALL LETTER A
	[0x62] = 0x0062,	// LATIN SMALL LETTER B
	[0x63] = 0x0063,	// LATIN SMALL LETTER C
	[0x64] = 0x0064,	// LATIN SMALL LETTER D
	[0x65] = 0x0065,	// LATIN SMALL LETTER E
	[0x66] = 0x0066,	// LATIN SMALL LETTER F
	[0x67] = 0x0067,	// LATIN SMALL LETTER G
	[0x68] = 0x0068,	// LATIN SMALL LETTER H
	[0x69] = 0x0069,	// LATIN SMALL LETTER I
	[0x6A] = 0x006A,	// LATIN SMALL LETTER J
	[0x6B] = 0x006B,	// LATIN SMALL LETTER K
	[0x6C] = 0x006C,	// LATIN SMALL LETTER L
	[0x6D] = 0x006D,	// LATIN SMALL LETTER M
	[0x6E] = 0x006E,	// LATIN SMALL LETTER N
	[0x6F] = 0x006F,	// LATIN SMALL LETTER O
	[0x70] = 0x0070,	// LATIN SMALL LETTER P
	[0x71] = 0x0071,	// LATIN SMALL LETTER Q
	[0x72] = 0x0072,	// LATIN SMALL LETTER R
	[0x73] = 0x0073,	// LATIN SMALL LETTER S
	[0x74] = 0x0074,	// LATIN SMALL LETTER T
	[0x75] = 0x0075,	// LATIN SMALL LETTER U
	[0x76] = 0x0076,	// LATIN SMALL LETTER V
	[0x77] = 0x0077,	// LATIN SMALL LETTER W
	[0x78] = 0x0078,	// LATIN SMALL LETTER X
	[0x79] = 0x0079,	// LATIN SMALL LETTER Y
	[0x7A] = 0x007A,	// LATIN SMALL LETTER Z
	[0x7B] = 0x007B,	// LEFT CURLY BRACKET
	[0x7C] = 0x007C,	// VERTICAL LINE
	[0x7D] = 0x007D,	// RIGHT CURLY BRACKET
	[0x7E] = 0x007E,	// TILDE
	[0x7F] = 0x007F,	// DELETE
	[0x80] = 0x0080,	// <control>
	[0x81] = 0x0081,	// <control>
	[0x82] = 0x0082,	// <control>
	[0x83] = 0x0083,	// <control>
	[0x84] = 0x0084,	// <control>
	[0x85] = 0x0085,	// <control>
	[0x86] = 0x0086,	// <control>
	[0x87] = 0x0087,	// <control>
	[0x88] = 0x0088,	// <control>
	[0x89] = 0x0089,	// <control>
	[0x8A] = 0x008A,	// <control>
	[0x8B] = 0x008B,	// <control>
	[0x8C] = 0x008C,	// <control>
	[0x8D] = 0x008D,	// <control>
	[0x8E] = 0x008E,	// <control>
	[0x8F] = 0x008F,	// <control>
	[0x90] = 0x0090,	// <control>
	[0x91] = 0x0091,	// <control>
	[0x92] = 0x0092,	// <control>
	[0x93] = 0x0093,	// <control>
	[0x94] = 0x0094,	// <control>
	[0x95] = 0x0095,	// <control>
	[0x96] = 0x0096,	// <control>
	[0x97] = 0x0097,	// <control>
	[0x98] = 0x0098,	// <control>
	[0x99] = 0x0099,	// <control>
	[0x9A] = 0x009A,	// <control>
	[0x9B] = 0x009B,	// <control>
	[0x9C] = 0x009C,	// <control>
	[0x9D] = 0x009D,	// <control>
	[0x9E] = 0x009E,	// <control>
	[0x9F] = 0x009F,	// <control>
	[0xA0] = 0x00A0,	// NO-BREAK SPACE
	[0xA1] = 0x00A1,	// INVERTED EXCLAMATION MARK
	[0xA2] = 0x00A2,	// CENT SIGN
	[0xA3] = 0x00A3,	// POUND SIGN
	[0xA4] = 0x20AC,	// EURO SIGN
	[0xA5] = 0x00A5,	// YEN SIGN
	[0xA6] = 0x0160,	// LATIN CAPITAL LETTER S WITH CARON
	[0xA7] = 0x00A7,	// SECTION SIGN
	[0xA8] = 0x0161,	// LATIN SMALL LETTER S WITH CARON
	[0xA9] = 0x00A9,	// COPYRIGHT SIGN
	[0xAA] = 0x00AA,	// FEMININE ORDINAL INDICATOR
	[0xAB] = 0x00AB,	// LEFT-POINTING DOUBLE ANGLE QUOTATION MARK
	[0xAC] = 0x00AC,	// NOT SIGN
	[0xAD] = 0x00AD,	// SOFT HYPHEN
	[0xAE] = 0x00AE,	// REGISTERED SIGN
	[0xAF] = 0x00AF,	// MACRON
	[0xB0] = 0x00B0,	// DEGREE SIGN
	[0xB1] = 0x00B1,	// PLUS-MINUS SIGN
	[0xB2] = 0x00B2,	// SUPERSCRIPT TWO
	[0xB3] = 0x00B3,	// SUPERSCRIPT THREE
	[0xB4] = 0x017D,	// LATIN CAPITAL LETTER Z WITH CARON
	[0xB5] = 0x00B5,	// MICRO SIGN
	[0xB6] = 0x00B6,	// PILCROW SIGN
	[0xB7] = 0x00B7,	// MIDDLE DOT
	[0xB8] = 0x017E,	// LATIN SMALL LETTER Z WITH CARON
	[0xB9] = 0x00B9,	// SUPERSCRIPT ONE
	[0xBA] = 0x00BA,	// MASCULINE ORDINAL INDICATOR
	[0xBB] = 0x00BB,	// RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK
	[0xBC] = 0x0152,	// LATIN CAPITAL LIGATURE OE
	[0xBD] = 0x0153,	// LATIN SMALL LIGATURE OE
	[0xBE] = 0x0178,	// LATIN CAPITAL LETTER Y WITH DIAERESIS
	[0xBF] = 0x00BF,	// INVERTED QUESTION MARK
	[0xC0] = 0x00C0,	// LATIN CAPITAL LETTER A WITH GRAVE
	[0xC1] = 0x00C1,	// LATIN CAPITAL LETTER A WITH ACUTE
	[0xC2] = 0x00C2,	// LATIN CAPITAL LETTER A WITH CIRCUMFLEX
	[0xC3] = 0x00C3,	// LATIN CAPITAL LETTER A WITH TILDE
	[0xC4] = 0x00C4,	// LATIN CAPITAL LETTER A WITH DIAERESIS
	[0xC5] = 0x00C5,	// LATIN CAPITAL LETTER A WITH RING ABOVE
	[0xC6] = 0x00C6,	// LATIN CAPITAL LETTER AE
	[0xC7] = 0x00C7,	// LATIN CAPITAL LETTER C WITH CEDILLA
	[0xC8] = 0x00C8,	// LATIN CAPITAL LETTER E WITH GRAVE
	[0xC9] = 0x00C9,	// LATIN CAPITAL LETTER E WITH ACUTE
	[0xCA] = 0x00CA,	// LATIN CAPITAL LETTER E WITH CIRCUMFLEX
	[0xCB] = 0x00CB,	// LATIN CAPITAL LETTER E WITH DIAERESIS
	[0xCC] = 0x00CC,	// LATIN CAPITAL LETTER I WITH GRAVE
	[0xCD] = 0x00CD,	// LATIN CAPITAL LETTER I WITH ACUTE
	[0xCE] = 0x00CE,	// LATIN CAPITAL LETTER I WITH CIRCUMFLEX
	[0xCF] = 0x00CF,	// LATIN CAPITAL LETTER I WITH DIAERESIS
	[0xD0] = 0x00D0,	// LATIN CAPITAL LETTER ETH
	[0xD1] = 0x00D1,	// LATIN CAPITAL LETTER N WITH TILDE
	[0xD2] = 0x00D2,	// LATIN CAPITAL LETTER O WITH GRAVE
	[0xD3] = 0x00D3,	// LATIN CAPITAL LETTER O WITH ACUTE
	[0xD4] = 0x00D4,	// LATIN CAPITAL LETTER O WITH CIRCUMFLEX
	[0xD5] = 0x00D5,	// LATIN CAPITAL LETTER O WITH TILDE
	[0xD6] = 0x00D6,	// LATIN CAPITAL LETTER O WITH DIAERESIS
	[0xD7] = 0x00D7,	// MULTIPLICATION SIGN
	[0xD8] = 0x00D8,	// LATIN CAPITAL LETTER O WITH STROKE
	[0xD9] = 0x00D9,	// LATIN CAPITAL LETTER U WITH GRAVE
	[0xDA] = 0x00DA,	// LATIN CAPITAL LETTER U WITH ACUTE
	[0xDB] = 0x00DB,	// LATIN CAPITAL LETTER U WITH CIRCUMFLEX
	[0xDC] = 0x00DC,	// LATIN CAPITAL LETTER U WITH DIAERESIS
	[0xDD] = 0x00DD,	// LATIN CAPITAL LETTER Y WITH ACUTE
	[0xDE] = 0x00DE,	// LATIN CAPITAL LETTER THORN
	[0xDF] = 0x00DF,	// LATIN SMALL LETTER SHARP S
	[0xE0] = 0x00E0,	// LATIN SMALL LETTER A WITH GRAVE
	[0xE1] = 0x00E1,	// LATIN SMALL LETTER A WITH ACUTE
	[0xE2] = 0x00E2,	// LATIN SMALL LETTER A WITH CIRCUMFLEX
	[0xE3] = 0x00E3,	// LATIN SMALL LETTER A WITH TILDE
	[0xE4] = 0x00E4,	// LATIN SMALL LETTER A WITH DIAERESIS
	[0xE5] = 0x00E5,	// LATIN SMALL LETTER A WITH RING ABOVE
	[0xE6] = 0x00E6,	// LATIN SMALL LETTER AE
	[0xE7] = 0x00E7,	// LATIN SMALL LETTER C WITH CEDILLA
	[0xE8] = 0x00E8,	// LATIN SMALL LETTER E WITH GRAVE
	[0xE9] = 0x00E9,	// LATIN SMALL LETTER E WITH ACUTE
	[0xEA] = 0x00EA,	// LATIN SMALL LETTER E WITH CIRCUMFLEX
	[0xEB] = 0x00EB,	// LATIN SMALL LETTER E WITH DIAERESIS
	[0xEC] = 0x00EC,	// LATIN SMALL LETTER I WITH GRAVE
	[0xED] = 0x00ED,	// LATIN SMALL LETTER I WITH ACUTE
	[0xEE] = 0x00EE,	// LATIN SMALL LETTER I WITH CIRCUMFLEX
	[0xEF] = 0x00EF,	// LATIN SMALL LETTER I WITH DIAERESIS
	[0xF0] = 0x00F0,	// LATIN SMALL LETTER ETH
	[0xF1] = 0x00F1,	// LATIN SMALL LETTER N WITH TILDE
	[0xF2] = 0x00F2,	// LATIN SMALL LETTER O WITH GRAVE
	[0xF3] = 0x00F3,	// LATIN SMALL LETTER O WITH ACUTE
	[0xF4] = 0x00F4,	// LATIN SMALL LETTER O WITH CIRCUMFLEX
	[0xF5] = 0x00F5,	// LATIN SMALL LETTER O WITH TILDE
	[0xF6] = 0x00F6,	// LATIN SMALL LETTER O WITH DIAERESIS
	[0xF7] = 0x00F7,	// DIVISION SIGN
	[0xF8] = 0x00F8,	// LATIN SMALL LETTER O WITH STROKE
	[0xF9] = 0x00F9,	// LATIN SMALL LETTER U WITH GRAVE
	[0xFA] = 0x00FA,	// LATIN SMALL LETTER U WITH ACUTE
	[0xFB] = 0x00FB,	// LATIN SMALL LETTER U WITH CIRCUMFLEX
	[0xFC] = 0x00FC,	// LATIN SMALL LETTER U WITH DIAERESIS
	[0xFD] = 0x00FD,	// LATIN SMALL LETTER Y WITH ACUTE
	[0xFE] = 0x00FE,	// LATIN SMALL LETTER THORN
	[0xFF] = 0x00FF,	// LATIN SMALL LETTER Y WITH DIAERESIS
};

static struct map *charset_map safe;
DEF_LOOKUP_CMD(charset_handle, charset_map);

DEF_CMD(charset_char)
{
	wint_t ret;
	wchar_t *tbl = ci->home->data;

	ret = home_call(ci->home->parent, "doc:byte", ci->focus,
			ci->num, ci->mark, NULL,
			ci->num2, ci->mark2);

	if (!ci->mark2 && ret != CHAR_RET(WEOF) && ret >0)
		ret = CHAR_RET(tbl[ret & 0xff]);
	return ret;
}

struct charsetcb {
	struct command c;
	struct command *cb safe;
	struct pane *p safe;
	bool noalloc;
	wchar_t *tbl safe;
};

DEF_CB(charset_content_cb)
{
	struct charsetcb *c = container_of(ci->comm, struct charsetcb, c);
	struct buf b;
	int rv, i, bsize;

	if (!ci->str || ci->num2 <= 0 || c->noalloc)
		return comm_call(c->cb, ci->key, c->p,
				 c->tbl[ci->num & 0xff], ci->mark, ci->str,
				 ci->num2, NULL, NULL,
				 ci->x, 0);
	/* Buffer for utf8 content could be as much as 4 times ->str,
	 * but that is unlikely.  Allocate room for double, up to 1M.
	 */
	bsize = ci->num2 * 2;
	if (bsize > 1024*1024)
		bsize = 1024*1024;
	buf_init(&b);
	buf_resize(&b, bsize);
	for (i = 0; i < ci->num2 && b.len < 1024*1024-2; i++) {
		unsigned char cc = ci->str[i];
		buf_append(&b, c->tbl[cc]);
	}
	rv = comm_call(c->cb, ci->key, c->p,
		       c->tbl[ci->num & 0xff], ci->mark,
		       buf_final(&b), b.len,
		       NULL, NULL, ci->x, 0);
	if (rv <= 0) {
		/* None of the extra was consumed. Assume that will continue */
		c->noalloc = True;
		free(b.b);
		return rv;
	}
	if (rv >= b.len + 1) {
		/* All of the extra (that we decoded) was consumed */
		free(b.b);
		return i + 1;
	}
	/* Only some was consumed.  We needed to map back to number of bytes. */
	buf_reinit(&b);
	for (i = 0; i < ci->num2 && b.len < (rv-1); i++)
		buf_append(&b, c->tbl[(unsigned char)ci->str[i]]);
	free(b.b);
	return i + 1;
}

DEF_CMD(charset_content)
{
	struct charsetcb c;
	wchar_t *tbl = ci->home->data;

	if (!ci->comm2 || !ci->mark)
		return Enoarg;

	c.c = charset_content_cb;
	c.cb = ci->comm2;
	c.p = ci->focus;
	c.tbl = tbl;
	c.noalloc = False;
	return home_call_comm(ci->home->parent, ci->key, ci->home,
			      &c.c, 0, ci->mark, NULL, 0, ci->mark2);
}

static int charset_to_utf8(const struct cmd_info *ci safe, const wchar_t tbl[])
{
	struct buf b;
	const char *s;

	s = ci->str;
	if (!s || !ci->comm2)
		return Enoarg;
	buf_init(&b);
	while (*s) {
		buf_append(&b, tbl[*s & 0xff]);
		s += 1;
	}
	comm_call(ci->comm2, "cb", ci->focus, 0, NULL, buf_final(&b));
	free(b.b);
	return 1;
}

DEF_CMD(win1251_to_utf8)
{
	return charset_to_utf8(ci, WIN1251_UNICODE_TABLE);
}

DEF_CMD(win1252_to_utf8)
{
	return charset_to_utf8(ci, WIN1252_UNICODE_TABLE);
}

DEF_CMD(iso8859_1_to_utf8)
{
	return charset_to_utf8(ci, ISO_8859_1_UNICODE_TABLE);
}

DEF_CMD(iso8859_2_to_utf8)
{
	return charset_to_utf8(ci, ISO_8859_2_UNICODE_TABLE);
}

DEF_CMD(iso8859_15_to_utf8)
{
	return charset_to_utf8(ci, ISO_8859_15_UNICODE_TABLE);
}

DEF_CMD(win1251_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &charset_handle.c,
			  (wchar_t*) WIN1251_UNICODE_TABLE);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "cb", p);
}

DEF_CMD(win1252_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &charset_handle.c,
			  (wchar_t*)WIN1252_UNICODE_TABLE);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "cb", p);
}

DEF_CMD(iso8859_1_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &charset_handle.c,
			  (wchar_t*)ISO_8859_1_UNICODE_TABLE);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "cb", p);
}

DEF_CMD(iso8859_2_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &charset_handle.c,
			  (wchar_t*)ISO_8859_2_UNICODE_TABLE);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "cb", p);
}

DEF_CMD(iso8859_15_attach)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &charset_handle.c,
			  (wchar_t*)ISO_8859_15_UNICODE_TABLE);
	if (!p)
		return Efail;

	return comm_call(ci->comm2, "cb", p);
}

void edlib_init(struct pane *ed safe)
{
	charset_map = key_alloc();

	key_add(charset_map, "doc:char", &charset_char);
	key_add(charset_map, "doc:content", &charset_content);
	/* No doc:content-bytes - that wouldn't make sense */

	/* Use 1251 for any unknown 'windows' charset */
	call_comm("global-set-command", ed, &win1251_attach, 0, NULL,
		  "attach-charset-windows-", 0, NULL,
		  "attach-charset-windows.");
	call_comm("global-set-command", ed, &win1251_to_utf8, 0, NULL,
		  "charset-to-utf8-windows-", 0, NULL,
		  "charset-to-utf8-windows.");

	call_comm("global-set-command", ed, &win1252_attach, 0, NULL,
		"attach-charset-windows-1252");
	call_comm("global-set-command", ed, &win1252_to_utf8, 0, NULL,
		"charset-to-utf8-windows-1252");

	/* Use iso-8859-15 for any unknown iso-8859, and for ascii */
	call_comm("global-set-command", ed, &iso8859_15_attach, 0, NULL,
		  "attach-charset-iso-8859-", 0, NULL,
		  "attach-charset-iso-8859.");
	call_comm("global-set-command", ed, &iso8859_15_to_utf8, 0, NULL,
		  "charset-to-utf8-iso-8859-", 0, NULL,
		  "charset-to-utf8-iso-8859.");

	call_comm("global-set-command", ed, &iso8859_15_attach, 0, NULL,
		"attach-charset-us-ascii");
	call_comm("global-set-command", ed, &iso8859_15_to_utf8, 0, NULL,
		"charset-to-utf8-us-ascii");

	call_comm("global-set-command", ed, &iso8859_1_attach, 0, NULL,
		"attach-charset-iso-8859-1");
	call_comm("global-set-command", ed, &iso8859_1_to_utf8, 0, NULL,
		"charset-to-utf8-iso-8859-1");

	call_comm("global-set-command", ed, &iso8859_2_attach, 0, NULL,
		"attach-charset-iso-8859-2");
	call_comm("global-set-command", ed, &iso8859_2_to_utf8, 0, NULL,
		"charset-to-utf8-iso-8859-2");

}
