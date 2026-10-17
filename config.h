/* Menus and bindings. Edit and rebuild (suckless style).
   label NULL ends a list; label "-" is a separator; sub makes a submenu. */
#ifndef CONFIG_H
#define CONFIG_H
#include "tsheet.h"

/* Export: save the sheet in a format, or set how delimited text is written.
   Each item has a description, shown in the status bar while it is highlighted. */
static const MenuItem export_menu[] = {
    {"Save",                "Ctrl+S", A_SAVE,        NULL,
     "write to the current file, in its own format"},
    {"-",                   "",       0,             NULL, NULL},
    {"CSV (.csv)",          "",       A_SAVEAS_CSV,  NULL,
     "delimited text, one row per line; the delimiter is chosen below"},
    {"TSV (.tsv)",          "",       A_SAVEAS_TSV,  NULL,
     "tab-separated text, one row per line"},
    {"XLSX (.xlsx)",        "",       A_SAVEAS_XLSX, NULL,
     "Excel workbook with values and formulas; opens in Excel, LibreOffice, Google Sheets"},
    {"ODS (.ods)",          "",       A_SAVEAS_ODS,  NULL,
     "OpenDocument spreadsheet, for LibreOffice and other office suites"},
    {"XML (.xml)",          "",       A_SAVEAS_XML,  NULL,
     "flat XML with one element per cell and its position"},
    {"JSON (.json)",        "",       A_SAVEAS_JSON, NULL,
     "cells and merged blocks as JSON; opens back into tsheet"},
    {"-",                   "",       0,             NULL, NULL},
    {"Delimiter: comma",    "",       A_XOPT_COMMA,  NULL,
     "CSV fields separated by commas; fields with commas or quotes are quoted"},
    {"Delimiter: semicolon", "",      A_XOPT_SEMI,   NULL,
     "fields separated by semicolons, common where the comma is the decimal mark"},
    {"Delimiter: tab",      "",       A_XOPT_TAB,    NULL,
     "fields separated by tabs"},
    {"Delimiter: pipe",     "",       A_XOPT_PIPE,   NULL,
     "fields separated by |, for data that already has commas and tabs"},
    {"Delimiter: space",    "",       A_XOPT_SPACE,  NULL,
     "fields separated by spaces; a value with spaces is quoted, a blank cell is \"\""},
    {"-",                   "",       0,             NULL, NULL},
    {"CRLF line endings",   "",       A_XOPT_CRLF,   NULL,
     "end lines with CR LF (Windows) instead of LF; toggles"},
    {"Selection only",      "",       A_XOPT_SELONLY, NULL,
     "save just the selected block (one area); toggles"},
    {"Show settings",       "",       A_XOPT_SHOW,   NULL,
     "report the current export and import settings in the status bar"},
    {NULL}
};

/* Import: open files, and set how delimited text is read. */
static const MenuItem import_menu[] = {
    {"Open...",             "Ctrl+O", A_OPEN,        NULL,
     "open a file; text files with no known extension use the delimiter below"},
    {"Find file...",        "Ctrl+F", A_FIND,        NULL,
     "search the current folder for a file by name, or by *.ext"},
    {"-",                   "",       0,             NULL, NULL},
    {"Delimiter: comma",    "",       A_IMP_COMMA,   NULL,
     "read fields separated by commas, with quoted fields (RFC 4180)"},
    {"Delimiter: semicolon", "",      A_IMP_SEMI,    NULL,
     "read fields separated by semicolons"},
    {"Delimiter: tab",      "",       A_IMP_TAB,     NULL,
     "read fields separated by tabs"},
    {"Delimiter: pipe",     "",       A_IMP_PIPE,    NULL,
     "read fields separated by |"},
    {"Delimiter: space",    "",       A_IMP_SPACE,   NULL,
     "read fields separated by runs of spaces; extra spaces and blank ends are ignored"},
    {NULL}
};

static const MenuItem copy_menu[] = {
    {"Formulas and values", "",     A_COPY_RAW,    NULL, NULL},
    {"Values only",         "",     A_COPY_VALUES, NULL, NULL},
    {"With formatting",     "",     A_COPY_FMT,    NULL, NULL},
    {"As LaTeX table",      "",     A_COPY_LATEX,  NULL, NULL},
    {NULL}
};

static const MenuItem paste_menu[] = {
    {"Default",             "",     A_PASTE,            NULL, NULL},
    {"Values only",         "",     A_PASTE_VALUES,     NULL, NULL},
    {"Keep formulas",       "",     A_PASTE_FORMULAS,   NULL, NULL},
    {"With formatting",     "",     A_PASTE_FMT,        NULL, NULL},
    {"Transpose",           "",     A_PASTE_TRANSPOSE,  NULL, NULL},
    {NULL}
};

static const MenuItem fill_menu[] = {
    {"Down",                "Ctrl+D", A_FILL_DOWN,  NULL, NULL},
    {"Right",               "Ctrl+R", A_FILL_RIGHT, NULL, NULL},
    {NULL}
};

static const MenuItem file_menu[] = {
    {"New sheet",           "Ctrl+N", A_NEW,        NULL, NULL},
    {"Open...",             "Ctrl+O", A_OPEN,       NULL, NULL},
    {"Find file...",        "Ctrl+F", A_FIND,       NULL, NULL},
    {"Save",                "Ctrl+S", A_SAVE,       NULL, NULL},
    {"Import...",           "",       A_DLG_IMPORT, NULL,
     "open files and set how delimited text is read"},
    {"Export...",           "",       A_DLG_EXPORT, NULL,
     "save the sheet in another format, or set how delimited text is written"},
    {"Close sheet",         "Ctrl+W", A_CLOSE,      NULL, NULL},
    {"-",                   "",       0,            NULL, NULL},
    {"Quit",                "Ctrl+Q", A_QUIT,       NULL, NULL},
    {NULL}
};

static const MenuItem edit_menu[] = {
    {"Undo",                "Ctrl+Z", A_UNDO,       NULL, NULL},
    {"Redo",                "Ctrl+Y", A_REDO,       NULL, NULL},
    {"-",                   "",       0,            NULL, NULL},
    {"Cut",                 "Ctrl+X", A_CUT,        NULL, NULL},
    {"Copy",                "Ctrl+C", 0,            copy_menu, NULL},
    {"Paste",               "Ctrl+V", 0,            paste_menu, NULL},
    {"-",                   "",       0,            NULL, NULL},
    {"Clear contents",      "Del",    A_CLEAR,      NULL, NULL},
    {"Fill",                "",       0,            fill_menu, NULL},
    {"Select all",          "Ctrl+A", A_SELALL,     NULL, NULL},
    {"-",                   "",       0,            NULL, NULL},
    {"Sort ascending",      "",       A_SORT_ASC,   NULL, NULL},
    {"Sort descending",     "",       A_SORT_DESC,  NULL, NULL},
    {"-",                   "",       0,            NULL, NULL},
    {"Go to cell...",       "Ctrl+G", A_GOTO,       NULL, NULL},
    {NULL}
};

/* Format > Numbers: how numbers are grouped or written. The applied style shows *. */
static const MenuItem integer_menu[] = {
    {"Plain",               "",       A_INT_PLAIN,   NULL,
     "numbers as they are typed, no grouping"},
    {"Comma: 1,000",        "",       A_INT_COMMA,   NULL,
     "group the digits in threes with commas (1,234,567.5)"},
    {"Space: 1 000",        "",       A_INT_SPACE,   NULL,
     "group the digits in threes with spaces (1 234 567.5)"},
    {"Scientific: 1*10^3",  "",       A_INT_SCI,     NULL,
     "mantissa times a power of ten (1.234*10^3, 0.5*10^-3)"},
    {"Engineering: 1 k",    "",       A_INT_ENG,     NULL,
     "powers of a thousand with SI prefixes k to E (1.234 k); under 1000 shown plain"},
    {NULL}
};

/* Format: a pop-up. An option shows * when the whole selection has it; choosing it
   again removes it from the selection. */
static const MenuItem format_menu[] = {
    {"Bold",                "Ctrl+B", A_FMT_BOLD,    NULL,
     "toggle bold: removed when every cell is bold, added to all otherwise"},
    {"Highlight",           "",       A_FMT_HL,      NULL,
     "toggle the yellow highlight the same way; highlighted text is dark"},
    {"-",                   "",       0,             NULL, NULL},
    {"Align left",          "",       A_ALIGN_L,     NULL,
     "left-align the selection; shows as applied when every cell is left-aligned"},
    {"Center",              "",       A_ALIGN_C,     NULL,
     "centre the selection; shows as applied when every cell is centred"},
    {"Align right",         "",       A_ALIGN_R,     NULL,
     "right-align the selection; shows as applied when every cell is right-aligned"},
    {"Merge cells",         "",       A_MERGE,       NULL,
     "merge the selected block into one cell (one area only)"},
    {"Unmerge cells",       "",       A_UNMERGE,     NULL,
     "split the merged blocks in the selection back into cells"},
    {"-",                   "",       0,             NULL, NULL},
    {"Date/time format...", "",       A_FMT_DATE,    NULL,
     "show numbers as dates and times; removed when applied everywhere, else asks for a format"},
    {"Decimal places...",   "",       A_FMT_DEC,     NULL,
     "show numbers with fixed decimal places; removed when applied everywhere, else asks"},
    {"Prefix...",           "",       A_PREFIX,      NULL,
     "text before numbers, UTF-8 or LaTeX (\\$, \\euro); empty clears"},
    {"Suffix...",           "",       A_SUFFIX,      NULL,
     "text after numbers, UTF-8 or LaTeX (\\%, \\text{kg}, m^{2}); empty clears"},
    {"Numbers",             "",       0,             integer_menu,
     "how numbers are grouped or written, whole or with decimals"},
    {"Unix time (clear)",   "",       A_FMT_CLEAR,   NULL,
     "clear the number format: numbers show as they are stored, no prefix or suffix"},
    {"-",                   "",       0,             NULL, NULL},
    {"Resize mode",         "F3",     A_RESIZE,      NULL,
     "change column widths and row heights with the arrow keys"},
    {NULL}
};

static const MenuItem sheet_menu[] = {
    {"New sheet",           "Ctrl+N", A_NEW,        NULL, NULL},
    {"Next sheet",          "Ctrl+PgDn", A_NEXT_SHEET, NULL, NULL},
    {"Previous sheet",      "Ctrl+PgUp", A_PREV_SHEET, NULL, NULL},
    {NULL}
};

static const MenuItem help_menu[] = {
    {"Keys",                "F1",     A_HELP,       NULL, NULL},
    {NULL}
};

/* Background of the cell under the mouse pointer (an SGR parameter). */
#define HOVER_SGR "0;100"

/* Background of highlighted cells (Format > Highlight), an SGR parameter. */
#define HIGHLIGHT_SGR "43"

/* Text colours by kind of value. Each entry is an SGR parameter: 30-37 are the
   terminal's normal colours and 90-97 its bright ones, so they follow the
   terminal's own colour scheme. 39 is the terminal's default foreground.
   Order matches KIND_TEXT, KIND_NUMBER, KIND_TIME, KIND_FORMULA, KIND_ERROR. */
static const char *kind_fg[KIND_COUNT] = {
    "39",   /* text: default foreground */
    "34",   /* numbers: blue */
    "35",   /* dates and times: magenta */
    "32",   /* formulas: green */
    "31",   /* errors: red */
};

/* Graph settings. The command field starts with GRAPH_DEFAULTS. Every option
   below is a flag, an argument or a choice; the editor, the context menu and the
   command preview are all built from this table. Order matters: GO_* indexes it.
   Descriptions are taken from the gnuplot documentation (help text). */
#define GRAPH_DEFAULTS "set key top right; set style data lines; set colorsequence classic"

static const char *const legend_choices[] = {"top right", "top left", "bottom right",
    "bottom left", "outside", "off", NULL};
static const char *const style_choices[]  = {"lines", "points", "linespoints", "boxes", NULL};
static const char *const color_choices[]  = {"classic", "default", "podo", NULL};

enum { GO_LOGX, GO_LOGY, GO_GRID, GO_LEGEND, GO_STYLE, GO_FILL, GO_COLOR,
       GO_TITLE, GO_XLABEL, GO_YLABEL, GO_XRANGE, GO_YRANGE, GO_XTICS, GO_YTICS,
       GO_XFMT, GO_YFMT, GO_POINTSIZE, GO_BOXWIDTH, GO_SAMPLES, GO_TIMEFMT, GO_COUNT };

static const GraphOpt graph_opts[GO_COUNT] = {
    {"log x",          GO_FLAG,   "set logscale x", NULL, NULL,
     "Logarithmic scale on the x axis. The default base is 10."},
    {"log y",          GO_FLAG,   "set logscale y", NULL, NULL,
     "Logarithmic scale on the y axis. The default base is 10."},
    {"grid",           GO_FLAG,   "set grid", NULL, NULL,
     "Draw grid lines at the major tick marks."},
    {"legend",         GO_CHOICE, "set key %s", "unset key", legend_choices,
     "The key: a title and a sample for each plot. Choose its position, or off."},
    {"plot style",     GO_CHOICE, "set style data %s", NULL, style_choices,
     "Default style for data plots: lines, points, linespoints or boxes."},
    {"fill boxes",     GO_FLAG,   "set style fill solid 0.5", NULL, NULL,
     "Default fill style for boxes, histograms and other filled plots."},
    {"colour sequence", GO_CHOICE, "set colorsequence %s", NULL, color_choices,
     "Repeating sequence of colours for successive plots. classic is the default."},
    {"title",          GO_ARG,    "set title '%s'", NULL, NULL,
     "A title, centred at the top of the plot."},
    {"x label",        GO_ARG,    "set xlabel '%s'", NULL, NULL,
     "The label of the x axis."},
    {"y label",        GO_ARG,    "set ylabel '%s'", NULL, NULL,
     "The label of the y axis."},
    {"x range",        GO_ARG,    "set xrange [%s]", NULL, NULL,
     "The horizontal range shown, e.g. 0:10. Use *:* for automatic scaling."},
    {"y range",        GO_ARG,    "set yrange [%s]", NULL, NULL,
     "The vertical range shown, e.g. 0:100. Use *:* for automatic scaling."},
    {"x tics",         GO_ARG,    "set xtics %s", NULL, NULL,
     "Major tick marks on the x axis, e.g. 1, or auto."},
    {"y tics",         GO_ARG,    "set ytics %s", NULL, NULL,
     "Major tick marks on the y axis, e.g. 10, or auto."},
    {"x format",       GO_ARG,    "set format x '%s'", NULL, NULL,
     "printf-style format of the x tick labels, e.g. %.1f."},
    {"y format",       GO_ARG,    "set format y '%s'", NULL, NULL,
     "printf-style format of the y tick labels, e.g. %.1f."},
    {"point size",     GO_ARG,    "set pointsize %s", NULL, NULL,
     "Scale factor for point markers, e.g. 1.5."},
    {"box width",      GO_ARG,    "set boxwidth %s", NULL, NULL,
     "Width of boxes. Add relative for a fraction of the spacing, e.g. 0.8 relative."},
    {"samples",        GO_ARG,    "set samples %s", NULL, NULL,
     "Number of sample points used when plotting functions, e.g. 1000."},
    {"time format",    GO_ARG,    "set timefmt '%s'", NULL, NULL,
     "Format of time values in the data file, e.g. %s for unix seconds."},
};

/* graph items in the right-click menu (flags and quick choices) */
static const MenuItem graph_ctx[] = {
    {"Draw graph (F9)",     "F9",     A_GRAPH,                        NULL, NULL},
    {"View in gnuplot window", "",    A_GRAPH_VIEW,                   NULL, NULL},
    {"Log scale X",         "",       A_GOPT_BASE + GO_LOGX,          NULL, NULL},
    {"Log scale Y",         "",       A_GOPT_BASE + GO_LOGY,          NULL, NULL},
    {"Grid",                "",       A_GOPT_BASE + GO_GRID,          NULL, NULL},
    {"Legend on/off",       "",       A_GOPT_BASE + GO_LEGEND,        NULL, NULL},
    {"Fill boxes",          "",       A_GOPT_BASE + GO_FILL,          NULL, NULL},
    {"Style: lines",        "",       A_GOPT_VAL + GO_STYLE * 32 + 0, NULL, NULL},
    {"Style: points",       "",       A_GOPT_VAL + GO_STYLE * 32 + 1, NULL, NULL},
    {"Style: boxes",        "",       A_GOPT_VAL + GO_STYLE * 32 + 3, NULL, NULL},
    {"-",                   "",       0,                              NULL, NULL},
    {"All options...",      "",       A_GRAPH_OPTIONS,                NULL, NULL},
    {NULL}
};

static const MenuItem options_menu[] = {
    {"Vim keys",            "",       A_VIM,         NULL, NULL},
    {NULL}
};

/* start with vim keys on (also: tsheet --vim) */
static const int vim_default = 0;

/* the top menu bar */
/* Convert menu: unit conversions for the selection. A cell with a unit as its
   suffix converts from that unit; a plain number converts from the "from" unit of
   the item. Formulas keep working. The number of an item is its index in conv_pairs
   (A_CONV_BASE + index); keep the two in step. */
static const char *const conv_pairs[][2] = {
    {"m", "ft"},
    {"ft", "m"},
    {"in", "cm"},
    {"cm", "in"},
    {"mi", "km"},
    {"km", "mi"},
    {"nmi", "km"},
    {"yd", "m"},
    {"lb", "kg"},
    {"kg", "lb"},
    {"oz", "g"},
    {"g", "oz"},
    {"st", "kg"},
    {"grain", "mg"},
    {"h", "min"},
    {"min", "s"},
    {"d", "h"},
    {"yr", "d"},
    {"s", "ms"},
    {"wk", "d"},
    {"L", "gal"},
    {"gal", "L"},
    {"mL", "floz"},
    {"floz", "mL"},
    {"ft3", "L"},
    {"L", "ft3"},
    {"qt", "L"},
    {"ha", "acre"},
    {"acre", "m2"},
    {"ft2", "m2"},
    {"m2", "ft2"},
    {"km2", "mi2"},
    {"mi2", "km2"},
    {"bar", "psi"},
    {"psi", "kPa"},
    {"atm", "kPa"},
    {"kPa", "psi"},
    {"mmHg", "kPa"},
    {"bar", "atm"},
    {"torr", "Pa"},
    {"eV", "J"},
    {"J", "cal"},
    {"cal", "J"},
    {"kWh", "J"},
    {"BTU", "J"},
    {"J", "eV"},
    {"kcal", "kJ"},
    {"hp", "kW"},
    {"kW", "hp"},
    {"W", "BTU/h"},
    {"lbf", "N"},
    {"N", "lbf"},
    {"kgf", "N"},
    {"km/h", "m/s"},
    {"m/s", "km/h"},
    {"mph", "km/h"},
    {"km/h", "mph"},
    {"kn", "km/h"},
    {"mph", "m/s"},
    {"m/s", "mph"},
    {"°C", "°F"},
    {"°F", "°C"},
    {"°C", "K"},
    {"K", "°C"},
    {"°F", "K"},
    {"deg", "rad"},
    {"rad", "deg"},
    {"rpm", "Hz"},
    {"Hz", "rpm"},
    {"GHz", "MHz"},
};

static const MenuItem conv_length[] = {
    {"m to ft", "", A_CONV_BASE + 0, NULL,
     "1 m = 3.28084 ft"},
    {"ft to m", "", A_CONV_BASE + 1, NULL,
     "1 ft = 0.3048 m"},
    {"in to cm", "", A_CONV_BASE + 2, NULL,
     "1 in = 2.54 cm"},
    {"cm to in", "", A_CONV_BASE + 3, NULL,
     "1 cm = 0.393701 in"},
    {"mi to km", "", A_CONV_BASE + 4, NULL,
     "1 mi = 1.60934 km"},
    {"km to mi", "", A_CONV_BASE + 5, NULL,
     "1 km = 0.621371 mi"},
    {"nmi to km", "", A_CONV_BASE + 6, NULL,
     "1 nmi = 1.852 km"},
    {"yd to m", "", A_CONV_BASE + 7, NULL,
     "1 yd = 0.9144 m"},
    {NULL}
};

static const MenuItem conv_mass[] = {
    {"lb to kg", "", A_CONV_BASE + 8, NULL,
     "1 lb = 0.453592 kg"},
    {"kg to lb", "", A_CONV_BASE + 9, NULL,
     "1 kg = 2.20462 lb"},
    {"oz to g", "", A_CONV_BASE + 10, NULL,
     "1 oz = 28.3495 g"},
    {"g to oz", "", A_CONV_BASE + 11, NULL,
     "1 g = 0.035274 oz"},
    {"st to kg", "", A_CONV_BASE + 12, NULL,
     "1 st = 6.35029 kg"},
    {"grain to mg", "", A_CONV_BASE + 13, NULL,
     "1 grain = 64.7989 mg"},
    {NULL}
};

static const MenuItem conv_time[] = {
    {"h to min", "", A_CONV_BASE + 14, NULL,
     "1 h = 60 min"},
    {"min to s", "", A_CONV_BASE + 15, NULL,
     "1 min = 60 s"},
    {"d to h", "", A_CONV_BASE + 16, NULL,
     "1 d = 24 h"},
    {"yr to d", "", A_CONV_BASE + 17, NULL,
     "1 yr = 365.25 d"},
    {"s to ms", "", A_CONV_BASE + 18, NULL,
     "1 s = 1000 ms"},
    {"wk to d", "", A_CONV_BASE + 19, NULL,
     "1 wk = 7 d"},
    {NULL}
};

static const MenuItem conv_volume[] = {
    {"L to gal", "", A_CONV_BASE + 20, NULL,
     "1 L = 0.264172 gal"},
    {"gal to L", "", A_CONV_BASE + 21, NULL,
     "1 gal = 3.78541 L"},
    {"mL to floz", "", A_CONV_BASE + 22, NULL,
     "1 mL = 0.033814 floz"},
    {"floz to mL", "", A_CONV_BASE + 23, NULL,
     "1 floz = 29.5735 mL"},
    {"ft3 to L", "", A_CONV_BASE + 24, NULL,
     "1 ft3 = 28.3168 L"},
    {"L to ft3", "", A_CONV_BASE + 25, NULL,
     "1 L = 0.0353147 ft3"},
    {"qt to L", "", A_CONV_BASE + 26, NULL,
     "1 qt = 0.946353 L"},
    {NULL}
};

static const MenuItem conv_area[] = {
    {"ha to acre", "", A_CONV_BASE + 27, NULL,
     "1 ha = 2.47105 acre"},
    {"acre to m2", "", A_CONV_BASE + 28, NULL,
     "1 acre = 4046.86 m2"},
    {"ft2 to m2", "", A_CONV_BASE + 29, NULL,
     "1 ft2 = 0.092903 m2"},
    {"m2 to ft2", "", A_CONV_BASE + 30, NULL,
     "1 m2 = 10.7639 ft2"},
    {"km2 to mi2", "", A_CONV_BASE + 31, NULL,
     "1 km2 = 0.386102 mi2"},
    {"mi2 to km2", "", A_CONV_BASE + 32, NULL,
     "1 mi2 = 2.58999 km2"},
    {NULL}
};

static const MenuItem conv_pressure[] = {
    {"bar to psi", "", A_CONV_BASE + 33, NULL,
     "1 bar = 14.5038 psi"},
    {"psi to kPa", "", A_CONV_BASE + 34, NULL,
     "1 psi = 6.89476 kPa"},
    {"atm to kPa", "", A_CONV_BASE + 35, NULL,
     "1 atm = 101.325 kPa"},
    {"kPa to psi", "", A_CONV_BASE + 36, NULL,
     "1 kPa = 0.145038 psi"},
    {"mmHg to kPa", "", A_CONV_BASE + 37, NULL,
     "1 mmHg = 0.133322 kPa"},
    {"bar to atm", "", A_CONV_BASE + 38, NULL,
     "1 bar = 0.986923 atm"},
    {"torr to Pa", "", A_CONV_BASE + 39, NULL,
     "1 torr = 133.322 Pa"},
    {NULL}
};

static const MenuItem conv_energy[] = {
    {"eV to J", "", A_CONV_BASE + 40, NULL,
     "1 eV = 1.60218e-19 J"},
    {"J to cal", "", A_CONV_BASE + 41, NULL,
     "1 J = 0.239006 cal"},
    {"cal to J", "", A_CONV_BASE + 42, NULL,
     "1 cal = 4.184 J"},
    {"kWh to J", "", A_CONV_BASE + 43, NULL,
     "1 kWh = 3.6e+06 J"},
    {"BTU to J", "", A_CONV_BASE + 44, NULL,
     "1 BTU = 1055.06 J"},
    {"J to eV", "", A_CONV_BASE + 45, NULL,
     "1 J = 6.24151e+18 eV"},
    {"kcal to kJ", "", A_CONV_BASE + 46, NULL,
     "1 kcal = 4.184 kJ"},
    {NULL}
};

static const MenuItem conv_power[] = {
    {"hp to kW", "", A_CONV_BASE + 47, NULL,
     "1 hp = 0.7457 kW"},
    {"kW to hp", "", A_CONV_BASE + 48, NULL,
     "1 kW = 1.34102 hp"},
    {"W to BTU/h", "", A_CONV_BASE + 49, NULL,
     "1 W = 3.41214 BTU/h"},
    {NULL}
};

static const MenuItem conv_force[] = {
    {"lbf to N", "", A_CONV_BASE + 50, NULL,
     "1 lbf = 4.44822 N"},
    {"N to lbf", "", A_CONV_BASE + 51, NULL,
     "1 N = 0.224809 lbf"},
    {"kgf to N", "", A_CONV_BASE + 52, NULL,
     "1 kgf = 9.80665 N"},
    {NULL}
};

static const MenuItem conv_speed[] = {
    {"km/h to m/s", "", A_CONV_BASE + 53, NULL,
     "1 km/h = 0.277778 m/s"},
    {"m/s to km/h", "", A_CONV_BASE + 54, NULL,
     "1 m/s = 3.6 km/h"},
    {"mph to km/h", "", A_CONV_BASE + 55, NULL,
     "1 mph = 1.60934 km/h"},
    {"km/h to mph", "", A_CONV_BASE + 56, NULL,
     "1 km/h = 0.621371 mph"},
    {"kn to km/h", "", A_CONV_BASE + 57, NULL,
     "1 kn = 1.852 km/h"},
    {"mph to m/s", "", A_CONV_BASE + 58, NULL,
     "1 mph = 0.44704 m/s"},
    {"m/s to mph", "", A_CONV_BASE + 59, NULL,
     "1 m/s = 2.23694 mph"},
    {NULL}
};

static const MenuItem conv_temperature[] = {
    {"°C to °F", "", A_CONV_BASE + 60, NULL,
     "0 °C = 32 °F, offsets applied"},
    {"°F to °C", "", A_CONV_BASE + 61, NULL,
     "0 °F = -17.7778 °C, offsets applied"},
    {"°C to K", "", A_CONV_BASE + 62, NULL,
     "0 °C = 273.15 K, offsets applied"},
    {"K to °C", "", A_CONV_BASE + 63, NULL,
     "0 K = -273.15 °C, offsets applied"},
    {"°F to K", "", A_CONV_BASE + 64, NULL,
     "0 °F = 255.372 K, offsets applied"},
    {NULL}
};

static const MenuItem conv_angle[] = {
    {"deg to rad", "", A_CONV_BASE + 65, NULL,
     "1 deg = 0.0174533 rad"},
    {"rad to deg", "", A_CONV_BASE + 66, NULL,
     "1 rad = 57.2958 deg"},
    {NULL}
};

static const MenuItem conv_frequency[] = {
    {"rpm to Hz", "", A_CONV_BASE + 67, NULL,
     "1 rpm = 0.0166667 Hz"},
    {"Hz to rpm", "", A_CONV_BASE + 68, NULL,
     "1 Hz = 60 rpm"},
    {"GHz to MHz", "", A_CONV_BASE + 69, NULL,
     "1 GHz = 1000 MHz"},
    {NULL}
};

static const MenuItem convert_menu[] = {
    {"To unit...",          "",  A_CONV_TO, NULL,
     "type a unit: ft converts the selection to feet; m ft says the plain numbers are metres"},
    {"-",                   "",  0,         NULL, NULL},
    {"Length",             "", 0,         conv_length,
     "metres, feet, inches, miles, nautical miles"},
    {"Mass",             "", 0,         conv_mass,
     "kilograms, pounds, ounces, stones, grains"},
    {"Time",             "", 0,         conv_time,
     "seconds, minutes, hours, days, years"},
    {"Volume",             "", 0,         conv_volume,
     "litres, gallons, fluid ounces, cubic feet"},
    {"Area",             "", 0,         conv_area,
     "square metres, acres, hectares, square feet"},
    {"Pressure",             "", 0,         conv_pressure,
     "pascals, bar, psi, atmospheres, mmHg"},
    {"Energy",             "", 0,         conv_energy,
     "joules, calories, electronvolts, kWh, BTU"},
    {"Power",             "", 0,         conv_power,
     "watts, horsepower, BTU per hour"},
    {"Force",             "", 0,         conv_force,
     "newtons, pound-force, kilogram-force"},
    {"Speed",             "", 0,         conv_speed,
     "m/s, km/h, mph, knots"},
    {"Temperature",             "", 0,         conv_temperature,
     "Celsius, Fahrenheit, kelvin (offsets applied)"},
    {"Angle",             "", 0,         conv_angle,
     "radians, degrees"},
    {"Frequency",             "", 0,         conv_frequency,
     "hertz, rpm, GHz"},
    {NULL}
};

static const MenuBar menubar[] = {
    {"File",    file_menu},
    {"Edit",    edit_menu},
    {"Format",  format_menu},
    {"Convert", convert_menu},
    {"Graph",   NULL},                     /* the all-options dialog */
    {"Options", options_menu},
    {"Sheet",   sheet_menu},
    {"Help",    help_menu},
    {NULL, NULL}
};

/* right-click menu on the grid */
static const MenuItem context_menu[] = {
    {"Cut",                 "Ctrl+X", A_CUT,        NULL, NULL},
    {"Copy",                "Ctrl+C", 0,            copy_menu, NULL},
    {"Paste",               "Ctrl+V", 0,            paste_menu, NULL},
    {"-",                   "",       0,            NULL, NULL},
    {"Fill",                "",       0,            fill_menu, NULL},
    {"Graph",               "",       0,            graph_ctx, NULL},
    {"Clear contents",      "Del",    A_CLEAR,      NULL, NULL},
    {NULL}
};

#endif
