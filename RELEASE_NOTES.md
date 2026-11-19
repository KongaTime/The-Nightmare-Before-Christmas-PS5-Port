## Version 1.0

First release of **The Nightmare Before Christmas PS5 Port**, based on Pallypots' Project Oogie.

## Quick start from the V1.0 ZIP

Use a PS5 setup that already runs native homebrew from `/data/homebrew/` and has FTP access.

1. Download `Nightmare.Before.Christmas.PS5.Port.zip` from [V1.0 Releases](https://github.com/KongaTime/The-Nightmare-Before-Christmas-PS5-Port/releases/tag/v1.0) and unzip it.
2. Upload the included `PPSA90030` folder to `/data/homebrew/`. The executable should be at `/data/homebrew/PPSA90030/eboot.bin`.
3. On a fresh install, create `/data/oogie-ps5/saves/` over FTP, with `TitleData`, `UserData`, `Cache`, and `SystemData` inside `saves/`.
4. Rename your supported USA original Xbox disc image to `Oogie.iso` and upload it to `/data/homebrew/PPSA90030/Oogie.iso`, next to `eboot.bin`.
5. Refresh your homebrew launcher, open the game, and select **Prepare Game**. Once copying finishes, select **Play Game**.

## Features

- Prepare a supported USA Xbox ISO on PS5, then launch the game from the same menu.
- Play in widescreen with adjustable internal resolution, brightness, and texture filtering.
- Adjust music, dialogue, and effects independently and remap DualSense face buttons.

On PS5, the FTP-staged ISO prepared successfully and launched the game. Saving and loading worked, and the playtest cleared Chapters 1–3. USB detection and later chapters still need testing.

Follow the [README](https://github.com/KongaTime/The-Nightmare-Before-Christmas-PS5-Port#readme) for build requirements, exact console paths, and first-launch steps.
