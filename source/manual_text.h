#pragma once
// Generated from docs/manual.txt
static const char *const kManualText = R"MANUAL(# Welcome
Dungeon Crawl Stone Soup is a roguelike: a turn-based dungeon adventure where nothing happens until you act, every level is new each game, and death is permanent.

Your goal is to find the Orb of Zot in the depths of the dungeon and carry it back to the surface. Along the way you collect runes, find gods to follow, and learn what works for your character.

## This port
The New 3DS version is the full game with Crawl's own tile art. The touch screen is the dungeon and the top screen shows your character, the monsters in view, a map and the messages.

Press Start at any time to open this manual. The game waits while you read.

## Using this manual
- Circle Pad or C-Stick up/down: scroll the text (hold to keep scrolling).
- D-Pad: move between the sections on the touch screen. L / R: previous / next section.
- Tap a section on the touch screen to jump to it.
- B, Start or Close: back to the game.

# The two screens
## Touch screen
The dungeon, 20 x 15 squares centred on you, so everything you can see fits on screen. Darker squares are remembered but out of sight.

Buttons run down both edges, outside your line of sight, so they never hide a monster.

## Top screen
- Your name, health and magic bars, defences (AC, EV, SH), stats, level and place.
- Status effects in colour under your stats (poisoned, hasted, and so on).
- In view: the monsters you can see, with their pictures.
- A map of the whole level. The box shows the part on the touch screen.
- The messages: what just happened. The newest line is at the bottom.

# Touch controls
| Tap a square | walk there (several steps are taken for you) |
| Tap a monster | attack it if it is next to you, otherwise walk towards it |
| Tap yourself | take the stairs you stand on, pick up items here, or wait a turn |
| Tap during --more-- | continue |
| Yes / No buttons | answer the question in the messages |

## Left column
| explr | explore the level automatically |
| fight | attack the nearest monster |
| rest | rest until healed (stops if a monster appears) |
| wait | wait one turn |
| pick up | pick up what is on your square |
| up / down | take the stairs, or travel to the nearest ones |

## Right column
| items | your inventory |
| cast | cast a spell |
| abil | use an ability (gods, species) |
| map | the level map |
| char | your character overview |
| msgs | read the past messages |
| keys | the touch keyboard (any key Crawl knows) |

# Buttons
| D-pad | move one square (hold to keep going) |
| Circle Pad | move in 8 directions, diagonals too |
| A | fight the nearest monster |
| B | cancel / back |
| X | explore |
| Y | pick up |
| L | zoom the dungeon map out |
| R | zoom the dungeon map in (three levels; remembered) |
| C-Stick up | inventory (press again to close) |
| ZL | swap the screens: the game fills the top screen and the touch screen becomes a touchpad (slide moves the pointer, tap taps there) |
| ZR | cast a spell (press again to close) |
| Select | touch keyboard |
| Start | this manual |
| HOME | save and quit |

## On questions and menus
A answers Yes and continues past --more--. B answers No or backs out.

## Left-handed play
At the bottom of the manual's touch screen, "Left hand" swaps the D-pad with the A, B, X and Y buttons: Y and Up, A and Right, B and Down, X and Left. It is remembered, and applies to this manual too.

# Menus and typing
Menus (inventory, spells, character creation, shops) appear on the top screen. The touch screen lists every choice as a big button: tap one to pick it.

At the bottom: Esc, Enter, Page Up, Page Down and Keys.

When Crawl wants you to type (your name, a travel destination, a search), the touch keyboard opens by itself. Shft and Ctrl stick for one key. The arrow keys are in its top row, next to Close. Close it with Close or Select.

## Quick commands
The keyboard's Cmds key (bottom right) swaps the letters for blue buttons: the commands without a button on the play screen. Tap keys, Cmds, then one of them; the keyboard closes by itself. ABC brings the letters back.
| Fire | fire or throw what is quivered |
| Learn | learn a spell from a book |
| Look | look around, examine monsters |
| Close | close a door |
| Skills | choose what to train |
| Travel | travel to another level |
| Search | find items or places you have seen |
| Religion | your god and standing |
| Overview | the dungeon overview |
| Mutation | your mutations |
| Swap | swap weapons |
| Orders | give orders to allies |

## Commands without a button
Open the keyboard and press the key. The common ones:
| w | wield a weapon |
| W / T | wear / take off armour |
| P / R | put on / remove jewellery |
| q | drink a potion |
| r | read a scroll |
| f | fire or throw what is quivered |
| V | evoke a wand or other item |
| d | drop |
| p | pray (at an altar: join a god) |
| G | travel to another level |
| x | look around |
| ? | Crawl's full help |

## How the game names keys
Crawl's hints and help name the 3DS controls:
| [items] | a button on the touch screen (here: items) |
| A / B | the A and B buttons |
| L / R | page up / down in menus |
| [keys] r | tap keys, then press r on the keyboard |

# Your first game
## Picking a character
On the main menu pick Dungeon Crawl, then a species and a background. Good first characters:
- Minotaur Fighter: strong, tough, simple.
- Gargoyle Fighter: hard to hurt.
- Troll Fighter (claws): heals fast, hits hard.
- Mountain Dwarf Berserker: follows Trog, a war god, from the start.

Choose Tutorial for Dungeon Crawl on the main menu for guided lessons.

## The rhythm of play
1. Press explore. Your character walks until something interesting happens.
2. When a monster appears, fight it (A or the fight button), or step back if it is dangerous.
3. Rest to heal before exploring on.
4. Pick up what you find, try it, and wear better armour.
5. When the level is explored, take the down stairs.

# Exploring and fighting
- Explore stops for monsters, items and features. Press it again to carry on.
- Fight attacks the nearest monster but will not walk into danger blindly. It stops when your health is low.
- Fight in corridors so only one monster can reach you at a time.
- Retreat up the stairs if a fight goes badly: monsters next to you follow, others do not.
- The colour of a monster's name in the In view list shows how dangerous it is: grey is easy, yellow tough, red very dangerous.

## Health
Your health regenerates slowly. Resting is free unless a monster is near. Potions of heal wounds are your emergency button.

# Items
- items (or C-Stick up) shows your inventory. Tap an item: its actions (Read, Drink, Wear, Drop...) appear as buttons on the touch screen. Tap one.
- Reading a scroll: items, tap the scroll, tap Read. Drinking a potion is the same, with Drink.
- Unknown potions and scrolls are identified by using them. Try them when you are safe and at full health.
- Better armour and weapons are worth carrying; Crawl tells you their bonuses when you inspect them.
- Gold buys items in shops. Walk into a shop and press > to enter.

# Magic and gods
- cast (or ZR) lists your spells. Pick one, then choose the target: tap it on the map, or move the cursor and press A.
- abil lists your species and god abilities.
- Find an altar, stand on it and press p on the keyboard to learn about its god and join. Each god asks for different things and grants different powers.

# Status effects
Effects show in colour on the top screen under your stats: for example Poison, Slow, Haste, Might, Conf (confused), Weak.

- Poison hurts every turn until it wears off.
- Slow and Conf are dangerous in a fight: get away first.
- New effects are also announced in the messages.

# Saving and loading
- HOME saves the game and quits. Your character is waiting on the main menu next time.
- There is one save per character. Death is permanent: the game ends and goes to the high scores.
- Saves, morgue files (a record of each game) and your settings are in sdmc:/3ds/crawl/.

# Tips
- Explore one level at a time and do not dive too fast.
- Unknown potions and scrolls are safest to try at full health with no monsters around.
- If you are overwhelmed, read a scroll of teleportation or blinking, or use the stairs.
- Look at monsters before fighting them: x on the keyboard, then move the cursor onto them.
- Crawl's own help (? on the keyboard) explains every command in detail.

# Troubleshooting
## The screen is too dark
Open the keyboard (the "keys" button or SELECT) and tap Bright to step through five brightness levels (0 is the original look, 2 is the default). Your choice is remembered. The 3DS's own brightness setting in the HOME Menu also helps; power-saving mode dims the screens.

## The first start takes minutes
Crawl builds its databases and level cache when the game data changes (rarely, with a new version). Other starts take seconds.

## The screen looks wrong or a button does nothing
Press B to back out, then try again. Send a photo of anything that looks broken.

## The game crashed
Your character is saved at most a few turns back. Errors are written to sdmc:/3ds/crawl/stdout.txt.

## Changing this manual
A file named manual.txt in sdmc:/3ds/crawl/ replaces the built-in text. Sections start with "# ", headings with "## ", list items with "- ", numbered steps with "1. ", table rows with "| ".
)MANUAL";
