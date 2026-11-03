# Draft: keys twice under wine 11.21, VK_PROCESSKEY and then the key

A draft comment for wine's Bugzilla, bug 60449 ("Keys not used by the X
input method are delivered twice, as VK_PROCESSKEY and as the real key,
even with the IME closed"; imm32, UNCONFIRMED, version 11.19), not yet
sent. That report has the cause -- `ImeProcessKey` claims every key but
the modifiers (`/* TODO: should be ctx->fOpen */`) -- seen with a Japanese
layout and fcitx5 on X11, and points at a patch on bug 60448. What it has
not got, and this adds: the same with a German layout and no CJK input
method, from the first dead key on X11 and from the first key on Wayland;
that 11.0 does neither, so it is a regression, and the commits where it
began; and a reproducer typed through the RemoteDesktop portal rather than
XTEST.

Seen in the stress round on this desktop on 2026-11-01 (DESIGN *Wine 11 on
this desktop*), reproduced alone with `tools/probe/keytwice.c` and
`keytwice.py`, and read in the 11.20 source against 11.0's. Master was
11.21 on 2026-11-01 and its `dlls/imm32/ime.c`, `dlls/win32u/imm.c`,
`dlls/winex11.drv/xim.c` and `dlls/winewayland.drv/wayland_text_input.c`
are 11.20's. The tracker searched the same day (VK_PROCESSKEY, ImeProcessKey,
keys twice): 60449, and 60448 and 59737 beside it, nothing for Wayland. The
patch on 60448 was not tried here. To attach: `keytwice.c`.

## Comment

The same happens without any CJK input method, and under winewayland.drv
as well as winex11.drv. It is a regression: 11.0 does not do it.

Setup: Ubuntu 26.04.1, GNOME Shell 50.1 (mutter 50.1) Wayland session,
Xwayland 24.1.10, ibus 1.5.34 as the session's input method
(XMODIFIERS=@im=ibus), keyboard layout German (`de+nodeadkeys`; the dead
key below is with `de`), LANG=en_US.UTF-8. WineHQ's packages for Ubuntu
resolute: wine-stable 11.0, wine-devel 11.21, wine-staging 11.21, each in
a fresh prefix. Keys typed through the xdg-desktop-portal RemoteDesktop
session (NotifyKeyboardKeycode), which reaches the window as a keyboard's
keys do.

keytwice.c (attached) is a plain window with default IME handling that
prints every WM_KEYDOWN/WM_KEYUP with its virtual key and scan code:

    x86_64-w64-mingw32-gcc -O2 -Wall -mconsole -o keytwice.exe keytwice.c

1. winewayland.drv (run with DISPLAY unset), 11.21: every key, from the
   first, comes twice, down and up alike. Pressing A:

       down vk 0xe5 scan 0x01e
       down vk 0x41 scan 0x01e
       up   vk 0xe5 scan 0x01e
       up   vk 0x41 scan 0x01e

   Nothing was composed and the input method was never switched on;
   mutter offers zwp_text_input_v3, and text_input_enter activates the
   IME HKL (activate_keyboard_hkl(hwnd, TRUE)).

2. winex11.drv, 11.21: each key comes once (vk 0x41 alone) until the
   first key the input method filters -- here the dead acute under the
   `de` layout, then E, which gives é. From then on every key comes
   twice as above, and stays so after switching back to `de+nodeadkeys`.
   The first XIM preedit or commit activates the IME HKL
   (activate_ime_hkl() in xim.c), after which is_ime_hkl() is true for
   the thread's layout and ImeProcessKey claims every key.

3. 11.0, the same steps, both drivers: every key comes once.

wine-staging 11.21 is the same as wine-devel 11.21 (checked on
winewayland.drv).

What changed between the two, as far as I can read it: 11.0's
ImeProcessKey asked the driver (WINE_IME_PROCESS_KEY); since
4f8cb7a9 "win32u: Move IME processing to ImeToAsciiEx" and 9804d31d
"win32u: Remove builtin WINE_IME_PROCESS_KEY driver call" (11.7) it
returns TRUE for every non-modifier key whenever the layout is an IME
HKL, and since 4575a84e "winex11: Activate an IME HKL before sending IME
updates" and c0312c47 "imm32: Only notify the host IME for known IME
HKLs" (11.6) the drivers switch to such a layout as soon as the host
input method is involved. The unconsumed key then comes back through
WM_IME_KEYDOWN, so the window gets VK_PROCESSKEY and then the key.

For a program that reads keys by scan code -- a game, or a toolkit that
maps scan codes to its own key codes -- that is every key pressed twice,
so every toggle undoes itself. On a GNOME Wayland session that is every
key from the start, with a European layout and nothing composed.
