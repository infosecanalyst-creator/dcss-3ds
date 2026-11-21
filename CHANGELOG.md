# Changelog

## 2026-11-21

- Long menus page properly: dragging on the touch screen, Pg Up / Pg Dn and L / R now send the keys Crawl's menus really use (before, some of them did nothing).
- The alert strip names the monster that came into view, and no longer misreads the map edge.
- The dashboard builds the monster list once per screen update (smoother).
- HOME works while the first-start database build is running.
- The HOME hand-off happens on the main thread (more reliable).
- Quieter touch click.
- Compiler warnings in the port code fixed.

## 2026-11-20

- One-item menus get their button instead of opening the keyboard.
- The menu shows "ZL Swap" only where ZL actually swaps.
- Boot, sound and HOME logs are written only with an empty /3ds/crawl/debug.txt (the exit line always); a log past 1 MB starts over.
- Install path: the program is /3ds/crawl.3dsx. The START menu shows the build version.

## 2026-11-19

- Crawl's menus page by dragging the stylus; rows act when you lift it.
- Alert strip over the map: low health, big hit, poisoned, monster in view.
- Manual: drag the stylus to scroll the text, with a little momentum.
