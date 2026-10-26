# Screenshots

The images the project `README.md` shows. Replace a file to update it; the names below are the ones the README links.

## Files

| File | Screen | Size | What it shows |
|---|---|---|---|
| `preview.png` | Both | 400x500 | Header image: the top screen over the bottom screen. |
| `top_gameplay_wide.png` | Top | 400x240 | Gameplay in `WIDE`. |
| `top_gameplay_bezel.png` | Top | 400x240 | `PIXEL PERFECT` with the GBA frame and the LCD filters. |
| `title_wide.png` | Top | 400x240 | Title screen in `WIDE` (nothing generated, black at the sides). |
| `title_wide_ai.png` | Top | 400x240 | Title screen in `WIDE + AI`. |
| `bottom_map.png` | Bottom | 320x240 | **MAP** tab: the area map, zoom and item counts. |
| `bottom_status.png` | Bottom | 320x240 | **STATUS** tab: equipment, abilities and downloaded maps. |
| `bottom_states.png` | Bottom | 320x240 | **STATE** tab: the list of save states. |
| `bottom_achievements.png` | Bottom | 320x240 | **ACHIEVEMENTS** tab: the RetroAchievements list. |
| `bottom_options.png` | Bottom | 320x240 | **OPTIONS** tab: display, controls and updates. |

Use the English interface and turn the FPS counter off (OPTIONS -> DISPLAY) before taking them.

## How to take them

### From the PC, with a debug build running

`tools/mzm-link/mzm-link.py` takes both screens as PNG, already the right size and orientation
(`docs/3ds-debug-tools.md`, "FTP server, install and remote control"):

```sh
tools/mzm-link/mzm-link.py shot /tmp/mzm-shot      # top.png and bottom.png
```

It can also tap the bottom screen (`touch X Y`) to switch tabs and open the settings.

### With Luma3DS

1. On the console, open the Rosalina menu (`L + Down + Select`) and choose **Take screenshot**.
2. The images are saved on the SD card in `/luma/screenshots/`.
3. Copy them to the PC, name them as in the table and put them in this folder, replacing the existing ones.
4. Rebuild the header image:

   ```sh
   python3 tools/process_screenshots.py --composite
   ```

### From the debug tools' screen dump

1. In a build made with `DEBUG_TOOLS=1`, open the **DEBUG** tab and tap **DEBUG TOOLS -> SCREEN DUMP**.
2. The `.rgb` dumps are written to `sdmc:/3ds/Metroid Zero Mission 3DS/debug/`. Copy that folder to the PC.
3. Convert them:

   ```sh
   python3 tools/process_screenshots.py --import-dir /path/to/debug/
   ```

   The converter rotates the console's portrait orientation to landscape and turns BGR8 into RGB8.

`--placeholders` writes a labelled grey image for each name that does not exist yet; it never
replaces a file that is there.
