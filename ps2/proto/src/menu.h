/* Start-button menu: the Warrior's equipment, inventory and status with DSR's names and icons (read-only) */
#ifndef MENU_H
#define MENU_H

#include "player.h"

/* pressed: buttons that went down this frame (libpad PAD_* bits). Start opens / closes, the d-pad moves the cursor,
 * Circle closes. Returns 1 while the menu is open (the game then ignores movement input). */
int menu_update(unsigned short pressed);
void menu_draw(const Player *p);
int menu_open(void);  /* 1 while any menu screen is up */

#endif
