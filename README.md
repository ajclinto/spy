spy
===

A terminal directory browser with vim keybindings.

Spy was originally developed by Side Effects Software (www.sidefx.com). This public domain source was re-written from scratch but uses the same general interface as the original.

## Building

Spy depends on the following low-level Linux libraries so to build you should have headers installed for these packages:
* ncurses
* tinfo
* readline

Build by running make:

    make

## Getting Started

To start spy, from a terminal run:

    ./spy

To exit, type 'q' or Ctrl-D.

Spy will start with a grid view of the files in the current working directory of the terminal where it was run.

To get the list of all installed key mappings press the '?' character.

## Navigating the directory tree

Basic navigation commands are the following (some will be familiar to vim users):
* 'j': Go down one file
* 'k': Go up one file
* 'h': Go left one file
* 'l': Go right one file
* 'Page Up': Go up one page
* 'Page Down': Go down one page
* 'Home': Go to the first file
* 'End': Go to the last file

Directory tree traversal:
* 'u': Go up one directory
* 'd': Navigate into the highlighted directory (or view the highlighted file)
* 'v': Open the highlighted file in vim

Spy keeps a memory of the highlighted file/directory in every directory that has previously been navigated, so it is possible to quickly navigate up and down the tree without the need to find your previous position in the list.

Searching will also be familiar to vim users:
* '/': Search for a file using a pattern
* 'n': Search for the next match
* 'N': Search for the previous match

To navigate to an arbitrary directory:
* 'g', 'J': Prompt for a directory to navigate to

## Interfacing with the terminal

Spy executes on top of a terminal, and provides a way to switch back and forth between that view:
* ',': Switch to terminal mode
* 'Enter':  Exit terminal mode and return to the directory view

Executing commands:
* '!', ';', ':': Switch to a command prompt. Enter a command then hit enter to execute it and show output in the terminal view.
* '.': Re-execute the previous command

Command prompt mode keeps a history like the terminal. To look at previous commands use the up and down arrows.

Like vim, commands executed within spy may use the '%' character to substitute the currently selected file within a command to avoid typing it out.

## Customizing

Spy may be customized using the .spyrc file (located in your home directory). A few useful customizations follow.

Coloring files by type (colors: blue black red purple cyan white green yellow):

    color -dir yellow
    color -x green
    color -ro purple
    color core blue
    color -link cyan

Single key shortcuts to navigate to a directory:

    map 1   jump    =~/projects/spy

3-way toggle to sort by file size or modification date:

    map y   detailtoggle

Single key shortcuts for commands that take a prompt:

    map c   prompt_interactive  qcd
    map b   prompt_interactive  rcd

Relaxing case:

    relaxprompt
    relaxsearch
    relaxcase

