# Sudz for GarageBand (Audio Unit)

Sudz as an Audio Unit effect you insert on a GarageBand audio track. The plug-in window is the same Sudz editor you use in the browser, with all its tools: pitch, Draw and Time tools, glides, sibilance and breath levels, clip repair, MIDI guide and scale correction.

GarageBand has no ARA (the link Melodyne uses in Logic to read a track directly), so Sudz works the way Melodyne does in hosts without ARA:

1. **Transfer.** Press **Transfer** in the plug-in, then press Play in GarageBand. The plug-in records a copy of the track, stamped with GarageBand's timeline position. Press Stop when the part is done.
2. **Edit.** The notes appear in the plug-in window. Edit as usual.
3. **Play back.** GarageBand now plays the tuned version of that stretch, sample-accurately against the timeline. Outside the transferred stretch the track passes through untouched. Song export and bouncing use the tuned audio too.
4. **Save.** Saving the GarageBand project saves the transferred recording, the tuned audio and every edit inside the plug-in.

**Live effects.** Open **Live effects › Polish this track live…** in the plug-in window and tick **Polish this track live in GarageBand**. The vocal chain then runs inside GarageBand as the song plays, on the tuned vocal: low cut, vocal rider, compressor, tone, saturation, exciter, de-esser, reverb, doubler, chorus, slapback and tempo-synced echo. Pick a style, or adjust reverb, echo, doubler, evenness, de-esser, presence, air, warmth and low cut while it plays. GarageBand's automation lanes for Sudz (**Reverb amount**, **Echo amount**, **Doubler amount**, **Vocal level**) ride on top, so you can open the reverb up in a chorus or throw an echo on one word. Noise reduction, de-reverb, tone matching, harmonies and the choir need the whole song at once, so they stay in the browser version's export. On a mono track the plug-in outputs stereo, so the reverb and doubler spread across the stereo field.

**AI clean-up.** **Clean-up › AI clean-up…** removes background noise (fans, traffic, the room, chatter) from the transferred take with a trained network (DeepFilterNet3), while Sudz protects the harmonics of every sung note. The engine and model are compiled into the plug-in, so it works offline; a 4½-minute take takes about half a minute. GarageBand then plays the cleaned, tuned vocal.

**Native AI.** The installer also puts ONNX Runtime 1.19.2 (the newest build that runs on macOS 11) in `~/Library/Application Support/Sudz/lib`. With it, AI clean-up and **Split a song into vocal and backing** run on the Mac's own processor instead of inside the web view: several times faster (a 3-minute song splits in a few minutes instead of 10 to 15). Without it, Sudz falls back to its built-in engine. The separation model (166 MB) is chosen once and copied into `~/Library/Application Support/Sudz/models`.

**Line up with the lead.** Put Sudz on the lead and on a double or harmony track and transfer both. In the double's window, **Notes › Line up with the lead…** lists the other Sudz tracks; pick the lead and Sudz moves the double's notes to where the lead's words start and end (optionally its pitch too). Instances find each other because GarageBand runs every track's plug-ins in one process.

**Original** in the plug-in's transport switches playback back to the untouched track, for A/B comparison. GarageBand's own transport (Space bar, Play, the playhead) drives playback; the plug-in's playhead follows it.

## What you need

- A Mac with macOS 12 or later (Apple silicon or Intel).
- **Xcode** from the Mac App Store (open it once and accept the licence), **or**, if your Mac can't run the current Xcode, just Apple's free **Command Line Tools**: the build script offers to install them.
- **CMake**. Either `brew install cmake` (with [Homebrew](https://brew.sh)) or the installer from cmake.org.
- GarageBand 10.

## Build and install

In Terminal, from this folder:

```bash
./build-mac.sh
```

The first build downloads JUCE 8.0.4 and takes a few minutes. When it finishes, the Audio Unit is copied to `~/Library/Audio/Plug-Ins/Components/Sudz.component`, and the script runs Apple's validator (`auval`). GarageBand only lists plug-ins that pass it. Look for `AU VALIDATION SUCCEEDED` near the end.

You also get `Sudz.app`, a standalone version, in `build/Sudz_artefacts/Release/Standalone/`, which is handy for checking the window opens.

The build is unsigned and for your own Mac. To install it on other Macs, see **Share it with other Macs** below.

## Use it in GarageBand

1. **GarageBand > Settings > Audio/MIDI**: tick **Enable Audio Units**. Restart GarageBand if it was open.
2. Select the vocal track and open **Smart Controls** (press B), then the **Plug-ins** area.
3. Click an empty slot, choose **Audio Units > Sudz > Sudz**. Put it in the **first** slot, so it gets the dry vocal before other effects.
4. Click the plug-in name to open its window.
5. Move GarageBand's playhead just before the vocal, press **Transfer** in Sudz, press **Play** in GarageBand, and **Stop** at the end of the part.
6. Edit. Press Play in GarageBand to hear the result.

A new transfer replaces the previous one and its edits. For a long song, either transfer the whole vocal once, or put one Sudz on each vocal track.

## Things to know

- **Big projects.** The plug-in stores the transferred audio and the tuned audio in the GarageBand project at 24-bit. A 4½-minute mono vocal adds about 70 MB to the project.
- **Changing sample rate.** Keep the GarageBand project at the sample rate you transferred at. If it changes, transfer again.
- **If GarageBand doesn't report its timeline position**, Sudz tells you so. Playback then lines up from where you pressed Play, so start playback from the same point you transferred from.
- **Timing edits** (Time tool) move audio within the transferred stretch. The plug-in can't make the stretch longer than what was transferred.
- **AI clean-up licences.** DeepFilterNet3 (Hendrik Schröter) is MIT/Apache-2.0; ONNX Runtime Web (Microsoft) is MIT. Both licences allow bundling in your own build.
- **Licence.** JUCE is free under the AGPLv3, or under JUCE's own licence (free for personal use and small businesses below its revenue limit). See juce.com/get-juce before selling a build.

## Share it with other Macs

macOS only runs plug-ins from other Macs if they are signed with a Developer ID and notarised by Apple. That needs an [Apple Developer Program](https://developer.apple.com/programs/) membership.

`sign-mac.sh` does the whole job after `./build-mac.sh`: it signs the plug-in, wraps it in an installer, sends it to Apple for notarisation, and staples the result. The one-time setup (two certificates and a saved notarisation profile) is described at the top of the script. Then:

```bash
APP_ID="Developer ID Application: Your Name (YOURTEAMID)" \
INSTALLER_ID="Developer ID Installer: Your Name (YOURTEAMID)" \
./sign-mac.sh
```

You get `dist/Sudz-<version>.pkg`. On another Mac, open it, enter the admin password, and the plug-in is installed for every user in `/Library/Audio/Plug-Ins/Components`.

## How it fits together

- `Source/PluginProcessor.*`: audio. The transfer FIFO (lock-free from the audio thread), the transferred recording keyed to the host timeline, sample-accurate playback of the tuned audio, and saving it all in the plug-in state.
- `Source/SudzFx.h`: the live effects chain (plain C++, real-time safe, no allocation while playing). It takes the same settings as the editor's polish chain and is tested against it: on the same vocal, loudness within 0.6 dB, tone within 1.6 dB per band and the same stereo width for the standard styles.
- `Source/PluginEditor.*`: the window. A JUCE 8 web view showing `ui/index.html` (compiled into the plug-in), with native functions for transfer, saving edits and receiving tuned audio. The page sends only the stretches of audio that changed after each edit.
- `Source/NativeAI.*`: the native AI engine. ONNX Runtime's C API, loaded at run time with `dlopen` (never linked, so Sudz still opens without it); sessions are shared by every Sudz instance and run on one background thread. `Source/ort/` holds ONNX Runtime's header and licence (MIT).
- `ui/index.html`: the Sudz editor. It's the same file as the browser version and switches to plug-in mode when it finds JUCE's bridge.

To update the editor, convert a newer Sudz page and rebuild:

```bash
python3 ascii_ui.py /path/to/Sudz.html ui/index.html
./build-mac.sh
```

(`ascii_ui.py` rewrites the page as pure ASCII so no web view can misread its text encoding.)
