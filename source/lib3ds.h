/**
 * @file
 * @brief Nintendo 3DS console backend declarations (see lib3ds.cc).
**/
#pragma once

#ifndef O_BINARY
#define O_BINARY 0
#endif

// Called first thing in main(): brings up the screens, a debug console on
// the bottom screen for early errors, mounts romfs and creates the save
// directory tree on the SD card.
void lib3ds_early_init();
// Boot trace: appends one line to sdmc:/3ds/crawl/boot.log
void lib3ds_boot_mark(const char *what);
// Item / feature description actions as bottom-screen buttons (describe.cc).
#include <string>
#include <utility>
#include <vector>
void lib3ds_set_actions(const std::vector<std::pair<std::string, int>> &actions);

// Same contract as libunix/libw32c: "headless" is only used by crawl's
// test/arena modes; never true in normal play on the 3DS.
bool in_headless_mode();
void enter_headless_mode();
void fakecursorxy(int x, int y);
