# Laptop progress captures

216 actual EmbLinkOS captures document the original laptop work, its
integration with master through `a281865`, system-menu polish after PR #40,
recoverable document workspaces, the Contour desktop redesign, keyboard
interaction, private terminal sessions, VT screen emulation, process compatibility, ordered POSIX spawn, spawn-based sh, shared descriptor I/O, managed system upgrades, live display configuration, several monitors, GPU composition, the native shell running any program and a real Terminal: keys, the shells' own line editing, an xterm-compatible screen, its own fonts and renderer, tabs, selection, find and links, and profiles with Preferences and windows that show through; and then the prompt, Powerlevel10k-style: rc.esh, a right side, a transient prompt, Powerlevel10k's own engine drawing a Mac's configuration and every style, git status read by the system's own daemon, Powerlevel10k's configuration wizard, and the line suggested from history and coloured as it is typed; and then the desktop measuring what composing it costs, keeping time at 120 Hz, windows moving on springs, windows casting real shadows, glass you can see through, and a new face. PNG exports preserve
original pixels and dimensions; no retouching, reconstruction or upscaling.
All are QEMU captures. They do not establish physical laptop compatibility.

Use [gallery.json](gallery.json) for ordering, captions, dimensions, source and
PNG hashes, fixture flags, featured images and historical/current metadata.
Captures 01–20 are historical (`c90c478`, 800 × 600). Captures 21–40 show the
merged OS; desktop captures are 1280 × 800, recovery captures 800 × 600.
The merged-desktop guest (21–38) includes a **synthetic ACPI fixture** throughout;
backlight, battery and lid readings do not come from physical hardware.

## Latest: the desktop measures itself, then keeps time

The first step of the fluid desktop: before anything is made faster, the compositor says what composing costs -- every compose timed and charged to the cause that asked for it and the window that presented, and the pointer's motion timed to the screen ([DESKTOP_PERFORMANCE.md](../../DESKTOP_PERFORMANCE.md)). The second gives it a frame clock: one compose a frame, at a rate that rises to the display's top under a hand and falls back to 60, on a beat kept to the nanosecond.

### What composing costs, on the screen it costs

GUI+Shift+P: the compositor's own account at the top right -- CPU composing 1280 × 800, composes and megapixels in the last second, compose time (median, 95th percentile, worst), the pointer's motion-to-screen latency, and composes by cause. The baseline at 241cb9d, after `tools/perf_smoke.py`'s five scenes, under TCG.

![The EmbLinkOS desktop with Settings open and the compositor statistics overlay at the top right](208-perf-hud.png)

### Nothing to compose, so nothing composed

The same five scenes after P0: the overlay reads 0 composes a second on a desktop where nothing changes, where the baseline composed ten. The pixel loops run -O2, and nothing is presented that did not change. Under TCG.

![The EmbLinkOS desktop with Settings open and the compositor statistics overlay reading 0 composes a second](209-perf-after-p0.png)

### A real core composing

arm64 under HVF, on the host's own core: window motion at about 100 frames a second (A31 -- 31 frames to open and close a Terminal), a compose median of 2.7 ms in that scene, the pointer reaching the screen in 0.2 ms. The text layout here is wrong -- icons below their labels, the top bar cut off -- and was before P0; it is recorded in [TODO.md](../../TODO.md).

![The EmbLinkOS desktop on arm64 with the compositor statistics overlay; Settings' sidebar icons sit below their labels](210-perf-arm64-hvf.png)

### On the beat at 120 Hz

The frame clock told the display is a fixed 120 Hz one (`perf display 120`) and keeping the beat (`perf beat`): the overlay reads 120 of 120 Hz, 120 frames in the last second, a beat of 8.33 ms with a 95th percentile of 9.53. Under TCG; arm64 under HVF keeps the same beat to a p95 of 8.8 ms ([the beat](../../DESKTOP_PERFORMANCE.md#the-beat)).

![The EmbLinkOS desktop with Settings open and the compositor statistics overlay reading 120/120Hz, 120 fps, beat 8.33 ms](211-frame-clock-120.png)

### A window on its way to the dock

Settings minimised with GUI+M, photographed in flight: the ghost of its window, fully opaque, on four springs toward its own tile in the dock -- the tile the desktop told the compositor about. Slowed ten times (`perf slowmo 10`) to be photographed; the motion is the same. Under TCG.

![The EmbLinkOS desktop with a shrunken copy of the Settings window in mid-air between where it was and the dock](212-park-in-flight.png)

### Into its own tile

A moment later: the window over the Settings tile and fading into it -- opaque the whole way down, fading only in the last tenth of the trip. Clicked back before it lands, it turns around from where it is.

![The EmbLinkOS dock with a small fading copy of the Settings window landing on the Settings tile](213-park-at-the-tile.png)

### A shadow with two layers

Settings casting the new shadow: a wide, soft key shadow well below the window and a tight ambient one at its edge, both rounded with the window's corners. The shadow it replaces was one square-cornered band, 16 px wide and 3 px down (compare 211). Under TCG, composed on the CPU.

![The EmbLinkOS desktop with the Settings window casting a soft, rounded shadow onto the wallpaper](214-two-layer-shadow.png)

### Glass you can see through

The Applications launcher on the new default material: translucent (35%) over a 12 px blur, the backdrop saturated as it shows through (vibrancy), with the two-layer shadow beneath the sheet. The default had been 18% over 6 px -- a surface 82% opaque. Under TCG.

![The EmbLinkOS Applications launcher, a translucent dark sheet over the blurred, darkened wallpaper](215-launcher-glass.png)

### Set in Inter

Settings in the interface's new face, Inter: Regular for text, SemiBold for headings and emphasis, DejaVu Sans kept as the fallback for anything Inter does not cover. Compare 214, the same window in DejaVu Sans. Under TCG.

![The EmbLinkOS Settings window, Appearance page, set in the Inter typeface](216-inter.png)

## The prompt, Powerlevel10k-style

With the terminal real, the prompt that runs in it: the system the Mac's Powerlevel10k setup is built on, here in the native shell. First the plumbing -- `~/.config/shell/rc.esh` read by every shell a person types into, the `prompt` builtin setting options by Powerlevel10k's names, a right side the editor keeps out of the typing's way, and a transient prompt ([SHELL.md](../../SHELL.md#the-prompt)).

Then the engine: Powerlevel10k 1.20's prompt ported to C in the shell, held cell for cell to recordings of p10k itself on a Mac -- 18 configurations, 288 prompts. A person's `.p10k.zsh` is read by `prompt import-p10k`, or its settings written as `prompt set` lines; `prompt style` draws the styles `p10k configure` starts from.

Then the wizard: `prompt configure` is p10k configure, its questions and pages held cell for cell to recordings of p10k's own (454 pages), and the configuration it writes line for line. A new account's first Terminal starts it by itself; the answers from the Mac's own header make the Mac's prompt.

And the line itself, as the Mac's zsh-autosuggestions and zsh-syntax-highlighting draw it: the rest of a history line suggested after the cursor, and the colours of what is typed -- held cell for cell to the two plugins under zsh.

### A suggestion from history

Two letters typed, and the rest of the newest line in history that starts with them follows the cursor in colour 8, as zsh-autosuggestions draws it; ec itself is red until it is a command.

![A suggestion from history, in the EmbLinkOS Terminal](204-line-suggestion.png)

### Right takes it

At the end of the line Right takes the whole suggestion: echo is a command, green, and the quoted text yellow.

![Right takes it, in the EmbLinkOS Terminal](205-line-suggestion-taken.png)

### Colours as the line is typed

zsh-syntax-highlighting's default styles on this shell's language: ls and where green, the directory that is there underlined, the string yellow, the comment bold -- nothing run yet.

![Colours as the line is typed, in the EmbLinkOS Terminal](206-line-colours.png)

### A word on its way to a file

read is not a file, but readme.txt is: while the word ends the line it is underlined, as zsh-syntax-highlighting's path_prefix is.

![A word on its way to a file, in the EmbLinkOS Terminal](207-line-path-on-its-way.png)

### The wizard, on its own, in a new account's first shell

A fresh account opens the Terminal for the first time: its shell finds no prompt configuration and starts Powerlevel10k's configuration wizard, p10k's greeting saying why. First, the font: does it draw a diamond?

![The wizard, on its own, in a new account's first shell: prompt configure on EmbLinkOS](188-prompt-wizard-first-start.png)

### Does the font draw a lock?

The glyph that tells a font with Font Awesome icons from one without. On the Mac the answers come from, it did not.

![Does the font draw a lock?: prompt configure on EmbLinkOS](189-prompt-wizard-lock.png)

### Let's try another one

A second lock, from another icon set. No again: diamonds without icons, so POWERLEVEL9K_MODE is powerline.

![Let's try another one: prompt configure on EmbLinkOS](190-prompt-wizard-lock-again.png)

### Prompt style

Lean, classic, rainbow and pure, drawn from the wizard's own sample prompts as p10k draws them -- the page is held cell for cell to p10k configure's.

![Prompt style: prompt configure on EmbLinkOS](191-prompt-wizard-style.png)

### Character set

Unicode, or ASCII for a font with neither.

![Character set: prompt configure on EmbLinkOS](192-prompt-wizard-charset.png)

### Prompt colours

Lean in 256 colours, or in the terminal's 8.

![Prompt colours: prompt configure on EmbLinkOS](193-prompt-wizard-colors.png)

### Show current time?

No clock, a 12-hour one or a 24-hour one.

![Show current time?: prompt configure on EmbLinkOS](194-prompt-wizard-time.png)

### Prompt height

One line, or two: the information above, the typing below.

![Prompt height: prompt configure on EmbLinkOS](195-prompt-wizard-height.png)

### Prompt connection

What joins the two sides of the first line: nothing, dots or a solid line.

![Prompt connection: prompt configure on EmbLinkOS](196-prompt-wizard-connection.png)

### Prompt frame

No frame, on the left, on the right, or both.

![Prompt frame: prompt configure on EmbLinkOS](197-prompt-wizard-frame.png)

### Frame colour

Four greys for the frame, with p10k's marker pointing at it; the Mac's answer is the darkest.

![Frame colour: prompt configure on EmbLinkOS](198-prompt-wizard-frame-color.png)

### Prompt spacing

Compact, or sparse: an empty line before each prompt.

![Prompt spacing: prompt configure on EmbLinkOS](199-prompt-wizard-spacing.png)

### Prompt flow

Concise, or fluent: on master, took 5s, at 04:23:42 PM.

![Prompt flow: prompt configure on EmbLinkOS](200-prompt-wizard-flow.png)

### Transient prompt

Whether a line, once accepted, keeps only ❯ -- so the scrollback shows the commands, not every full prompt.

![Transient prompt: prompt configure on EmbLinkOS](201-prompt-wizard-transient.png)

### The prompt the answers make

The answers in the header of the Mac's ~/.p10k.zsh make that file again, byte for byte but the date, at ~/.config/shell/p10k.zsh; prompt.esh loads it in every shell from now on. The frame, the home, the 12-hour clock.

![The prompt the answers make: prompt configure on EmbLinkOS](202-prompt-wizard-done.png)

### prompt configure, and q

Typed in a second tab -- which loaded the configuration by itself -- `prompt configure` greets as p10k configure does; q leaves it, saying how to run it again.

![prompt configure, and q: prompt configure on EmbLinkOS](203-prompt-wizard-quit.png)

### Git status, from the repository itself

gitstatus.elf, the shell's gitstatusd, reads the sample repository ~/Projects/hello from its own files -- its packed history one commit ahead of origin/main (⇡1), an edited file (!1), a new one (?1), a file under the ignored build/ left out -- and the prompt draws it as on the Mac.

![Git status in the prompt, drawn on EmbLinkOS](187-prompt-gitstatus.png)

### The Mac's prompt, on EmbLinkOS

The configuration p10k configure wrote on a Mac -- lean, frame, 12-hour clock, transient -- as `prompt set` lines in rc.esh, drawn by the shell's own Powerlevel10k engine: the directory with its anchors, the branch (grey until the git status daemon answers), Ctrl+C's ✘ INT, a background job's ≡, and every accepted line kept as ❯ alone, red after a failure.

![The Mac’s prompt, drawn on EmbLinkOS](182-prompt-p10k-mac-config.png)

### Classic, by `prompt style`

`prompt style classic`: Powerlevel10k's classic style from /system/share/powerlevel10k, its powerline segments and frame drawn in the Terminal's own Meslo Nerd Font.

![Classic, by `prompt style` in EmbLinkOS](183-prompt-p10k-classic.png)

### Rainbow

`prompt style rainbow`: the same engine, every segment on its own background.

![Rainbow in EmbLinkOS](184-prompt-p10k-rainbow.png)

### Pure

`prompt style pure`: the style of Pure, in Powerlevel10k's terms.

![Pure in EmbLinkOS](185-prompt-p10k-pure.png)

### Lean

`prompt style lean`, the style the Mac's configuration began from.

![Lean in EmbLinkOS](186-prompt-p10k-lean.png)

### A right side, and the short way

rc.esh, read by the new tab's shell, gives the prompt a right side -- the last command's status (✘ and its code), its duration past three seconds, the time -- and a transient prompt: lines already run keep only the prompt character, green or red by how the command before went.

![A right side, and the short way in EmbLinkOS](180-prompt-right-side.png)

### The prompt’s options, as commands

`prompt` lists the options rc.esh set; `prompt set transient off` from the prompt itself puts the full prompt back, the right side kept as zsh keeps its RPROMPT.

![The prompt’s options, as commands in EmbLinkOS](181-prompt-options.png)

## A real Terminal, key by key

The Terminal is being rebuilt into a real one -- then a Powerlevel10k-style prompt system on top (docs/TODO.md has the roadmap). First, keys: the character stream cannot tell Up from Ctrl+S (both 0x13) or Backspace from Ctrl+H, and F1-F12 never arrived at all. The kernel queue now records which key made every byte, USB keyboards are decoded like PS/2 ones, and the Terminal encodes keys exactly as xterm does. See [TERMINAL.md](../../TERMINAL.md#keys).

### Keys with identity

Up and Ctrl+S are the same byte, and F5 has none; the kernel queue now records which key made each byte. ttyprobe --identity shows what the Terminal sent for keys pressed over QMP: F-keys, modified arrows, Alt as Meta, Backspace against Ctrl+Backspace, and the keypad in application mode -- each as xterm sends it.

![Keys with identity in EmbLinkOS](156-terminal-key-identity.png)

The shell then took its line back from the terminal: it draws its prompt, edits, completes and recalls history itself, as readline is bash's ([SHELL.md](../../SHELL.md#10-the-line--the-shell-edits-its-own)).

### Completion from what the shell knows

Tab on a word many commands share opens a menu of the shell's builtins, its keywords and the programs in /system/bin and /data/apps; Tab again chooses. The shell completes, not the terminal.

![Completion from what the shell knows in EmbLinkOS](157-shell-edit-completion.png)

### Searching history as you type

Ctrl+R and three letters find the earlier command containing them; the search line sits under the prompt, Enter runs the match and Escape puts the line back.

![Searching history as you type in EmbLinkOS](158-shell-edit-search.png)

### A block typed the way it is written

Enter on an open brace adds a line under a '...' prompt instead of reporting an error; the closed block runs as one program.

![A block typed the way it is written in EmbLinkOS](159-shell-edit-multiline.png)

### History that outlives the window

The Terminal was closed and a new one opened: its shell lists what the first one ran from ~/.local/share/shell/history, and Up recalls the multi-line program whole.

![History that outlives the window in EmbLinkOS](160-shell-edit-history.png)

### sh on the same editor

With both shells editing their own lines, the Terminal's own line editor is gone: it is a screen. The POSIX sh port edits its line with the native shell's editor: a loop typed one line at a time continues under '...' until done closes it, and Tab completes from sh's builtins and the programs on the system.

![sh on the same editor in EmbLinkOS](161-sh-edit.png)

Then the screen itself: the model behind the Terminal is now xterm-compatible -- the sequences programs send, the answers they expect, colours kept by palette index, a scrollback of logical lines, and text that re-wraps on every resize ([TERMINAL.md](../../TERMINAL.md#screen-model-and-interactions)).

### What a program can draw, and what the terminal answers

ttyprobe --xterm runs with TERM=xterm-256color and COLORTERM=truecolor. Its card shows bold, dim, strike, inverse and overline; single, double, curly, dotted and dashed underlines, two of them in their own colours (58); the 16- and 256-colour palettes; a truecolour gradient; and an OSC 8 link. The model holds italic, the CJK cells and the combining accent too: the renderer draws them in M5. Below the card are seven questions programs ask a terminal (DA1, DA2, XTVERSION, DECRQM, OSC 10, XTGETTCAP, CSI 18 t), each answer checked by the program.

![What a program can draw, and what the terminal answers in EmbLinkOS](162-terminal-xterm-card.png)

### Mouse and focus reports

With mouse reporting on (1000, SGR encoding 1006), a left click, a right click and the wheel both ways reach the program with the cell they happened on. Focus reports (1004) say when another window takes the keyboard and when it comes back.

![Mouse and focus reports in EmbLinkOS](163-terminal-xterm-mouse-focus.png)

### Text re-wraps when the window narrows

Narrowed from 156 columns to 74, the 233-character line re-wraps from two rows to four. The cursor stays at the prompt, and the rows that no longer fit above it moved into the scrollback.

![Text re-wraps when the window narrows in EmbLinkOS](164-terminal-xterm-reflow.png)

### The scrollback re-wraps too

Shift+PgUp pages back to the top of the card. The scrollback stores logical lines, not rows, so what was written at 156 columns is read back laid out at 74, with its colours and underlines kept.

![The scrollback re-wraps too in EmbLinkOS](165-terminal-xterm-scrollback.png)

And then the pixels: Terminal draws in MesloLGS Nerd Font Mono, the face of the Mac setup it follows, in four faces with DejaVu after it; draws box lines, blocks, braille and Powerline separators itself so they meet cell to cell; and repaints and presents only the rows that changed ([TERMINAL.md](../../TERMINAL.md#fonts-and-the-renderer)).

### Meslo Nerd Font, Powerline segments and frames that join

ttyprobe --glyphs writes a sample in the Terminal: the four MesloLGS Nerd Font Mono faces, Armenian letters drawn from DejaVu because Meslo has none (the Ω and ♫ after them are Meslo's own), and a Powerlevel10k-style framed prompt -- Powerline segments meeting without a seam, the frame lines joining the rows, Nerd Font icons for the OS, the folder and the branch. Below it are frames of every weight, blocks, braille and the Powerline shapes, which Terminal draws itself to fill the cell; a row of icons, one of them from Unicode plane 15; then the underline styles in their own colours. The real prompt engine comes later; this is a program writing what one would.

![Meslo Nerd Font, Powerline segments and frames that join in EmbLinkOS](166-terminal-render-glyphs.png)

### A screen that scrolls every frame

ttyprobe --bench writes 1 MiB of coloured lines as fast as Terminal takes them: 13,618 lines in 145 s on this host, which emulates every x86 instruction. Each frame replaces the whole screen here, so the renderer draws every row (about 35 ms) and the window presents in full; output that changes a row presents that row.

![A screen that scrolls every frame in EmbLinkOS](167-terminal-render-bench.png)

Then the window around the screen: tabs that each keep their own shell, a mouse selection that stays on its text as output scrolls, find in the scrollback, links that open in the app they belong to, a menu on the right button, and zoom ([TERMINAL.md](../../TERMINAL.md#tabs-selection-find-links)).

### Two tabs, two shells

Super+T opens a second tab with its own shell, screen and scrollback; the first keeps what it had. The strip of tabs shows once there are two. Ctrl+Tab moves between them, and Super+W closes one, its shell reaped and its PTY freed.

![Two tabs, two shells in EmbLinkOS](168-terminal-tabs.png)

### A selection made with the mouse

A drag selects exactly the cells it covers, here first-tab. A double click takes the word (first-tab-42, hyphens and all) and a triple click the line; Super+C copies the selection and Super+V pastes it at the prompt.

![A selection made with the mouse in EmbLinkOS](169-terminal-selection.png)

### Find in the scrollback

Super+F opens a field over the screen. Every match is marked, the one found more strongly, and the view follows it into the scrollback. Enter and the arrows step between matches; Escape or Done closes the field.

![Find in the scrollback in EmbLinkOS](170-terminal-find.png)

### A link under Super

With Super held, the link under the pointer is underlined and its address shows in the title. A click opens it: a file:// folder in Files, a file in Editor, a web address in Vellum.

![A link under Super in EmbLinkOS](171-terminal-link.png)

### The right-button menu

Copy, Paste, Select All, Find, Clear Scrollback, New Tab and Close Tab, each with its key. The title still reports the last action: Files opened /data/apps.

![The right-button menu in EmbLinkOS](172-terminal-menu.png)

### Zoom

Super+= and Super+- change the font a pixel at a time and Super+0 puts it back. Every tab reflows to the new cell size, and the title shows it.

![Zoom in EmbLinkOS](173-terminal-zoom.png)

Then its profiles: Clear Dark by default -- the Mac profile it follows, converted from its plist -- at 95% over a blur of the desktop; Preferences beside the screen; any change a profile file goes through, taken by every window showing it ([TERMINAL.md](../../TERMINAL.md#profiles-and-preferences)).

### Clear Dark, as the Mac has it

The default profile is the Mac's Clear Dark, converted from its Terminal plist: MesloLGS Nerd Font Mono at 14 px, its sixteen colours in sRGB, a 120 x 30 window, and the background at 95% over a blur of the desktop -- the faint teal is the wallpaper behind it.

![Clear Dark, as the Mac has it in EmbLinkOS](174-terminal-clear-dark.png)

### Preferences beside the screen

Super+, opens Preferences as a panel beside the screen, so a change is seen as it is made: the profile, its font, size and spacing, bold, the cursor, the colours, the window and the shell. Edits go to the profile file, which other windows showing it read again.

![Preferences beside the screen in EmbLinkOS](175-terminal-preferences.png)

### The sixteen, and a colour picker

Each of the sixteen colours, and the text, bold, background, selection and cursor colours, is a swatch; a click opens a picker for it, and the screen takes the colour as it is dragged.

![The sixteen, and a colour picker in EmbLinkOS](176-terminal-colours.png)

### A window that shows through

The profile file written from the shell -- as another window or an editor would -- and the window takes it within a second: at 60% the Files window behind it shows through, blurred. The compositor keeps the blur and redoes only where the backdrop changes.

![A window that shows through in EmbLinkOS](177-terminal-translucent.png)

### The same, composed by the GPU

The same window with the host's GPU composing (virgl): the same tent blur from two half-radius passes, frame for frame with the CPU's.

![The same, composed by the GPU in EmbLinkOS](178-terminal-translucent-gpu.png)

### A new window in the default profile

terminal.conf names Solarized Light, and the next window opens in it, opaque; its bars switch to the light theme so their text can be read on it.

![A new window in the default profile in EmbLinkOS](179-terminal-solarized-light.png)

## The native shell runs any program

The structured shell used to speak only to programs built with its own SDK. A text program -- sh, a port, a script -- now takes its part in a pipeline: what is piped in arrives as text, what it prints is captured as a string when a later stage wants it, and at the end of a line it keeps the terminal. Redirections are words with the arrow attached (`out>`, `err>`, `in<`), because `>` compares here; scripts run by their `#!` line; `source` loads definitions; arguments that break the kernel's limits are refused by name. These are QEMU captures of the Terminal, typed into over QMP; `test shellio` checks the same behaviour on the OS. See [SHELL.md](../../SHELL.md).

### Text programs in pipelines

sh links no shell SDK, so it is a text program: its output is captured and split by lines, values piped into it arrive as text (a list as lines, ls's table as its grid), and -c is a flag, not a minus.

![Text programs in pipelines in EmbLinkOS](151-shell-text-pipes.png)

### Redirections that do not collide with >

out>, err>, out>> and in< on sh and on builtins: sysinfo's text goes to a file, sh's stdout and stderr to two, a third line appended, and range's value written as text and counted back.

![Redirections that do not collide with > in EmbLinkOS](152-shell-redirections.png)

### Scripts run by their #! line

A native script and an sh script saved from the prompt run as commands: the kernel reads the #! line and starts the interpreter with the script and its arguments. source loads a definition into the session.

![Scripts run by their #! line in EmbLinkOS](153-shell-scripts.png)

### What a command is, and limits that say so

which tells a text program from a structured one. 200 arguments break the kernel's limit of 126, and the shell names it before anything is spawned.

![What a command is, and limits that say so in EmbLinkOS](154-shell-which-limits.png)

### An interactive sh from the native prompt

A text program that ends a statement keeps the terminal: sh runs interactively in the Terminal, and exit returns to the native prompt.

![An interactive sh from the native prompt in EmbLinkOS](155-shell-interactive-sh.png)

## GPU composition

The desktop is composed on the GPU where the adapter can do it: through virgl on QEMU's virtio-vga-gl -- the host's OpenGL, here Mesa's llvmpipe -- and with the Gen9 blitter on Intel laptops. These are captures of the virgl path; a GL scanout has no surface for QEMU's screendump, so they were read over VNC from an egl-headless display, unmodified. The same guest run with composition kept on the CPU gave the comparison frames; the Intel blitter has only met a model of its engine. See [DISPLAYS.md](../../DISPLAYS.md#gpu-composition).

### The desktop composed on the host's GPU

After setup and login the compositor draws through virgl: the wallpaper window, the translucent menu bar, the frosted dock and the pointer are draws in a 3D context, straight into the scanout resource. It matches the CPU-composed desktop pixel for pixel.

![The desktop composed on the host's GPU in EmbLinkOS](146-gpu-desktop.png)

### Kernel title bars, rings and shadows on the GPU

The Windows demo, started from the Terminal: two windows with kernel title bars over a third. Rounded corners, the 1px border ring and the focus shadow are fragment shaders; the title bars are drawn by the CPU on a canvas the GPU shows.

![Kernel title bars, rings and shadows on the GPU in EmbLinkOS](147-gpu-windows.png)

### Settings names what composes

Settings > Displays > Rendering reports host GPU (virgl) once the compositor's probe frame came back right.

![Settings names what composes in EmbLinkOS](148-gpu-settings.png)

### The same scene composed on the CPU

The same guest and the same input with composition kept on the CPU: Rendering says Software. The smoke test compares every GPU frame with its CPU twin.

![The same scene composed on the CPU in EmbLinkOS](149-cpu-settings.png)

### A resolution trial under the GPU

Apply Resolution at 1024 × 768: a new surface and scanout resource, attached to the 3D context, the canvas made again over the new backbuffer. Revert brings 1280 × 800 back, still composed on the GPU.

![A resolution trial under the GPU in EmbLinkOS](150-gpu-resolution-trial.png)

## Several monitors

The desktop spans every monitor in use. Each monitor shows its own rectangle of one surface and its own wallpaper, and the primary carries the menu bar and the dock. A monitor plugged in extends the desktop. Settings arranges the displays through the same keep-or-revert trial as a resolution change, and a kept arrangement returns for the same monitors after login. These are QEMU captures of a two-head virtio-vga (each head captured separately); native Intel external monitors have only met a register model. See [DISPLAYS.md](../../DISPLAYS.md).

### A second monitor extends the desktop

The second VirtIO head is connected at login and the kernel extends the desktop to 2304 × 800. The 1280 × 800 primary keeps the menu bar, the dock and the desktop icons.

![A second monitor extends the desktop in EmbLinkOS](138-multimonitor-extended.png)

### The second monitor shows its own wallpaper

The 1024 × 768 head to the right shows a whole wallpaper of its own, not a slice of one picture stretched across both.

![The second monitor shows its own wallpaper in EmbLinkOS](139-multimonitor-second-wallpaper.png)

### Fill on the monitor the window is on

The Editor was dragged across the seam with its unsaved text. Fill from the zoom light covers the second monitor exactly.

![Fill on the monitor the window is on in EmbLinkOS](140-multimonitor-window-filled.png)

### Arranging the displays

With two monitors connected, Settings → Displays draws them to scale: Extend or Mirror, and for the selected display Use this display, Resolution, Position and Make Primary.

![Arranging the displays in EmbLinkOS](141-multimonitor-settings.png)

### The second monitor moved to the left

After Apply Arrangement and Keep Changes, the second monitor is left of the panel. The panel keeps the menu bar, and the preferences are saved.

![The second monitor moved to the left in EmbLinkOS](142-multimonitor-arranged-left.png)

### Make Primary moves the menu bar and the dock

The second monitor is now primary: the menu bar and the dock are on it. The Editor kept its place and its text.

![Make Primary moves the menu bar and the dock in EmbLinkOS](143-multimonitor-primary-moved.png)

### A mirror trial

Mirror shows one picture on both monitors at 1024 × 768, the size both can show, with the 30-second confirmation. Revert puts the extended arrangement back.

![A mirror trial in EmbLinkOS](144-multimonitor-mirror-trial.png)

### The arrangement returns after a cold boot

After a new UEFI/NVMe boot and password login, the desktop restores the kept arrangement for these two monitors and confirms it once it has drawn a frame of it.

![The arrangement returns after a cold boot in EmbLinkOS](145-multimonitor-restored.png)

## Live display configuration

Settings changes the actual VirtIO/Bochs scanout, asks for confirmation and
retains the previous framebuffer for rollback. Timeout, closing the requester
and locking the session restore the confirmed mode. The desktop, top bar and
shared app runtime reflow; the Editor’s unsaved document survives. Confirmed
per-user dimensions return after a new boot and password login. These are
virtual-device tests; general native laptop modesetting, multiple outputs and
GPU rendering remain open. See [DISPLAYS.md](../../DISPLAYS.md).

### Display identity and resolution

Settings reports the VirtIO adapter, CPU rendering, validated monitor identity and recommended resolution.

![Display identity and resolution in EmbLinkOS](128-display-identity.png)

### A safe live resolution trial

The actual scanout changes to 800 × 600. Settings remains visible with Keep Changes, Revert and a 30-second confirmation deadline.

![A safe live resolution trial in EmbLinkOS](129-display-trial.png)

### A confirmed resolution

The active 1024 × 768 mode is saved to the authenticated user’s atomic settings file. The top bar, desktop, dock and applications reflow.

![A confirmed resolution in EmbLinkOS](130-display-kept.png)

### An unconfirmed mode stays temporary

A second 800 × 600 trial is intentionally left unconfirmed so the OS must restore the retained previous framebuffer.

![An unconfirmed mode stays temporary in EmbLinkOS](131-display-timeout.png)

### Timeout restores the confirmed mode

The scanout and shared application geometry return to 1024 × 768 after the confirmation deadline.

![Timeout restores the confirmed mode in EmbLinkOS](132-display-timeout-restored.png)

### Closing Settings restores the display

The requester exits during a trial. The OS restores 1024 × 768, and the Editor’s unsaved document remains intact.

![Closing Settings restores the display in EmbLinkOS](133-display-requester-exit.png)

### Locking cancels the display trial

Lock Screen is chosen through the real top-bar menu during an 800 × 600 trial. The session gate locks and the OS restores its confirmed 1024 × 768 mode.

![Locking cancels the display trial in EmbLinkOS](134-display-lock-restored.png)

### The display choice survives a cold boot

After a new UEFI/NVMe boot and password login, the desktop restores the confirmed 1024 × 768 preference and confirms it after presenting a frame.

![The display choice survives a cold boot in EmbLinkOS](135-display-cold-boot.png)

### An independent display backend

The same Settings interface identifies Bochs DISPI, validates firmware monitor identity when available and exposes only modes that fit its VRAM.

![An independent display backend in EmbLinkOS](136-bochs-display.png)

### Bochs applies and saves a new mode

The independent DISPI backend switches from its 1024 × 768 startup mode to 1280 × 800. The same keep, revert, timeout, close, lock and cold-boot checks run on both backends.

![Bochs applies and saves a new mode in EmbLinkOS](137-bochs-display-kept.png)

## Managed system and application updates

Software Update now checks a signed release, streams an OS and paired EFI to
the inactive system slot, verifies disk readback, and offers an explicit restart.
A real administrator logged in before and after upgrade and rollback. The
account, appearance preferences and a pre-existing document survived the cold
boots. The paired kernel's update authority and mapping checks pass natively;
package installation publishes synced immutable versions through CURRENT.

These are original-pixel QMP captures using a temporary local signing key and
release server. The versions are acceptance fixtures, not a public release feed.
No production signing key or public OS/application registry is configured by
default. See [the update contract](../../UPDATES.md) for supported layouts and
remaining work. Captures 01–116 and their metadata are preserved.

### Software Update in Settings

A real administrator account opens Software Update on managed UEFI/NVMe. The pane reports release 1.0.0 from the boot journal; the independent application registry is explicitly unconfigured.

![Software Update in Settings in EmbLinkOS](117-software-update-settings.png)

### A verified release is available

The native worker checked an ECDSA-P256 signed release from a local acceptance fixture. Version and release notes come from the verified descriptor. These are test releases, not a configured public update channel.

![A verified release is available in EmbLinkOS](118-signed-release-available.png)

### Installing the next system

The running desktop remains in the confirmed system while the native worker streams the signed OS and paired EFI into the inactive deployment. The progress is read from the actual update engine.

![Installing the next system in EmbLinkOS](119-background-system-install.png)

### Verifying the installed bytes

The kernel reads the inactive deployment back from NVMe and verifies both image digests before publishing a trial boot. Settings navigation was exercised during this bounded verification phase.

![Verifying the installed bytes in EmbLinkOS](120-system-readback-verification.png)

### Restart when ready

Both payloads passed readback verification and the trial journal was flushed. Restart is an explicit user action, with a save-your-work confirmation.

![Restart when ready in EmbLinkOS](121-system-update-ready.png)

### The upgraded system is confirmed

After a cold boot into OS-B, the actual desktop/top-bar frames and live supervised service remained healthy for five seconds. The engine confirmed release 1.1.0 and retained OS-A for rollback. The account and saved appearance preferences survived.

![The upgraded system is confirmed in EmbLinkOS](122-upgraded-desktop.png)

### Choose the previous system

Settings selected the retained local deployment for the next restart. The test then cold-booted OS-A and confirmed it through the same real desktop health path.

![Choose the previous system in EmbLinkOS](123-previous-system-selected.png)

### Personal files survive upgrade and rollback

Terminal reads the document created before installation, after upgrade to OS-B and rollback to OS-A. A real administrator authenticated across all three boots; saved appearance preferences also matched. No host filesystem or physical laptop was used.

![Personal files survive upgrade and rollback in EmbLinkOS](124-files-survive-system-rollback.png)

## Managed recovery and installation acceptance

### The confirmed desktop is restored

The real firmware rejected a signed but unloadable trial EFI and booted confirmed OS-A instead. This is the restored live desktop; the boot journal and serial acceptance log establish the failure and fallback. The confirmed OS hash stayed unchanged across rejected catalogs, short/altered payloads, hard interruption and firmware fallback.

![The confirmed desktop is restored in EmbLinkOS](125-failed-update-restored.png)

### Installing the managed system

The recovery console copied and read back a managed ESP, OS-A, OS-B and shared Data onto a separate disposable NVMe disk. It rewrote boot identities and expanded Data while preserving the source hash. This operation never selected a host disk.

![Installing the managed system in EmbLinkOS](126-managed-recovery-install.png)

### The installed system boots alone

The newly installed NVMe disk booted without the source media. The selected OS mounted read-only, shared Data mounted writable, the update engine recognized its rewritten identities, and real first-account setup presented its frame.

![The installed system boots alone in EmbLinkOS](127-installed-managed-first-boot.png)

## Earlier: shared descriptions and socket I/O

The native witness passes 223 checks in Terminal, including concurrent file
records, append, shared status and socket ownership. Through sh, 83 network
checks verify actual TCP/UDP echo traffic after child exit/termination, pending
reads across close/reuse, empty datagrams and oversized-packet refusal. The
[contract](../../FILE_DESCRIPTIONS.md) records bounds and remaining work.

These original-pixel QMP captures use a disposable UEFI/NVMe live session with
an emulated e1000e and local echo servers, without a synthetic ACPI fixture.
Captures 01–114 and their metadata are unchanged.

![Shared descriptors and concurrent file I/O](115-shared-descriptors-and-append.png)

![Socket ownership through the development shell](116-shared-socket-traffic.png)

## Earlier: spawn-based sh

The image now includes an initial mrsh-based shell alongside the native prompt.
The shared 83-case script corpus passes 338 checks in Terminal and twice through
the serial wrapper. Interactive input runs loops, arithmetic, command substitution
and a verified 66,049-byte three-stage pipeline. This is a bounded development
shell; see [the contract](../../POSIX_SHELL.md) for supported options and gaps.

These original-pixel QMP captures use a disposable UEFI/NVMe live session without
a synthetic ACPI fixture. Captures 01–112 and their metadata are unchanged.

![Shell scripts in the native Terminal](113-posix-shell-script-checks.png)

![Interactive sh, expansion and a three-stage pipeline](114-posix-shell-interactive.png)

## Earlier: POSIX spawn

The spawn adapter passes 357 checks in the normal logged-in Terminal namespace,
including ordered file actions, close-on-exec, PATH lookup, exact child
environments and directory setup before constructors run. Two children
transform 66,049 bytes through pipes, and a following shell command succeeds.
The same checks pass twice through the serial wrapper. This is a bounded
adapter on native child handles; the later sh milestone above builds on it.

This original-pixel QMP capture uses a disposable UEFI/NVMe live session without
a synthetic ACPI fixture. Captures 01–111 and their metadata are unchanged.

![Ordered spawn actions and a POSIX pipeline](112-posix-spawn-pipeline.png)

## Latest: process compatibility

The native process witness passes 203 checks inside Terminal, retaining 32 child statuses and transforming 66,049 bytes through two child processes. It verifies EOF, nonblocking writes, actual PIDs, copyout retry and competing waiters; a following shell command succeeds. This is the spawn-based compatibility foundation, not a ported Unix shell.

This original-pixel QMP capture uses a disposable UEFI/NVMe live session without a synthetic ACPI fixture.

![Pipes and child waits in the native terminal](111-process-pipeline-waits.png)

## Latest: full-screen terminal

Seven real QMP captures show the native screen witness, cross-application clipboard and restored shell. They use a disposable UEFI/NVMe session without a synthetic ACPI fixture. The witness exercises the emulator; it does not establish Vim, zsh or xterm compatibility.

### A screen with independent regions

The native raw-mode terminal witness keeps its heading and footer fixed while twelve log entries scroll inside its margins. Application-cursor Down selects the second row; colours and Unicode cells are rendered by the actual emulator.

![A screen with independent regions](104-terminal-screen-region.png)

### Cursor position, round trip

The native application positions its cursor, requests a report and verifies the terminal reply through its own standard input. This is a screen-emulation witness, not an upstream application compatibility claim.

![Cursor position, round trip](105-terminal-cursor-reply.png)

### One screen, a larger workspace

The real window control expands Terminal. The application queries updated PTY dimensions and redraws its screen at the new size; restoring the window is verified in the same run.

![One screen, a larger workspace](106-terminal-live-resize.png)

### Multiline paste reaches the program

Text copied from Editor reaches the running raw-mode application inside bracketed-paste delimiters. The program verifies 22 bytes and one newline without treating that newline as an ordinary command.

![Multiline paste reaches the program](107-terminal-bracketed-paste.png)

### Screen text back in Editor

Copy screen exports plain UTF-8 text without terminal escape sequences. Editor receives the complete multiline grid through a single paste command, replaces its selection and clears the retained highlight.

![Screen text back in Editor](108-terminal-screen-editor.png)

### Return to the native shell

Leaving the alternate screen restores the previous native command and its result. The witness confirms its exit; alternate-screen content does not enter the primary output.

![Return to the native shell](109-terminal-primary-restored.png)

### A large clipboard in the code editor

The guest doubles text using Editor selection/copy/paste commands, then copies more than 8 KiB into Note++. Its separate application paste hook receives the full multiline text without indexing beyond its clipboard buffer.

![A large clipboard in the code editor](110-terminal-long-editor-paste.png)

## Latest: interactive terminal sessions

Four real QMP captures accompany the terminal-device milestone. They come from a disposable UEFI/NVMe live session with no synthetic ACPI fixture. They show the native shell and terminal witness, not a ported Unix shell or a full VT screen emulator.

### The native prompt, preserved

Terminal runs its native shell on a private pseudo-terminal while retaining local caret editing and command history. The guest verifies edited commands and their separate result lines.

![The native prompt, preserved](100-terminal-prompt.png)

### A program takes keyboard input

The child reports measured terminal dimensions, receives an edited canonical line and switches to raw input. It is waiting for one key without Enter.

![A program takes keyboard input](101-terminal-raw-input.png)

### One key, delivered immediately

The native program receives z as byte 0x7a without Enter, then restores canonical input. Ctrl+D returns control to the shell; the same guest also verifies Ctrl-letter bytes and ANSI arrow input.

![One key, delivered immediately](102-terminal-direct-input.png)

### Interrupt a command and keep working

Ctrl+C stops a foreground child and interrupts a native shell loop. Subsequent commands produce their expected results in the same Terminal session.

![Interrupt a command and keep working](103-terminal-interrupt.png)

## Latest: keyboard interaction

Four real guest captures accompany keyboard acceptance. They use a disposable UEFI/NVMe live session, with no synthetic ACPI fixture.

### Keyboard focus stays in dialogs

Reset appearance opens with Cancel focused. The guest verifies forward traversal, keyboard cancellation and restoration to the initiating control.

![Keyboard focus stays in dialogs](96-keyboard-dialog.png)

### Reverse traversal, the same boundary

Shift+Tab wraps to Reset inside the confirmation; Tab returns to Cancel without reaching controls behind the dialog.

![Reverse traversal, the same boundary](97-reverse-dialog.png)

### Menus work from the keyboard

Right reaches the adjacent workspace menu, Down selects its second item, Home returns to its first, and Escape releases menu ownership.

![Menus work from the keyboard](98-keyboard-menu.png)

### A safe choice for unsaved work

The dirty Editor guard focuses Cancel, contains keyboard traversal and blocks background Save. Cancelling preserves the exact text and restores typing focus.

![A safe choice for unsaved work](99-document-dialog.png)

## Contour desktop

These are actual UEFI/NVMe guest captures of the redesign, including customization, applications and authenticated document workflows. They contain no synthetic ACPI fixture.

### A calmer desktop

Contour introduces mineral surfaces, compact original icons and a readable command strip.

![A calmer desktop](62-contour-desktop.png)

### The application library

A bounded, searchable library launches installed native applications through keyboard or pointer input.

![The application library](63-contour-library.png)

### One shared appearance

Settings coordinates semantic colours, typography, control density and live application appearance.

![One shared appearance](64-contour-appearance.png)

### Paper after a cold boot

The saved Paper preset returns after an independent UEFI boot and account authentication.

![Paper after a cold boot](65-contour-paper.png)

### Workspaces preserve running work

Editor moves to another workspace while preserving its text; hidden windows are separately checked for input isolation.

![Workspaces preserve running work](66-contour-spaces.png)

### Window and workspace preferences

Shared decorations, shadows, corner geometry and workspace behavior are configurable in Settings.

![Window and workspace preferences](67-contour-windows.png)

### Readable and reduced effects

Text size, contrast, cursor appearance and reduced-motion preferences share the same configuration system.

![Readable and reduced effects](68-contour-accessibility.png)

### Terminal with native chrome

Terminal keeps a monospace transcript while its surrounding controls follow the selected interface family.

![Terminal with native chrome](69-contour-terminal.png)

### Files follows the desktop

Files uses semantic selection, a shared sidebar surface and compact native window controls.

![Files follows the desktop](70-contour-files.png)

### Editor in Contour

Editor inherits shared typography and chrome while retaining safe saving and recovery.

![Editor in Contour](71-contour-editor.png)

### Photos in Contour

The photo application uses the shared palette, window controls and preferred corner geometry.

![Photos in Contour](72-contour-photos.png)

### An honest empty music library

Music explains its empty state and retains the common desktop hierarchy.

![An honest empty music library](73-contour-music.png)

### Measured system activity

Activity Monitor presents actual process measurements within the shared native interface.

![Measured system activity](74-contour-monitor.png)

### Notification history

The history window has an original icon and a readable empty state.

![Notification history](75-contour-notifications.png)

### A dock on the left

A reserved side band keeps application windows clear of the configurable dock.

![A dock on the left](76-contour-left-dock.png)

### A dock on the right

The same dock supports the opposite screen edge with shared placement rules.

![A dock on the right](77-contour-right-dock.png)

### The command strip moves

The command strip can occupy either horizontal screen edge.

![The command strip moves](78-contour-bottom-strip.png)

### Menus open toward the workspace

At the bottom edge, the system menu opens upward and retains its dismissal surface.

![Menus open toward the workspace](79-contour-bottom-menu.png)

### A different typographic character

A real serif regular/bold family updates Settings and the command strip.

![A different typographic character](80-contour-serif.png)

### Scale and density are independent

The acceptance run selects 115 percent interface scale and compact spacing through the actual controls.

![Scale and density are independent](81-contour-density.png)

### Reset remains a deliberate choice

Reset appearance asks for confirmation; acceptance checks cancellation and reset separately, including a scaled serif explanation.

![Reset remains a deliberate choice](82-contour-reset.png)

### A coherent sign-in surface

The account sign-in screen uses the shared default visual language and retains the authentication service.

![A coherent sign-in surface](83-contour-login.png)

### The lock follows the session

The themed lock acquires screen ownership; workspace shortcuts are checked while the session is locked.

![The lock follows the session](84-contour-lock.png)

### The shared Save panel

The session file service uses Contour chrome, user places and the same shared controls as its callers.

![The shared Save panel](85-contour-picker.png)

### A real multi-document workspace

Note++ keeps a monospace document canvas and shared native chrome; both dirty tabs survive a forced stop.

![A real multi-document workspace](86-contour-note-workspace.png)

### Recovery retains the new interface

An independent boot restores both Note++ drafts, with exact document contents checked by the acceptance run.

![Recovery retains the new interface](87-contour-recovered.png)

### Original monochrome silhouettes

The shell uses matching line silhouettes, preserving distinct application glyphs in a single ink.

![Original monochrome silhouettes](88-contour-monochrome.png)

### One icon family throughout the library

The application library follows the same monochrome icon preference as the dock and desktop.

![One icon family throughout the library](89-contour-monochrome-library.png)

### A coordinated custom palette

A custom light surface and rust accent are entered through Settings, persisted and applied live.

![A coordinated custom palette](90-contour-custom-palette.png)

### Power status follows the provider

The emulated guest reports no battery; the bar avoids suggesting a permanently full battery.

![Power status follows the provider](91-contour-power-status.png)

### A compact, readable window

Wrapped descriptions and previews remain readable at the actual 640 by 420 minimum window size.

![A compact, readable window](92-contour-compact-window.png)

### Fill the available workspace

Window fill respects the shared command-strip and dock insets.

![Fill the available workspace](93-contour-fill.png)

### Restore the previous geometry

The window control restores the position and dimensions recorded before filling.

![Restore the previous geometry](94-contour-restored.png)

### Tile with a deliberate layout board

The native hover board places Settings in the right half while preserving its usable minimum size.

![Tile with a deliberate layout board](95-contour-tiled.png)

## Document workspaces

Captures 47–61 follow real file creation, saving, unsaved-close protection, forced-stop recovery and reopening after independent boots. They use authenticated QEMU accounts and disposable NVMe storage, with no synthetic ACPI fixture.

### A shared Save dialog

Both editors use the session file service. The picker shows the actual authenticated home and user places.

![A shared Save dialog](47-shared-save.png)

### A durable saved document

Editor reports the completed save in Documents after synced temporary-file replacement.

![A durable saved document](48-document-saved.png)

### Confirming file replacement

Save As asks before replacing the existing Milestone.txt; cancelling keeps the original path and contents.

![Confirming file replacement](49-replace-confirmation.png)

### Editor protects unsaved changes

GUI+W opens Save, Discard and Cancel. The window remains alive beyond the former compositor close deadline.

![Editor protects unsaved changes](50-editor-unsaved-close.png)

### Quitting a multi-document workspace

Note++ checks every dirty tab. The acceptance test also cancels a partially answered quit and verifies dirty state is retained.

![Quitting a multi-document workspace](51-workspace-unsaved-close.png)

### Two dirty tabs before a forced stop

Both modified tabs remain open after cancelling quit. The VM is then terminated without logout or shutdown.

![Two dirty tabs before a forced stop](52-workspace-before-crash.png)

### Editor recovers after a cold boot

The original saved document and its unsaved addition return from a separately checkpointed draft on NVMe.

![Editor recovers after a cold boot](53-editor-recovered.png)

### Both Note++ drafts recovered

A new boot restores both unsaved tabs, including the second draft that directory mutation could previously skip.

![Both Note++ drafts recovered](54-workspace-recovered.png)

### Saving the recovered workspace

A recovered document is saved through the common picker. Its path and cursor are recorded for session reopening.

![Saving the recovered workspace](55-workspace-saved.png)

### Saved work reopens after another reboot

A third independent boot authenticates the user and reopens the saved Note++ document with its exact contents.

![Saved work reopens after another reboot](56-workspace-after-reboot.png)

### Browsing persisted documents

The shared Open panel lists both saved documents in the authenticated Documents folder. File rows now receive selection clicks.

![Browsing persisted documents](57-shared-open.png)

### The original document reopens from the picker

Editor loads Milestone.txt through the shared Open panel after the final cold boot; exact bytes are checked.

![The original document reopens from the picker](58-document-after-reboot.png)

### Creating a document in Files

The authenticated Files window offers New Document in the current folder.

![Creating a document in Files](59-files-new-document.png)

### Files hands the document to Editor

Files creates FromFiles.txt, supplies HOME and EDIT_FILE to Editor, and the edited document is saved in the same home.

![Files hands the document to Editor](60-files-created-and-saved.png)

### Duplicate document names are refused

Creating FromFiles.txt again reports that the name already exists and does not open a replacement editor.

![Duplicate document names are refused](61-files-name-conflict.png)

## System-menu milestone

Captures 41–46 compare the previous menu with the current native light/dark
palette, Escape dismissal, live preferences and action dispatch. Capture 41 is
the baseline; 42–46 use a fresh QEMU live image without an ACPI fixture.

### Before: mixed menu styling

The integrated desktop before this change: bright rows on the dark desktop. This baseline capture includes the synthetic ACPI fixture.

![Before: mixed menu styling in EmbLinkOS](41-menu-before.png)

### System menu in dark appearance

The bar applies the live dark palette; idle rows share one dense surface with a consistent outline.

![System menu in dark appearance in EmbLinkOS](42-menu-dark.png)

### System menu in light appearance

Changing Appearance through Settings updates the running menu bar without restarting it.

![System menu in light appearance in EmbLinkOS](43-menu-light.png)

### Escape dismisses the dropdown

The menu closes, releases keyboard ownership and returns the bar to 26 pixels. The test reopens it and then uses the dock.

![Escape dismisses the dropdown in EmbLinkOS](44-menu-escape.png)

### Live appearance controls

Actual pointer input switches to Light before the test verifies the running system menu palette.

![Live appearance controls in EmbLinkOS](45-menu-appearance.png)

### Menu action reaches notification service

About EmbLink still posts a native notification after repeated open, Escape, outside-click and appearance changes.

![Menu action reaches notification service in EmbLinkOS](46-menu-action.png)

## Merged desktop milestone

![Desktop after reboot and login](38-merged-persistence.png)

### First account on the merged desktop

Normal UEFI boot reaches first-account setup on a fresh NVMe disk.

![First account on the merged desktop in EmbLinkOS](21-first-account.png)

### Password login

The newly created account reaches the native login screen.

![Password login in EmbLinkOS](22-account-login.png)

### Merged desktop

The incoming graphite wallpaper, desktop and dock are preserved.

![Merged desktop in EmbLinkOS](23-merged-desktop.png)

### Appearance and wallpaper

The current Appearance pane retains theme, wallpaper, scale and accent controls.

![Appearance and wallpaper in EmbLinkOS](24-appearance.png)

### Wi-Fi status

Settings reports that this QEMU machine has no supported Wi-Fi radio. No physical connection is claimed.

![Wi-Fi status in EmbLinkOS](25-merged-wifi.png)

### HDA output

Settings discovers the emulated Intel HDA output; separate recorded-output tests measure real playback.

![HDA output in EmbLinkOS](26-hda-output.png)

### Mute applied

Pointer input enables mute, verified against the kernel audio state.

![Mute applied in EmbLinkOS](27-hda-muted.png)

### Saved output volume

Pointer input changes volume to 49%; the native test verifies kernel state and persistence after reboot.

![Saved output volume in EmbLinkOS](28-hda-volume.png)

### Display information

Settings reports 1280 × 800 VirtIO display and software rendering.

![Display information in EmbLinkOS](29-merged-display.png)

### Brightness round trip

The synthetic ACPI backlight fixture changes from 60% to 40%; kernel state is checked independently.

![Brightness round trip in EmbLinkOS](30-merged-brightness.png)

### Battery and power preferences

The pane uses synthetic ACPI batteries and lid data; mixed units leave total charge unavailable.

![Battery and power preferences in EmbLinkOS](31-merged-power.png)

### Keyboard settings

The incoming keyboard-layout pane remains accessible alongside the added laptop controls.

![Keyboard settings in EmbLinkOS](32-keyboard-layouts.png)

### Native notification banner

About EmbLink posts a real banner through the existing session notification service.

![Native notification banner in EmbLinkOS](33-notification-banner.png)

### Notification history

The same session message appears in the new history window.

![Notification history in EmbLinkOS](34-notification-history.png)

### Locked session

The native desktop owns the opaque password surface while the kernel gates other windows and input.

![Locked session in EmbLinkOS](35-merged-lock.png)

### Wrong password rejected

Authd refuses an incorrect password and the session stays locked.

![Wrong password rejected in EmbLinkOS](36-merged-rejection.png)

### Logout reaches login

Ending the native session stops its descendants and returns to login.

![Logout reaches login in EmbLinkOS](37-merged-logout.png)

### Desktop after a separate boot

The saved account signs in after a separate NVMe boot; volume preferences survive and cold integrity passes.

![Desktop after a separate boot in EmbLinkOS](38-merged-persistence.png)

### Recovery installation

Read-only recovery verifies and expands a disposable NVMe target while preserving the source image hash.

![Recovery installation in EmbLinkOS](39-merged-recovery.png)

### Installed disk boots alone

The installed disk boots without the source media through native NVMe into account setup.

![Installed disk boots alone in EmbLinkOS](40-merged-installed.png)

## Historical milestone (before the master merge)

The following captions describe the earlier implementation and its validation.
Current behavior and test evidence are in [LAPTOP_VALIDATION.md](../../LAPTOP_VALIDATION.md).

## Early bring-up to a persistent desktop

| Early UEFI status | Desktop after reboot and login |
| --- | --- |
| ![Early UEFI battery fixture](01-uefi-battery.png) | ![Persistent authenticated desktop](18-persistent-desktop.png) |

## Hardware bring-up

### Early UEFI battery status

An early firmware-status capture, before the desktop filled in. The 50% reading comes from a synthetic ACPI battery fixture.

![Early UEFI battery status in EmbLinkOS](01-uefi-battery.png)

## Desktop device controls

### Wi-Fi controls reach the desktop

The first integrated Wi-Fi Settings pane reports that QEMU has no supported Wi-Fi adapter. This is not a connected physical radio.

![Wi-Fi controls reach the desktop in EmbLinkOS](02-wifi-controls.png)

### Native sound output

Settings discovers the emulated AC97 stereo sink and starts at full volume. The native audio tests separately measured recorded PCM.

![Native sound output in EmbLinkOS](03-sound-output.png)

### Mute reaches the audio sink

Actual pointer input enables mute; the desktop acceptance test checks the corresponding kernel state.

![Mute reaches the audio sink in EmbLinkOS](04-sound-muted.png)

### Volume changes are applied

The output slider reaches 49% through the native audio control path. Microphone capture is still unavailable.

![Volume changes are applied in EmbLinkOS](05-sound-volume.png)

### Display and brightness controls

Settings reports VirtIO display with software rendering. A synthetic ACPI backlight fixture receives the brightness change from 60% to 40%.

![Display and brightness controls in EmbLinkOS](06-display-brightness.png)

### Power and automatic locking

Idle/lid locking and dimming preferences are wired up. Synthetic mixed battery units deliberately produce an unavailable combined percentage; sleep remains unavailable.

![Power and automatic locking in EmbLinkOS](07-power-preferences.png)

## Authenticated sessions

### First account setup

The native GUI test enters a disposable test account and masked password through real keyboard and pointer input.

![First account setup in EmbLinkOS](08-first-account.png)

### Password login

Normal boot presents the login screen after account creation instead of development autologin.

![Password login in EmbLinkOS](09-login.png)

### Lock screen

The authenticated session is hidden behind its unlock greeter after Ctrl+Alt+L.

![Lock screen in EmbLinkOS](10-locked.png)

### Incorrect passwords keep the session locked

The GUI test submits an incorrect password and verifies that the kernel session gate stays locked.

![Incorrect passwords keep the session locked in EmbLinkOS](11-password-rejected.png)

## Everyday desktop

### Files and Terminal together

Both applications run in the authenticated session. Alt+Tab and Alt+Shift+Tab are checked against the compositor focused-window ID.

![Files and Terminal together in EmbLinkOS](12-app-switching.png)

### Notification center

A native application posts Build / Workspace ready inside the live session. The notification center displays its application source and Clear action.

![Notification center in EmbLinkOS](13-notifications.png)

### Files in the live home

Files is launched from the dock with the live session home and its standard folders, including Trash. Copy/move/Trash/Restore behavior has separate native tests.

![Files in the live home in EmbLinkOS](14-files-home.png)

### Current Wi-Fi Settings

The current desktop continues to report absent radio hardware accurately. Physical AX200 WPA2/DHCP/DNS acceptance is still outstanding.

![Current Wi-Fi Settings in EmbLinkOS](15-wifi-status.png)

## Session persistence

### Logout from the desktop

The GUI regression chooses Log Out while a session child repeatedly writes and synchronizes a 64-KiB file.

![Logout from the desktop in EmbLinkOS](16-logout-menu.png)

### Logout returns to login

The whole former session has stopped, and a cold filesystem integrity check succeeds after the active writer is drained.

![Logout returns to login in EmbLinkOS](17-after-logout.png)

### Desktop after a new boot

A new VM boot uses the saved account, authenticates successfully and starts with an empty notification history.

![Desktop after a new boot in EmbLinkOS](18-persistent-desktop.png)

## Recovery and installation

### Verified installation from recovery

The recovery console completes a verified copy from a padded source image to disposable NVMe storage, expands the root and prints device/mount diagnostics.

![Verified installation from recovery in EmbLinkOS](19-recovery-install.png)

### Installed disk boots alone

The newly installed NVMe target boots without the source disk and presents first-account setup. Both disks are disposable QEMU images.

![Installed disk boots alone in EmbLinkOS](20-installed-first-boot.png)
