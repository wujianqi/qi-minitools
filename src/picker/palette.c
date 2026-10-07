/* palette.c - built-in palettes and the persisted favourites list. */
#include "palette.h"
#include <shlobj.h>

/* Compact initialiser helpers keep the tables readable. */
#define S(nm, r, g, b) { { r, g, b }, L"" nm }
#define SN(nm, r, g, b) { { r, g, b }, L"" nm }

/* ------------------------------------------------------------------ */
/* everyday / brand colours                                            */
/* ------------------------------------------------------------------ */

static const Swatch COMMON[] = {
    SN("微信绿",   7,  193, 100), SN("QQ蓝",     18, 183, 245),
    SN("钉钉蓝",  28, 132, 222), SN("淘宝橙",  255,  80,   0),
    SN("京东红",  227,  26,  26), SN("支付宝蓝", 22, 119, 255),
    SN("百度蓝",  49,  46, 129), SN("小红书红",255,  36,  66),
    SN("B站蓝",     0, 161, 214), SN("抖音黑",  22,  22,  22),
    SN("快手橙",  255,  90,  95), SN("微博红", 230,  22,  45),
    SN("GitHub",   36,  41,  46), SN("Google红",234,  67,  53),
    SN("Google黄",251, 188,   5), SN("Google绿", 52, 168,  83),
    SN("Google蓝", 66, 133, 244), SN("Facebook", 24, 119, 242),
    SN("Twitter",  29, 161, 242), SN("Netflix", 229,   9,  20),
    SN("Amazon橙",255, 153,   0), SN("Apple灰",  85,  85,  85),
    SN("Windows蓝", 0, 120, 212), SN("Office橙",243, 124,  32),
    SN("危险红",  220,  38,  38), SN("警告黄",  234, 179,   8),
    SN("成功绿",   34, 197,  94), SN("信息蓝",   59, 130, 246),
    SN("深灰",     31,  41,  55), SN("中灰",    107, 114, 128),
    SN("浅灰",    209, 213, 219), SN("背景灰",  243, 244, 246)
};

/* ------------------------------------------------------------------ */
/* CSS named colours                                                   */
/* ------------------------------------------------------------------ */

static const Swatch WEB[] = {
    SN("AliceBlue",240,248,255), SN("AntiqueWhite",250,235,215),
    SN("Aqua",       0,255,255), SN("Aquamarine",127,255,212),
    SN("Azure",    240,255,255), SN("Beige",    245,245,220),
    SN("Bisque",   255,228,196), SN("Black",      0,  0,  0),
    SN("BlanchedAlmond",255,235,205), SN("Blue",     0,  0,255),
    SN("BlueViolet",138, 43,226), SN("Brown",   165, 42, 42),
    SN("BurlyWood",222,184,135), SN("CadetBlue", 95,158,160),
    SN("Chartreuse",127,255,  0), SN("Chocolate",210,105, 30),
    SN("Coral",    255,127, 80), SN("CornflowerBlue",100,149,237),
    SN("Cornsilk", 255,248,220), SN("Crimson", 220, 20, 60),
    SN("Cyan",       0,255,255), SN("DarkBlue",   0,  0,139),
    SN("DarkCyan",   0,139,139), SN("DarkGoldenrod",184,134, 11),
    SN("DarkGray", 169,169,169), SN("DarkGreen",   0,100,  0),
    SN("DarkKhaki", 189,183,107), SN("DarkMagenta",139,  0,139),
    SN("DarkOliveGreen", 85,107, 47), SN("DarkOrange",255,140,  0),
    SN("DarkOrchid",153, 50,204), SN("DarkRed", 139,  0,  0),
    SN("DarkSalmon",233,150,122), SN("DarkSeaGreen",143,188,143),
    SN("DarkSlateBlue", 72, 61,139), SN("DarkSlateGray", 47, 79, 79),
    SN("DarkTurquoise",  0,206,209), SN("DarkViolet",148,  0,211),
    SN("DeepPink", 255, 20,147), SN("DeepSkyBlue",  0,191,255),
    SN("DimGray", 105,105,105), SN("DodgerBlue", 30,144,255),
    SN("FireBrick",178, 34, 34), SN("ForestGreen", 34,139, 34),
    SN("Fuchsia", 255,  0,255), SN("Gainsboro",220,220,220),
    SN("GhostWhite",248,248,255), SN("Gold",     255,215,  0),
    SN("Goldenrod",218,165, 32), SN("Gray",     128,128,128),
    SN("Green",      0,128,  0), SN("GreenYellow",173,255, 47),
    SN("Honeydew", 240,255,240), SN("HotPink", 255,105,180),
    SN("IndianRed", 205, 92, 92), SN("Indigo",   75,  0,130),
    SN("Ivory",     255,255,240), SN("Khaki",    240,230,140),
    SN("Lavender",  230,230,250), SN("LawnGreen",124,252,  0),
    SN("LemonChiffon",255,250,205), SN("LightBlue",173,216,230),
    SN("LightCoral",240,128,128), SN("LightCyan",224,255,255),
    SN("LightGray", 211,211,211), SN("LightGreen",144,238,144),
    SN("LightPink",255,182,193), SN("LightSalmon",255,160,122),
    SN("LightSeaGreen", 32,178,170), SN("LightSkyBlue",135,206,250),
    SN("LightSlateGray",119,136,153), SN("LightSteelBlue",176,196,222),
    SN("LightYellow",255,255,224), SN("Lime",      0,255,  0),
    SN("LimeGreen", 50,205, 50), SN("Linen",   250,240,230),
    SN("Magenta",  255,  0,255), SN("Maroon",  128,  0,  0),
    SN("MediumAquamarine",102,205,170), SN("MediumBlue",  0,  0,205),
    SN("MediumOrchid",186, 85,211), SN("MediumPurple",147,112,219),
    SN("MediumSeaGreen", 60,179,113), SN("MediumSlateBlue",123,104,238),
    SN("MediumSpringGreen",  0,250,154), SN("MediumTurquoise", 72,209,204),
    SN("MediumVioletRed",199, 21,133), SN("MidnightBlue", 25, 25,112),
    SN("MintCream",245,255,250), SN("MistyRose",255,228,225),
    SN("Moccasin",255,228,181), SN("NavajoWhite",255,222,173),
    SN("Navy",       0,  0,128), SN("OldLace", 253,245,230),
    SN("Olive",    128,128,  0), SN("OliveDrab",107,142, 35),
    SN("Orange",   255,165,  0), SN("OrangeRed",255, 69,  0),
    SN("Orchid",   218,112,214), SN("PaleGoldenrod",238,232,170),
    SN("PaleGreen",152,251,152), SN("PaleTurquoise",175,238,238),
    SN("PaleVioletRed",219,112,147), SN("PapayaWhip",255,239,213),
    SN("PeachPuff",255,218,185), SN("Peru",     205,133, 63),
    SN("Pink",     255,192,203), SN("Plum",     221,160,221),
    SN("PowderBlue",176,224,230), SN("Purple",  128,  0,128),
    SN("RebeccaPurple",102, 51,153), SN("Red",   255,  0,  0),
    SN("RosyBrown",188,143,143), SN("RoyalBlue", 65,105,225),
    SN("SaddleBrown",139, 69, 19), SN("Salmon", 250,128,114),
    SN("SandyBrown",244,164, 96), SN("SeaGreen", 46,139, 87),
    SN("Seashell",255,245,238), SN("Sienna", 160, 82, 45),
    SN("Silver", 192,192,192), SN("SkyBlue",135,206,235),
    SN("SlateBlue",106, 90,205), SN("SlateGray",112,128,144),
    SN("Snow",     255,250,250), SN("SpringGreen",  0,255,127),
    SN("SteelBlue", 70,130,180), SN("Tan",     210,180,140),
    SN("Teal",       0,128,128), SN("Thistle",216,191,216),
    SN("Tomato",   255, 99, 71), SN("Turquoise", 64,224,208),
    SN("Violet",   238,130,238), SN("Wheat",   245,222,179),
    SN("White",    255,255,255), SN("WhiteSmoke",245,245,245),
    SN("Yellow",   255,255,  0), SN("YellowGreen",154,205, 50)
};

/* ------------------------------------------------------------------ */
/* Material Design (2014)                                              */
/* ------------------------------------------------------------------ */

static const Swatch MATERIAL[] = {
    SN("Red",       244, 67,  54), SN("Pink",      233, 30,  99),
    SN("Purple",    156, 39, 176), SN("DeepPurple", 103, 58, 183),
    SN("Indigo",     63, 81, 181), SN("Blue",       33,150,243),
    SN("LightBlue",   3,169,244), SN("Cyan",       0,188,212),
    SN("Teal",        0,150,136), SN("Green",      76,175, 80),
    SN("LightGreen",129,199,132), SN("Lime",      205,220, 57),
    SN("Yellow",    255,235, 59), SN("Amber",     255,193,  7),
    SN("Orange",    255,152,  0), SN("DeepOrange",255, 87, 34),
    SN("Brown",     121, 85, 72), SN("Grey",      158,158,158),
    SN("BlueGrey",   96,125,139),
    SN("Red 50",   255,235,235), SN("Red 100",  255,204,204),
    SN("Red 200",  255,188,188), SN("Red 300",  255,174,174),
    SN("Red 400",  255,128,128), SN("Red 500",  255, 82, 82),
    SN("Red 600",  229, 57, 53), SN("Red 700",  211, 47, 47),
    SN("Red 800",  198, 40, 40), SN("Red 900",  183, 28, 28),
    SN("Blue 50",  227,242,253), SN("Blue 100", 187,222,251),
    SN("Blue 200", 147,197,253), SN("Blue 300",  96,165,250),
    SN("Blue 400",  66,133,244), SN("Blue 500",  33,150,243),
    SN("Blue 600",  25,118,210), SN("Blue 700",  21,101,192),
    SN("Blue 800",  16, 81,174), SN("Blue 900",  13, 71,161),
    SN("Green 50", 232,245,233), SN("Green 100",200,230,201),
    SN("Green 200",167,218,168), SN("Green 300",129,199,132),
    SN("Green 400",100,181,123), SN("Green 500", 76,175, 80),
    SN("Green 600", 56,142, 60), SN("Green 700", 46,125, 50),
    SN("Green 800", 27, 94, 32), SN("Green 900", 19, 77, 49),
    SN("Grey 50",  250,250,250), SN("Grey 100", 245,245,245),
    SN("Grey 200", 224,224,224), SN("Grey 300", 200,200,200),
    SN("Grey 400", 156,156,156), SN("Grey 500", 128,128,128),
    SN("Grey 600",  117,117,117), SN("Grey 700",  97, 97, 97),
    SN("Grey 800",  66, 66, 66), SN("Grey 900",  33, 33, 33)
};

/* ------------------------------------------------------------------ */
/* Tailwind subset                                                     */
/* ------------------------------------------------------------------ */

static const Swatch TAILWIND[] = {
    SN("slate 50", 248,250,252), SN("slate 100",241,245,249),
    SN("slate 200",226,232,240), SN("slate 300",203,213,225),
    SN("slate 400",148,163,184), SN("slate 500",100,116,139),
    SN("slate 600", 71, 85,105), SN("slate 700", 51, 65, 85),
    SN("slate 800", 30, 41, 59), SN("slate 900", 15, 23, 42),
    SN("red 400",  248,113,113), SN("red 500",  239, 68, 68),
    SN("red 600",  220, 38, 38),  SN("red 700",  185, 28, 28),
    SN("orange 400",251,146, 60), SN("orange 500",249,115, 22),
    SN("orange 600",234, 88, 12), SN("amber 400",251,191, 36),
    SN("amber 500",245,158, 11), SN("yellow 400",250,204, 21),
    SN("yellow 500",234,179,  8), SN("lime 400", 163,230, 53),
    SN("lime 500", 132,204, 22), SN("green 400", 74,222,128),
    SN("green 500", 34,197, 94), SN("green 600", 22,163, 74),
    SN("emerald 400",52,211,153), SN("emerald 500", 16,185,129),
    SN("emerald 600", 5,150,105), SN("teal 400",  45,212,191),
    SN("teal 500",  20,184,166), SN("teal 600",  13,148,136),
    SN("cyan 400",  34,211,238), SN("cyan 500",   6,182,212),
    SN("cyan 600",  8,145,178), SN("sky 400",    56,189,248),
    SN("sky 500",   14,165,233), SN("sky 600",    2,132,199),
    SN("blue 400",  96,165,250), SN("blue 500",  59,130,246),
    SN("blue 600",  37, 99,235), SN("indigo 400",129,140,248),
    SN("indigo 500", 99,102,241), SN("violet 400",167,139,250),
    SN("violet 500",139, 92,246), SN("purple 400",192,132,252),
    SN("purple 500",168, 85,247), SN("fuchsia 400",232,121,249),
    SN("pink 400", 244,114,182), SN("rose 400",  251,113,133),
    SN("rose 500", 244, 63, 94),  SN("stone 100",231,229,228),
    SN("stone 400",168,162,158), SN("stone 700", 87, 83, 78)
};

/* ------------------------------------------------------------------ */
/* theme palettes                                                      */
/*                                                                     */
/* Transcribed from the nine scheme boards in                            */
/* baijiahao.baidu.com/s?id=1722180419317727466 - each board is a 3x3   */
/* grid of ready-made three-colour combinations, printed with the       */
/* Chinese colour name, the hex and the GB/T 18922 standard-card code.  */
/*                                                                     */
/* Two deliberate departures from a literal transcription:              */
/*                                                                     */
/*  - Colours repeated inside one board are listed once. A grid of 27     */
/*    swatches where #f7c7c6 appears six times is not a palette, it is   */
/*    a repetition; the card order is kept so related hues stay next to  */
/*    each other.                                                        */
/*  - Board 6 labels one #f7c7c6 swatch "shuangbai" (a cool grey that is  */
/*    #d3d6d8 everywhere else it appears). The hex is the colour, so    */
/*    that swatch is named after what it actually is.                    */
/*                                                                     */
/* The odd-looking names (tengshu, yingse, yingmengmu, tuisui lan) are   */
/* not transcription slips - they are what those boards print.          */
/* Verified glyph by glyph against 3.5x crops; see                      */
/* test/zoom-theme-labels.ps1. Values cross-checked against per-board    */
/* pixel sampling in test/extract-article-themes.ps1, which is how the   */
/* qingbaiyu slip below was caught (#cac5a0, not #cac5d0).              */
/* ------------------------------------------------------------------ */

static const Swatch THEME_SOFT[] = {          /* 柔和、明亮、温柔 */
    SN("练色",   0xef,0xf1,0xd4), SN("水色",   0xc8,0xe7,0xe9),
    SN("红梅粉", 0xf7,0xc7,0xc6), SN("黄白游", 0xff,0xf7,0x99),
    SN("粉蓝",   0xd2,0xcc,0xe6), SN("蜜合色", 0xf8,0xc4,0x99),
    SN("远山紫", 0xc8,0xc8,0xd2), SN("淡天蓝", 0x94,0xc5,0xdc),
    SN("梅子青", 0xcb,0xe4,0xb9), SN("米红色", 0xf1,0x8f,0x69),
    SN("山茶红", 0xed,0x61,0x76), SN("欧碧",   0xc0,0xd6,0x95)
};

static const Swatch THEME_LIVELY[] = {        /* 活泼、快乐、有趣 */
    SN("凤仙粉", 0xd0,0x9c,0xa4), SN("女贞黄", 0xf7,0xee,0xad),
    SN("腾鼠",   0x5d,0x6f,0xa3), SN("萱草色", 0xf3,0x98,0x00),
    SN("稞草色", 0xf0,0xeb,0x45), SN("吐绥蓝", 0x41,0x82,0xa4),
    SN("山茶红", 0xea,0x61,0x76), SN("琥珀",   0xc4,0x66,0x23),
    SN("银鼠色", 0x9b,0x9e,0xa4), SN("鹦色",   0xce,0xd6,0x3c),
    SN("黄丹",   0xed,0x6d,0x1f), SN("杨桃",   0xf9,0xf2,0x7f),
    SN("冬瓜",   0x55,0x91,0x6d), SN("练色",   0xef,0xf1,0xd4),
    SN("青翠色", 0x00,0xa7,0x3c), SN("琥珀色", 0xcf,0x7c,0x34),
    SN("蜜合色", 0xf8,0xc4,0x99), SN("红踯躞", 0xb8,0x35,0x70),
    SN("纳户鼠", 0x00,0x8d,0x95), SN("中黄",   0xf4,0xc3,0x2b)
};

static const Swatch THEME_NATURAL[] = {       /* 希望、自然、健康 */
    SN("杨桃",   0xf9,0xf2,0x7f), SN("浅铬绿", 0x97,0xcf,0xac),
    SN("海松树叶",0x55,0x5f,0x41), SN("黄流",  0x9f,0x60,0x27),
    SN("葱绿",   0x8e,0xc5,0x4a), SN("苦瓜",   0xc1,0xce,0x88),
    SN("梅子青", 0xcb,0xe4,0xb9), SN("橄榄色", 0x86,0x81,0x21),
    SN("桑茶",   0xc9,0x96,0x39), SN("蒸栗色", 0xf6,0xef,0xc2),
    SN("银鼠色", 0x9b,0x9e,0xa4), SN("千岁绿", 0x26,0x5a,0x4f),
    SN("苗色",   0x7c,0xc1,0x6d), SN("欧碧",   0xc0,0xd6,0x95),
    SN("墨绿",   0x0a,0x28,0x13), SN("石发",   0x6a,0x8d,0x52),
    SN("青白玉", 0xca,0xc5,0xa0), SN("土黑色", 0x2f,0x29,0x17),
    SN("紫檀色", 0x5d,0x2d,0x22)
};

static const Swatch THEME_CLASSIC[] = {       /* 传统、神秘、优雅 */
    SN("柳茶",   0x92,0x97,0x57), SN("蒸栗色", 0xf6,0xef,0xc2),
    SN("木槿紫", 0xc4,0x90,0xbf), SN("红膜色", 0x91,0x70,0xaa),
    SN("黄粱",   0xc4,0xb7,0x98), SN("木贼色", 0x62,0x91,0x7a),
    SN("碧玉花", 0x46,0x8c,0x38), SN("远山紫", 0xc8,0xc8,0xd2),
    SN("兰紫色", 0x92,0x3d,0x92), SN("藤色",   0xbb,0xb3,0xd8),
    SN("黄封",   0xca,0xb2,0x72), SN("利休鼠", 0x69,0x74,0x6d),
    SN("凤仙粉", 0xd0,0x9c,0xa4), SN("银鼠色", 0x9b,0x9e,0xa4),
    SN("紫灰信子",0x68,0x3e,0x86), SN("枣褐",  0x62,0x33,0x20)
};

static const Swatch THEME_ELEGANT[] = {       /* 回味、优雅、女性化 */
    SN("霜白",   0xd3,0xd6,0xd8), SN("木槿紫", 0xc4,0x90,0xbf),
    SN("青莲",   0xbe,0x00,0x81), SN("齐紫",   0x6c,0x21,0x6d),
    SN("浓粉",   0xf0,0x91,0x92), SN("红梅粉", 0xf7,0xc7,0xc6),
    SN("莲红",   0xd9,0xa0,0xb3), SN("粉蓝",   0xd2,0xcc,0xe6),
    SN("灰蓝",   0x99,0xa7,0xc0), SN("蒸栗色", 0xf6,0xef,0xc2),
    SN("凤仙粉", 0xd0,0x9c,0xa4), SN("人参果", 0x87,0x66,0x94),
    SN("桔梗色", 0x67,0x44,0x98)
};

static const Swatch THEME_CLEAN[] = {         /* 简单、洁净、进步 */
    SN("远山紫", 0xc8,0xc8,0xd2), SN("玉白色", 0xff,0xfe,0xf7),
    SN("腾鼠",   0x5d,0x6f,0xa3), SN("淡天蓝", 0x94,0xc5,0xdc),
    SN("靛青",   0x00,0x76,0xb2), SN("栗华",   0xc0,0xad,0x5e),
    SN("露草",   0x00,0x9d,0xda), SN("蒸栗色", 0xf6,0xef,0xc2),
    SN("青花瓷", 0x1d,0x28,0x5c), SN("鹦萌木", 0x8f,0xc3,0x1f),
    SN("蓝玫瑰", 0x33,0x89,0xca), SN("欧碧",   0xc0,0xd6,0x95),
    SN("石涅",   0x68,0x6a,0x67), SN("石青",   0x29,0x4c,0x7a),
    SN("稞草色", 0xf0,0xeb,0x45)
};

static const Swatch THEME_RICH[] = {          /* 轻快、华丽、动态 */
    SN("红豆色", 0x8a,0x10,0x6d), SN("金雀花黄",0xf3,0xc5,0x17),
    SN("莓色",   0xba,0x0c,0x50), SN("吴须色", 0x26,0x49,0x9d),
    SN("鹅冠红", 0xe7,0x24,0x2e), SN("纳户鼠", 0x00,0x8d,0x95),
    SN("银星海棠",0xc1,0x38,0x57), SN("二蓝",  0x64,0x57,0x94),
    SN("稞草色", 0xf0,0xeb,0x45), SN("龙胆蓝", 0x02,0x4c,0x7e),
    SN("鹦色",   0xcf,0xdd,0x3c), SN("热带橙", 0xf3,0x99,0x39),
    SN("浓琉璃色",0x00,0x40,0x98), SN("黄丹",  0xed,0x6d,0x1f),
    SN("珊瑚赫", 0xc1,0x2c,0x1f), SN("红梅粉", 0xf7,0xc7,0xc6),
    SN("鹦萌木", 0x8f,0xc3,0x1f)
};

static const Swatch THEME_FEMININE[] = {      /* 华丽、花哨、女性化 */
    SN("红梅粉", 0xf7,0xc7,0xc6), SN("玉白色", 0xff,0xfe,0xf7),
    SN("魏紫",   0x90,0x37,0x54), SN("西瓜红", 0xe9,0x54,0x64),
    SN("铜青",   0x3d,0x8e,0x86), SN("钻绿",   0x6a,0xbe,0x79),
    SN("梅紫",   0xc1,0x4e,0x8e), SN("抚子",   0xf3,0xa9,0xc3),
    SN("浓琉璃色",0x00,0x40,0x98), SN("浅柳绿",0xc4,0xd7,0x00),
    SN("淡黄藤", 0xf7,0xed,0xa2), SN("紫三角梅",0x92,0x53,0x9d),
    SN("红紫色", 0xb7,0x00,0x7a), SN("蜜合色", 0xf8,0xc4,0x99),
    SN("山茶红", 0xea,0x61,0x76), SN("苦瓜",   0xc1,0xce,0x88),
    SN("齐紫",   0x6c,0x21,0x6d), SN("粉猫眼石",0xc4,0x7c,0xa0),
    SN("西南花", 0x8e,0xbb,0x74)
};

static const Swatch THEME_RATIONAL[] = {      /* 简单、进步、时尚 */
    SN("青花瓷", 0x1d,0x28,0x5c), SN("葱绿",   0x8e,0xc5,0x4a),
    SN("宝蓝",   0x42,0x5b,0xa8), SN("银鼠色", 0x9b,0x9e,0xa4),
    SN("空青",   0x66,0x8f,0x8b), SN("素鼠",   0x6e,0x71,0x6e),
    SN("远山紫", 0xc8,0xc8,0xd2), SN("熨斗日花",0x0a,0x5d,0x75),
    SN("铁铁",   0x1b,0x13,0x2a), SN("红碧",   0x65,0x81,0xc0),
    SN("柳茶",   0x92,0x97,0x57), SN("靛青",   0x00,0x76,0xb2),
    SN("千斋茶", 0x29,0x33,0x22), SN("栗华",   0xc0,0xed,0x5e)
};

/* The one-line rule each board's accompanying text states, in the author's
 * own terms but trimmed to a single readable line. Shown under the palette
 * tabs whenever a theme is selected. */
static const wchar_t *THEME_NOTES[PAL_THEME_COUNT] = {
    L"亮度高的色彩搭配在一起柔和明亮，用低亮度前景色调和，避免刺眼",
    L"选色更广泛，最重要的变化是把纯白换成低饱和有彩色或灰色",
    L"绿色是希望、自然、健康的主角，可搭配黄色与蓝色",
    L"传统文化里饱和度不高，配棕色很合适；紫色高雅、优雅又神秘",
    L"饱和度降下来，取蓝与红之间的相邻色相，调节明度与饱和度",
    L"色相上用蓝、绿表现，大面积留白，搭配低饱和甚至灰色",
    L"页面充斥有彩色，饱和度偏高，亮度适当减弱以强化印象",
    L"紫色和品红是主角，粉红、绿色也常用，彼此之间要高饱和搭配",
    L"以蓝色为主搭配灰色，明度统一、色相相邻，显得简洁"
};

const wchar_t *palette_note(PaletteId id)
{
    if (id < PAL_THEME_SOFT || id > PAL_THEME_RATIONAL) return NULL;
    return THEME_NOTES[id - PAL_THEME_SOFT];
}

int palette_is_theme(PaletteId id)
{
    return id >= PAL_THEME_SOFT && id <= PAL_THEME_RATIONAL;
}

/* ------------------------------------------------------------------ */
/* grayscale ramp + skin tones                                         */
/* ------------------------------------------------------------------ */

static Swatch g_grayStorage[11];
static const int  GRAY_STEPS[11] = { 0, 25, 50, 75, 100, 128, 150, 175,
                                      200, 225, 255 };
static int g_grayReady = 0;

static const Swatch SKIN[] = {
    SN("极浅", 255,241,225), SN("浅",    254,227,199),
    SN("象牙", 255,224,196), SN("标准",  244,208,176),
    SN("小麦", 231,191,153), SN("古铜",  207,163,113),
    SN("棕黑",  157,118, 83), SN("深棕",  123, 93, 66),
    SN("Negro",  80, 59, 42), SN("深黑",   56, 40, 28),
    SN("Fair",  255,219,172)
};

/* ------------------------------------------------------------------ */

static Swatch g_fav[128];
static int     g_favCount = 0;

static Palette PALETTES[PAL_COUNT] = {
    { L"fav",      L"我的收藏", NULL, 0 },
    { L"soft",     L"柔和",     THEME_SOFT,     (int)(sizeof(THEME_SOFT)     / sizeof(Swatch)) },
    { L"lively",   L"活泼",     THEME_LIVELY,   (int)(sizeof(THEME_LIVELY)   / sizeof(Swatch)) },
    { L"natural",  L"自然",     THEME_NATURAL,  (int)(sizeof(THEME_NATURAL)  / sizeof(Swatch)) },
    { L"classic",  L"传统",     THEME_CLASSIC,  (int)(sizeof(THEME_CLASSIC)  / sizeof(Swatch)) },
    { L"elegant",  L"优雅",     THEME_ELEGANT,  (int)(sizeof(THEME_ELEGANT)  / sizeof(Swatch)) },
    { L"clean",    L"简洁",     THEME_CLEAN,    (int)(sizeof(THEME_CLEAN)    / sizeof(Swatch)) },
    { L"rich",     L"华丽",     THEME_RICH,     (int)(sizeof(THEME_RICH)     / sizeof(Swatch)) },
    { L"feminine", L"女性",     THEME_FEMININE, (int)(sizeof(THEME_FEMININE) / sizeof(Swatch)) },
    { L"rational", L"理性",     THEME_RATIONAL, (int)(sizeof(THEME_RATIONAL) / sizeof(Swatch)) },
    { L"common",   L"常用",     COMMON,   (int)(sizeof(COMMON)   / sizeof(Swatch)) },
    { L"web",      L"网页",     WEB,      (int)(sizeof(WEB)      / sizeof(Swatch)) },
    { L"material", L"Material", MATERIAL, (int)(sizeof(MATERIAL) / sizeof(Swatch)) },
    { L"tailwind", L"Tailwind", TAILWIND, (int)(sizeof(TAILWIND) / sizeof(Swatch)) },
    { L"gray",     L"灰阶",     NULL, 0 },
    { L"skin",     L"肤色",     SKIN,    (int)(sizeof(SKIN)     / sizeof(Swatch)) }
};

static void ensure_gray(void)
{
    int i;
    if (g_grayReady) return;
    for (i = 0; i < 11; i++) {
        g_grayStorage[i].c.r = g_grayStorage[i].c.g = g_grayStorage[i].c.b =
            (BYTE)GRAY_STEPS[i];
        g_grayStorage[i].name[0] = 0;
        if (GRAY_STEPS[i] == 255)  wcsncpy(g_grayStorage[i].name, L"白", SWATCH_NAME_MAX - 1);
        else if (GRAY_STEPS[i] == 0) wcsncpy(g_grayStorage[i].name, L"黑", SWATCH_NAME_MAX - 1);
        else _snwprintf_s(g_grayStorage[i].name, SWATCH_NAME_MAX, _TRUNCATE,
                          L"%d", GRAY_STEPS[i]);
    }
    /* Every step carries its number as the name - 128 included. Blank names
     * are rendered as "未命名" (see the swatch tooltip), which is not what a
     * grey ramp wants to say about its own middle. */
    g_grayReady = 1;
}

const Palette *palette_get(PaletteId id)
{
    if (id < 0 || id >= PAL_COUNT) return &PALETTES[PAL_COMMON];
    ensure_gray();
    if (id == PAL_FAVORITE) {
        PALETTES[PAL_FAVORITE].items = g_fav;
        PALETTES[PAL_FAVORITE].count = g_favCount;
    } else if (id == PAL_GRAY) {
        PALETTES[PAL_GRAY].items = g_grayStorage;
        PALETTES[PAL_GRAY].count = 11;
    }
    return &PALETTES[id];
}

int palette_count(void) { return PAL_COUNT; }

PaletteId palette_next(PaletteId id)
{
    return (PaletteId)(((int)id + 1) % PAL_COUNT);
}

/* ------------------------------------------------------------------ */
/* favourites                                                          */
/* ------------------------------------------------------------------ */

int fav_count(void) { return g_favCount; }

const Swatch *fav_at(int i)
{
    if (i < 0 || i >= g_favCount) return NULL;
    return &g_fav[i];
}

int fav_add(Rgb c)
{
    int i;
    for (i = 0; i < g_favCount; i++)
        if (rgb_equal(g_fav[i].c, c)) return 0;
    if (g_favCount >= (int)(sizeof(g_fav) / sizeof(g_fav[0]))) return 0;
    g_fav[g_favCount].c = c;
    g_fav[g_favCount].name[0] = 0;
    _snwprintf_s(g_fav[g_favCount].name, SWATCH_NAME_MAX, _TRUNCATE,
                 L"#%02X%02X%02X", c.r, c.g, c.b);
    g_favCount++;
    return 1;
}

int fav_remove_at(int i)
{
    if (i < 0 || i >= g_favCount) return 0;
    memmove(&g_fav[i], &g_fav[i + 1],
            (size_t)(g_favCount - i - 1) * sizeof(Swatch));
    g_favCount--;
    return 1;
}

void fav_free(void) { g_favCount = 0; }

/* ------------------------------------------------------------------ */
/* persistence: a plain INI-ish text file under %APPDATA%               */
/* ------------------------------------------------------------------ */

static void fav_path(wchar_t *out, size_t cch)
{
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        wcsncpy(out, L"picker_fav.txt", cch - 1);
        out[cch - 1] = 0;
        return;
    }
    CreateDirectoryW(appdata, NULL);
    if (!PathCombineW(out, appdata, L"MuskTools")) {
        wcsncpy(out, L"picker_fav.txt", cch - 1);
        out[cch - 1] = 0;
        return;
    }
    CreateDirectoryW(out, NULL);
    PathCombineW(out, out, L"picker_fav.txt");
}

void fav_load(void)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    char buf[65536];
    DWORD got = 0, i = 0, n;

    g_favCount = 0;
    fav_path(path, MAX_PATH);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(f, buf, (DWORD)sizeof(buf) - 1, &got, NULL)) {
        CloseHandle(f);
        return;
    }
    CloseHandle(f);
    buf[got] = 0;

    while (i < got) {
        DWORD start = i, v;
        while (i < got && buf[i] != '\n') i++;
        n = i - start;
        while (n && (buf[start + n - 1] == '\r' || buf[start + n - 1] == ' '))
            n--;
        buf[start + n] = 0;
        if (n == 0 || buf[start] == '#') {
            if (i < got) i++;
            continue;
        }
        v = (DWORD)strtoul(buf + start, NULL, 16);
        {
            Rgb c;
            c.r = (BYTE)((v >> 16) & 0xFF);
            c.g = (BYTE)((v >> 8) & 0xFF);
            c.b = (BYTE)(v & 0xFF);
            fav_add(c);
        }
        if (i < got) i++;
    }
}

void fav_save(void)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    char buf[65536];
    DWORD written = 0, used = 0;
    int i;

    fav_path(path, MAX_PATH);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    used += (DWORD)_snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE,
                               "# MuskPicker favourites, one RRGGBB per line\n");
    for (i = 0; i < g_favCount && used + 16 < sizeof(buf); i++) {
        used += (DWORD)_snprintf_s(buf + used, sizeof(buf) - used, _TRUNCATE,
                                   "%02X%02X%02X\n",
                                   g_fav[i].c.r, g_fav[i].c.g, g_fav[i].c.b);
    }
    WriteFile(f, buf, used, &written, NULL);
    CloseHandle(f);
}
