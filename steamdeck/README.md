# Steam Deck

Skate 3 Recomp runs natively on the Steam Deck, at 60 fps or more with the native renderer. This folder holds the two files that make it behave well there, in Game Mode and in Desktop Mode.

| File | What it does |
| --- | --- |
| `steam-skate3.sh` | Launcher to add to Steam. It blocks idle sleep while the game runs, uses the system libraries instead of Steam's, and stops a copy of the game left running before starting a new one. |
| `skate3.toml` | Settings: standard vsync presentation, and never skipping a frame's present. |

## Setup

1. Get a build that includes the Steam Deck fixes in the rexglue SDK (see below), and put `skate3` and `librexruntime.so` in a folder on the Deck, for example `~/skate3recomp`.
2. Copy `steam-skate3.sh` and `skate3.toml` from this folder next to them, and make the launcher executable:
   ```sh
   chmod +x ~/skate3recomp/steam-skate3.sh
   ```
3. In Desktop Mode, run `skate3` once and install the game from your ISO, as described in the main README.
4. In Steam, choose **Add a Non-Steam Game**, browse to `steam-skate3.sh`, and add it. Launch it from Steam from now on, in either mode.

To quit, use the game's menu or Steam's **Exit Game**.

## What was fixed for the Deck

These are runtime fixes in the rexglue SDK (`third_party/rexglue-sdk`), collected in mchughalex/rexglue-skate3#22:

- Controllers work: the SDK's bundled SDL is built with udev/libusb, and pads that are already connected are picked up.
- The picture no longer freezes while the game keeps running.
- The intermittent boot stall on the first frame is gone.
- The post-event photo screen no longer stalls at about 5 fps.
- Music keeps its tempo with any Audio Buffer Size.
- An idle audio thread no longer burns a full CPU core.
- Crashes no longer leak memory into `/dev/shm`, which used to make Steam itself crash until a reboot.
- SteamOS's system RenderDoc is no longer loaded into every run.

## Building

Follow "Ubuntu/Linux Build" in the main README on a Linux PC. Make sure `libudev-dev` and `libusb-1.0-0-dev` are installed before the first configure, or the controller will not work. Copy `out/build/linux-release/skate3` and `out/build/linux-release/librexruntime.so` to the Deck.
