# Windows 11 Microsoft Edge single-window capture

This document records the custom Windows Sunshine setup on branch `windows-edge-capture`.

The goal is intentionally narrower than normal Sunshine desktop/game streaming:

- stream exactly one top-level Microsoft Edge window to Moonlight;
- never fall back to capturing the Windows desktop;
- keep remote keyboard, mouse, controller, pen, and touch input disabled;
- use the RTX 4080 NVENC encoder;
- keep Sunshine off until explicitly started;
- expose a small persistent tray controller for start/stop and optional audio;
- leave the stock Sunshine installation available as a fallback, but do not run its service.

This is a personal-use specialization of Sunshine, not a general-purpose upstream Windows capture mode.

## Source and branch

Repository:

```text
JaekeLabs/Sunshine
```

Branch:

```text
windows-edge-capture
```

Pinned upstream base:

```text
Sunshine v2026.914.233613
63d35f702ee9e362e43263742981836ec0710384
```

Important branch commits:

```text
4b999633999ee214eb6d612d209ed64109e611a4  Capture Microsoft Edge window with WGC
bff0e2cf04a81cb6630546aa05b64bf5960b998d  Document fail-closed Edge capture boundary
6a0d5b58b982e77117d0cfd6f4943f529f771942  Detect WGC window content resizing
c7501595d9a36637cdf49471fcf23258efec37f1  Reinitialize WGC capture when Edge resizes
```

The Linux/Halcyon work remains on the separate `halcyon-window-capture` branch and should not be conflated with this Windows branch.

## Capture behavior

The Windows implementation lives primarily in:

```text
src/platform/windows/display_wgc.cpp
src/platform/windows/display.h
```

The Windows Graphics Capture backend selects Microsoft Edge by top-level window handle rather than using `CreateForMonitor`.

A valid capture target must:

- be a real `HWND`;
- be visible;
- not be minimized during initial selection;
- be a root/top-level window;
- belong to a process whose executable basename is `msedge.exe`.

Selection policy is fail-closed:

1. If the foreground window is a valid Edge window, use it.
2. Otherwise enumerate valid top-level Edge windows.
3. If exactly one exists, use it.
4. If none exist, fail rather than capturing a monitor.
5. If multiple valid Edge windows exist and none is foreground, fail rather than guessing.

There is intentionally no monitor/desktop fallback.

Once attached to an Edge `HWND`, switching tabs inside that Edge window is visible to Moonlight. Opening a second Edge top-level window does not replace the window already being captured.

## Window geometry and resize fix

The initial WGC item size is used as the capture geometry instead of the physical monitor size. The Windows display object is adjusted to the Edge window dimensions with rotation set to identity and offsets set to zero.

A first live build exposed a resize artifact: after resizing Edge, stale/duplicated parts of the old frame could appear.

The cause was that a WGC frame-pool texture may retain the old pool dimensions until the frame pool is recreated. Looking only at the D3D texture description therefore does not reliably detect a resized window.

The fix stores the current WGC content width/height and checks `Direct3D11CaptureFrame.ContentSize` for every consumed frame. If the content dimensions change, the backend returns `capture_e::reinit` before forwarding the stale frame. Sunshine then rebuilds the capture backend/frame pool using the new Edge size.

Expected resize log sequence:

```text
Microsoft Edge capture size changed [OLD_WxOLD_H -> NEW_WxNEW_H]
Capturing Microsoft Edge window [NEW_WxNEW_H]
```

This was live-tested on Windows and removed the duplicated/stale resize artifact.

## Tested behavior

The following behavior has been verified on the Windows 11 host:

- Moonlight receives the Edge window itself rather than the composed desktop.
- Firefox placed over Edge is not visible remotely.
- Notepad placed over Edge is not visible remotely.
- Covering Edge with another application while leaving Edge restored does not expose the covering application.
- Resizing Edge works after the `ContentSize` reinitialization fix.
- Switching tabs in the captured Edge window is visible.
- A second Edge top-level window is not substituted for the already captured one.
- Minimizing Edge effectively freezes/pauses the last captured frame rather than falling back to the desktop.
- H.264, HEVC, and AV1 NVENC capability probes succeed on the RTX 4080.
- Audio works when Sunshine audio streaming is enabled.

The preferred workflow is to leave the dedicated Edge window restored/maximized and simply cover it locally with other windows when needed.

## Known limitations

- This is one **top-level Edge window**, not one browser tab.
- True Chromium tab capture would require a different architecture, likely involving the browser `tabCapture` API plus a bridge/new Sunshine capture source.
- Starting or reinitializing capture while multiple eligible Edge windows exist can fail closed unless the intended Edge window is foreground.
- Starting Sunshine while Edge is minimized can fail closed because minimized windows are rejected during target selection.
- Minimize-to-background playback is not implemented; minimizing normally leaves Moonlight on the last frame.
- Sunshine audio capture is not automatically isolated to Edge. When audio streaming is enabled, other sounds routed through the captured Windows output mix may also be heard remotely.
- Protected/DRM video paths may behave differently and are not guaranteed by this customization.

This setup reduces accidental desktop exposure but should not be treated as a hardened security sandbox.

## Windows host

Known host hardware for this build:

```text
CPU: Ryzen 7 9800X3D
GPU: NVIDIA GeForce RTX 4080
RAM: 64 GB DDR5
OS: Windows 11
Primary display: LG ULTRAGEAR, 2560x1440, ~164.958 Hz
Secondary display: ViewSonic XG2402, 1920x1080, ~143.996 Hz
```

Hardware-accelerated GPU scheduling is enabled.

## Local installation layout

The tested permanent custom installation lives at:

```text
C:\Tools\Sunshine-Edge
```

Important files:

```text
C:\Tools\Sunshine-Edge\sunshine.exe
C:\Tools\Sunshine-Edge\config\sunshine.conf
C:\Tools\Sunshine-Edge\config\apps.json
C:\Tools\Sunshine-Edge\config\credentials
C:\Tools\Sunshine-Edge\config\sunshine_state.json
C:\Tools\Sunshine-Edge\SunshineEdgeTray.ps1
C:\Tools\Sunshine-Edge\SunshineEdgeTray.vbs
```

On Windows, this Sunshine build resolves its application data to a `config` directory beside the running executable. Keep the configuration nested beside `sunshine.exe`; do not rely on the outer directory from the CI ZIP.

The stock LizardByte Sunshine installation can remain installed, but its Windows service is intentionally kept stopped/manual for this setup.

## Sunshine configuration policy

The important custom configuration policy is:

```ini
capture = wgc
controller = disabled
encoder = nvenc
keyboard = disabled
mouse = disabled
native_pen_touch = disabled
system_tray = disabled
upnp = disabled
```

Audio is deliberately user-toggleable:

```ini
stream_audio = disabled
```

is the normal/default state, and the tray controller can change it to:

```ini
stream_audio = enabled
```

when audio is wanted.

### Web UI choices

The tested Web UI configuration keeps the following policy:

- Capture Method: Windows.Graphics.Capture
- Force a Specific Encoder: NVIDIA NVENC
- Device/display configuration: Disabled
- UPnP: Disabled
- Address family: IPv4
- Web UI access: localhost only
- Audio Sink: blank/default
- Virtual Sink: blank/default
- Install Steam Audio Drivers: disabled
- Stream Audio: disabled by default
- Maximum bitrate: 0, allowing the client request
- Remote keyboard: disabled
- Remote mouse: disabled
- Remote controller: disabled
- Native pen/touch: disabled
- Sunshine's built-in system tray: disabled

NVENC-specific settings were otherwise left close to the tested defaults, including realtime priority and lower-latency behavior.

## Moonlight application model

The Moonlight application entry can remain the ordinary Sunshine `Desktop` entry. The application entry does not launch Edge.

Edge is launched locally on the host. The custom capture backend is what constrains the video source to Edge.

This preserves the desired behavior: the remote viewer does not get a launcher for arbitrary host applications.

## Tray controller

Canonical source for the tray helper is stored in this branch under:

```text
tools/windows-edge/SunshineEdgeTray.ps1
tools/windows-edge/SunshineEdgeTray.vbs
```

The installed copies live under `C:\Tools\Sunshine-Edge`.

The controller provides a persistent notification-area icon:

```text
gray          custom Sunshine is off
orange/yellow custom Sunshine is running
```

Right-click menu:

```text
Start Sunshine Edge
Stop Sunshine Edge
--------------------
[check] Stream Audio
[check] Start with Windows
--------------------
Open Sunshine Web UI
Open Edge
--------------------
Exit Tray Controller
```

Behavior:

- **Start Sunshine Edge** opens Edge if no visible Edge window exists, then launches the custom Sunshine binary.
- **Stop Sunshine Edge** stops only the custom `C:\Tools\Sunshine-Edge\sunshine.exe` instance.
- **Stream Audio** edits `stream_audio` in the custom `sunshine.conf`. If Sunshine is running, the controller restarts it so the new audio state takes effect.
- The audio menu text displays a check mark when audio is enabled.
- **Start with Windows** creates or removes the per-user Startup shortcut. This controls whether the tray controller itself launches automatically after sign-in; it does not autostart `sunshine.exe`.
- **Open Sunshine Web UI** opens `https://localhost:47990`.
- **Open Edge** launches Microsoft Edge.
- Double-clicking the tray icon toggles custom Sunshine on/off.
- **Exit Tray Controller** stops the custom Sunshine process and then exits the controller. Edge is left open.
- A named mutex prevents multiple tray-controller instances.

The controller uses the icon embedded in `sunshine.exe` and creates grayscale/off and orange/on variants at runtime.

The menu uses a custom `ToolStripRenderer` so Windows does not reintroduce the bright system-accent hover color. The tested renderer paints the menu background, selected-row background, text, separators, and border directly. The C# source is compiled with explicit references to the loaded WinForms and Drawing assemblies for Windows PowerShell 5.1 compatibility.

## Silent launcher

Launching a PowerShell tray script directly can briefly flash a console window at login.

The installed `SunshineEdgeTray.vbs` uses `WScript.Shell.Run(..., 0, False)` to start the PowerShell controller hidden through `wscript.exe`, avoiding the terminal flash.

Both the Startup shortcut and optional Desktop shortcut point to:

```text
%SystemRoot%\System32\wscript.exe
```

with:

```text
"C:\Tools\Sunshine-Edge\SunshineEdgeTray.vbs"
```

as the argument.

## Startup behavior

The per-user Startup shortcut is:

```text
%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Sunshine Edge Tray.lnk
```

When **Start with Windows** is enabled, Windows sign-in behaves as follows:

1. `wscript.exe` launches the hidden tray script.
2. The gray Sunshine icon appears.
3. Sunshine itself remains off; no `sunshine.exe` process is started.
4. The user starts Sunshine explicitly from the tray when wanted.

When **Start with Windows** is disabled, the Startup shortcut is removed and nothing from this custom setup starts automatically at sign-in.

An optional Desktop `Sunshine Edge.lnk` points to the same VBS launcher, allowing the tray controller to be brought back after choosing **Exit Tray Controller** without rebooting. Choosing **Exit Tray Controller** stops the custom Sunshine instance first, so no custom Sunshine/tray process remains afterward.

## Build and CI history

The resize-fix head `c7501595d9a36637cdf49471fcf23258efec37f1` was built in GitHub Actions run:

```text
Run: 35828115807
Windows AMD64 job: 107074317340
Artifact: build-Windows-AMD64
Artifact ID: 10736349186
Artifact SHA-256:
bb1c83ba1df7982e4174a044b1677f5fe83d7d662fa174a7b9d19b3e37ef8640
```

The overall workflow contained unrelated failing jobs, but the Windows AMD64 build/package/test job completed successfully.

The tested package was the lite Windows artifact. Its `Sunshine` directory was copied to `C:\Tools\Sunshine-Edge`.

## Rebuild/recovery checklist

If this installation needs to be reconstructed:

1. Check out `JaekeLabs/Sunshine` branch `windows-edge-capture`.
2. Build/package the Windows AMD64 target or use a known matching CI artifact.
3. Extract the lite package.
4. Copy the package's nested `Sunshine` directory to `C:\Tools\Sunshine-Edge`.
5. Restore the custom `config` directory beside `sunshine.exe`.
6. Confirm `capture = wgc`, `encoder = nvenc`, remote input disabled, `system_tray = disabled`, and `upnp = disabled`.
7. Copy `tools/windows-edge/SunshineEdgeTray.ps1` and `.vbs` to `C:\Tools\Sunshine-Edge`.
8. Launch the tray controller with the VBS helper and enable **Start with Windows** from the tray menu if automatic tray startup is desired.
9. Keep the stock `SunshineService` stopped/manual.
10. Open exactly one visible Edge window and test Moonlight.
11. Test covering Edge with another local window to verify no desktop/application leakage.
12. Resize Edge and confirm the capture reinitializes cleanly.
13. Test the tray audio toggle in both states.
14. Test the **Start with Windows** toggle by confirming the Startup shortcut is created/removed.
15. Confirm hover selection in the dark tray menu is dark gray rather than the Windows accent color.

## Relationship to Halcyon

Halcyon uses a different custom path:

```text
Linux/Wayland: capture = portal
Windows:       capture = wgc
```

Both setups share the same basic policy of single-window sharing, NVENC, disabled remote input, disabled UPnP, and no stock Sunshine tray UI.

Halcyon intentionally disables streamed audio. Windows keeps audio disabled by default but allows it to be toggled from the custom tray controller.
