# fteqw-sv: FTE QuakeWorld server with NextBot-style bots

This is a fork of [FTEQW](https://github.com/fte-team/fteqw), the engine
[FortressOne](https://www.fortressone.org/) servers run on. It adds server-side bots that join
like real players and play:

* QuakeWorld deathmatch: free-for-all and teamplay, with id's QW game code;
* Team Fortress: [FortressOne](https://github.com/FortressOne/server-qwprogs) and the classic
  TeamFortress QuakeWorld 2.8/2.9 progs. Every class and every map type the TF goal system
  describes: capture the flag (2fort5r, well6, 4-team 4f4), escort maps (border1), Hunted, and
  maps that score through touch goals and team triggers.

The bots need nothing from the game code: no QuakeC changes, no waypoint files. They use the
same inputs as a player (a usercmd every frame, impulses, client commands), so the stock game code
runs unchanged. Anything else in the server works as in upstream FTE; the bots are off unless you
set a quota.

The rest of this file after the next section is upstream FTEQW's README.

## The bots

The bots use a NextBot-style design (the architecture Valve described for Left 4 Dead and
TF2): a bot has locomotion, body (aim), vision and an intention, which is a stack of actions that
respond to events. Actions can continue, change to another action, suspend themselves for one,
or finish. This is a clean-room implementation in C under the GPL. It contains no Valve code.

* **Nav mesh.** Generated from the BSP the first time a map is played (a few seconds) and saved
  as `<gamedir>/nav/<map>.qnav`. The generator walks the map with the player hull from every
  spawn point, item and objective. It handles steps, jumps, drops, water, lava and slime,
  teleporters, jump pads (`trigger_push`), lifts (`func_plat` and elevator doors), and doors and
  hurt triggers. A mesh is regenerated automatically when its map changes.
* **Deathmatch.** Bots collect weapons, armour, health, ammo and powerups by need and distance.
  They choose a weapon for the range, lead projectiles, strafe without walking off ledges, chase
  enemies they lose sight of, climb out of lava, and roam.
* **Team Fortress.** The map's goal entities are read live:
  * `item_tfgoal` flags and keys, and their capture points (`items_allowed`);
  * touch goals and team triggers that score;
  * detpack goals, spanner goals and resupply rooms;
  * hazards, team-only doors, civilian teams and class restrictions.

  Teams that score from the same goal are treated as allies (Hunted's bodyguards and VIP,
  border1's Immigrants and MiB). Bots pick a team and class like players do, respecting
  `illegalclasses` and the `cr_*` limits, and get a mix of attackers and defenders. What they do:
  * steal flags, bring them to the capture point, escort friendly carriers and hunt enemy ones;
  * guard their own flag;
  * throw both grenade types, timed against the fuse;
  * snipers charge the rifle while standing still;
  * engineers build and upgrade sentry guns;
  * medics heal;
  * demomen set detpacks on detpack goals (well6's grates);
  * spies disguise as the enemy team;
  * on Hunted, the VIP heads for the escape lift and the bodyguards stay with him. Team-only
    teleporters and the President-only hazard are respected;
  * on escort maps with an escape vehicle (border1's truck: a door that travels far and holds a
    team trigger), the escaping team jumps into the vehicle, stands on its start trigger, and
    fights from inside it without stepping off while it crosses.

  FortressOne and the classic progs use different impulses; the bots detect which one is running.

### Settings

The `qw_bot_*` names are compatible with the earlier fteqw-sv bot build, so existing
`server.cfg` files keep working.

| Cvar | Default | |
|---|---|---|
| `qw_bot_enable` | 1 | 0 removes all bots |
| `qw_bot_quota` | 0 | number of bots |
| `qw_bot_quota_mode` | 1 | 0 = fixed number of bots, 1 = fill to `qw_bot_quota` players (humans replace bots) |
| `qw_bot_skill` | 0.6 | 0..1: aim speed, reaction time, accuracy |
| `qw_bot_force_class` (`qw_bot_forceclass`) | "" | TF: class for every bot (`soldier`, `medic`, ... or 1-9) |
| `qw_bot_stucktime` | 0 | seconds without scoring before a bot is respawned (0 = off) |
| `qw_bot_meleeonly` | 0 | axe only |
| `qw_bot_skins`, `qw_bot_random_colors` | | deathmatch skins and colours (the `skins/` folder is used when present) |
| `qw_bot_spawnloadouts` (+ `_time`, `_ssg`, `_ng`, `_sng`, `_gl`, `_rl`, `_lg`) | 0 | deathmatch: bots spawn with a random extra weapon |
| `qw_bot_nav_autogen` | 1 | generate a nav mesh for maps that have none |
| `qw_bot_mapcycle` | 1 | game-code map changes follow `<gamedir>/mapcycle.txt` (one map per line) |
| `qw_bot_verbose` | 1 | bot messages in the console |
| `qw_bot_debug` | 0 | 1 = stuck/death details, 2 = every action change |
| `discord_webhook_url` | "" | post real players' joins, leaves and chat to a Discord webhook |

| Command | |
|---|---|
| `qw_bot_add [count] [team] [class] [skill] [name]` | add bots (TF form; deathmatch: `qw_bot_add [count] [skill] [name]`) |
| `qw_bot_kick <name\|blue\|red\|yellow\|green\|all\|N>` | remove bots |
| `qw_bot_status` | every bot: team, class, health, frags, position, what it is doing |
| `qw_bot_nav_generate` | regenerate and save the current map's nav mesh |
| `qw_bot_navcheck` | reachability report: share of the mesh, items and goals reachable from the spawns |
| `qw_bot_tfinfo` | the TF goals the bots found on this map |
| `qw_bot_navinfo <slot>`, `qw_bot_navreach x y z`, `qw_bot_pathpts ...`, `qw_bot_pathtest <slot> <goal>` | debugging |

Other changes the bot build makes to the server:

* It ends intermission even when no human is there to press a button. It also changes map
  through `mapcycle.txt` when FortressOne's `GotoNextMap` would restart the same map.
* It adds `map` to the server status, for Gamedig and server lists.
* Bots apply the `color`/`skin`/`team` stuffcmds the game code sends, so TF's team-cheat check
  accepts them.

### Building

Same as upstream. The Windows server binary is cross-compiled with mingw-w64, for example under
WSL Ubuntu:

```
cd engine
make sv-rel FTE_TARGET=win64      # release/fteqwsv64.exe
make sv-rel                        # Linux: release/fteqw-sv
```

The bot code is in `engine/server/nextbot/`. `nb_local.h` describes the design.

### Install

Replace the server binary (`fteqwsv64.exe`, or `fortressonesv64.exe` for a FortressOne package)
and set `qw_bot_quota` in `server.cfg`. Nav meshes generate on first use.

### Tested

On a copy of a FortressOne server package (FTE server, 90 QW and 90 Fortress maps), with id's QW
game code, TeamFortress QuakeWorld 2.8 and FortressOne v1.0.4:

* Meshes generated for every map; 88 of the 90 Fortress maps have one.
  * On 77 of them, 95% or more of the mesh is reachable from the spawn points.
  * On 39, every item and every touch goal is reachable.
  * The missing items are mostly on rocket-jump ledges.
* Deathmatch: dm4, dm6.
* CTF: 4f4 (four teams) and 2fort5r.
* Hunted: the VIP reaches the escape lift with bodyguards around him.
* border1: the Immigrants ride the truck across and score.
* FortressOne: the bots join, change class and fight on 2fort5r.

### Limits

* Bots don't rocket-jump or concussion-jump, so items only reachable that way are skipped.
  `qw_bot_navcheck` lists them for a map.
* `func_train` lifts are not linked.
* A few maps generate no mesh because they have no spawn points the generator can stand on
  (in our set: 1on1r, endif).
* The bots are testers' opponents, not tournament players: they don't time items or call
  strategy.

---

# [FTEQW](https://fteqw.org)

![FTEQW Logo](engine/client/fte_eukara.ico)

Powerful engine for playing and modding idTech based games.

# What is FTEQW?

FTEQW is an advanced and portable Quake engine. It supports multiple games running on idTech, plus its own set of games that developers have created.

Due to the vast amount of supported formats, features, and innovations inside the engine and its very own QuakeC compiler (FTEQCC), it's very much considered the swiss-army knife of Quake engines.

### Highlights:

- Single & multi-player support
- Supports multiple games
- Vast amount of map, model, & image formats are supported
- Advanced console, with descriptions & autocompletion
- Plugin support, enabling use of FFMPEG, Bullet/ODE physics & more
- Extensive suite of QuakeC/entity debugging features
- Deep integration with FTEQCC (fork of QuakeC created for FTEQW), which can even be executed in-game
- Support for split-screen local multiplayer
- Voice-chat via Opus & Speex
- Support for hundreds of players on a single server
- Works on Windows, Linux, OpenBSD... & more
- New features are added all the time in cooperation with modders

# Contributions

Contributions and help is always welcomed.

### Guidelines:

- Be kind and respectful
- We follow [Gentoo's AI policy](https://wiki.gentoo.org/wiki/Project:Council/AI_policy), **_It is expressly forbidden to contribute any content that has been created with the assistance of Natural Language Processing artificial intelligence tools. This motion can be revisited, should a case been made over such a tool that does not pose copyright, ethical and quality concerns._** IBM also has a [great page](https://www.ibm.com/docs/en/watsonx/saas?topic=ai-risk-atlas) listing the many risks with current language models
- Engine contributions must be GPL2 licensed. Plugins may be under any other GPL-compatbile licenses
- This codebase follows [TRIPS copyright laws](https://en.wikipedia.org/wiki/TRIPS_Agreement)
- Always give credit from other codebases and make sure licenses are compatible
- Test your changes and ensure nothing else has been broken (games, plugins, formats, etc)

# Reporting Issues

Bug reports are welcomed! :)

### Required Information:

- Your system information such as your **Operating System** and **Hardware** (GPU/CPU)
- If the binary is pre-built (e.g. from fteqw.org) or if it was built manually
- What version of FTEQW you're using (type `version` in console)
- If it is a supported game/mod/plugin/etc you're having issues with, then provide the version info for it, and tell us how it should be behaving
- Make sure you have read the included documentation and ensure you have done everything right
- Remember to double check the problem hasn't already been reported
- Screenshots and/or video are generally desired if it is a visual malfunction

**Windows Users**

Please make sure you have not renamed your executable, `fteqw.exe`, to be `winquake.exe` or `glquake.exe` as Windows attempts compatability fixes that are not required for FTEQW and will cause problems.

# Documentation

Please see the `documentation` folder inside the repo for building, using the engine, tools, and more.

The `specs` folder is for more advanced users seeking QuakeC and idTech file format related information or examples.

# Contact

### Matrix

https://matrix.to/#/#fte:matrix.org

### IRC

**Server:** irc.quakenet.org

**Channel:** #fte

### Forums

**[Spike](https://forums.insideqc.com/memberlist.php?mode=viewprofile&u=26)** and **[eukara](https://forums.insideqc.com/memberlist.php?mode=viewprofile&u=949)** can be found on [insideqc.com](https://forums.insideqc.com/)

### Discord

https://discord.gg/p2ag7x6Ca6

# Credits

Please see the `Credits.md` file.

# License

Copyright (c) 2004-2025 FTE's team and its contributors
Quake source (c) 1999 id Software

FTEQW is supplied to you under the terms of the same license as the
original Quake sources, the GNU General Public License Version 2.
Please read the `LICENSE` file for details.

# Download

The latest source & binaries are always available at:

[fteqw.org](https://fteqw.org)

[fteqcc.org](https://fteqcc.org)
