# README update: Networking Support section

Add the following section to the README.md in the skate3recomp repo:

---

## Online / Private Server Support

This fork adds Xbox Live / EA Blaze networking support, enabling connection to custom private servers.

### Features
- DirtySDK socket emulation (socket, connect, send, recv, select)
- WSA event system for async I/O
- Blaze TDF protocol handshake (PreAuth → Auth.Login → UserAdded)
- XNetLogon/XOnline service emulation
- User sign-in with Gold membership and full privileges

### Running with a Private Server

1. Start a Blaze server (e.g., [Skate3BlazeServer](https://github.com/skate6743/Skate3BlazeServer)) on port 10744

2. Configure DNS redirect in `/etc/hosts`:
   ```
   127.0.0.1 gosredirector.ea.com
   127.0.0.1 downloads.skate.online.ea.com
   ```

3. Run the game:
   ```sh
   SDL_AUDIO_DRIVER=pulseaudio ./skate3
   ```

4. The game will connect to `127.0.0.1:10744` for Blaze services

### Audio Fix (Linux)

If you experience audio crackling, try:
```sh
SDL_AUDIO_DRIVER=pulseaudio ./skate3
```

### Known Limitations
- Identity/FESL services not implemented (game disconnects after ~2min)
- TU3 (Title Update 3) not supported in this build
- Some DLC content may be missing

### Network Flow
```
Game Boot → XNetStartup → WSAStartup → GetServiceInfo (127.0.0.1:10744)
→ XNetConnect → Blaze Handshake (PreAuth → Auth.Login → UserAdded)
→ Game online mode active
```

---
