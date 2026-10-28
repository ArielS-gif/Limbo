# Limbo

Limbo makes the old 2016 Fortnite Save the World alpha (OT6.5, build 2252016-3240987) playable again, with your friends.

You need your own copy of the OT6.5 build. Limbo doesn't come with any game files and has nothing to do with Epic Games.

## What you can do

- Play any mission from the Play tab, with your homebase fully built
- Play co-op with up to 4 people
- Fight husks around the map
- Chat with your friends
- Play the tutorial and go to the outposts

Still in progress: some missions haven't been tested all the way through, Flingers just stand there, and what you build at the outpost doesn't save.

## Setup

Put the folders next to each other:

```
OT6.5\
  Fortnite (2252016-3240987)\
  Limbo\
  Limbo-Backend\   (only the host needs this)
```

Get Limbo from the [Releases](https://github.com/ArielS-gif/Limbo/releases) page, and the backend (`Limbo-Backend.zip`) from [Limbo-Backend's Releases](https://github.com/ArielS-gif/Limbo-Backend/releases).

### Hosting

1. Run `Play Limbo.bat` and press Enter. The first time, it installs [Node.js](https://nodejs.org) if you don't have
   it (Windows asks for permission, click Yes) and sets up the backend. It shows the addresses your friends can join.
2. Log in with any name and password, pick a mission and press Play.

Accounts are saved in `Limbo-Backend\data`. Delete that folder to start over. New accounts skip the homebase tutorial;
press T in `Play Limbo.bat`'s menu to play it instead.

### Joining a friend

1. Get on the same network as them (same Wi-Fi, or Radmin VPN / Tailscale).
2. Run `Play Limbo.bat` after they started theirs. It finds their game on the network by itself; press Enter.
   (If it doesn't, press J and type the address their Limbo window shows, like `26.12.34.56`.)
3. Log in. Your friends are in the Friends list: press Join on one who is starting a mission or already in one, or
   press Play while they are.

The host has to allow TCP ports 3551 and 5222 and UDP 3551 (so friends find it) in the firewall (allowing Node.js when
Windows asks does it), and whoever hosts a mission needs UDP 7777.

## Is it safe?

All the code is right here, and the downloads on the Releases page are built by GitHub straight from it. The DLL only runs inside the game and only talks to your backend. Some antivirus programs don't like injectors, so it might get flagged.

## Building it yourself

Install Visual Studio with "Desktop development with C++" and run `build.bat`.

If you want to know how it all works, my notes are in [docs/DEVLOG.md](docs/DEVLOG.md).

## Credits

- [vopswtf](https://github.com/vopswtf) for Fortify-Alpha, which Limbo-Backend is based on
- [NOTPIES](https://github.com/NOTPIES) for the original OT6 research
- [Project Mercury](https://github.com/absoluteSpacehead/MercuryLauncher) for figuring out the patch pak

## License

MIT. Limbo-Backend's original code belongs to vopswtf.
