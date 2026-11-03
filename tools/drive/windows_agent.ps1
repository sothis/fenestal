# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
#
# What a Windows desktop can be *asked* about a window, for a harness that is
# not on it, and on a machine with no devices of its own to borrow, what it
# can be *made to do*. The harness copies this in and starts it there; it
# connects *out* and answers one line a question until told to go.
#
# Outwards because Windows' firewall lets a connection out unasked. From a
# VirtualBox guest the address is 10.0.2.2, the host's loopback as the
# machine's NAT shows it (once it has --nat-localhostreachable1 on, which is
# not its default). From a laptop it is the laptop's own loopback, which the
# harness's ssh forwards home. The other way round is a forwarding rule or a
# firewall rule, and somebody with administrator rights to click it.
#
# Two halves. The first is x11_driver.py's -- the rectangle, the focus, the
# monitor, the pointer -- and the clipboard's other program, since xclip is
# on the wrong desktop; both machines ask it. The second is for the laptop
# alone (native_driver.py): a guest's keyboard and mouse are VirtualBox's to
# move and its programs guest control's to start, and a laptop has neither,
# so here the input is SendInput and the program is this one's child.
#
# C# and not PowerShell below, because every answer is a user32 call. Kept to
# what the compiler in the box takes, which is C# 5.

param([int]$Port, [string]$Addr = "10.0.2.2")

Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class FnlAgent
{
    [StructLayout(LayoutKind.Sequential)]
    struct RECT { public int l, t, r, b; }
    [StructLayout(LayoutKind.Sequential)]
    struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct MONITORINFOEX
    {
        public int cb;
        public RECT mon;
        public RECT work;
        public uint flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string dev;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT
    {
        public int dx, dy;
        public uint data, flags, time;
        public UIntPtr extra;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct KEYBDINPUT
    {
        public ushort vk, scan;
        public uint flags, time;
        public UIntPtr extra;
    }
    [StructLayout(LayoutKind.Explicit)]
    struct INPUTUNION
    {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public INPUTUNION u; }
    [StructLayout(LayoutKind.Sequential)]
    struct LASTINPUTINFO { public uint size, tick; }
    [StructLayout(LayoutKind.Sequential)]
    struct BITMAPINFOHEADER
    {
        public uint size;
        public int w, h;
        public ushort planes, bits;
        public uint compression, bytes;
        public int xppm, yppm;
        public uint used, important;
    }
    delegate bool EnumProc(IntPtr h, IntPtr l);
    delegate bool MonProc(IntPtr m, IntPtr dc, IntPtr rect, IntPtr l);

    const string U = "user32.dll";
    [DllImport(U)] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport(U)] static extern bool IsWindow(IntPtr h);
    [DllImport(U)] static extern bool IsWindowVisible(IntPtr h);
    [DllImport(U)] static extern bool IsIconic(IntPtr h);
    [DllImport(U)] static extern uint GetWindowThreadProcessId(IntPtr h,
                                                               out uint pid);
    [DllImport(U, CharSet = CharSet.Unicode)]
    static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport(U)] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport(U)] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport(U)] static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport(U)] static extern bool GetCursorPos(out POINT p);
    [DllImport(U)] static extern IntPtr GetForegroundWindow();
    [DllImport(U)] static extern bool SetForegroundWindow(IntPtr h);
    [DllImport(U)] static extern bool BringWindowToTop(IntPtr h);
    [DllImport(U)] static extern bool ShowWindow(IntPtr h, int how);
    [DllImport(U)] static extern bool AttachThreadInput(uint a, uint b,
                                                        bool on);
    [DllImport(U)] static extern void keybd_event(byte vk, byte sc, uint fl,
                                                  UIntPtr extra);
    [DllImport(U)] static extern bool SetWindowPos(IntPtr h, IntPtr after,
        int x, int y, int w, int hh, uint flags);
    [DllImport(U)] static extern IntPtr MonitorFromWindow(IntPtr h, uint fl);
    [DllImport(U)] static extern IntPtr MonitorFromPoint(POINT p, uint fl);
    [DllImport(U)] static extern bool EnumDisplayMonitors(IntPtr dc,
        IntPtr clip, MonProc f, IntPtr l);
    [DllImport(U, CharSet = CharSet.Unicode)]
    static extern bool GetMonitorInfo(IntPtr m, ref MONITORINFOEX mi);
    [DllImport(U)] static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport(U)] static extern bool SetProcessDPIAware();
    [DllImport(U)] static extern uint GetDpiForSystem();
    [DllImport(U)] static extern int GetSystemMetrics(int which);
    [DllImport(U)] static extern bool OpenClipboard(IntPtr owner);
    [DllImport(U)] static extern bool CloseClipboard();
    [DllImport(U)] static extern bool EmptyClipboard();
    [DllImport(U)] static extern IntPtr GetClipboardData(uint fmt);
    [DllImport(U)] static extern IntPtr SetClipboardData(uint fmt, IntPtr h);
    [DllImport(U, CharSet = CharSet.Unicode)]
    static extern IntPtr LoadKeyboardLayoutW(string klid, uint fl);
    [DllImport(U)] static extern bool UnloadKeyboardLayout(IntPtr hkl);
    [DllImport(U)] static extern IntPtr GetKeyboardLayout(uint thread);
    [DllImport(U)] static extern int GetKeyboardLayoutList(int n,
                                                           IntPtr[] list);
    [DllImport(U)] static extern bool PostMessageW(IntPtr h, uint msg,
                                                   IntPtr wp, IntPtr lp);
    [DllImport(U, SetLastError = true)]
    static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport(U)] static extern bool GetLastInputInfo(ref LASTINPUTINFO i);
    [DllImport(U)] static extern bool SystemParametersInfoW(uint what,
        uint n, out uint value, uint fl);
    [DllImport(U)] static extern IntPtr GetDC(IntPtr h);
    [DllImport(U)] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    const string G = "gdi32.dll";
    [DllImport(G)] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport(G)] static extern IntPtr CreateCompatibleBitmap(IntPtr dc,
                                                               int w, int h);
    [DllImport(G)] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
    [DllImport(G)] static extern bool BitBlt(IntPtr to, int x, int y, int w,
        int h, IntPtr from, int sx, int sy, uint rop);
    [DllImport(G)] static extern int GetDIBits(IntPtr dc, IntPtr bmp,
        uint first, uint lines, byte[] bits, ref BITMAPINFOHEADER bi,
        uint usage);
    [DllImport(G)] static extern bool DeleteObject(IntPtr o);
    [DllImport(G)] static extern bool DeleteDC(IntPtr dc);
    const string K = "kernel32.dll";
    [DllImport(K)] static extern uint GetTickCount();
    [DllImport(K)] static extern uint GetCurrentThreadId();
    [DllImport(K)] static extern IntPtr GlobalAlloc(uint fl, UIntPtr bytes);
    [DllImport(K)] static extern IntPtr GlobalLock(IntPtr h);
    [DllImport(K)] static extern bool GlobalUnlock(IntPtr h);
    [DllImport(K)] static extern IntPtr GlobalFree(IntPtr h);
    const uint CF_UNICODETEXT = 13;

    static IntPtr hwnd = IntPtr.Zero;
    static uint pid = 0;
    // The program this agent started, on the machine where it starts one.
    static Process child = null;

    // The first window of a process called `image` whose title has `title`
    // in it. Both, because the title alone is any Fenestal window an earlier
    // run left up, and the process alone has windows that are not this one.
    static bool Find(string image, string title)
    {
        IntPtr found = IntPtr.Zero;
        uint owner = 0;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            if (!IsWindowVisible(h))
                return true;
            StringBuilder sb = new StringBuilder(256);
            GetWindowText(h, sb, sb.Capacity);
            if (sb.ToString().IndexOf(title) < 0)
                return true;
            uint p;
            GetWindowThreadProcessId(h, out p);
            try {
                string n = Process.GetProcessById((int)p).ProcessName;
                if (String.Compare(n, image, true) != 0)
                    return true;
            } catch (Exception) {
                return true;
            }
            found = h;
            owner = p;
            return false;
        }, IntPtr.Zero);
        hwnd = found;
        pid = owner;
        return found != IntPtr.Zero;
    }

    // Windows refuses the foreground to a process the user is not typing
    // into, which is every process a harness starts. A thread attached to
    // the foreground window's input is treated as that window's own, so the
    // request is honoured; and where that is refused too, a key pressed by
    // this process makes it the one that had the last input. Alt, because
    // alone it types nothing.
    static bool Foreground()
    {
        if (IsIconic(hwnd))
            ShowWindow(hwnd, 9);                    /* SW_RESTORE */
        if (GetForegroundWindow() == hwnd)
            return true;
        uint me = GetCurrentThreadId(), x;
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out x);
        bool joined = fg != 0 && fg != me && AttachThreadInput(me, fg, true);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (joined)
            AttachThreadInput(me, fg, false);
        if (GetForegroundWindow() != hwnd) {
            keybd_event(0x12, 0, 0, UIntPtr.Zero);  /* VK_MENU */
            SetForegroundWindow(hwnd);
            keybd_event(0x12, 0, 2, UIntPtr.Zero);  /* KEYEVENTF_KEYUP */
            sent = GetTickCount();
        }
        return GetForegroundWindow() == hwnd;
    }

    // The clipboard by its own calls and not through Windows Forms, whose
    // OLE route gives up while another program is looking -- and here one
    // always is: the Guest Additions watch the clipboard to share it.
    static void ClipOpen()
    {
        for (int i = 0; i < 100; i++) {
            if (OpenClipboard(IntPtr.Zero))
                return;
            Thread.Sleep(20);
        }
        throw new InvalidOperationException("the clipboard stayed busy");
    }

    static string ClipGet()
    {
        ClipOpen();
        try {
            IntPtr h = GetClipboardData(CF_UNICODETEXT);
            if (h == IntPtr.Zero)
                return "";
            IntPtr p = GlobalLock(h);
            if (p == IntPtr.Zero)
                return "";
            try { return Marshal.PtrToStringUni(p); }
            finally { GlobalUnlock(h); }
        } finally {
            CloseClipboard();
        }
    }

    static void ClipPut(string s)
    {
        char[] c = (s + "\0").ToCharArray();
        /* GMEM_MOVEABLE, which is the only kind the clipboard takes. */
        IntPtr h = GlobalAlloc(2, (UIntPtr)(uint)(c.Length * 2));
        if (h == IntPtr.Zero)
            throw new OutOfMemoryException();
        Marshal.Copy(c, 0, GlobalLock(h), c.Length);
        GlobalUnlock(h);
        ClipOpen();
        try {
            EmptyClipboard();
            // The clipboard owns the memory once this succeeds, and only
            // then.
            if (SetClipboardData(CF_UNICODETEXT, h) == IntPtr.Zero) {
                GlobalFree(h);
                throw new InvalidOperationException("SetClipboardData");
            }
        } finally {
            CloseClipboard();
        }
    }

    static string Rect()
    {
        // The client rectangle, which for a Fenestal window is the window:
        // WM_NCCALCSIZE takes the frame to nothing. Asked this way so that
        // the answer is the area the library lays out in either way.
        RECT r;
        POINT o = new POINT();
        if (!GetClientRect(hwnd, out r) || !ClientToScreen(hwnd, ref o))
            return "err no rectangle";
        return String.Format("ok {0} {1} {2} {3}", o.x, o.y, r.r, r.b);
    }

    static string Monitor()
    {
        POINT origin = new POINT();
        IntPtr m = IsWindow(hwnd) ? MonitorFromWindow(hwnd, 2)
                                  : MonitorFromPoint(origin, 1);
        MONITORINFOEX mi = new MONITORINFOEX();
        mi.cb = Marshal.SizeOf(typeof(MONITORINFOEX));
        if (!GetMonitorInfo(m, ref mi))
            return "err no monitor";
        string name = mi.dev.StartsWith("\\\\.\\") ? mi.dev.Substring(4)
                                                    : mi.dev;
        return String.Format("ok {0} {1} {2} {3} {4} {5} {6} {7} {8}",
            name, mi.mon.l, mi.mon.t, mi.mon.r - mi.mon.l,
            mi.mon.b - mi.mon.t, mi.work.l, mi.work.t,
            mi.work.r - mi.work.l, mi.work.b - mi.work.t);
    }

    /* Every monitor, six words each: the name as Monitor() gives it, 1 for
       the primary, and the rectangle. */
    static string Monitors()
    {
        StringBuilder all = new StringBuilder("ok");
        EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero,
            delegate(IntPtr m, IntPtr dc, IntPtr rect, IntPtr l) {
                MONITORINFOEX mi = new MONITORINFOEX();
                mi.cb = Marshal.SizeOf(typeof(MONITORINFOEX));
                if (!GetMonitorInfo(m, ref mi))
                    return true;
                string name = mi.dev.StartsWith("\\\\.\\")
                    ? mi.dev.Substring(4) : mi.dev;
                all.AppendFormat(" {0} {1} {2} {3} {4} {5}", name,
                    mi.flags & 1, mi.mon.l, mi.mon.t, mi.mon.r - mi.mon.l,
                    mi.mon.b - mi.mon.t);
                return true;
            }, IntPtr.Zero);
        return all.ToString();
    }

    // A layout's handle as eight hex digits. Windows sign-extends one into
    // a pointer and Hkl() does the same going back, so the two round-trip.
    static string Hex(IntPtr hkl)
    {
        return ((uint)(hkl.ToInt64() & 0xffffffff)).ToString("x8");
    }

    static IntPtr Hkl(string hex)
    {
        return new IntPtr(unchecked((int)UInt32.Parse(hex,
            System.Globalization.NumberStyles.HexNumber)));
    }

    // Load a layout by its registry name, without making it anybody's: the
    // handle, and whether the session had it already -- which is whether it
    // is this harness's to unload afterwards. Every layout Windows ships can
    // be loaded this way; none has to be in the user's list of languages.
    static string LayoutLoad(string klid)
    {
        IntPtr[] had = new IntPtr[64];
        int n = GetKeyboardLayoutList(had.Length, had);
        /* KLF_NOTELLSHELL: not the taskbar's business. */
        IntPtr hkl = LoadKeyboardLayoutW(klid, 0x0080);
        if (hkl == IntPtr.Zero)
            return "err no layout " + klid;
        int was = 0;
        for (int i = 0; i < n && i < had.Length; i++)
            if (had[i] == hkl)
                was = 1;
        return "ok " + Hex(hkl) + " " + was;
    }

    // ---- the laptop's half: input, a program, a picture ------------------

    static string B64(string s)
    {
        return Encoding.UTF8.GetString(Convert.FromBase64String(s));
    }

    // SendInput enters above the device drivers: Windows marks the event
    // injected and then delivers it as it would a real one, raw input
    // included. Refused when the input desktop is not this one -- the lock
    // screen, a consent prompt -- and by a window more privileged than this
    // process, which nothing this project builds is.
    static string Send(INPUT i)
    {
        INPUT[] one = new INPUT[] { i };
        if (SendInput(1, one, Marshal.SizeOf(typeof(INPUT))) != 1)
            return "err the input was refused: error " +
                   Marshal.GetLastWin32Error();
        sent = GetTickCount();
        return "ok";
    }

    // Whether somebody is at this machine, which a harness about to type
    // into its desktop wants to know first. Windows says when the last
    // input was and not whose, and the last input is usually an earlier
    // round's. So each agent leaves the time of its own last one in a file
    // as it goes, and input no later than that is nobody's hand.
    //
    // Milliseconds since a hand was last on it, or -1 for none since the
    // harness's own. A tick count starts again at a restart, when the file
    // is ahead of the clock and says nothing.
    static uint sent = 0;
    static string sentFile = null;

    static string Idle(string file)
    {
        sentFile = file;
        LASTINPUTINFO li = new LASTINPUTINFO();
        li.size = 8;
        if (!GetLastInputInfo(ref li))
            return "err no last input";
        uint now = GetTickCount(), ours = 0;
        try { ours = UInt32.Parse(File.ReadAllText(file).Trim()); }
        catch (Exception) { }
        if (ours != 0 && ours <= now && li.tick <= ours + 100)
            return "ok -1";
        return "ok " + (now - li.tick);
    }

    static void LeaveMark()
    {
        if (sentFile == null || sent == 0)
            return;
        try { File.WriteAllText(sentFile, sent.ToString()); }
        catch (Exception) { }
    }

    static string Mouse(int dx, int dy, uint data, uint flags)
    {
        INPUT i = new INPUT();
        i.type = 0;                                 /* INPUT_MOUSE */
        i.u.mi.dx = dx;
        i.u.mi.dy = dy;
        i.u.mi.data = data;
        i.u.mi.flags = flags;
        return Send(i);
    }

    // An absolute move ranges over 0..65535 across every monitor together,
    // whatever their sizes, and Windows divides it back down. The middle of
    // the pixel is asked for, so that the division lands on it and not on
    // its neighbour.
    static string Warp(int x, int y)
    {
        long vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
        long vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        if (vw <= 0 || vh <= 0)
            return "err no desktop to move over";
        /* MOVE | VIRTUALDESK | ABSOLUTE */
        return Mouse((int)(((x - vx) * 65536 + 32768) / vw),
                     (int)(((y - vy) * 65536 + 32768) / vh), 0,
                     0x0001 | 0x4000 | 0x8000);
    }

    // A key by its scancode, which is what the library reads off the
    // message. Pause is the exception: on the wire it is a six-byte
    // sequence no flag here spells, so it goes by virtual key, with the
    // scancode a keyboard's Pause arrives with beside it.
    static string Key(string[] a)
    {
        INPUT i = new INPUT();
        i.type = 1;                                 /* INPUT_KEYBOARD */
        i.u.ki.scan = UInt16.Parse(a[1]);
        i.u.ki.vk = UInt16.Parse(a[4]);
        uint fl = i.u.ki.vk == 0 ? 0x0008u : 0u;    /* KEYEVENTF_SCANCODE */
        if (a[2] == "1")
            fl |= 0x0001;                           /* EXTENDEDKEY */
        if (a[3] != "1")
            fl |= 0x0002;                           /* KEYUP */
        i.u.ki.flags = fl;
        string sent = Send(i);
        if (sent == "ok")
            Typematic(i);
        return sent;
    }

    // A held key repeats because its keyboard says it again, or the
    // keyboard's driver does: below SendInput either way, so a key this
    // presses and holds would be shown once and never again. The repeat is
    // therefore made here, as the driver makes it -- the press sent again,
    // after the delay and at the rate this user has set -- for the newest
    // key down and until that key comes up.
    static readonly object held = new object();
    static int holding = 0;
    static INPUT heldKey;
    static bool heldDown = false;
    // Every key down, and not only the newest: a chord's modifiers go down
    // first and are never the one that repeats, and LetGo() releases all.
    static readonly System.Collections.Generic.List<INPUT> keysDown =
        new System.Collections.Generic.List<INPUT>();

    static bool SameKey(INPUT a, INPUT b)
    {
        return a.u.ki.scan == b.u.ki.scan && a.u.ki.vk == b.u.ki.vk &&
               ((a.u.ki.flags ^ b.u.ki.flags) & 1) == 0;
    }

    static void Typematic(INPUT i)
    {
        bool down = (i.u.ki.flags & 0x0002) == 0;
        lock (held) {
            int at = -1;
            for (int k = 0; k < keysDown.Count && at < 0; k++)
                if (SameKey(keysDown[k], i))
                    at = k;
            if (down && at < 0)
                keysDown.Add(i);
            else if (!down && at >= 0)
                keysDown.RemoveAt(at);
            if (!down && !(heldDown && SameKey(heldKey, i)))
                return;                 // some other key's release
            int mine = ++holding;
            heldDown = down;
            if (!down)
                return;
            heldKey = i;
            uint d, r;
            /* SPI_GETKEYBOARDDELAY: 0..3, a quarter of a second each.
               SPI_GETKEYBOARDSPEED: 0..31, for 2.5 to 30 a second. */
            if (!SystemParametersInfoW(0x0016, 0, out d, 0))
                d = 1;
            if (!SystemParametersInfoW(0x000A, 0, out r, 0))
                r = 31;
            int wait = 250 * ((int)d + 1);
            int every = (int)(1000.0 / (2.5 + r * 27.5 / 31.0));
            Thread t = new Thread(delegate() {
                Thread.Sleep(wait);
                for (;;) {
                    lock (held) {
                        if (holding != mine)
                            return;
                        Send(i);
                    }
                    Thread.Sleep(every);
                }
            });
            t.IsBackground = true;
            t.Start();
        }
    }

    // What is still down when the harness goes is let go: nothing else
    // would ever send its release.
    static void LetGo()
    {
        lock (held) {
            holding++;
            heldDown = false;
            foreach (INPUT k in keysDown) {
                INPUT up = k;
                up.u.ki.flags |= 0x0002;
                Send(up);
            }
            keysDown.Clear();
        }
    }

    // The far end of the program's stderr is a relay beside the harness,
    // reached through the same forwarded loopback as the harness itself. A
    // forwarded port accepts whether or not anything is behind it, so the
    // relay says one byte first and a connection that does not is tried
    // again: the relay is started a moment before this is asked.
    static TcpClient Dial(int port)
    {
        for (int tries = 0; tries < 50; tries++) {
            TcpClient c = null;
            try {
                c = new TcpClient("127.0.0.1", port);
                c.NoDelay = true;
                c.ReceiveTimeout = 2000;
                if (c.GetStream().ReadByte() == 6)
                    return c;
            } catch (Exception) {
            }
            if (c != null)
                c.Close();
            Thread.Sleep(100);
        }
        throw new InvalidOperationException("no relay on port " + port);
    }

    // Bytes and not lines: what the program wrote is what arrives. The
    // exit code follows a byte no program here prints.
    static void Pump(Stream from, NetworkStream ns)
    {
        byte[] buf = new byte[4096];
        int n;
        while ((n = from.Read(buf, 0, buf.Length)) > 0)
            lock (ns)
                ns.Write(buf, 0, n);
    }

    static void Relay(Process p, TcpClient c, Thread other)
    {
        try {
            NetworkStream ns = c.GetStream();
            Pump(p.StandardError.BaseStream, ns);
            other.Join();
            p.WaitForExit();
            byte[] tail = Encoding.ASCII.GetBytes(
                "\x1e" + "exit " + p.ExitCode + "\n");
            ns.Write(tail, 0, tail.Length);
        } catch (Exception) {
            // The relay has gone, and with it anybody to run this for.
            try { p.Kill(); } catch (Exception) { }
        } finally {
            c.Close();
        }
    }

    // run <port> <exe> <command line or -> [<NAME=value> ...], all but the
    // port in base64. On this desktop, because this process is on it: a
    // program started from an ssh session has no desktop to open a window
    // on. `all` sends standard output down the same way, for a program
    // that is run to be read and not to be driven.
    static string Start(string[] a, bool all)
    {
        ProcessStartInfo si = new ProcessStartInfo(B64(a[2]));
        si.Arguments = a[3] == "-" ? "" : B64(a[3]);
        si.UseShellExecute = false;
        si.CreateNoWindow = true;
        si.RedirectStandardError = true;
        si.RedirectStandardOutput = true;
        si.WorkingDirectory = Path.GetDirectoryName(si.FileName);
        for (int i = 4; i < a.Length; i++) {
            string kv = B64(a[i]);
            int eq = kv.IndexOf('=');
            si.EnvironmentVariables[kv.Substring(0, eq)] =
                kv.Substring(eq + 1);
        }
        // Before the program, so that nothing it says is said to nobody.
        TcpClient c = Dial(Int32.Parse(a[1]));
        Process p;
        try {
            p = Process.Start(si);
        } catch (Exception) {
            c.Close();
            throw;
        }
        child = p;
        // Drained, to nowhere unless asked for: a pipe nobody reads fills,
        // and the program then blocks in a write with its message loop
        // stopped.
        Thread o = new Thread(delegate() {
            try {
                if (all)
                    Pump(p.StandardOutput.BaseStream, c.GetStream());
                else
                    p.StandardOutput.BaseStream.CopyTo(Stream.Null);
            } catch (Exception) { }
        });
        o.IsBackground = true;
        o.Start();
        Thread t = new Thread(delegate() { Relay(p, c, o); });
        t.IsBackground = true;
        t.Start();
        return "ok " + p.Id;
    }

    // spawn <exe> <command line or -> <log>, all in base64: on this desktop
    // as run is, and then left to itself -- its two streams to the log and
    // not to a relay, and not `child`, so that neither this process going
    // nor the link going stops it. For a program whose point is the time
    // the link is down. Through cmd.exe, which is what opens the log. It is
    // still in the scheduled task's job, so the next round's `schtasks
    // /end` ends it.
    static string Spawn(string[] a)
    {
        string exe = B64(a[1]);
        string args = a[2] == "-" ? "" : " " + B64(a[2]);
        ProcessStartInfo si = new ProcessStartInfo(
            Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe");
        si.Arguments = "/d /s /c \"\"" + exe + "\"" + args + " > \"" +
            B64(a[3]) + "\" 2>&1\"";
        si.UseShellExecute = false;
        si.CreateNoWindow = true;
        si.WorkingDirectory = Path.GetDirectoryName(exe);
        return "ok " + Process.Start(si).Id;
    }

    // Every monitor together, off the screen's own device context: what
    // the compositor put there, without the pointer. A BMP written by hand,
    // because the only encoder in the box is in an assembly this file does
    // not otherwise need; the harness makes a PNG of it.
    static string Shot(string path)
    {
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
        int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        byte[] px = new byte[4 * vw * vh];
        BITMAPINFOHEADER bi = new BITMAPINFOHEADER();
        bi.size = 40;
        bi.w = vw;
        bi.h = vh;                      /* positive: bottom row first */
        bi.planes = 1;
        bi.bits = 32;
        bi.bytes = (uint)px.Length;
        IntPtr screen = GetDC(IntPtr.Zero);
        IntPtr dc = CreateCompatibleDC(screen);
        IntPtr bmp = CreateCompatibleBitmap(screen, vw, vh);
        int got = 0;
        try {
            IntPtr was = SelectObject(dc, bmp);
            /* SRCCOPY | CAPTUREBLT, the second for layered windows. */
            bool ok = BitBlt(dc, 0, 0, vw, vh, screen, vx, vy,
                             0x00CC0020 | 0x40000000);
            // Out of the context first: GetDIBits refuses a bitmap that is
            // selected into one.
            SelectObject(dc, was);
            if (ok)
                got = GetDIBits(screen, bmp, 0, (uint)vh, px, ref bi, 0);
        } finally {
            DeleteObject(bmp);
            DeleteDC(dc);
            ReleaseDC(IntPtr.Zero, screen);
        }
        if (got != vh)
            return "err no picture of the screen";
        using (BinaryWriter w = new BinaryWriter(File.Create(path))) {
            w.Write((byte)'B');
            w.Write((byte)'M');
            w.Write((uint)(54 + px.Length));
            w.Write((uint)0);
            w.Write((uint)54);
            w.Write(bi.size);
            w.Write(bi.w);
            w.Write(bi.h);
            w.Write(bi.planes);
            w.Write(bi.bits);
            w.Write((uint)0);                       /* BI_RGB */
            w.Write(bi.bytes);
            w.Write(bi.xppm);
            w.Write(bi.yppm);
            w.Write(bi.used);
            w.Write(bi.important);
            w.Write(px);
        }
        return "ok " + vx + " " + vy + " " + vw + " " + vh;
    }

    // ---- the laptop's own input, held for a round ------------------------
    //
    // Low-level hooks on a thread of their own, whose message loop is what
    // Windows calls them from: every keyboard and mouse event not marked
    // injected -- the laptop's own keys, touchpad and mouse -- is dropped,
    // and what SendInput sends, marked so (LLKHF_INJECTED, LLMHF_INJECTED),
    // goes through. Not BlockInput, which wants an elevated process and
    // lets through only the blocking thread's own SendInput. Ctrl+Alt+
    // Escape gives the input back (held 2), and Windows removes the hooks
    // with the process, so an agent that ends lets go; Ctrl+Alt+Delete is
    // never a hook's to drop.

    delegate IntPtr HookProc(int code, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)]
    struct MSG
    {
        public IntPtr hwnd;
        public uint message;
        public IntPtr w, l;
        public uint time;
        public POINT pt;
    }
    [DllImport(U, SetLastError = true)]
    static extern IntPtr SetWindowsHookExW(int id, HookProc fn, IntPtr mod,
                                           uint thread);
    [DllImport(U)] static extern bool UnhookWindowsHookEx(IntPtr h);
    [DllImport(U)] static extern IntPtr CallNextHookEx(IntPtr h, int code,
                                                       IntPtr w, IntPtr l);
    [DllImport(U)] static extern int GetMessageW(out MSG m, IntPtr h,
                                                 uint lo, uint hi);
    [DllImport(U)] static extern bool PostThreadMessageW(uint t, uint msg,
                                                         IntPtr w, IntPtr l);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr GetModuleHandleW(string name);

    static HookProc keyHook = KeyHook, mouseHook = MouseHook;
    static Thread holder;
    static volatile uint holderId;
    static volatile int holdState;   // 0 not held, 1 held, 2 given back
    // Set by Release() before it posts the quit, and checked by the holder
    // once its hooks are in: a quit posted before the holder had a queue to
    // take it is lost, and the flag is what still stops that holder.
    static volatile bool holdCancel;
    static bool ctrlDown, altDown;

    static IntPtr KeyHook(int code, IntPtr w, IntPtr l)
    {
        if (code < 0 || holdState != 1)
            return CallNextHookEx(IntPtr.Zero, code, w, l);
        int vk = Marshal.ReadInt32(l);
        int flags = Marshal.ReadInt32(l, 8);
        if ((flags & 0x10) != 0)                    // LLKHF_INJECTED
            return CallNextHookEx(IntPtr.Zero, code, w, l);
        bool down = w == (IntPtr)0x100 || w == (IntPtr)0x104;
        if (vk == 0x11 || vk == 0xA2 || vk == 0xA3)
            ctrlDown = down;
        if (vk == 0x12 || vk == 0xA4 || vk == 0xA5)
            altDown = down;
        if (down && vk == 0x1B && ctrlDown && altDown) {
            holdState = 2;
            PostThreadMessageW(holderId, 0x12, IntPtr.Zero, IntPtr.Zero);
        }
        return (IntPtr)1;
    }

    static IntPtr MouseHook(int code, IntPtr w, IntPtr l)
    {
        if (code < 0 || holdState != 1 ||
            (Marshal.ReadInt32(l, 12) & 1) != 0)    // LLMHF_INJECTED
            return CallNextHookEx(IntPtr.Zero, code, w, l);
        return (IntPtr)1;
    }

    static void Holding()
    {
        holderId = GetCurrentThreadId();
        IntPtr mod = GetModuleHandleW(null);
        IntPtr k = SetWindowsHookExW(13, keyHook, mod, 0);   // WH_KEYBOARD_LL
        IntPtr m = SetWindowsHookExW(14, mouseHook, mod, 0); // WH_MOUSE_LL
        if (k != IntPtr.Zero && m != IntPtr.Zero && !holdCancel) {
            ctrlDown = altDown = false;
            holdState = 1;
            // Never held past input_hold.py's LIMIT, released or not.
            uint me = holderId;
            using (new System.Threading.Timer(
                       _ => PostThreadMessageW(me, 0x12, IntPtr.Zero,
                                               IntPtr.Zero),
                       null, 1800 * 1000, Timeout.Infinite)) {
                MSG msg;
                while (GetMessageW(out msg, IntPtr.Zero, 0, 0) > 0) { }
            }
        }
        if (k != IntPtr.Zero) UnhookWindowsHookEx(k);
        if (m != IntPtr.Zero) UnhookWindowsHookEx(m);
        if (holdState == 1)
            holdState = 0;
    }

    static string Hold()
    {
        // A holder still alive holds only while it says so: one given back
        // by hand, or cancelled, and not yet gone is no hold to report.
        if (holder != null && holder.IsAlive)
            return !holdCancel && holdState == 1
                ? "ok" : "err the last hold has not let go";
        holdState = 0;
        holdCancel = false;
        holderId = 0;       // never a stale holder's to be posted the quit
        holder = new Thread(Holding);
        holder.IsBackground = true;
        holder.Start();
        for (int i = 0; i < 100 && holdState == 0 && holder.IsAlive; i++)
            Thread.Sleep(20);
        if (holdState == 1)
            return "ok";
        // Not one that starts holding after the round was told it did not.
        Release();
        return "err the hooks were refused";
    }

    static string Release()
    {
        holdCancel = true;
        if (holder != null && holder.IsAlive) {
            if (holderId != 0)
                PostThreadMessageW(holderId, 0x12, IntPtr.Zero, IntPtr.Zero);
            holder.Join(2000);
        }
        // One still alive is kept, so that Hold() refuses rather than
        // start a second beside it.
        if (holder != null && holder.IsAlive)
            return "err the hold would not end";
        holder = null;
        return "ok";
    }

    static string Answer(string[] a)
    {
        switch (a[0]) {
        case "hold":
            return Hold();
        case "held":
            return "ok " + holdState;
        case "release":
            return Release();
        case "hello":
            return "ok dpi " + GetDpiForSystem();
        case "find":
            return Find(a[1], a[2]) ? "ok " + pid : "ok 0";
        case "kill": {
            int n = 0;
            for (int i = 1; i < a.Length; i++)
                foreach (Process p in Process.GetProcessesByName(a[i])) {
                    try { p.Kill(); p.WaitForExit(3000); n++; }
                    catch (Exception) { }
                }
            return "ok " + n;
        }
        case "ptr": {
            // Refused while the input desktop is another one -- a consent
            // prompt, the lock screen -- and a zeroed point taken for an
            // answer is a pointer sent to the top-left corner mid-drag.
            POINT p;
            if (!GetCursorPos(out p))
                return "err no pointer: the desktop is not the input one";
            return "ok " + p.x + " " + p.y;
        }
        case "mon":
            return Monitor();
        case "mons":
            return Monitors();
        case "desk":
            /* SM_XVIRTUALSCREEN and the three after it: every monitor
               together, which is what an absolute pointer ranges over. */
            return String.Format("ok {0} {1} {2} {3}", GetSystemMetrics(76),
                GetSystemMetrics(77), GetSystemMetrics(78),
                GetSystemMetrics(79));
        case "alive":
            // The process and not the window: a window goes a moment before
            // its process, and only the second is "exited".
            try {
                return Process.GetProcessById((int)pid).HasExited ? "ok 0"
                                                                  : "ok 1";
            } catch (ArgumentException) {
                return "ok 0";
            }
        case "clipget":
            return "ok " + Convert.ToBase64String(
                Encoding.UTF8.GetBytes(ClipGet()));
        case "clipput":
            ClipPut(Encoding.UTF8.GetString(Convert.FromBase64String(a[1])));
            return "ok";
        case "layoutload":
            return LayoutLoad(a[1]);
        case "layoutdrop":
            return UnloadKeyboardLayout(Hkl(a[1])) ? "ok 1" : "ok 0";
        case "warp":
            return Warp(Int32.Parse(a[1]), Int32.Parse(a[2]));
        case "nudge":
            // Relative, which is what raw input counts and so all a
            // captured pointer hears.
            return Mouse(Int32.Parse(a[1]), Int32.Parse(a[2]), 0, 0x0001);
        case "button": {
            /* LEFTDOWN, RIGHTDOWN, MIDDLEDOWN; each one's UP is twice it. */
            uint[] down = { 0x0002, 0x0008, 0x0020 };
            uint fl = down[Int32.Parse(a[1]) - 1];
            return Mouse(0, 0, 0, a[2] == "1" ? fl : fl * 2);
        }
        case "wheel":
            // A step towards the user is a positive one to the harness and
            // a negative WHEEL_DELTA to Windows.
            return Mouse(0, 0, unchecked((uint)(-120 * Int32.Parse(a[1]))),
                         0x0800);
        case "key":
            return Key(a);
        case "run":
            return Start(a, false);
        case "runall":
            return Start(a, true);
        case "spawn":
            return Spawn(a);
        case "idle":
            return Idle(B64(a[1]));
        case "shot":
            return Shot(B64(a[1]));
        }

        // Everything below is about the window found.
        if (!IsWindow(hwnd))
            return "err no window";
        switch (a[0]) {
        case "rect":
            return Rect();
        case "fg":
            return GetForegroundWindow() == hwnd ? "ok 1" : "ok 0";
        case "focus":
            return Foreground() ? "ok 1" : "ok 0";
        case "iconic":
            return IsIconic(hwnd) ? "ok 1" : "ok 0";
        case "moveby": {
            RECT r;
            GetWindowRect(hwnd, out r);
            /* NOSIZE | NOZORDER | NOACTIVATE */
            SetWindowPos(hwnd, IntPtr.Zero, r.l + Int32.Parse(a[1]),
                         r.t + Int32.Parse(a[2]), 0, 0, 0x0001 | 0x0004 |
                         0x0010);
            return "ok";
        }
        case "layout": {
            // The window's thread's, which is the one its keys are typed
            // through: a layout is per thread, and this process has its own.
            uint x;
            return "ok " + Hex(GetKeyboardLayout(
                GetWindowThreadProcessId(hwnd, out x)));
        }
        case "layoutask":
            // Asked of the window as the language bar asks it, and not set:
            // WM_INPUTLANGCHANGEREQUEST, which DefWindowProc answers by
            // activating the layout. ActivateKeyboardLayout from here would
            // change this thread's and leave the window's alone.
            return PostMessageW(hwnd, 0x0050, IntPtr.Zero, Hkl(a[1]))
                ? "ok 1" : "ok 0";
        case "module":
            // Whether the program has that library loaded, which is the
            // one way to know a layer asked for by the environment is
            // there: a loader that cannot load one says so to nobody.
            foreach (ProcessModule m in
                     Process.GetProcessById((int)pid).Modules)
                if (String.Compare(m.ModuleName, a[1], true) == 0)
                    return "ok 1";
            return "ok 0";
        case "close":
            // Asked to go as its own close button asks: the program then
            // leaves by its ordinary way out, and what it had buffered to
            // say is said, which a killed one takes with it.
            return PostMessageW(hwnd, 0x0010, IntPtr.Zero, IntPtr.Zero)
                ? "ok 1" : "ok 0";                  /* WM_CLOSE */
        case "cpu":
            return "ok " + Process.GetProcessById((int)pid)
                .TotalProcessorTime.TotalSeconds
                .ToString(System.Globalization.CultureInfo.InvariantCulture);
        }
        return "err what is " + a[0];
    }

    public static void Run(string addr, int port)
    {
        // Per-monitor, before the first question: an unaware process is
        // shown rectangles divided by the scale, and a pointer aimed with
        // those misses by the same factor.
        if (!SetProcessDpiAwarenessContext((IntPtr)(-4)))
            SetProcessDPIAware();
        using (TcpClient c = new TcpClient(addr, port)) {
            c.NoDelay = true;
            NetworkStream ns = c.GetStream();
            StreamReader rd = new StreamReader(ns, new UTF8Encoding(false));
            StreamWriter wr = new StreamWriter(ns, new UTF8Encoding(false));
            wr.NewLine = "\n";
            wr.AutoFlush = true;
            string line;
            try {
                while ((line = rd.ReadLine()) != null) {
                    string[] a = line.Split(' ');
                    if (a[0] == "quit")
                        break;
                    string reply;
                    try {
                        reply = Answer(a);
                    } catch (Exception e) {
                        reply = "err " + e.GetType().Name + ": " +
                            e.Message.Replace("\n", " ").Replace("\r", "");
                    }
                    wr.WriteLine(reply);
                }
            } finally {
                // A harness that went without saying so leaves no window
                // behind for the next round to find.
                LetGo();
                LeaveMark();
                if (child != null) {
                    try { child.Kill(); } catch (Exception) { }
                }
            }
        }
    }
}
'@

[FnlAgent]::Run($Addr, $Port)
