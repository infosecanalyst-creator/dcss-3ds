# Changelog

## 2026-11-21

- Touch keyboard open in the dungeon: L and R no longer send "go up" / "go down" (on a staircase they took the stairs); they do nothing there. B closes the keyboard. Shft and Ctrl never stay latched: they are let go when the keyboard closes or a button is pressed. Close is in gold.
- With the keyboard open in the dungeon the buttons keep their play jobs (A fights, X explores) unless Crawl is waiting for typed text.
- B in the dungeon no longer prints "Unknown command." when there is nothing to cancel. X pressed while exploring no longer stops it with "Key pressed, stopping explore." (B stops it).
- Main menu: B and Esc no longer quit the program at once; leave with START > EXIT GAME.
- Species, background and weapon pickers: every choice has a button now (the lists were cut off after 21); a long list is shown in pages (More 1/3, Back, or L / R). Labels are in one font size on one line where they fit, group headings no longer end up inside the button beside them, and choices Crawl does not recommend no longer look switched off.
- START menu: full-width buttons, a bigger footer that names the real buttons in left-hand mode, and a "MENU OPEN: THE GAME WAITS" strip on the top screen.
- Screens swapped (ZL): the touch keyboard stays on the touch screen.
- The keys button reopens the page last used (letters or Cmds), so a quick command is two taps away.
- The top screen's level map is drawn at two dots per square (it was one: a smudge).
- "explr" is spelled out: explore.
- Main menu: every game and saved game is a button on the touch screen (it was a text list worked with a keyboard), and "quick-load last game" is the first button once there is one. The name box starts filled in (Hero the first time, then the last name used), so there is no empty name prompt and no second name question after the weapon.
- Before the dungeon (main menu, species, background, weapon, name question) the top screen shows a card in the normal font with the screen's heading and explanation, instead of 80 columns of 5 px text with PC key hints.
- The game's own help (? in the dungeon) no longer stops the game: its three help files (manual, aptitudes, quick start) were missing from the program and are built in now.
- The first start no longer builds the databases and level cache for minutes: they come ready-made inside the program (about 11 MB more). The old build on the SD card remains as a fallback, and now also lists the databases as it builds them.
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
