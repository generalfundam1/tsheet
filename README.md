# tsheet

A spreadsheet for the linux terminal. Excel-like: click, drag, etc.
to select several areas, and menus that open on hover. Written in C with the C library only; the X11 clipboard is used when the
X11 headers are installed, and otherwise the terminal's OSC 52 clipboard is used.

![tsheet demo](demo.gif)

## Build and install

    make                    # builds ./tsheet
    make install            # installs to /usr/local/bin (PREFIX=... to change)
    make uninstall
    make clean
    make help               # lists the targets

Needs a C11 compiler. No other libraries. `libX11` is optional (clipboard).

    tsheet [file]           # open a file: .csv .tsv .xlsx .ods .json .xml or text
    tsheet --version

## Files

Open and save: CSV, TSV, JSON, XML, text; XLSX and ODS (open reads the first sheet,
save writes all of it). Merged cells, formats and formulas are kept.

Fortran double precision numbers read as numbers everywhere (cells, formulas,
files, paste, graphs): `1.5D+03`, `1.d2`, `2.0Q-5`, and the E-less `1.234567-100`
Fortran writes for three-digit exponents. Blanks around a number are ignored.
Opening a delimited file, pasting or typing gives such cells the Fortran number
format, with their mantissa digits (1.d2 shows as 1.D+02, 2.50D+01 as 2.50D+01;
more than 9 digits shows as many as the value needs). The D look comes only from
that format: Format > Numbers > Plain shows 1.5D+03 as 1500.
A number too wide for its column is shown with fewer digits, exponent kept (a
D stays a D), or as `####`; it is never cut short.

Delimited text (.csv, .txt, anything not otherwise recognised) has its delimiter
detected: tab, semicolon, pipe, comma or runs of spaces. Opening such a file asks
which delimiter to use, with the detected one highlighted and a preview of how the
first line splits; Enter opens, Esc cancels. File > Import turns the question off
("Ask for delimiter on open") or fixes one delimiter for every file. Saving a CSV
writes it back with the delimiter it was read with, unless Export sets another.

## Features

- Selection: drag, Shift-click, Ctrl-click areas, click column or row headers, the
  corner selects the used range. Ctrl-click a selected cell to unselect it.
- Formats: bold, highlight, alignment, date and time, decimal places, numbers in
  comma or space groups, scientific, engineering or Fortran notation (1.5D+03),
  and any prefix or suffix (UTF-8 or LaTeX). Choosing a format applies it to the
  whole selection, or removes it when every cell already has it.
- Copy and paste: plain, values, formulas, with formatting (HTML), or LaTeX. Pasted
  HTML and text keep their formats; "$1,234.50" and "12 kg" arrive as numbers with
  the prefix or suffix. Ctrl+V splits multi-line CSV-like text into cells, and
  Edit > Paste > "Delimited into rows" / "Delimited into columns" splits any text
  on its detected delimiter, laying each line out as a row or as a column (so
  "1 2 3 4" goes across a row or down a column).
- Convert menu: 70 scientific unit conversions (length, mass, time, volume, area,
  pressure, energy, power, force, speed, temperature, angle, frequency), and
  "To unit..." for any unit the table knows.
- Graphs through gnuplot, in a window or as a PDF. The selection need not be a
  rectangle: Ctrl-click columns (or parts of them) and the leftmost becomes X, the
  rest series. Cells pair by row when X covers them, otherwise by position; a lone
  column is plotted against 1, 2, 3. The gnuplot commands live in
  `~/.config/tsheet/plot` (or `$XDG_CONFIG_HOME/tsheet/plot`), one per line: edit
  it freely, tsheet rereads it before every graph. The plot line may use
  `@SERIES`, `@DATA`, `@N` and `@XNAME`, e.g.
  `plot for [i=2:@N+1] @DATA using 1:i with linespoints`. Tick labels can be
  Fortran style (1.5D+03): Graph > x format / y format, or right-click > Graph >
  Fortran labels X / Y.
- Undo and redo, sorting, find file, selection statistics in the status bar.

## Layout of the source

    app.c       the editor: keys, mouse, menus, commands
    ui.c        drawing: grid, menus, dialogs, status bar
    term.c      terminal and mouse input, output buffering
    sheet.c     cells, formulas, formats, number parsing
    io.c        CSV, TSV, JSON, XML and the writers
    read.c      reading XLSX and ODS (inflate, zip, XML scanning)
    html.c      HTML clipboard tables, in and out
    clip.c      clipboard: X11 or OSC 52
    graph.c     gnuplot
    units.c     unit table for the Convert menu
    config.h    menus: every menu item and its description lives here
    tsheet.h    shared types and declarations

Menus are data in `config.h`: add an item there and give it an action in `app.c`.

## Limits

- Only the first sheet of an XLSX or ODS file is read; charts and styles are not.
- A sheet holds up to 10000 rows and 702 columns (A to ZZ).
- Formulas use a syntax close to Excel's; see the help menu for the functions.

## Licence

GNU General Public License, version 2
