# Colorsoft: the first test on the device

The app picks how color reaches a Colorsoft at start (spec: `docs/superpowers/specs/2026-11-22-colorsoft-design.md`).
Nobody has run it on one yet. One run of the steps below shows which way works and which waveforms look right.

## 1. What the app found

Start Zotero once as usual, close it, and copy these lines out of `/mnt/us/zotero/log/zotero.log`:

```sh
grep -E "FBInk:|X: |color" /mnt/us/zotero/log/zotero.log | tail -20
```

- `FBInk: ColorSoft 2 (Julep, …), view …, 32 bpp, …, color panel yes, MTK yes`: FBInk knows the device.
  The model name is one of `ColorSoft`, `Scribe ColorSoft` or `ColorSoft 2`. `color panel` must say `yes`.
- `X: default visual depth 24 (32 bpp), TrueColor, masks ff0000/00ff00/0000ff; …: 32-bit color image`. The X
  server can show color, so the app uses the **X path**. `depth 8 …: 8-bit gray image` means X is gray, so the
  app uses the **framebuffer path**.
- `color: panel yes, setting on, … -> path x|fb, color on`: the choice. `path gray, color off` on a Colorsoft is
  wrong, and the lines before it say why.

## 2. The test card

Close Zotero first, because two copies must not run at once. Then, over SSH or in kterm:

```sh
cd /mnt/us/zotero && DISPLAY=:0 ./bin/zotero --color-test 2>&1 | tee log/color-test.log
```

The test shows a card of color bars, a gray ramp, a rainbow and Zotero's eight highlight colors. Before each
step the screen flashes white. Each step stays up for 3 s, and its heading names the step, the path and the
waveform:

1. `path gray  waveform GC16`: what every Kindle shows today, in gray.
2. For each path the device has (`x`, `fb`, or both): `GL16`, `GLRC16`, `GC16`, `GCC16`, `AUTO`.

**Take a photo of every step**, or at least note what changes from one step to the next. For each step, look at:

- **Colors at all:** are the bars red, green, blue, cyan, magenta and yellow, or gray?
- **Where the card sits:** does it fill exactly the app's window, below the status bar? It must not cover the
  status bar or be shifted (`fb` path: shifted or doubled pixels mean wrong framebuffer coordinates).
- **The gray ramp:** 16 distinct steps from black to white, with no color cast.
- **The highlight rows:** is the text on each tinted row easy to read, and are the tints distinguishable?
- **Flashing and speed:** did the update flash? How long did it take? The log gives the time of each
  refresh (`refresh done in … ms`).

Send back the photos and `log/color-test.log`.

## 3. The app itself

Open a PDF with color figures and an EPUB with color images, make a highlight in each color, and then check:

- **Pages and highlights:** the PDF's figures and the EPUB's pictures are in color, text stays black, and each
  highlight shows its Zotero color as a light tint with readable text. Underlines, the annotation bar's color
  dots and the color marks beside highlights in Notes are in the full color. Do page turns feel slower than
  with Color off? (Color pages take three times the memory, so only the next page is drawn ahead, not the
  previous one.)
- **Reader → Display:** the panel shows a **Color** row with On and Off, but only on a color panel. Turn it Off.
  The whole screen should redraw in gray and look as it does on any other Kindle, and the page should be drawn
  again in gray. Turn it On again: the page comes back in color.
- **Landscape:** turn to landscape in Display and look at the log. A line saying
  `color: the framebuffer's view … isn't turned as the window … gray here` means that on the framebuffer path
  the framebuffer doesn't turn with the window. The app then stays gray in landscape on purpose, so that it
  never writes outside its window. Say whether you saw this line.
- **Nothing drawn over the system's windows**, which matters most on the `fb` path. With Color on, open
  the keyboard (Search) and type, pull down the quick settings from the top, and let the screensaver
  come on while a sync runs. The app must never draw over the keyboard, the pulldown or the screensaver.
  The log shows `color: framebuffer writes held (…)` while something covers the window and `resumed` after.
  When the keyboard closes over a colored page, the window should show in color again (`color: the window
  again in color`).
- **The `fb` path writes only color.** Only an update that has color in it goes to the framebuffer; the home
  screen, the library, menus and gray pages are drawn by X alone, exactly as on a gray Kindle. If the
  framebuffer path garbles a page (shifted or doubled pixels), the rest of the app should still look right,
  and Reader → Display → Color Off is the way out. If a framebuffer write fails, the log says
  `color: writing … to the framebuffer failed (…): no framebuffer path` once, and the app leaves the `fb` path (gray, or `x` where X has color) until it
  is started again.
- **Menus and typing** should be as fast as on other Kindles. Color waveforms are only used where there is
  color.

## 4. Overrides, if something looks wrong

Each override is set when Zotero starts, for example:
`ZK_COLOR_PATH=fb sh /mnt/us/documents/Zotero.sh` over SSH. They work the same way with `--color-test`.

| Variable | Effect |
|---|---|
| `ZK_COLOR=0` | Treats the device as a gray Kindle, exactly as before (no Color row, no color). |
| `ZK_COLOR=1` | Treats the device as a color panel even if FBInk says it isn't one. |
| `ZK_COLOR_PATH=x` / `fb` | Forces that path where it is possible. The log says which path was taken. |
| `ZK_COLOR_WFM=auto` | Lets the driver choose the waveform (AUTO) instead of GL16/GLRC16/GC16/GCC16, for comparison. |
| `ZK_COLOR_BOOST=<pct>` | Sets the saturation boost of page content, PDF pages and EPUB pictures (0 to 100, default 25). Try 0 and 50 if colors look washed out or garish. |

## What to report

1. The `grep` lines from step 1.
2. The photos and `log/color-test.log` from step 2: which steps showed color, and which looked best for text
   and for images.
3. Whether the landscape line from step 3 appeared, and how Color Off looked.
