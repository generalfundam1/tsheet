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

## Features

- Selection: drag, Shift-click, Ctrl-click areas, click column or row headers, the
  corner selects the used range. Ctrl-click a selected cell to unselect it.
- Formats: bold, highlight, alignment, date and time, decimal places, numbers in
  comma or space groups, scientific or engineering notation, and any prefix or
  suffix (UTF-8 or LaTeX). Choosing a format applies it to the whole selection, or
  removes it when every cell already has it.
- Copy and paste: plain, values, formulas, with formatting (HTML), or LaTeX. Pasted
  HTML and text keep their formats; "$1,234.50" and "12 kg" arrive as numbers with
  the prefix or suffix.
- Convert menu: 70 scientific unit conversions (length, mass, time, volume, area,
  pressure, energy, power, force, speed, temperature, angle, frequency), and
  "To unit..." for any unit the table knows.
- Graphs through gnuplot, in a window or as a PDF.
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
