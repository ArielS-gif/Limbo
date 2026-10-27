# Limbo

Limbo brings the February 2016 **Fortnite: Save the World alpha** (Online Test 6.5, build 2252016-3240987) back to a
playable state: missions, co-op with friends, husks, chat, the tutorial and the outposts. It restores the server
pieces that were stripped out of the game client, so your own game runs the zone and hosts it for your friends.

> Limbo is not affiliated with Epic Games and contains no game files. You need your own copy of the OT6.5 build.
> Made for personal, educational and game-preservation use.

## What works

- **Missions from the Play tab:** Close the Gate (one to four gates), Deliver the Goods, Launch the Rocket, Retrieve
  the Data and Power the Storm Shield. Tiles for missions this build doesn't have open the nearest one it does.
- **Co-op for up to 4 players:** start a mission together from one launch screen, or join a game that's already
  running. Every player keeps their own hero, inventory and backpack, saved to their own account.
- **Husks:** the Atlas defense waves, guardians by the chests, and roaming groups around the map.
- **Chat:** the GLOBAL channel, friends and presence, through a small chat server in the backend.
- **The tutorial** (Onboarding Fort, "Help Ramirez") with its cutscenes, and **the four outposts**.
- Building, editing, harvesting, crafting, traps, gadgets, mission rewards, and your backpack carried between zones.

Work in progress: some of the newer missions haven't been played all the way through yet, Flingers don't fight, and
what you build at an outpost isn't kept between visits.

## How it works

The OT6.5 client was shipped without its server code. Limbo is a DLL (`gameserver.dll`) loaded into the game that puts
those pieces back: hosting and replication, the mission flow, the AI director, the player's setup. A modified
[Fortify-Alpha](https://github.com/vopswtf/Fortify-Alpha) backend,
[Limbo-Backend](https://github.com/ArielS-gif/Limbo-Backend), keeps accounts and profiles and adds co-op launch
screens, chat and mission fixes.

## What's in this repository

| Path | What it is |
| --- | --- |
| `src/gameserver` | The DLL (C++): everything that runs inside the game |
| `src/injector` | `injector.exe`: loads the DLL into the game (only the 32-bit OT6.5 client in its own folder) |
| `Play Limbo.bat` | Starts everything: the backend on the host's PC, the game, and Limbo |
| `tools` | The patch-pak builder, test scripts and the reverse-engineering helper |
| `docs/DEVLOG.md` | Every finding along the way: what was stripped from the client and how each piece was restored |

## Is it safe?

- **All of the code is here.** Limbo doesn't download or run anything you can't read in this repository.
- **The release files are built by GitHub Actions** from this exact source, so you can check the build log and the
  SHA-256 checksums. You can also build them yourself (below).
- **What the DLL does:** it runs only inside the game. It changes the game's code and data in memory (every change is
  listed in the dev log), talks to the backend you point it at, and writes its logs next to itself. It doesn't touch
  files elsewhere, doesn't start with Windows, and sends nothing anywhere except your backend.
- **The injector** uses standard DLL injection (a remote thread that calls LoadLibrary) and refuses any process but the
  32-bit OT6.5 client. Some antivirus programs flag any injector on principle.

## Folder layout

Put Limbo (a [release](https://github.com/ArielS-gif/Limbo/releases), or this repository built) next to the game:

```
OT6.5\
  Fortnite (2252016-3240987)\   your game, as it comes (with its FortniteLauncher.exe)
  Limbo\                        this
  Limbo-Backend\                the backend (only on the host's PC)
```

Limbo adds one file to the game, `FortniteGame\Content\Paks\FortniteGame-WindowsClient_p.pak`, built from your own
game's files the first time you start it; everything else happens in memory while the game runs.

## Hosting (the PC that runs the backend)

1. Install [Node.js](https://nodejs.org) and [MongoDB Community Server](https://www.mongodb.com/try/download/community).
2. Get [Limbo-Backend](https://github.com/ArielS-gif/Limbo-Backend) into `OT6.5\Limbo-Backend`, run `npm install` in
   it, and copy `.env.example` to `.env`: `PORT=3551`, `MONGO_URI=mongodb://127.0.0.1/limbo` and a long random
   `JWT_SECRET` of your own.
3. Run **Play Limbo.bat**. It builds the patch pak the first time, starts the backend, starts the game and loads Limbo.
4. Log in with any name and password (the account is created on first login), pick a mission and press **Play**.

## Friends on other PCs

1. Get on one network with the host: the same LAN, or a free VPN such as Radmin VPN or Tailscale.
2. The host allows the game and Node through Windows Firewall: TCP 3551 (backend) and TCP 5222 (chat). Whoever hosts
   a mission also needs UDP 7777 (the game) allowed.
3. Install [Node.js](https://nodejs.org) (for the patch pak), put Limbo next to your game, and write the host's
   address into `Limbo\out\backend.txt`, like `26.12.34.56:3551`.
4. Run **Play Limbo.bat**, log in with any name, and press **Play** while someone's launch screen is up or while they
   are in a mission.

Whoever presses Play first hosts the mission; up to 4 players. If you change backends later, delete the patch pak
(above) so it is built again for the new address.

## Building

Visual Studio 2022 or newer with "Desktop development with C++", then run `build.bat`. It produces
`out\gameserver.dll` and `out\injector.exe` (both 32-bit, like the game).

## Credits

- [vopswtf](https://github.com/vopswtf): Fortify-Alpha, the backend that Limbo-Backend is forked from.
- [NOTPIES](https://github.com/NOTPIES): the original OT6 research.
- [Project Mercury](https://github.com/absoluteSpacehead/MercuryLauncher): its launcher showed that OT6.5 loads an
  extra `_p` patch pak, and how to lay one out.

## License

Limbo's own code is under the [MIT License](LICENSE). Limbo-Backend is a fork of Fortify-Alpha, which has no license
of its own; its original code stays its author's.
