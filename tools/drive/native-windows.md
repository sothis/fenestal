# Testing on a native Windows 11 laptop

A strategy and a setup. Phases 0 to 3 are done on two laptops, each on
both of its GPUs, and most of phase 4 with them; `DESIGN.md` *Measured on
Windows itself* has the figures. `TODO.md` has what is owed and the
questions left open.

## What the laptop is for

Every Windows figure in `DESIGN.md` is wine's or a VirtualBox guest's. Wine
is not Windows' window manager or its waits; the guest is, but it has no GPU
and its frame rate is its desktop's size. The laptop is the first place where
all three are real at once: the compositor, the kernel's waits and a Vulkan
driver that presents to a display. So it is the only machine that can speak
for *pacing* on Windows, and the only one that can say whether the 17–25 fps
seen on native Windows is gone.

It does not replace the guest. The guest is driven at an emulated keyboard
and mouse, below the Windows input stack; the laptop can only be driven from
inside it (see *Input* below). Keep both.

## Phase 0 — write down what the machine is

Before any figure, because every figure is read against these:

- the Windows build, and whether it is plugged in or on battery, and the
  power mode;
- every GPU, its driver version, and which one drives the panel — on a
  hybrid laptop the discrete GPU's frames are copied through the integrated
  one, and `--gpu=<name>` or `FNL_GPU` picks between them (a number does
  not: which device is `1` follows the executable);
- the panel's refresh rate, whether variable refresh is on, the display
  scaling, HDR on or off.

## Phase 1 — first contact, by hand

No harness, an hour, and it answers the two questions that matter most.
Copy `build/win64/release/` and the built `tools/probe/` programs to
the laptop on a stick and run, from a console:

1. `fenestal-testbed.exe --frame-times` — keep the `renderer:` line, the
   `GPU n:` lines and twenty `frame` lines. Then `--no-late-latch` for the
   floor, then each GPU, and each renderer by `set FNL_RENDERER=vulkan` (or
   `d3d12`) in the console first: unnamed, the library takes Direct3D 12.
2. `waittest.exe` — how late each kind of wait returns on this kernel, and
   whether the high-resolution timer was created or refused.
3. `vmcost.exe` — the clock and the hand-off, to read the guest's against.
4. `fenestal-dev.exe pace <rate> 12 --settle 4` at 0, 15, 60 and 250 — the
   sweep by hand, frames only; it prints the frame lines itself.

What the testbed's frame time says decides the next step:

| the testbed reads | then |
|---|---|
| the panel's rate, hold above zero | the wait fix holds; go to phase 2 |
| half the panel's rate, steadily | the two-image swapchain; phase 1b |
| 17–25 fps or anything irregular | profile; phase 1c |
| the rate only with `--no-late-latch` | the controller; see below |

In the last case the wait is sound and the late-latch controller is not:
keep the lines, since the `hold` column says what it settled on.

**1b. The swapchain.** A build with one line changed — the image count asked
for is the driver's minimum plus one — run beside the unchanged one, with
what the driver reported and what was built logged at startup. Two binaries,
not a switch in the tree. If the extra image restores the rate, the rule
becomes "never fewer than three under FIFO" and is measured on the other
three backends before it is kept.

**1c. The profile.** `sampler.exe fenestal-testbed.exe 10 <driver dll>` needs no
elevation. On a machine of one's own, Windows Performance Recorder is there
too and names functions, which the sampler cannot.

## Phase 2 — a way to drive it

The guest gives the harness three things, and a laptop gives none of them:

| the harness needs | the guest has | the laptop has |
|---|---|---|
| a program started on the desktop | `guestcontrol` | the agent's `run` |
| what only that desktop knows | the agent | the same agent |
| keys and pointer | emulated devices | the agent's `SendInput` |

So it is one path, the agent (`windows_agent.ps1`), and
`win64-vulkan-native` beside `win64-vulkan-vm` in `environment.py`.
`windows_driver.Desk` is what both machines ask; `native_driver.py` is the
laptop's way in. tools/drive/README.md *Driving a window* has how it works
and why.

### Setting the laptop up, once

The first three are the laptop's user's to do. The rest is one command
once ssh answers, and what it found is the next:

```sh
python3 tools/drive/native_driver.py --setup schenker
python3 tools/drive/native_driver.py --status schenker
```

Steps 4 and 5 below are what `--setup` does, for doing by hand. A round
leaves nothing on the machine but the files in `C:\fenestal\drive` and
its own data on the clipboard, written over what was there. While it runs
it has the desktop: it holds the laptop's own keyboard, mouse and touchpad
(`input_hold.py`: the agent's hooks drop what is not its own,
Ctrl+Alt+Escape gives them back, Ctrl+Alt+Delete always reaches Windows'
own screen (Task Manager, sign out), and the hold ends after 30 minutes
regardless; `FNL_DRIVE_HOLD=0` holds nothing),
and loads a keyboard layout with dead keys for a few checks, dropped again
unless the user had it.
Two laptops are set up so, `schenker` and `aspire`; the second is a German
Windows, which the harness does not mind.

1. **An address that stays.** A DHCP reservation, and an entry for the
   laptop in `~/.ssh/config` on the harness machine, with its user:

   ```
   Host schenker
       HostName 192.0.2.10
       User user
   ```

   The name alone may resolve to an IPv6 link-local address, which ssh
   cannot use without an interface beside it.

2. **The OpenSSH server**, from an elevated PowerShell, and the network set
   to Private, which is the profile its firewall rule is for:

   ```powershell
   Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
   Set-Service sshd -StartupType Automatic; Start-Service sshd
   ```

   The harness machine's public key goes in
   `C:\ProgramData\ssh\administrators_authorized_keys` for an
   administrator's account and in the user's `.ssh\authorized_keys`
   otherwise. `ssh <name> whoami` has to answer without a prompt.

   A laptop that answers ARP and no port at all is one whose network is
   Public or whose rule is missing, and the whole of this step for an
   administrator is then, elevated, with the key's one line pasted in:

   ```powershell
   $key = 'ssh-ed25519 AAAA... user@host'
   Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
   Set-Service sshd -StartupType Automatic; Start-Service sshd
   Get-NetConnectionProfile | Set-NetConnectionProfile -NetworkCategory Private
   if (-not (Get-NetFirewallRule -Name fenestal-ssh -ErrorAction Ignore)) {
       New-NetFirewallRule -Name fenestal-ssh -DisplayName fenestal-ssh `
           -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow `
           -Profile Private,Domain
   }
   $f = 'C:\ProgramData\ssh\administrators_authorized_keys'
   Add-Content -Encoding ascii $f $key
   icacls $f /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F'
   ```

3. **A desktop that is there.** Automatic logon, no lock on idle, no
   screensaver, no sleep and no display-off on mains, the lid's action
   "do nothing". Injected input does not reach a locked screen, and the
   harness says so and skips.

4. **The task that puts the agent on the desktop**, registered by the user
   who is logged on, from a PowerShell that is not elevated:

   ```powershell
   New-Item -ItemType Directory -Force C:\fenestal\drive | Out-Null
   $action    = New-ScheduledTaskAction -Execute 'C:\fenestal\drive\agent.cmd'
   $principal = New-ScheduledTaskPrincipal `
       -UserId "$env:USERDOMAIN\$env:USERNAME" `
       -LogonType Interactive -RunLevel Limited
   $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
       -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero)
   Register-ScheduledTask -TaskName 'fenestal-agent' -Action $action `
       -Principal $principal -Settings $settings
   ```

   No trigger: it runs when the harness asks. Interactive, so it is on the
   desktop and no password is stored. Limited, so the agent is no more
   privileged than the program it drives. The battery switches, because a
   task otherwise will not start unplugged and is killed when the plug is
   pulled. `agent.cmd` is the harness's to write, each round.

5. **A quiet machine.** A fixed power mode, written down; Do Not Disturb
   on; the widgets off the taskbar, which open under a pointer; Windows
   Update's active hours over the hours of testing; and, elevated,
   `Add-MpPreference -ExclusionPath 'C:\fenestal'`, or each round's fresh
   binary is scanned before it starts.

6. **The Vulkan SDK**, if the validation layer is wanted there, and the
   Visual C++ runtime (`https://aka.ms/vs/17/release/vc_redist.x64.exe`,
   `/install /quiet /norestart`), without which the layer is registered
   and does not load.

Then, on the harness machine, as for any other target:

```sh
python3 tools/drive/stress.py win64-vulkan-native --capture
python3 tools/drive/pace.py win64-vulkan-native
VK_LOADER_LAYERS_ENABLE='*validation' VK_LAYER_VALIDATE_SYNC=1 \
VK_LAYER_VALIDATE_BEST_PRACTICES=1 \
  python3 tools/drive/stress.py win64-vulkan-native
tools/probe/native_run.py build/win64/release/fenestal-testbed.exe 10 \
  --env FNL_RENDERER=vulkan --frame-times
```

Each of them once for each GPU. Write the devices after the machine's
name in `~/.local/state/fenestal/native-hosts` — `schenker intel nvidia` —
and a round or a sweep is run on each without being told, or on one as
`win64-vulkan-native@schenker:intel`; `FNL_GPU=intel` in front does the
same for one run (`--env FNL_GPU=intel` for the probe). The round prints
the device it is on. And once for each monitor where there are two:
`FNL_DRIVE_WIN_OUTPUT=DISPLAY2` puts the window on that one from the
start, by the name the round's own first lines give it.

### Its drivers

A driver is copied over and installed over ssh, silently, with a restore
point first; the machine restarts and logs in by itself. Intel's installer
is `gfx_win_<n>.exe --overwrite -s`. NVIDIA's package is unpacked first
(it is a 7-Zip archive) and cut down to `Display.Driver`, `HDAudio`,
`NVI2`, `NvCpl`, `NvContainer`, `PPC`, `NVPCF`, `MSVCRT` and the files
beside them, with the three `<file>` lines of `setup.cfg` that name files
under `NvApp` taken out, or `setup.exe -s -noreboot -noeula -nofinish`
fails with "path not found". That leaves the NVIDIA App, its overlay and
its notifications off a machine that is meant to be quiet.

### What it does not test, and whom it trusts

`SendInput` enters above the device drivers. Windows marks the events as
injected and delivers them as it would real ones, raw input included, but a
touchpad's driver, a precision touchpad's gestures and a real keyboard's
quirks are never exercised. It cannot reach an elevated window, which the
testbed is not. The foreground rule is the same as everywhere: `environment.py`
asks the agent which window has the keyboard before every key and refuses
otherwise.

The agent runs programs it is sent. It connects only to the laptop's own
loopback, which exists only while the harness's ssh connection forwards it;
whoever holds the ssh key is whoever it obeys. No password is involved.

## Phase 3 — the rounds

Done on both laptops, each device, with a second monitor on the first.
What it set out to do:

- `stress.py win64-vulkan-native`, with and without `--capture` (the agent
  takes the picture). The cross-monitor check runs where the machine has a
  second output: the first laptop, with its external monitor attached.
- `pace.py` has a column for each laptop: Windows with a display. CPU
  comes from the agent's `cpu`. This is where the spacing's period estimate
  and the lateness of its hold are read on a real compositor.
- Vulkan validation, if the Vulkan SDK may be installed on the laptop: the
  round with the layer's environment in front of it, as on the native Linux
  targets. These were the first validation runs against a Windows driver.
  The sanitizers stay impossible: mingw-w64 has no runtime for them.

## Phase 4 — what only a laptop has

Each of these is one row of a table in DESIGN.md once measured, and each is
a way a frame rate can be wrong for a reason that is not the library's. The
two GPUs, the scales the panels offer (100 to 175%; neither has 200%), the
external monitor at another rate and scale, and the desktop's size are
measured; the hybrid copy to an external output, the battery, a panel above
60 Hz and sleep are `TODO.md`'s.

- integrated against discrete GPU, and the hybrid copy between them;
- on battery and in battery saver, where Windows may coarsen timers for a
  process it has throttled — `waittest.exe` again, in that state;
- display scaling at 100, 150 and 200%;
- a panel above 60 Hz, and variable refresh on and off;
- an external monitor at another rate and another scale, and the window
  dragged between the two;
- sleep and resume, and the lid, which is the device-loss path on real
  hardware;
- the size of the desktop, which halves the guest's rate: expected to cost
  nothing here, and to be shown rather than assumed.

## Where the answers go

A question from `TODO.md` is answered in `DESIGN.md` — *Win32* for the waits
and the compositor, *Late latching* and the spacing tables for pacing,
*Measured on Windows itself* for the system — with the measurement and the
machine it was taken on, and then comes off the list.
