# The harness

Everything under `tools/drive/` drives a real window on a real desktop: the
round (`stress.py`), the pace instrument (`pace.py`), the benchmark
(`bench.py`), the signal and selftest rounds, and the ways into a
Windows laptop, a Mac and a Linux machine with no
desktop. The library's own DESIGN is the spec; this is how the measurements
in it are taken, and what the harness has had to learn about the desks it
runs on. What the harness found about the *library* on Windows itself -- the
size request's cost, the reaper, the monitors, the scales, suspend and
resume, Intel's stall -- stays in DESIGN.md, under *Backend notes*,
*Measured on Windows itself*.

A figure here carries the date it was measured, and is the tree's as it was
that day: counts of checks grow as checks are added, and a rate read before
a change to the frame loop stays as it was read. `status.py` says what was
last run where, and `docs/benchmarks.md` holds the latest full set.

## The MCP port's check

`python3 tools/drive/mcp_check.py <env> --socket <path>` (or `--command "ssh
host fenestal-mcp"`, how Windows and a Mac are reached) drives a testbed that
was started with `FNL_MCP=1 --text-input` as an agent would, and says PASS or
FAIL for nineteen things; it is not part of `stress.py`, whose environments
it does not start. What it found and where it has passed is DESIGN *Driven
from inside*; TODO *The MCP port* has what is owed.

## A round through the port

`python3 tools/drive/mcp_round.py [--variant asan] [x11-vulkan
wayland-vulkan]` drives a testbed it starts itself, through the MCP port:
the titlebar's own controls, every knob, keys, text, the pointer and its
capture, the frame read back, a move to another GPU, the forced faults in a
Debug tree, and a second window. No portal, no consent, no input held from
the desk. What the window server does with the window stays in
`stress.py`. DESIGN *Driven from inside*.

## The round

```sh
python3 tools/drive/stress.py                    # every environment
python3 tools/drive/stress.py x11-vulkan         # one of them
python3 tools/drive/stress.py --variant tsan     # under a checker
python3 tools/drive/stress.py --capture          # opt in to pictures
```

**A checker round runs under the validation layer.** `--variant asan`,
`tsan` or `gc` puts `VK_LAYER_KHRONOS_validation` in front, with sync and
best-practices validation, unless `--validation off` declines it; `on` asks
for it at release too. The round then reads the program's own mappings, and
its next check after the backend's and the renderer's (below) is that the
layer is in it: a loader that cannot find a layer says nothing, and a round
run without it would read as one run with it. Made unfindable
(`VK_LAYER_PATH=/nonexistent`), that check fails and the round with it.
What the layer says lands in the log, and `log is clean` fails on any line
there that no list expects, a `VUID` among them.

**A round holds the machine's own input.** For its length the keyboards,
mice and touchpads of the machine it drives are held away from the desktop
(`input_hold.py`): grabbed by root over ssh on this desk and the bare
machines -- here as `root@localhost`, with the key `FNL_DRIVE_ROOT_KEY`
names, or else the path in `~/.local/state/fenestal/root-key`, or else
whatever ssh's own configuration gives (`root_key_args()` in
`environment.py`) -- and dropped by the agent's hooks on a Windows laptop
and its event tap on the Mac, which let the harness's own input through. A
hand there would otherwise land in the middle of a drag. Ctrl+Alt+Escape
(Control+Option+Escape on the Mac) gives it back, and the sweep stops after
that round; a hold ends after 30 minutes regardless, and `FNL_DRIVE_HOLD=0`
holds nothing.

**A soak is the long run no round is.** `tools/drive/soak.py` holds
`fenestal-dev pace` resizing at 15 sizes a second for an hour by default and
samples its resident set; after a two-minute warm-up it must stay within 5%.
Ten minutes of it on X11 release held at 67,100 to 67,116 KiB, about 7,200
resizes. Under a checker the set is printed and not judged -- asan's
quarantine climbs to 256 MiB of its own -- and the checker's reports decide
instead. Opt-in: nothing runs it.

**One round rather than one per environment**, and the reason is the same one
`pace.py` is written for: a file per environment asks mostly the same questions
a different way each and answers them to a different depth each, with nobody
having decided the difference — a per-environment round is how a gap in one of
them hides. Written once, the differences are declared rather than incidental: a
check says what it *needs* — `server-rect`, `wm-props`, `grab-ring`,
`unminimize`, `composited` and the others — and the environment says what it
has, in its `CAN` set in `tools/drive/environment.py`, which is the whole list.
Nothing in the round tests an environment by name.

Those differences are facts about the window server rather than about this
library. A Wayland client is never told where its window is, so the rectangle
checks do not apply to it; it has a transparent grab ring and an X window does
not; and it cannot ask to be unminimized, which is why the off-screen skip is
checked where a window can be brought back. Each is reported as *not
applicable* with the reason, not quietly omitted.

**A Linux environment names its backend.** The Linux tree, `linux`, has
Wayland, X11 and KMS, so an environment is a tree and a backend --
`x11-vulkan` is `linux` told `FNL_BACKEND=x11`, `kms-vulkan-bare` is
`linux` told `kms` -- and the variable goes to the
program here and over the bare machine's ssh alike (`BACKEND` in
`environment.py`). Never the library's own choice: this desktop offers
both window servers, and a round of one under the other's name would pass
for it. So the round's first check is that the program's log says it
opened the backend it was named (`backend: X11, named by FNL_BACKEND`).
A Windows or macOS tree has one backend and is told none; the variable is
kept from it, since such a build refuses a name it has not got.

**And every environment names its renderer.** Every tree has every
renderer of its system in each program -- `win64` Direct3D 12 and Vulkan,
`linux` Vulkan, `macos` Metal and Vulkan -- so `win64-vulkan` and
`win64-d3d12` (and their `-wl`, `-vm`, `-native` and bare twins), and
`macos-metal-native` and `macos-vulkan-native` are each one tree told
`FNL_RENDERER=vulkan`, `d3d12` or `metal`. Left to itself the
library takes the first renderer of its system's order and steps down on
a failure, so a round of one renderer under another's name would pass for
it. Every environment is told its renderer (`RENDERER` in
`environment.py`): over the bare machine's ssh, by the laptop's agent and by the Mac's ssh, as `VK_*` and
`FNL_GPU` reach each. The variable is never inherited from the harness's
own environment, here or forwarded to a machine. The round's next check,
after the backend's, is that the program's log says it drew with what it
was named (`renderer: Direct3D 12, named by FNL_RENDERER`). `FNL_GPU` is
the *device*, a different question.

**A round writes down that it ran.** Each environment's totals go to a ledger
as one line — the commit, the environment (its name and its machine), the
variant, whether the validation layer was in front, the GPU the program said
it was on, the checks and the failures — and a `pace.py` sweep leaves one for
each column (`tools/drive/ledger.py`). The file is
`~/.local/state/fenestal/ledger.jsonl`, beside the portal's token and for the
same reason: it has to outlive `rm -rf build`, and it is this desk's account
of its own machines and so not tracked. `python3 tools/drive/status.py` reads
it back as what was last run where and how many commits the tree has moved
since, with `never` for a round an environment is owed and has not had; a
laptop's GPUs are a block each, from when they are written down for it
and not from when one is first run on. A skipped round is a line too, so a
machine that was off is seen to have been asked.

## Measuring the pace

`fenestal-dev pace` (`tools/drive/pace.c`) is the harness that drives the
*library* rather than the window server. It is a caller: it includes the public
header and no other of the library's, registers no callbacks but the empty one
`--draw --no-vsync` asks with, and asks for its own sizes at a fixed rate from
a thread of its own through `fnl_request_size_pt()`, which is `[any]` on every
backend precisely so a caller may. CMake builds `fenestal-dev` into every tree
beside the testbed, so it exists for the checker variants and for the cross
targets as well.

```sh
./build.sh
python3 tools/drive/pace.py                  # every target, release
python3 tools/drive/pace.py x11-vulkan       # one of them
python3 tools/drive/pace.py --variant tsan   # under a checker
python3 tools/drive/pace.py --rates 0,15,60 --seconds 8
```

`bench.py` is the other half: `fenestal-testbed`'s benchmark mode, a frame
with an application in it, measured under options every one of which the
record names (DESIGN *Benchmarking*). It runs where a round runs -- this
desk's environments, the bare machines', the Windows laptops'
and the Mac's, whose agents relay the program's stderr home -- and keeps
each `fenestal-bench/1` record in the state directory's `bench/`, with the
tree's commit written into it; `tools/bench/report.py` makes the tables,
and with `--html` a page of them with charts. `--hands` moves the pointer
round a small circle over the window, a step every 10 ms or as fast as the
machine's input answers, through each measure, for the record's latency
rows -- everywhere but this desk, whose portal reaches whatever is focused.
`--sizes 640x360,1280x720` runs each environment once per area, a record
a size: the window is asked for it in points and, on KMS, whose window is
its output, as a display mode. `report.py --fit` reads such records into
a frame's fixed cost and its cost a megapixel, which one size cannot
separate.

```sh
python3 tools/drive/bench.py kms-vulkan-bare@schenker
python3 tools/drive/bench.py x11-vulkan-bare -- --bench-repeats=3 \
    --bench-vsync=off --bench-frames
python3 tools/drive/bench.py --hands win64-d3d12-native@schenker:nvidia
python3 tools/drive/bench.py --sizes 640x360,1280x720 x11-vulkan-bare
python3 tools/bench/report.py                # every record kept, a row a series
python3 tools/bench/report.py --compare <a> <b>
python3 tools/bench/report.py --fit
python3 tools/bench/report.py --html > report.html
```

Why a program of its own: the rounds are clicks and keys, timed by what the
harness waits for — and a figure taken that way measures the harness. This one
puts the same stream of sizes through every environment and prints the answers
side by side.

Why side by side: a pacing rule that is wrong on two platforms of three — the
rebuild anchor in DESIGN *Frame pacing* is one — does not show in either's own
column. Reading down one says nothing: 90 fps looks like a client that is
simply fast. Reading across a row, against its neighbours, says it at once.

Registering no callbacks is the other half of what it is for. What
`--frame-times` reports then is the library's own floor, with nothing of a
caller's in it; the testbed is the other measurement and answers the other
question. Both are worth having, because they separate a regression here from a
regression in the testbed.

The CPU figure in this desk's and the bare machines' columns is `utime + stime`
out of `/proc`, which is why the runner is Linux's there; the Windows laptops'
and the Mac's columns ask their agents instead (`measure_native()`). For
`win64-vulkan` the process is wine, so that column carries wine's
overhead too — comparable across rates on that target, and only roughly
comparable with the other columns. A single sweep is an indication; DESIGN.md's
tables are two sweeps agreeing.
`docs/benchmarks.md` keeps the latest full set, every platform side by side.

## Driving a window

`tools/drive/` has **one way in** to this desktop: the RemoteDesktop portal,
for all three backends. `wayland_driver.py` is the portal session and the
aiming;
`x11_driver.py` is what an X server can be *asked* about a window — the native
X11 build and the wine-hosted one alike, since a wine window is an X window —
and drives nothing. `environment.py` is what a round may ask of a window
whichever server is holding it, and it carries the wine-hosted half — the launch
and the truncated process name — as a subclass of the X11 one. `stress.py` is
the round itself, one over every environment, and `options_wayland.sh` sweeps
the command line under the checkers.

**Nothing here uses XTEST, and nothing should.** XWayland runs with
`-enable-ei-portal`, so every XTEST request from every X client is itself
routed through the RemoteDesktop portal — by XWayland, on the client's behalf,
and with no restore token, because XWayland has nowhere to keep one. The first
`fake_input` of each process therefore raises a consent dialog. Until somebody
answers it mutter holds a full-screen guard window above the stack and no X
client receives pointer input, so a round that meets one fails from end to end,
every failure reading as a finding about window management. The symptom is
XTEST that looks "intermittent"; the cause is the dialog.

The portal session this harness already holds a token for has none of that.
What is injected there enters the compositor as real input and reaches an X
window through XWayland the way a mouse does — which is also the truer test,
since it is the path a hand takes, and it is what delivers the raw-input deltas
wine never gets from XTEST. Aiming is steered two ways: in the X environments by
reading the pointer and the window rectangle back from the server, which needs
nothing from the application and so works over a wine titlebar that echoes no
motion; and by the application's own echo everywhere else. One
`fake_input`, in this directory or in a throwaway script, is one dialog.
The one XTEST in the tree is `bare_driver.py`'s, and it is sent to a plain
Xorg on another machine, over a socket that module forwarded itself: no
portal there, and never `$DISPLAY`.

**The Win32 build is driven a second time, on Windows itself.** Wine is not
Windows, so `win64-vulkan-native` runs the same `win64` binary on a Windows
laptop and puts the same round to it (below). What only that desktop knows --
the rectangle, the foreground window, the monitor, the pointer, the clipboard
-- is answered by `windows_agent.ps1`, which the harness copies in and which
connects *out* to it, because Windows' firewall lets a connection out unasked.

The pointer is placed by absolute reports, which is exact, and a captured one
is moved by a relative report, because the library reads a captured pointer
from raw input and raw input counts only device motion. The clipboard's other
program is the agent and not `xclip`, which is on the wrong desktop; an
environment says which program that is (`clipboard_put()`, `clipboard_get()`).

**And on a laptop, which is the one with a GPU.** `win64-vulkan-native` is a
Windows machine reached over ssh: the agent starts the program and sends the
input. `Win32Native` in `environment.py` inherits what `WindowsAgent` does
with an answer, `windows_driver.Desk` is the asking, and `native_driver.py` is
the way in. Input is the agent's `SendInput` -- scancodes, absolute moves over
the virtual desktop, and relative ones for a captured pointer -- which enters
above the device drivers: Windows marks the events injected and then delivers
them as it would a hand's, but nothing below that is exercised. The program is
the agent's child, started on the desktop because the agent is on it; a
program started from an ssh session has no desktop to open a window on. Its
stderr comes back to `native_relay.py`, which is the process under test as
far as `testbed.py` can tell.

ssh is the way there and the way home. One connection is held for the round;
every copy and command is multiplexed over it, and it forwards two of the
laptop's loopback ports to this machine's, so the agent connects to
`127.0.0.1` where it is, nothing listens on the network at either end and
neither firewall has a rule for it. The agent reaches the desktop through a
scheduled task, `fenestal-agent`, registered once by the laptop's user to run
only while they are logged on; the harness asks for it to be run and writes
the one-line `agent.cmd` it runs, since the port is new each round. It needs,
once: the OpenSSH server on the laptop and a key in it, that task, and a
desktop that is logged in and not locked — `tools/drive/native-windows.md`
has each as a command. `FNL_DRIVE_NATIVE` names the machines as
`~/.ssh/config` knows them, commas between them, and a round is
`win64-vulkan-native@<name>`; GPUs go after a name as they do in the file,
and one named without any takes the file's. With that unset they are the
lines of `~/.local/state/fenestal/native-hosts`, so a bare round drives the
laptop. A laptop that is on its live key instead is
brought to Windows and back by `power.py boot <name> windows|stick`, which
sets the firmware's next boot for one boot only (efibootmgr from the key,
`bcdedit {fwbootmgr} bootsequence` from Windows) and waits for the system
asked for; `power.py wake <name>` is Wake-on-LAN, to the ports `power.py
learn` found. All three wake from off into the stick: schenker in 66 s,
the thinkpad in 54 (its *Startup -> Network Boot* is the stick, not PXE,
which a wake would boot first), aspire in 43 (on its own port, with Wake
on LAN on in its BIOS; a USB adapter is unpowered when it is off). A
machine listed in `cold-hosts` (schenker) goes into Windows and macOS
from off instead: next boot set from the key, powered off, woken. A
restart from the key into Windows left its panel dark twice in a day,
and a start from off did not (`power.py`'s comment). A round types into that
desktop and overwrites its
clipboard, and a laptop is a machine somebody may be sitting at, so it is
asked first: Windows says when the last input was and not whose, so each
agent leaves the time of its own last one in a file as it goes, and input
later than that inside two minutes (`FNL_DRIVE_NATIVE_BUSY`) is a hand, and
a skipped environment. A laptop just booted reads busy for its first two
minutes, the logon's input; with nobody at it nothing else does (schenker
polled 12.6 minutes under Windows and 65 under macOS, 2026-10-31, without
one reset), so `power.py boot` waits the window out before it returns and
the check needs no exception. The picture is the screen's own device
context, every monitor together, so it is what Windows composed. A key the
agent holds would never repeat — the repeat is the keyboard's or its
driver's, below `SendInput` — so the agent sends the press again itself,
after the delay and at the rate that user has set.

One laptop has been put through it: Windows 11 build 26300, an Intel HD 630
driving a 1920x1080 panel at 60 Hz with a GTX 1050 Ti beside it, the display
at 125%. The round is 66 checks with `--capture` and 63 without, none failed,
with the move between monitors skipped for want of a second. It was the first
round anywhere at a scale other than 100%, and what it turned up was the
harness's: the echo's `area` is pixels and the round had been aiming and
comparing with it as points, which at 100% are the same number. `testbed.py`
now carries both. It is also the first screen smaller than the round: the
default window at 125% is 1600x943 of the panel's 1080 rows, so the resize
drag first moves the window back towards the monitor's origin and is cut to
the room that leaves (`room_to_grow()`), which changes nothing where there was
room. With those changes in, and the agent both Windows machines run grown for
the laptop, the other rounds are where they were (2026-10-13): X11 74, wine
65, wine on Wayland 41, Wayland 56, none failed. Wayland's picture of the bar
straight after fullscreen was once of a window 1321 pixels wide where 1278 was wanted — the compositor's animation,
with other windows on the desk beside it — so that check now looks again
before it fails. The hand check was put to a pointer moved on the laptop by
something other than the agent, and the round was skipped five seconds later.

The rest of what the other targets have, it has. `native_driver.py
--status <name>` says how a machine is set up and `--setup <name>` does what
can be done over ssh — the task, Defender's exclusion, no sleep on mains,
the name written down. A round opens with what its figures are read
against: the Windows build, each GPU and its driver, the panel's mode, the
power source and plan, the scale. `VK_*` in the harness's environment is
handed to the program there, so the validation layer goes in front of a
round as on this desktop; and since a loader that cannot load a layer says
so to nobody, the round looks for the layer's library *in* the program and
stops without it. That was not idle: the SDK's layer needs the Visual C++
runtime, the laptop had none, and the first "validated" round there
validated nothing. With it loaded, sync and best-practices on, the round is
63 checks, none failed, and the log is clean — the first validation run
against a Windows driver, NVIDIA's 582.78 with Intel's 31.0.101.2145 on the
panel. `pace.py` has a column for each laptop, the program started by the
agent and its processor time the agent's account of it.
`tools/probe/native_run.py` runs one program on the laptop and prints what
it says.

**A laptop's GPUs are part of what a round is called.** After a machine's
name in `native-hosts` go the devices a round is run on there, each by a
part of its name as `FNL_GPU` takes one — `schenker intel nvidia` — and a
bare round is then `win64-vulkan-native@schenker:intel` and
`win64-vulkan-native@schenker:nvidia`, one after the other, with a
`pace.py` column for each. Written down rather than asked of the machine,
so that `status.py` knows what a laptop is owed while it is off; a machine
with none written gets one round on the device the library picks. A target
on the command line is an environment and as much of `@machine:device` as
narrows it. `FNL_GPU` in the environment is
still one device for one run, whatever is written, and a round on a laptop
says which device its program is on and stops if that is not the one
named. Once for each of the two, on the build that has the name
(2026-10-13):

| | Intel HD 630 | NVIDIA GTX 1050 Ti |
|---|---|---|
| `stress.py --capture` | 66, none failed | 66, none failed |
| validation, sync and best-practices | 63, none failed, log clean | 63, none failed, log clean |

**A Windows machine's monitor is a variable of its own.**
`FNL_DRIVE_WIN_OUTPUT=DISPLAY2` runs a round, a sweep or `pace.py` on that
monitor of a laptop, by the name Windows gives it there, which the
round's first lines print; unset, the window opens where Windows puts it, on
the primary. `FNL_DRIVE_OUTPUT` names an output of this desktop and
`FNL_DRIVE_BARE_OUTPUT` and `FNL_DRIVE_MAC_OUTPUT` one of those machines', so
none of them is mistaken for another's. A name the machine has no monitor for
skips the environment, with the names it has.

And `pace.py`, one sweep each, frames presented and CPU of one core
(2026-10-13, before a window at rest drew nothing):

| sizes a second | Intel HD 630 | NVIDIA GTX 1050 Ti |
|---|---|---|
| idle | 60 fps, 2.6% | 60 fps, 6.5% |
| 5 | 57 fps, 13.5% | 72 fps, 16.9% |
| 15 | 46 fps, 37.0% | 104 fps, 46.1% |
| 30 | 34 fps, 68.2% | 95 fps, 54.7% |
| 45 | 30 fps, 81.2% | 56 fps, 71.6% |
| 60 | 26 fps, 81.7% | 38 fps, 67.7% |
| 250 | did not end in 42 s | 3 fps, 94.5% |

Idle is the panel's rate on both. Under size requests neither is: every
other environment holds 59 or 60 down that column, and here the rate falls
with the request rate on the device that owns the panel and first *rises*
past the panel's on the one that does not, which a FIFO swapchain should
not allow. One sweep each, so an indication; the run past the panel was
the period estimate's, which waits on the fence for it, and DESIGN
*Measured on Windows itself* has that and the sweep after it, with nothing
past the panel. The same program at 250 on the Intel device ended on time
when it was run again by itself. The bare round on the build with the
name, captures on (2026-10-13): Wayland 56, X11 74, wine 65, wine on
Wayland 41 and the laptop 66, none failed.

**A second laptop, and what it changed.** An Acer Aspire VN7-792G: the
same Windows build, an HD 530 on a 1920x1080 panel at 60 Hz and 100%, a
GTX 960M, a German Windows. NVIDIA's 582.78 went on as on the first (its
five-year-old driver was 382.05); Intel's 31.0.101.2111 stayed, the
package after it being for the seventh generation on. Two things in the
harness were the first laptop's and not Windows':

- what ssh brings back is in the console's code page, and a German
  message with an umlaut in it stopped the round as a decoding error. A
  byte that does not decode is now replaced (`native_driver.py`);
- "a minimized window costs no CPU" wanted a visible window with vsync off
  to cost over 20% of a core, so that the drop is from something. On the
  HD 530 it costs 16 to 18: the processor waits on a device the frame
  keeps busy. The check is now taken on a vsynced window, as every
  window is: that it was drawing is its frames, counted from the
  `--frame-times` lines the round starts the testbed with
  (`DRAWING_FPS`), and the drop is a ratio of the CPU, under a fifth of
  it when minimized. Restored, it draws at three quarters of the visible
  rate or more (`RESTORED_SHARE`), counted the same way: Windows charges
  a process's time in whole ticks of 15.6 ms, and Direct3D 12 on
  schenker's HD 630 draws for 2% of a core, which a three-second sample
  read once as 1.0, two ticks. Minimized, the window draws nothing, by
  its frames as well ("a minimized window draws nothing", under
  `HIDDEN_FPS`), and its CPU is taken over 8 s (`HIDDEN_SAMPLE`): over
  4 s, aspire's HD 530 under Direct3D 12 read 1.2% three times in 30
  rounds, three ticks charged to a window whose log has no frame drawn
  in the time.

| on the Aspire, 2026-10-13 | Intel HD 530 | NVIDIA GTX 960M |
|---|---|---|
| `stress.py --capture` | 66, none failed | 66, none failed |
| validation, sync and best-practices | 63, none failed, log clean | 63, none failed, log clean |

| sizes a second | Intel HD 530 | NVIDIA GTX 960M |
|---|---|---|
| idle | 60 fps, 2.3% | 60 fps, 4.9% |
| 5 | 59 fps, 13.0% | 59 fps, 10.9% |
| 15 | 54 fps, 34.3% | 57 fps, 18.2% |
| 30 | 38 fps, 61.1% | 60 fps, 35.1% |
| 45 | 36 fps, 76.2% | 57 fps, 43.5% |
| 60 | 28 fps, 80.4% | 60 fps, 48.4% |
| 250 | 27 fps, 83.0% | 9 fps, 61.2% |

The Intel column is the first laptop's Intel column over again, so that
one is the integrated device's or its driver's and not a machine's. The
NVIDIA column is not the first laptop's: here it holds the panel's rate
to 60 requests a second, and there it ran past it and then fell, on the
same driver version.

**A hand on the border, timed.** Every row of a sweep is the caller asking
for sizes; none was the window server's own resize, which on Win32 is the
manager's loop from the press to the release. `pace.py` has a last row for
it, `border`: the harness holds the bottom-right corner of a window and
carries it 200 by 150 points in and back, a third of a second each way, for
the cell's seconds (`Driver.scrub()`). The window is `fenestal-testbed`'s --
the harness finds one by the events it echoes, and `fenestal-dev` drains
none -- so the row has the testbed's scene in it and is read across. Twelve
seconds a cell, frames and CPU of a core:

| | X11 | wine | wine on Wayland | HD 630 | GTX 1050 Ti | HD 530 | GTX 960M |
|---|---|---|---|---|---|---|---|
| `border` | 61, 11.7% | 59, 16.8% | 60, 18.3% | 16, 118.9% | 48, 37.2% | 20, 118.7% | 59, 32.8% |

The native Wayland window has no cell: its edges are the grab ring, which the
harness has no way to aim at. The window is left alone for three seconds before
the press (`BORDER_REST`), so that the row is a settled window's, with its
late-latch hold learnt. A border taken in a window's first second is what the
borrowed period is for (DESIGN *The period is measured*), and with it reads as
the row does on the NVIDIA devices, 50 and 57.

On the Intel devices a hand is fifteen to twenty sizes a second, with more
than a core busy. Timed in a build that printed each step, on the HD 630:
`vkCreateSwapchainKHR` 43 to 47 ms for a swapchain of 1600x942 pixels,
against the 16 of DESIGN *Measured on Windows itself* for one of 1100x620
points at most, and
the drain ahead of a retired one's destroy 11 to 30 ms, against 2, with the
testbed's scene in the queue. Unrested the same drag read 28 fps and not 16,
and no more sizes: without a late-latch hold yet, the loop draws a frame at
the old size between two rebuilds, and half its frames are those. So the
figure to read on Intel is the sizes, about fifteen a second either way.
Two sweeps of each laptop column agreeing to within three frames, and four
alternated on the HD 630.

**And on a Mac.** `macos-metal-native` and `macos-vulkan-native` are the one
`macos` tree, told each of its two renderers, on schenker booted from its macOS
stick, and `CocoaNative` in `environment.py` is the laptop's environment once
more: `windows_driver.Desk`'s questions, answered by an agent of the Mac's own
(`mac_agent.m`, which `mac_driver.py` compiles here with the cross toolchain and
copies over each round). Getting in is simpler than on Windows. A program
started from an ssh session as the logged-in user reaches the window server, so
there is no scheduled task and no forwarded port: the agent is the last command
of one session, its stdin and stdout the line, and the program is the last
command of another, whose stderr is the echo. One master connection carries both
and every copy. What only a hand on the Mac can give is two grants to
`/usr/libexec/sshd-keygen-wrapper`, which is what an ssh session is to the
privacy database: Accessibility, without which a posted event is dropped without
a word, and Screen Recording, without which a window's title is withheld and a
picture is the wallpaper. The agent asks for both before a round and the round
is skipped, saying which, when either is missing. `mac_driver.py --status
<name>` says which are there, and `--setup <name>` raises Accessibility's dialog
on the Mac and writes the machine down; the machines are `FNL_DRIVE_MAC`'s or
the lines of `~/.local/state/fenestal/mac-hosts`, and `power.py boot schenker
macos` takes schenker there.

Input is posted `CGEvent`s at the HID tap, which macOS delivers as a
hand's; modifiers go as flag changes with the side's device bit, and a held
key is repeated by the agent at the user's own delay and rate, since the
repeat is the keyboard's and a posted press has none. The picture is
`screencapture`, matched to sRGB on the Mac by ColorSync before it comes
back: matched here through the profile it is written with, the testbed's
field, sRGB 14, read 10, under every rendering intent. The busy check is
the laptop's, against the HID idle clock: input in the last two minutes
that came after the agent's own last (`FNL_DRIVE_MAC_BUSY`) is a hand, and
`power.py boot` waits that window out before it returns, as it does for
Windows -- there until IOHIDSystem's own idle clock (`HIDIdleTime`, the
one the agent reads) has passed it from the desktop's login on, which
Remote Login's answer precedes by 25 s on schenker (2026-11-04). The
clock is not reset by every posted event -- once it stood 1.3 s before
the agent's last button-up -- so only input *after* the agent's counts.
The agent finds a window by its title before an untitled one: AppKit
gives each program five untitled windows of its own besides, off screen
on the same layer, and the first of those has no rectangle a round can
use.

**Its displays.** The macOS stick is an OpenCore install (`-wegnoegpu` in
its boot arguments, the discrete GPU off), and it drives both panels, the
built-in one and the external PL4071UH on its HDMI, from power-up. Four
things make that so, and each is needed:

- The firmware's hidden "Primary IGFX Boot Display" is LFP, so that the
  firmware lights the built-in panel beside the external; at its default
  it lights the external alone. A BIOS reset to defaults undoes it.
- An OpenCore driver of the lab's, run before macOS, switches off every
  pipe the firmware lit beside the built-in panel's. Apple's Kaby Lake
  driver always scans the built-in panel out of a framebuffer slot in
  stolen memory, and only slot 0 -- the firmware's boot pipe -- shows a
  picture there; booted on the external, the panel trains its link, takes
  slot 1 and stays black. With the panel dark the driver also raises its
  supply, or macOS finds no EDID over AUX and drops it.
- `igfxagdc=0`: with AGDC on, an external plugged in later stops at "AGDC
  managing Display" and never gets a pipe.
- `igfxonln=1 igfxonlnfbs=0x01`: on wake the framebuffer driver asks the
  HDMI port for its status over DisplayPort AUX, times out and frees the
  port; forcing framebuffer 1 online keeps the external across a display
  sleep. WhateverGreen ignores the list without `igfxonln`.

The external's 3840x2160 mode is 30 Hz over that port and has shown
artifacts when set at boot; 2560x1440 @60 is the one to keep, set with a
display configuration (`CGBeginDisplayConfiguration()`,
`CGConfigureDisplayWithDisplayMode()`, `CGCompleteDisplayConfiguration()`
with `.permanently`), since `CGDisplaySetDisplayMode()` reports success and
changes nothing. Beside the disabled GPU `pmset`'s `gpuswitch` is 1
(integrated).

What the first rounds found was the library's, each now in DESIGN *macOS*:
the layer was untagged and its sRGB shown as the panel's own colours; keys
were lost in fullscreen, where AppKit makes the window and not the view
the first responder; press-and-hold held a key's first five repeats back;
and a drag event ended the press the titlebar's move is handed, so the
window never moved. One machine, the HD 630 under macOS 26.7.1, 1920x1080
at 100% (2026-10-22):

| | `macos-metal-native` | `macos-vulkan-native` (MoltenVK) |
|---|---|---|
| `stress.py --capture` | 64, none failed | 64, none failed |

| sizes a second | `metal@schenker-mac` | `vulkan@schenker-mac` |
|---|---|---|
| idle | 0 fps, 0.3% | 0 fps, 0.2% |
| 5 | 20 fps, 4.3% | 20 fps, 5.3% |
| 15 | 20 fps, 12.7% | 20 fps, 15.4% |
| 30 | 60 fps, 17.7% | 60 fps, 24.9% |
| 45 | 60 fps, 23.9% | 60 fps, 31.7% |
| 60 | 60 fps, 25.3% | 60 fps, 37.2% |
| 250 | 60 fps, 60.4% | 60 fps, 67.8% |
| draw | 59 fps, 9.2% | 60 fps, 12.4% |
| border | 60 fps, 20.5% | 60 fps, 28.5% |

The shape is every other environment's -- at rest when nothing asks, the rest
rule's three to four frames a size under the panel's rate (the 15 row's 20
is the frame-times reading DESIGN *At rest* corrects: 47 counted), the
panel's rate from 30 -- and
unlike Windows on the same device it holds 60 to 250 a second. The three
not applicable are the move to another monitor, with one there; the
pointer drawn where it is, which the window server draws; and a display
mode picked from the titlebar, since the window server sizes and places
the window.

**And X11 with nobody managing it.** Every X window above is on XWayland
under mutter, so every question the library puts to a window manager has
been answered, and the paths behind `_NET_SUPPORTING_WM_CHECK` being unset
-- the drag, the edges, fullscreen and maximize done by the library, the
server's focus with nobody to give it -- are reached by no environment on
this desktop. `x11-vulkan-bare` runs them: the `linux` tree's
binary, told to open X11, on another Linux machine's Xorg, with no window
manager, desktop or compositor there (`bare_driver.py`). Xorg is started as
root on `:9`, on VT 7, as the transient unit `fenestal-xorg`, and left up
between rounds; the program runs as the machine's own account over one held
ssh connection, which also forwards the server's socket to a loopback port
here. So the questions are `x11_driver.py`'s, put from this machine to that
server, and the input is XTEST on the same connection, absolute motion only,
since XTEST's relative motion is accelerated and the portal's is not.

The machines are `FNL_DRIVE_BARE`'s or the lines of
`~/.local/state/fenestal/bare-hosts`, a name and the ssh destination
(`thinkpad user@thinkpad`), and a round there is
`x11-vulkan-bare@thinkpad`. A `KEY=VALUE` word after the destination is
that machine's: every program a round, `pace.py`, `bench.py` or
`bare_run.py` starts there is given it unless this desk's environment
names the variable too (`thinkpad user@thinkpad LP_NUM_THREADS=2`, the
setting that keeps that machine from resetting).
`bare_driver.py --setup <name> [user@address]` installs what a minimal
Debian or Arch with Xorg lacks -- the Vulkan loader and Mesa's drivers,
`vulkan-tools`, the validation layers, `xclip` -- and writes the name down;
`--status`, `--up` and `--down` are the rest; `--down` gives the panel's
VT back with the servers, since a round's VT is a console nothing has
written to and a panel left on it is black. The two distributions are told
apart by `/etc/os-release`, and `DISTROS` holds all that differs between
them; a machine booted from its stick is Arch. The tree a machine is sent
is its `uname -m`'s (`TREES`): `linux-arm64` on the Raspberry Pi 4 (`pi4`),
where the Windows environments, with no wine to run under, are skipped
with the reason. `x11-vulkan-bare` declares no `unminimize` (iconifying is
a request to a manager) and no `desk-layout` (GNOME's layout is not that
server's). It speaks for the X server's behaviour and not for frame rates:
the first machine is a ThinkPad Edge with an i3 M370, whose Ironlake
graphics no Vulkan driver covers, so lavapipe draws. `pace.py` has a column
for it all the same, `bare@thinkpad`,
since what it measures there is the pacing and the CPU, and a lavapipe machine
is where a loop nothing throttles shows: the column is what found the spacing
short of a refresh (DESIGN *Frame pacing*). Run it with
`MESA_VK_WSI_DEBUG=noshm` in front, which `MESA_*` forwarding takes there, or
Mesa's MIT-SHM path leaks a segment a swapchain image and the 250 row stops the
machine's X server taking any more (DESIGN *X11*).

The checker rounds run there too, with this desktop's sanitizer runtimes
shipped beside the binary: the instrumentation is gcc 15's, Debian's runtime
gcc 14's. The ARM tree's are the Pi's own, since the runtimes beside
Ubuntu's cross compiler are another gcc's (`_runtimes()` says why). Under
ASan the driver is preloaded, since an unloaded lavapipe leaves its globals'
allocations reported as leaks from an unknown module; under TSan
`tsan-lavapipe.supp` goes with `tsan.supp`, for two races inside lavapipe's
device thread. asan, tsan and gc are 62 checks each, none failed, under the
validation layer.

A bare round drives the machine as it drives the laptops, and
asks first as a laptop is asked: a console login there with a key pressed in
the last two minutes (`FNL_DRIVE_BARE_BUSY`) makes it a skipped environment,
since a round takes the panel to VT 7. Read off the tty, not the X server,
whose idle time the last round's XTEST moved. `tools/probe/bare_run.py` is
`native_run.py` for it: one program copied over, run on that server for so
many seconds, and its stdout and stderr printed here; with `--kms` on the
bare display instead, with `--weston` on weston (how
`tools/probe/heldpresent.sh` runs that probe), and with `--driver` the
NVIDIA GPU moved to that kernel driver first.

**A second monitor there is arranged, not left to Xorg.** With no desktop,
nobody decides where a monitor goes: Xorg lights what is connected when it
starts and leaves one plugged in later dark. So each time a round asks for
the server, `Bare.arrange()` sets the layout in one `xrandr`: the
primary (the panel) at the origin, every other connected output right of
the one before, every disconnected output off. The environment declares
`multi-output` -- with one output the section skips itself -- and
`FNL_DRIVE_BARE_OUTPUT=HDMI1` runs a round on that output of the machine,
as `FNL_DRIVE_WIN_OUTPUT` does for a Windows one.

With schenker's Samsung moved to the thinkpad's HDMI, the round is 65 checks
on the panel and 64 on the Samsung, none failed, the move between outputs run
both ways; one check fewer on the Samsung, whose window has room without
being taken in. A pull and a replug with the window on the Samsung, and the
output switched off and on under it, are in DESIGN *X11: xcb, not Xlib*.
schenker's own round loses its second monitor for as long as the Samsung is
here.

**The same machine with weston, or with nothing.** `wayland-vulkan-bare`
runs the same `linux` binary on weston there, told to open Wayland,
and `kms-vulkan-bare` runs it told to open KMS, with neither server up, so
that it opens the DRM node first and has the panel. A tree's programs go
to a directory of their own there, `fenestal-drive/<tree>-<variant>`, which
every round of one tree shares; a Windows program's to one for each
renderer it is told, `win64-<renderer>-<variant>`, since a Direct3D 12
round's has DXVK's DXGI beside it. weston is started by root
as a login session of the account's (`PAMName=login`, VT 7, unit
`fenestal-weston`, socket `wayland-fenestal`), which is what has logind
hand it the seat; its `weston.ini`, written into `fenestal-drive` each
time, has no panel, no lock, no animation and an unaccelerated pointer,
the console's layout, and a repaint window of 10 ms, since at weston's 7
the thinkpad's Ironlake misses every vblank. Only one of Xorg and weston
runs at a time: asking for either stops the other, and a KMS round stops both.
`bare_driver.py --weston <name>` starts weston by hand.

Both are driven by uinput: `uinput_hands.py`, copied there and run by
root, makes a keyboard and a mouse the kernel offers like any others, so
weston reads them through libinput and the KMS backend through evdev
itself. That is XTEST's place on this machine taken by the kernel's own
devices, allowed for XTEST's reason -- it reaches that machine and nothing
of this one. The windows are found by the echo, as the native Wayland
window is: nothing there says where they are.

The weston round declares `grab-ring` and `composited`
(`weston-screenshooter`, which weston allows under `--debug`), and the
library's line at open says which optional globals weston left unbound.
The KMS round declares what a window that is the output has -- its picture
(`composited`, below), a pointer only the library draws (`drawn-pointer`),
the output's modes in the titlebar (`display-modes`) -- and not
`resizable`, so the round's size sections are skipped there.
`--setup` also installs weston and xkeyboard-config's files (`xkb-data`;
the KMS backend reads its layout from them, with no tool), puts the
account in group `input` and, where the distribution has them, `video` and
`render` -- Xorg's VT switch leaves the seat with no active session and
logind then removes the DRM nodes' ACLs, which a desktop that keeps the user
out of those groups has nothing else to fall back on -- and loads `uinput`
now and at every boot, sends the
kernel's messages to VT 12, installs `tools/gpu-driver.sh` for the
library's own move between kernel drivers, and fetches the debug info of
the installed Vulkan drivers and layers and of Mesa's drivers, so that a
checker names their frames.

**This desktop is a bare machine too, booted into its console.** The GRUB
entry `fenestal-console` (`grub-reboot fenestal-console`, then
`systemctl reboot -i`) comes up with no display manager; `~/.ssh/config`
names it `black` as the account and as root, so
`FNL_DRIVE_BARE="black user@black" FNL_DRIVE_BARE_BUSY=0 python3
tools/drive/stress.py kms-vulkan-bare@black` runs a KMS round on it --
`BUSY=0` because the hand that started the round is logged in on tty1,
and `modprobe uinput` first, which is not persistent there. The DRM node
is the seat's by logind's ACL. It is not in `bare-hosts`: a bare sweep
from a GNOME session would try to take the display GNOME holds.

**The Windows tree there, under wine.** `win64-vulkan-bare`,
`win64-vulkan-wl-bare` and `win64-vulkan-cage-bare` run the `win64`
tree's program under wine on that machine's Xorg, on its weston and on
cage, told to draw with Vulkan, and `win64-d3d12-*` the same program told
the other renderer: six environments. Wine has no
driver for a display with no server on it, so the KMS round puts the
least server there is in between: cage, a kiosk compositor on the DRM
node, started as weston is (unit `fenestal-cage`, a login session on VT
7), with the program its one window. cage accelerates its pointer and
has nothing to turn that off, so the hands there are an absolute pointer
(`uinput_hands.py --absolute`), and its picture is `grim`'s. One of Xorg,
weston and cage runs at a time.

The program goes over with every `.dll` of its tree -- a Direct3D 12
round's from the directory `_Vkd3dProton` stages, vkd3d-proton beside it --
into a directory of its renderer's own, and runs in a prefix in `/tmp`, made
the first time a boot asks for it (`WINE_PREFIX_UP`), with `Managed=N`:
wine's own setting for an X server with no window manager. The shell
there waits for the program and says when it has exited, since wine's
helpers hold the ssh open for seconds after it. The X round gives the
window the X focus once, which wine does not take for itself; and the
rounds skip, with the reason, what is wine's and not the library's: the
dead keys on X, which Xlib's Compose table composes, the console's layout
on Wayland, and on weston the way back from fullscreen to maximized.
DESIGN *Wine on the bare machine* has the measurements. `--setup`
installs wine, cage and grim; `bare_driver.py --cage <name>` starts cage
by hand.

weston has every connected output on, laid out by weston itself and read
back off its scene graph (`Bare.weston_layout()`), so one weston serves
whichever output a round is on (`FNL_DRIVE_BARE_OUTPUT`, else the panel):
the pointer is put on that output before the program starts, and weston
opens the window where the pointer is. The pointer pushed into the
top-left output's corner is a place that layout names, which is what lets
`make_room()` find the window and take it in before the round drags its
edges, as the X11 round does by asking the server: on the thinkpad's
1366x768 panel a 1280x720 window has no room for them. 49 checks with
`--capture` on the panel and 48 on HDMI-A-1, none failed (DESIGN *Across
two outputs*).

On the thinkpad the KMS round runs the copy path: lavapipe answers
`vkGetDrmDisplayEXT` with `VK_ERROR_EXTENSION_NOT_PRESENT`, so each frame
is drawn headless and flipped on by the backend (DESIGN *KMS: no window
server*). Its picture is the framebuffer the CRTC scans out, read by root
there (`kms_grab.py`): the screen itself, so the round declares
`composited`. 39 checks with `--capture`, none failed. The harness reports
a program gone before its window as a skipped environment, with its last line
as the reason.

Kernel drivers written after a machine's address in `bare-hosts`
(`schenker user@schenker nouveau nvidia`), or named by
`FNL_DRIVE_BARE_DRIVER`, are a KMS round each,
`kms-vulkan-bare@schenker:nouveau`: the servers stopped, the NVIDIA GPU
moved to that driver with no reboot by `tools/gpu-driver.sh` as root
(`bare_driver.py --driver <name> <driver>` by hand), and the program told
`FNL_GPU_DRIVER=switch`, so that the round's hands move it to the other
driver and back from the titlebar's GPU box. KMS's alone: under Xorg,
weston or cage the GPU may be the server's, and those rounds run on the
driver the machine booted (DESIGN *Another kernel driver, with the window
open*).

**And a third time, under wine on the compositor itself.** `win64-vulkan-wl`
is the same binary again with wine drawing through `winewayland.drv` and not
its X11 driver. The launch alone chooses it: started with `DISPLAY` unset,
wine 10.0 loads `winewayland.drv` in the program and in its `explorer.exe`,
no X window appears, and Vulkan still reaches RADV — nothing is written to
the prefix, and `Graphics=wayland` was not needed. Which of the two a window
is on is something the program can say: `fnl_window_host()` answers from the
display driver the process has loaded, and the testbed's status readout shows it
beside the platform — "Vulkan on Win32 (wine on Wayland)" and "(wine on X11)"
under the two launches, and the bare "Vulkan on Win32" on Windows, each read off a
capture.

The environment is the native Wayland one with a different program
(`Win32Wayland` in `environment.py`): the focus from the echo, a picture from
the portal, and no server to ask where anything is. It is in the round like the
rest: 41 checks with `--capture`, none failed and four not applicable, in a
round of six — 56, 74 and 65 on Wayland, X11 and wine on X11 — that took 371 s. What it has shown:

- **The bar and the edges report no motion**, as under wine on X11: they
  are non-client area. There the X server says where the pointer is; here
  nothing does, so a point on either is reckoned from one the echo confirmed
  inside them. That holds because the portal's motion is not accelerated —
  100 asked for was 100 reported, three moves of three.
- **The resize edge works, and wine does the resizing.** The corner dragged
  by 160x120 took the area from 1280x720 to 1439x840, and each of the eight
  drags after it moved the size a step at a time, forty points out and
  forty back. The resize checks read the area the application was handed
  and no longer the server's rectangle, in every environment that has an edge
  to drag — the library being told and rebuilding is what is claimed, and an
  environment with no server to ask is then asked the same.
- **Wine's monitors are not to be taken at their word.** Asked with
  `tools/probe/wherewin.c` beside the window: under `winewayland.drv` the
  virtual screen is 0,0 3840x2160 and both monitors are at 0,0 — 1920x1200 and
  3840x2160 — while their *work* rectangles keep the desk's real offsets, 0,960
  and 1920,0, at the monitors' full sizes. `WM_GETMINMAXINFO` is answered from
  the two: `ptMaxPosition` is work minus monitor, 1920 here, which put the
  maximized window at 1920,0 on a screen 3840 wide. The pointer is held to that
  screen, so motion was reported up to x = 1919 of a window 3773 wide and not
  past it, the maximize button of a maximized window could not be reached, and
  the round stopped there. The same probe under `winex11.drv` has the layout
  whole: a virtual screen of -1920,0 5760x2160 and the large monitor's work
  area, 67,26 3773x2134, inside it. So a work area that does not lie inside its
  monitor is not believed (`monitor_work()` in `win32.c`): the monitor stands in
  for it, and the compositor's configure sets the size as it does for every
  maximize under wine. With that the round goes on through the restore, the
  pointer's capture, the keys and the typing to its end, and the other five
  rounds are what they were (2026-10-12) — 47, 69 and 57 checks on Wayland, X11
  and wine on X11, none failed. Whether a hand on a mouse
  is held as the portal's pointer was has not been tried.
- **This wine's Wayland driver has no clipboard.** A copy in the window did
  not reach `xclip`, which still held what the harness last put there, and a
  paste gave the window back its own 25 bytes and not the other program's
  640000. `winewayland.so` in wine 10.0 names every Wayland interface it
  binds and `wl_data_device_manager` is not among them, so there is nothing
  to cross by. The environment asks that of the driver the program loaded
  (`clipboard_missing()`) and skips the section with the reason; a wine
  whose driver has one is put to the checks with nothing here to change.
- **Two wine drivers cannot follow each other at once.** The wine server
  outlives its last program by a few seconds, with the desktop process that
  chose a driver still in it, and a program started meanwhile joins that
  desktop whatever its environment says: after the X11 driver's run the
  Wayland one's `CreateWindowExW` fails with 1400. Both wine environments wait
  for the old server to go before starting (`wine_clear()`), by its
  process, since this distribution keeps `wineserver` off the PATH.
- **Pacing is the same, and cheaper.** `pace.py`, one sweep of the two:

  | sizes/s | wine on X11 | wine on Wayland |
  |---|---|---|
  | idle | 60 fps, 2.0% | 60, 2.8% |
  | 5 | 60, 6.5% | 60, 4.3% |
  | 15 | 60, 9.7% | 60, 6.8% |
  | 30 | 60, 13.7% | 60, 11.0% |
  | 45 | 60, 17.3% | 60, 14.8% |
  | 60 | 60, 21.7% | 60, 16.8% |
  | 250 | 59, 39.2% | 60, 33.2% |

- **Wine never says the window changed monitor.** Dragged by its bar from
  the large monitor to the other, the window is there in a capture — its
  bar at 354,1198 where it had been at 3234,717 — and the program goes on
  being told `DISPLAY2`, 3840x2160; `wherewin` has the window at 0,960
  throughout. So `fnl_output_name()` and `fnl_output_size_pt()` are the
  first monitor's for the window's life there, and the section is not
  declared. Under wine on X11 the same move is followed, `DISPLAY1` to
  `DISPLAY2` and 1920x1200, and back; that environment runs the section now,
  with the names taken as wine's (`on_output()`). **Nothing is done about
  it, and that is decided:** every call a Win32 program has puts the window
  where wine believes it is, and wine believes it has not moved, so there
  is no second opinion to ask. A size list filtered against that output is
  the first monitor's until wine's driver follows the window; the library
  reports what its window server told it and does not guess.
- **It does not type through the desk's layout.** The letters tried, in
  either case, and the space are right. Past them, under German with or without
  dead keys: the accent key types nothing and holds nothing, the sharp s
  types a hyphen, the circumflex key a plus and the o-umlaut a u-umlaut.
  Driven through the portal, which is the road a keyboard's keys take.
  There is therefore no dead key to switch to, and `desk-layout` is not
  declared.
- **Not asked:** minimise, here as on the native Wayland environment. Its checks
  want the window brought back, and nothing can do that for a Wayland client:
  the client has no request for it and the harness types nothing at a desktop
  whose focus is not the window's. Nor where a titlebar drag took the window,
  which no Wayland client is told; that the drag did not resize it is asked of
  every environment, off the echo.
- **The program's log** was not kept under either wine: `testbed.py` wrote it
  when the program's stderr closed, and wine's server holds that open past
  the harness's exit. It is written when the program is stopped as well.

**Another wine than the distribution's.** Everything above is wine 10.0's,
the `wine` on the path. WineHQ's builds sit beside it under `/opt`, none
on the path, and `FNL_DRIVE_WINE` puts the four wine environments here --
and `pace.py`, `bench.py` and `soak.py` with them -- on one: a branch
(`stable`, `devel`, `staging`: `/opt/wine-<branch>`), or a path to a wine
binary or the directory its `bin/` is in.

    FNL_DRIVE_WINE=stable python3 tools/drive/stress.py win64-vulkan
    FNL_DRIVE_WINE=devel FNL_DRIVE_VKD3D=wine \
        python3 tools/drive/stress.py win64-d3d12 win64-d3d12-wl

Each wine named has a prefix of its own, under the state directory
(`wine-prefix/<its directory>`), made by the first start with no display
and without Mono or Gecko (`_DeskWine.clear()`), about 25 s: a prefix a
later wine opens is upgraded past 10.0's, so `~/.wine` stays the path's.
The declarations above were measured of 10.0, and each holds for the wines
it has been seen to hold for (`WINE_HELD`): the floor and the opening
monitor under the Wayland driver for 11.0 as well, the move to Direct3D 12
on wine's own vkd3d, `multi-output` and `desk-layout` for 10.0 alone.
Another wine is put to the checks with them lifted, and its round says
which still hold (DESIGN *Wine 11 on this desktop*).
`FNL_DRIVE_VKD3D=wine` runs the Direct3D 12 environments, here and on the
bare machine, on wine's own vkd3d and not on vkd3d-proton. The ledger has
the wine as it gives its version (`wine`) and `vkd3d` where either was
named, and `status.py` a row for each. `FNL_DRIVE_BARE_WINE` is the bare
machine's own: unset, its `wine` on its path; a branch word, WineHQ's build
of that branch under `/opt` there (as `FNL_DRIVE_WINE` names one here); or a
path to its directory or `bin/`. A named wine gets a prefix of its own
beside the machine's, since a wine 11 will not open a prefix a wine 10
made. `tools/probe/keytwice.c`, with `keytwice.py`
to type into it, is a window that prints every key message wine sends it.

**The portal types into the desktop, not into a window**, and the harness
asks before every key. A key goes to whatever the compositor holds focused,
and a window can lose the focus half a second after opening, with a round's
next keystrokes going to another program. So `environment.py` checks first — the
X server's input focus *and* the manager's `_NET_ACTIVE_WINDOW` on X11 and
wine, the application's own echoed focus events on Wayland — and a key that
cannot be shown to be going to the window under test is not sent: the round
stops with `InputRefused` instead of typing a paste and an Enter into
somebody's chat. The rounds also use the real clipboard and leave their own
text on it.

**The harness waits on evidence, not on a clock, and only asks for the
focus.** `focus()` sends `_NET_ACTIVE_WINDOW` and nothing beside it. A
`SetInputFocus` of the harness's own is the alternative, and it is wrong: under
a Wayland compositor it moves the X server's answer without moving where the
compositor sends a key, so the server says "focused" about a window that is
sent nothing — measured as about two rounds in fifteen losing one action after
the window came back from being minimized. Both accounts have to agree before a
key goes, and each key is then waited for in the application's echo and sent
once more if it never shows — once, and only after silence, since a key that
arrived and was pressed again would undo every toggle a round presses.
Silence is not proof, though: a first press can land after the wait for it
ended, so a key sent twice is counted afterwards, and where both arrived the
round says `arrived twice` and presses it a third time, which leaves a toggle
where one press would (a key that typed is only reported). Uncounted, that
was one F11 under wine on the thinkpad leaving the window fullscreen and six
failures in the GPU section after it, far from the key. The
minimize click is waited for the same way, on the manager's
`_NET_WM_STATE_HIDDEN`, because a wine titlebar echoes no press. And a
transition is waited for in what the library reports of it: fullscreen and
maximize on the echoed `window` event, a capture taken or released on the
echoed `capture` event, rather than on a sleep long enough for the slowest
environment under the slowest checker.

That is the rule throughout `tools/drive/`: `Environment.until()` waits for a
predicate, `quiet()` for the echo to have said nothing more, `press()` and
`let_go()` for a key's edges, `rect_still()` for the server to stop moving a
rectangle. What is left on a clock is a handful of named constants, each with
its reason beside it — `PARK_BEAT`, `MAP_BEAT`, `SWEEP_BEAT`, `ROW_BEAT`,
`CORNER_BEAT`, `PAUSE`, `SELECTION_BRIDGE` and `AIM_BEAT` — for the places
with nothing to wait on. `AIM_BEAT` is the one that looks replaceable and
is not: polling for the first changed pointer reading from XWayland instead
lost the window at a resize corner three rounds in ten. Waiting on evidence is
also the faster harness: the `stress.py` round of three environments it then
was took 143 s on this machine against 185 s on fixed sleeps (2026-10-11).

**Any program built on the library can be driven.** The echo is the
library's (`src/core/echo.c`): it runs
inside `fnl_poll_event()` on the event being returned, so it is still exactly
a caller's view, and `FNL_ECHO_EVENTS=1` in the environment switches it on
with not a line in the program.

Driving the native Wayland window needs three pieces, none of which needs root:

* **Input** — the `org.freedesktop.portal.RemoteDesktop` portal. One consent
  dialog, and `persist_mode: 2` returns a restore token that skips it
  afterwards. mutter injects the events into its own stack, so what is under
  test is the whole path: libinput vocabulary → mutter →
  `wl_pointer`/`wl_keyboard` → `src/backend/wayland`. There is no second way
  in on this desktop: `wtype` wants a virtual-keyboard protocol mutter does not
  implement, and `ydotool` wants `/dev/uinput`, which is `root:root 0600` — so
  the portal is what "none of which needs root" costs.
* **Aiming** — the portal offers only *relative* motion, and absolute would need
  a ScreenCast stream and a PipeWire consumer. The library's echo prints
  every `FnlEvent` with its window-relative point, and the harness steers
  by that: nudge, read back where the application says the pointer is, correct.
  A Wayland client is never told where its window is, so neither is anything
  outside it — and this sidesteps the question entirely, along with the scale
  factor and the grab ring, because all three cancel out of a difference between
  two points in the same space.
* **Seeing** — the Screenshot portal, for what the *compositor* made of the
  frame, beside the Mesa layer's view of what the client *produced*. The two can
  disagree; only one of them can tell you so.

Artifacts land in `build/drive/`, which `rm -rf build` clears. Each
environment's program log there is `<environment>-<variant>.log`, written
again by its next round, and `<environment>-<program>-<variant>.log` for
a round of another program (`Environment.log_path()`); a round that failed
keeps a copy, `<environment>-<variant>.failed-<time>.log`, which the round
prints and the ledger's `log` field names. `pace.py`'s logs are its own,
`pace-<environment>-<variant>-<row>.log`, the border row's among them.
**The restore token does not live there**:

```sh
python3 tools/drive/wayland_driver.py --consent   # once per machine
python3 tools/drive/wayland_driver.py --status    # held, and where
python3 tools/drive/wayland_driver.py --forget    # ask again next time
```

It goes in `$XDG_STATE_HOME/fenestal/portal-token.json`, defaulting to
`~/.local/state/fenestal/`, because it is a credential this desktop handed the
user rather than an artifact of a build — and `rm -rf build` is this project's
only clean. Under `build/` every clean would cost a consent dialog, and not an
idempotent one: losing the token does not revoke the grant, it *orphans* it,
and each one stays in the portal's own permission store
(`~/.local/share/flatpak/db/remote-desktop`).

**The Screenshot portal is a second permission, and the client cannot carry
it.** The API has no restore token and no persist mode, so unlike the input
grant there is nothing for the harness to save: whether a picture costs a dialog
is entirely the desktop's stored permission, which only the desktop can create.
On this one it is already stored — `~/.local/share/flatpak/db/screenshot`, a
blanket grant for non-sandboxed callers — and a request is usually answered
from it. Measured: `stress.py wayland-vulkan --capture` takes six pictures and
runs in 51 s against 37 s without them, twice over — the pictures and the
three checks that read them are the fourteen seconds — and the store's mtime
does not move: nothing asked.

*Usually*, and not more than that: the same command, on the same machine with
the same grant in the store, has put up dialogs, and neither the store nor the
API accounts for it. What a dialog nobody answers costs is not the picture. It
is modal, so it takes the keyboard from whatever else is running, and it maps
mutter's guard window, which stops pointer input reaching *any* X client — so
rounds that have nothing to do with the capture fail end to end, every failure
reading as a finding about window management.

So captures are opt-in, and the reason is that "usually" rather than a figure:
a machine that has not granted it gets a dialog the harness has no way to
persist, and this one gets one occasionally, unexplained.
`stress.py --capture` turns them on; without it the composited checks report
*skipped* with the reason, which is the same answer a portal that refused would
get.

**Taking the input consent deliberately is the other half**, and it is not
tidiness.
The dialog is modal and takes the keyboard, so a round that meets one has
already lost the focus checks it is about to make — whether or not somebody
eventually clicks Allow. So the round asks with `prompt=False`: with
no grant each of this desktop's environments is skipped, saying how to get
one, and a round in which nothing ran exits **77**, which automake and meson
both read as *skipped*, and which is neither the 0 of a round that passed nor
the 1 of a round that found something. A round that could not run has not found
a bug, and reporting it as one is how a suite stops being read.

**`No such interface “org.freedesktop.portal.RemoteDesktop”` out of
`Portal.start()` is a stale portal frontend, not a missing portal.**
`xdg-desktop-portal` chooses its backend once, at its own startup, from
`$XDG_CURRENT_DESKTOP`, and can outlive the session that started it; one still
carrying another desktop's value loads a backend that implements no
RemoteDesktop and never exports the interface. The tell is *partial*
availability: `gdbus introspect --session --dest org.freedesktop.portal.Desktop
--object-path /org/freedesktop/portal/desktop` lists `Screenshot`, which has a
gtk implementation, and neither `RemoteDesktop` nor `ScreenCast`.
`systemctl --user restart xdg-desktop-portal.service` fixes it, because the
systemd user manager's environment is the current session's. It also drops
every portal session on the desktop — an in-flight file dialog or screen share
goes with it — and it is worth typing before concluding anything about the
backend.

## Capturing a frame

Several measurements in this file are pixel values off a real frame. Mesa's
screenshot layer takes the image out of the swapchain, which is also the *right*
place to look — it is the buffer the client actually produced, with no
compositor between:

```sh
VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_screenshot \
VK_LAYER_MESA_SCREENSHOT_CONFIG="frames=150,output_dir=/tmp/shots" \
    FNL_BACKEND=wayland FNL_RENDERER=vulkan \
        ./build/linux/release/fenestal-testbed
```

Three things to know before reading numbers off the result.

It **does not compose with the validation layer** — enable both and no file is
written.

It **crashes the process on any swapchain rebuild**. Resize, maximize or press
`F11` with the layer loaded and the program takes a SIGSEGV inside the layer,
which keeps per-swapchain state and does not survive the old one being retired.
It is the layer's bug; the same sequence without it round-trips indefinitely. So
a capture run and a resize run are two different runs, which is why the Wayland
harness takes its pictures through the Screenshot portal instead.

The **PNG holds linear light, not sRGB**. On an `_SRGB` surface the swapchain
bytes are encoded, and the layer writes those values decoded. The titlebar,
authored as sRGB 41, reads 6. Compare against `to_linear()` of the authored
value.

And capture with **motion off**: `scene_time == 0` is the one instant
reproducible across runs and machines.
