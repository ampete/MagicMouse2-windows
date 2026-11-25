# MM2 — Magic Mouse 2 gestures for Windows 11

Makes an Apple **Magic Mouse 2** (Lightning, Bluetooth) behave on Windows 11 x64
much like it does on a Mac: smooth touch scrolling with momentum, swipes for
back/forward and app switching, desktop switching, Task View and smart zoom.

It has two parts:

* **MM2Filter.sys**, a small kernel driver. It switches the mouse into
  multitouch mode and passes the raw touch data to user mode. Pointer movement
  and clicks keep going through Windows' normal Bluetooth mouse driver.
* **mm2gestures**, a normal program. It reads the touch data, recognizes
  gestures and sends the matching scroll and keyboard input to Windows.

Status: working on the author's machine with test signing. This is a personal
project, not signed by Microsoft. See [Setup](#setup) for what that means.

## What you can do

| Gesture | What happens |
|---|---|
| Slide one finger forward or back | Smooth scrolling. A quick flick keeps gliding (momentum), and touching the mouse again stops it. |
| Swipe one finger left | Back (browser, Explorer, …) |
| Swipe one finger right | Forward |
| Swipe two fingers left or right | Opens the app switcher (Alt+Tab). Keep your fingers down and keep sliding to move through the apps; lift them to switch. A quick swipe right jumps to the app you used last. |
| Swipe three fingers left or right | Next or previous virtual desktop |
| Double-tap with two fingers | Task View (all windows and desktops) |
| Double-tap with one finger | Smart zoom: zoom in (about 200% in browsers); double-tap again to go back to 100% |
| Click, right-click, move | Unchanged: Windows handles these as usual |

Every gesture can be changed or turned off, and scrolling speed is
adjustable. See [Settings](#settings).

Details:

* **Classic scrolling direction** by default: finger forward scrolls up, like
  a mouse wheel. Mac-style "natural" scrolling is a setting.
* **No accidental scrolling:** scrolling is ignored while a button is pressed
  or while the mouse is being moved. A second finger resting on the mouse
  doesn't stop one-finger scrolling.
* **High-resolution scrolling:** small wheel steps instead of 120-unit
  notches. Browsers, Office, Explorer and VS Code scroll smoothly; some older
  apps scroll in notches.
* **Startup:** multitouch turns on a few seconds after the mouse connects or
  wakes up. Before that the mouse works as a plain pointer.
* **Admin windows:** Windows doesn't let a normal program send input to
  windows running as administrator. Gestures do nothing while such a window is
  in front.

## Settings

Settings are in `build\x64\Release\mm2gestures.ini`. The file is created with
comments the first time the program runs. After editing, restart the gesture
program (see [Everyday use](#everyday-use)).

**`[scroll]`**

| Key | Meaning | Default |
|---|---|---|
| `natural` | `0` classic (like a wheel), `1` natural (content follows the finger) | `0` |
| `speed` | Overall scroll speed; `1.5` is 50% faster | `1.0` |
| `acceleration` | Extra speed for fast finger movement; `0` off | `1.0` |
| `momentum` | Keep gliding after a flick; `1` on, `0` off | `1` |
| `momentum_decay` | How long the glide lasts, in seconds | `0.30` |
| `horizontal` | One-finger sideways scrolling. Only applies if `one_finger_swipe=none`, because otherwise a sideways slide is the back/forward swipe. | `1` |

**`[gestures]`**

| Key | Choices | Default |
|---|---|---|
| `one_finger_swipe` | `back_forward`, `app_switch`, `desktops`, `none` | `back_forward` |
| `two_finger_swipe` | same | `app_switch` |
| `three_finger_swipe` | same | `desktops` |
| `one_finger_double_tap` | `smart_zoom`, `task_view`, `none` | `smart_zoom` |
| `two_finger_double_tap` | same | `task_view` |
| `zoom_steps` | How far smart zoom zooms in (1–15; 5 ≈ 200% in browsers) | `5` |

## Everyday use

After setup, `scripts\autostart.ps1` makes the gestures start hidden at
login, using `mm2gesturesw.exe` (a copy without a console window). Run these
from the repository folder:

```powershell
.\build\x64\Release\mm2gestures.exe --stop      # stop the running copy (hidden or not)
.\build\x64\Release\mm2gesturesw.exe            # start it again, hidden
.\build\x64\Release\mm2gestures.exe --verbose   # run in a window and print each recognized touch
.\scripts\autostart.ps1 -Remove                 # don't start at login any more
```

Only one copy runs at a time; a second one says so and exits. `--verbose`
prints a line per touch, for example
`touch: 2 finger(s), 180 ms, max travel 410, mouse moved 3 -> app switch`.
That line helps when a gesture isn't recognized.

To check the driver itself:

```powershell
.\build\x64\Release\rawtouch.exe info    # connected? multitouch on? frame counters
.\build\x64\Release\rawtouch.exe         # stream decoded finger positions (Ctrl+C to stop)
.\build\x64\Release\rawtouch.exe --hex   # the same, plus raw bytes
```

Only one program can read the touch data at a time, so stop mm2gestures
before streaming with rawtouch.

## Setup

### What this needs on the machine

The driver is test-signed (signed with a certificate you create yourself),
not signed by Microsoft. Windows only loads such drivers when these settings
are in place:

* **Test signing on:** `bcdedit /set testsigning on` (admin), then reboot.
  The desktop then shows a "Test Mode" watermark. This needs **Secure Boot
  off** in the firmware settings. Undo with `bcdedit /set testsigning off`.
* **Smart App Control off:** Windows Security > App & browser control. It
  blocks test-signed drivers and unsigned programs. On most Windows 11 builds
  it can't be turned back on without reinstalling Windows.
* **Memory integrity (HVCI)** can stay on. The driver is built to be
  compatible with it.

These settings lower the protection of your PC: test mode allows any
test-signed driver, not just this one. Decide deliberately.

### Requirements for building

* Visual Studio 2026 (18.x) with "Desktop development with C++", the Spectre
  libraries and the WDK component (`Component.Microsoft.Windows.DriverKit`)
* WDK and SDK come from **NuGet**, pinned to **10.0.28000.2526**
  (`driver/packages.config`); no separate WDK install is needed. The 26100
  packages lack the MSBuild tasks for VS 2026.
* KMDF 1.33 (Windows 11 22H2 or newer)

### Build

From a "Developer PowerShell for VS" (the driver build needs the WDK tools on
the PATH):

```powershell
msbuild MM2.sln /t:Restore /p:RestorePackagesConfig=true   # fetches WDK/SDK into .\packages
msbuild MM2.sln /p:Configuration=Release /p:Platform=x64
```

Outputs in `build\x64\Release\`: `MM2Filter\` (driver package),
`mm2gestures.exe`, `mm2gesturesw.exe`, `rawtouch.exe`.

### Sign, install, autostart

If PowerShell refuses to run the scripts, allow them for the current window
only: `Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass`.

```powershell
.\scripts\testsign.ps1             # normal user: creates the test certificate once, signs the package
.\scripts\install.ps1              # admin: trusts the certificate, installs the driver (asks first)
.\scripts\autostart.ps1            # normal user: start the gestures at login
```

`install.ps1` asks before each step. The driver is installed as an
**extension INF** for the mouse's hardware ID
`BTHENUM\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0269`. The
normal Bluetooth HID driver stays in charge; MM2Filter is added below it as
a lower filter.

### Updating the driver

Rebuild, run `testsign.ps1` and `install.ps1` again, then reboot (or
reconnect the mouse). Windows keeps the previous driver packages. List them
and remove old ones (admin):

```powershell
pnputil /enum-drivers                      # find the entries with Original Name mm2filter.inf
pnputil /delete-driver oemNN.inf           # remove an old one (not the newest, which is in use)
```

### Uninstall

```powershell
.\scripts\autostart.ps1 -Remove
.\build\x64\Release\mm2gestures.exe --stop
.\scripts\uninstall.ps1 -RemoveCertificate      # admin
```

If the mouse misbehaves, the keyboard and touchpad don't depend on this
driver. As a last resort, remove the mouse in Settings > Bluetooth and pair it
again.

## How it works

```
 HidClass / mouhid      <- normal 8-byte mouse reports: pointer and clicks as usual
     hidbth.sys
 ┌───────────────┐
 │ MM2Filter.sys │ --raw touch frames--> \\.\MM2Touch --> mm2gestures (or rawtouch)
 └───────────────┘                                          └─> SendInput: wheel, keys
 BTHENUM / BTHPORT      (L2CAP: control PSM 0x11, interrupt PSM 0x13)
```

The protocol reference is the Linux driver `drivers/hid/hid-magicmouse.c`.

**Driver (kernel, kept thin; no gesture logic):**

* **Channel tracking:** a KMDF lower filter under hidbth.sys that sees all
  L2CAP traffic (`IOCTL_INTERNAL_BTH_SUBMIT_BRB`). It learns the control and
  interrupt channel handles from the open/close requests.
* **Multitouch enable:** the mouse sends touch data only after it gets a
  HID SET_REPORT (Feature) `F1 02 01`. The filter sends `53 F1 02 01` on the
  control channel when the interrupt channel opens. Right after connecting,
  the mouse often acknowledges this but stays in plain mouse mode, so whenever
  a plain 8-byte mouse frame arrives the filter sends it again, at most every
  500 ms. This also covers reconnects after the mouse sleeps.
* **Input split:** in multitouch mode a report `0x12` is 14 bytes plus 8 per
  finger. The filter copies each frame into a 64-frame ring and hands
  hidbth only the first 8 bytes, the mouse report that the native HID
  descriptor declares. So pointer and clicks are untouched.
* **Sideband device:** `\\.\MM2Touch` (Administrators full access,
  logged-in users read/write). `IOCTL_MM2_READ_FRAMES` waits until frames
  arrive; `GET_INFO`, `ENABLE_MT` and `GET_DIAG` are for diagnostics.
  [shared/mm2_ioctl.h](shared/mm2_ioctl.h) is the contract.

**Why a sideband device instead of a new HID descriptor:** the mouse's native
descriptor declares `0x12` as the 8-byte mouse report and doesn't declare
`0xF1` at all, so the HID stack rejects touch frames and can't send the
enable. Replacing the descriptor (hidbth fetches it via SDP, and those
requests do pass through this filter) would mean rewriting reports for the
whole HID stack. A separate device keeps the mouse path unchanged and the
kernel code small.

**Gesture service (user mode):**

* [shared/mm2_touch.h](shared/mm2_touch.h) decodes frames: finger id,
  position, size, state.
* [tools/gestures/engine.cpp](tools/gestures/engine.cpp) is the recognizer.
  It's pure logic with no Windows calls, so it can be unit-tested with
  synthetic touches. A touch session is undecided until it clearly becomes a
  scroll, a swipe or a tap. Clicks and moving the mouse body block scrolling
  and taps.
* [tools/gestures/inject.cpp](tools/gestures/inject.cpp) sends the result
  with `SendInput`:

  | Action | Input sent |
  |---|---|
  | Scrolling | high-res `MOUSEEVENTF_WHEEL`/`HWHEEL` |
  | Back / forward | mouse buttons X1/X2 |
  | Desktops | Ctrl+Win+←/→ |
  | Task View | Win+Tab |
  | App switcher | Alt held, Tab / Shift+Tab |
  | Zoom | Ctrl + wheel, Ctrl+0 |

## Repository layout

```
driver/           MM2Filter.sys - KMDF lower filter + extension INF
shared/           mm2_ioctl.h (kernel/user contract), mm2_touch.h (frame decoder)
tools/gestures/   mm2gestures.exe / mm2gesturesw.exe - gesture service
tools/rawtouch/   rawtouch.exe - raw touch viewer and driver diagnostics
tests/            decode_test.c, gesture_test.cpp
scripts/          testsign.ps1, install.ps1, uninstall.ps1, autostart.ps1
```

## Tests

From a Developer Command Prompt:

```cmd
cl /W4 /I shared tests\decode_test.c && decode_test.exe
cl /EHsc /std:c++20 /W4 /WX /I tools\gestures tests\gesture_test.cpp tools\gestures\engine.cpp && gesture_test.exe
```

The gesture tests feed synthetic touch frames through the recognizer. They
cover scrolling direction, momentum, swipes, app switching, taps, and the
cases that must *not* trigger anything.

## Not done yet

* USB-C Magic Mouse (PID 0x0323): commented out in the INF until tested.
* Battery level (report 0x90 is in the native descriptor but unused).
* Some gesture tuning is built in rather than in the settings file: swipe
  distance, app switcher step, double-tap timing.

## License

Not chosen yet. The code is an independent implementation. The Linux driver
(GPL-2.0) was used only as a protocol reference.
