# Ghost Recon: Future Soldier for PS5

**Tom Clancy's Ghost Recon: Future Soldier (Xbox 360) running natively on a jailbroken PS5**, upscaled with FSR, with PlayStation buttons, DualSense features, trophies and System Link multiplayer over the internet.

![Gameplay on the PS5](docs/screenshots/gameplay.jpg)

| PS5 and PC in one match through the relay | Title screen |
|---|---|
| ![Scoreboard](docs/screenshots/crossplay-scoreboard.jpg) | ![Title](docs/screenshots/title.jpg) |

> ### Built on ReXGlue
> This port exists because of **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, the static recompiler and runtime that turns Xbox 360 code into native code, and **elripalda**'s PS5 layer for it, which the native PS5 ports before this one were made with. The recompiled game and most of what it does come from their work. This release takes Ghost Recon: Future Soldier to the PS5.
>
> All the credit goes to them. Go star the original projects and support them.

## Download
Get the latest version from **[Releases](../../releases/latest)** or from the download page, **[grfs-ps5.duckdns.org](https://grfs-ps5.duckdns.org)**.

## What's in it
- The campaign and multiplayer recompiled to run natively on the PS5
- Rendered at 2080x1200 and upscaled to 4K with AMD FSR, gameplay at 30 FPS and menus at up to 60 FPS
- PlayStation button prompts and DualSense controller diagrams
- DualSense rumble, adaptive triggers (R2 a gun's trigger, L2 a light resistance) and light bar
- Trophies, from the main menu, earned ones first
- System Link over your LAN, or over the internet through the project's relay server ("Online (VPS)"), PS5 and PC in the same match
- Multiplayer progression in System Link: class XP, ranks and the decision points' item choices, kept on the server for each console
- Attachment credits (one per rank) and the attachments you bought, kept on the server too
- No online pass or account linking needed: those checks always pass
- Saves backed up to the server and restored on a new install
- Ghost Feed news, with a notice when a newer release is out
- Installs the game from your own `.iso`, found on the console or a USB drive
- Shaders from earlier play shipped with it, so the first matches don't stutter
- Texts in Portuguese and English

## What you need
- A jailbroken PS5 that runs homebrew apps from `/data/homebrew/`
- Your **own** copy of Ghost Recon: Future Soldier for the **Xbox 360**, as an `.iso`. No game files are included.
- A payload loader (etaHEN, elfldr or any other on port 9021) for a fast install. Without one it still works, just much slower.
- About 13 GB free for the game, plus the `.iso` until it's installed

## Which files work
| | Works |
|---|---|
| Disc image | The Xbox 360 disc as an `.iso` (title ID `55530833`) |
| Extracted files | The disc's files extracted by hand, with `default.xex` and `AICLASS_X360_F.xex` |

These don't work:
- PC or PS3 files of the game
- A disc image that isn't the Xbox 360 game, or a bad or modified rip

> [!TIP]
> ### 💙 Enjoying the port?
> If you've gotten some enjoyment out of this project, donations and love are always appreciated. They keep the servers running. You can help from the game's **"Help keep me active"** screen (PIX QR code). Thank you!
>
> [![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/pmK6zDYQSP)

## Install
1. Download `GRFS-PS5-v1.0.7.zip` from [Releases](../../releases/latest) and copy its `PPSA95210` folder to `/data/homebrew/` on your PS5.
2. Put your disc image in one of these places:

```
/data/homebrew/PPSA95210/
/data/
the root of a USB drive
```

3. Launch it. Without the game's files a screen lists the `.iso` files it found: pick yours and press X. With a payload loader running the install takes about a minute; without one the PS5 slows the app's writes down and it takes much longer. The game starts when it's done, and you can delete the `.iso` afterwards.

Or extract the disc on a PC (with extract-xiso, for example) and copy all its files to `/data/homebrew/PPSA95210/assets/`, with `default.xex` in lower case.

To update, copy the new `PPSA95210` folder over the old one. The installed game, your saves and your settings stay.

## The install screen
| Button | Does |
|---|---|
| Up / Down | Pick a disc image |
| Cross | Extract and play |
| Square | Search again |
| Circle | Cancel the install |

## Multiplayer
In Multiplayer → Connection, pick **Local Network** (same network) or **Online (VPS)** (over the internet, through the project's server). Your progression is kept on the server by your console's ID.

## Settings
`grfs.toml` in the app's folder: the resolution scale (1 = the Xbox 360's 1040x600, 2 = 2080x1200, 3 = 3120x1800), the upscaling, and the relay and news servers.

## Known issues
- Gameplay runs at 30 FPS, as on the Xbox 360.
- The first start takes longer while it prepares the language files and compiles the shaders.

## Reporting a problem
Report bugs on the **[Discord](https://discord.gg/pmK6zDYQSP)**. Send the newest file from `/data/homebrew/PPSA95210/logs/`, with what you were doing.

## Building from source
The port's source is in this repository: the app and its screens in [`src/`](src), the install payload in [`payload/`](payload), the server and the build tools in [`tools/`](tools). You don't need the game to read it, and nothing from the game is stored here, but building needs the code generated from your own `default.xex` and `AICLASS_X360_F.xex` into `generated/` (`tools/ps5_patch_generated.py` adapts it).

It needs the rexglue-ps5 SDK (ReXGlue with the PS5 layer) and the [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) toolchain:

```
cmake --preset ... -B out/ps5     # with cmake/ps5-toolchain.cmake
ninja -C out/ps5 grfs
tools/ps5_package.sh out/ps5 <ps5-plume-triangle checkout>
```

The app folder lands in `out/ps5/ps5-app/PPSA95210`.

### Server
[`tools/news_server.py`](tools/news_server.py) serves the news, the donors, the support screen, the players and bans (with an admin page), a public `/ranking` page, level-ups on a Discord webhook, the shared shaders and the saves' backups. The System Link relay is `system_link_relay.py` of rexglue-ps5. Both are plain Python 3.

## Credits
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**: the static recompiler and runtime, which builds on [Xenia](https://github.com/xenia-project/xenia), [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and [rexdex's recompiler](https://github.com/rexdex/recompiler)
- **[elripalda](https://github.com/elripalda)**: ReXGlue's PS5 layer (rexglue-ps5) and the native PS5 ports before this one
- **[Nazky](https://github.com/Nazky)**: [ps5-plume-triangle](https://github.com/Nazky/ps5-plume-triangle) and UnleashedRecomp-Prospero, the link and packaging tooling
- **[mihawk-99](https://github.com/mihawk-99)**: the PS5 Payload SDK fork and [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), the PS5 Vulkan driver
- **[John Törnblom](https://github.com/ps5-payload-dev)**: the original PS5 payload SDK
- **[cptlamarca](https://github.com/cptlamarca)**: the PS5 port, its screens, the servers and the PlayStation buttons
- Mesa (RADV), [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [FFmpeg](https://ffmpeg.org), AMD FidelityFX FSR, [stb](https://github.com/nothings/stb), [QR Code generator](https://github.com/nayuki/QR-Code-generator) by Project Nayuki, the Noto Sans fonts and extract-xiso
- Thanks to everyone who played and tested it

See [CREDITS.md](CREDITS.md) for the full list.

## License
The port's code is under the [GPL-3.0](LICENSE). ReXGlue and the other projects keep their own licenses. None of these cover the game, its artwork or its music.

*An unofficial fan project, not affiliated with or endorsed by Ubisoft, Microsoft or Sony. No game files are included.*
