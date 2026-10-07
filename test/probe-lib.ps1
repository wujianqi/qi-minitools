# probe-lib.ps1 - the plumbing every probe in this folder would otherwise
# re-declare for itself: one P/Invoke block, window lookup, message posting,
# and the pass/fail bookkeeping.
#
#   . (Join-Path $PSScriptRoot 'probe-lib.ps1')
#
# The existing probes each carry their own Add-Type block. That is deliberate
# for them - they are standalone files that can be run on their own and are
# already written - but a new probe should dot-source this instead of copying
# forty lines of interop, and a change to how a message is posted should be
# made once here rather than in four places that slowly disagree.
#
# ASCII only, and no BOM. Windows PowerShell 5.1 reads a .ps1 without a BOM as
# ANSI, and mojibake in a comment can swallow the next statement (see
# probe-focus.ps1). Write UTF-8 without a BOM or plain ASCII, nothing else.

if (-not ('ProbeLib.U' -as [type])) {
Add-Type -Namespace ProbeLib -Name U -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
    public int cbSize; public int flags;
    public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
    public RECT rcCaret;
}
[StructLayout(LayoutKind.Sequential)] public struct MINMAXINFO {
    public POINT ptReserved; public POINT ptMaxSize; public POINT ptMaxPosition;
    public POINT ptMinTrackSize; public POINT ptMaxTrackSize;
}
public delegate bool EnumProc(IntPtr h, IntPtr p);

[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr x);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetDlgCtrlID(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
[DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, System.Text.StringBuilder l);
[DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", ExactSpelling=true)] public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr res);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr h, IntPtr r, IntPtr rgn, uint flags);
[DllImport("user32.dll")] public static extern IntPtr GetCapture();
[DllImport("user32.dll")] public static extern IntPtr SetCapture(IntPtr h);
[DllImport("user32.dll")] public static extern bool ReleaseCapture();
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO gti);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Explicit)] public struct INPUTUNION {
    [FieldOffset(0)] public MOUSEINPUT mi;
    [FieldOffset(0)] public KEYBDINPUT ki; }
[StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
[DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int size);
[DllImport("user32.dll")] public static extern short GetKeyState(int v);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetUpdateRect(IntPtr h, out RECT r, bool erase);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
[DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr proc, uint flags);
[DllImport("kernel32.dll")] public static extern bool GetProcessHandleCount(IntPtr proc, out uint cnt);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
[DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
[DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
[DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
'@
}

# Every window process calls app_set_dpi_aware() at startup, and a probe that
# does not is reading a different coordinate space than the one it is talking
# about: an unaware process gets virtualised metrics back from
# GetSystemMetrics and virtualised rectangles from GetWindowRect, so the
# origin it reads is not the origin the app adds to its own mouse messages.
# That difference is not visible at 100% scaling and is 25% at 125%, which is
# how a probe aimed at a button that the app has drawn ends up clicking the
# gap beside it. Every existing probe that reads geometry calls this itself;
# doing it once here means a new probe cannot forget.
[void][ProbeLib.U]::SetProcessDPIAware()

# --- window constants ----------------------------------------------------
# Grouped here so a probe never retypes a magic number that the app already
# spells out in src. Anything the probe invents for itself is marked as such.
$script:WM_NULL            = 0x0000
$script:WM_CLOSE           = 0x0010
$script:WM_GETTEXT         = 0x000D
$script:WM_GETMINMAXINFO   = 0x0024
$script:WM_SETFOCUS        = 0x0007
$script:WM_KEYDOWN         = 0x0100
$script:WM_CHAR            = 0x0102
$script:WM_COMMAND         = 0x0111
$script:WM_TIMER           = 0x0113
$script:WM_HOTKEY          = 0x0312
$script:WM_MOUSEMOVE       = 0x0200
$script:WM_LBUTTONDOWN     = 0x0201
$script:WM_LBUTTONUP       = 0x0202
$script:WM_RBUTTONDOWN     = 0x0204
$script:WM_RBUTTONUP       = 0x0205
$script:WM_KEYUP           = 0x0101
$script:WM_MOUSELEAVE      = 0x02A3
$script:WM_MOUSEWHEEL      = 0x020A
$script:WM_CAPTURECHANGED  = 0x0215
$script:WM_CONTEXTMENU     = 0x007B
$script:WM_SIZE            = 0x0005
$script:WM_APP1            = 0x8001
$script:WM_APP2            = 0x8002
$script:MK_LBUTTON         = 0x0001
$script:MK_RBUTTON         = 0x0002
$script:KEYEVENTF_KEYUP    = 0x0002
$script:SMTO_ABORTIFHUNG   = 0x0002
$script:SMTO_BLOCK         = 0x0001
$script:RDW_INVALIDATE     = 0x0001
$script:RDW_UPDATENOW      = 0x0100
$script:RDW_ALLCHILDREN    = 0x0080
$script:RDW_ERASE          = 0x0004
$script:SW_HIDE            = 0
$script:SW_SHOWNORMAL      = 1
$script:SW_SHOWMINIMIZED   = 2
$script:SW_MAXIMIZE        = 3
$script:SW_SHOW            = 5
$script:SW_MINIMIZE        = 6
$script:SW_RESTORE         = 9
$script:GAUSAGE_HANDLES    = 0x00000001
$script:PROCESS_QUERY_INFO = 0x0400

# --- small accessors -----------------------------------------------------

function Get-ClassName([IntPtr]$h) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    $sb = New-Object System.Text.StringBuilder 256
    [void][ProbeLib.U]::GetClassNameW($h, $sb, 256)
    $sb.ToString()
}

# GetWindowTextW returns "" for another process's controls by design, so that
# callers cannot hang on a busy app. WM_GETTEXT is one of the few messages
# USER32 marshals across the process boundary, so a real control's contents
# have to be read that way. This is the one accessor that can block, which is
# why every caller in a probe only uses it on a window the app has already
# answered for.
function Get-WinText([IntPtr]$h, [int]$Max = 4096) {
    if ($h -eq [IntPtr]::Zero) { return '' }
    $sb = New-Object System.Text.StringBuilder $Max
    [void][ProbeLib.U]::SendMessageW($h, $script:WM_GETTEXT, [IntPtr]$Max, $sb)
    $sb.ToString()
}

function Get-WinRect([IntPtr]$h) {
    $r = New-Object ProbeLib.U+RECT
    [void][ProbeLib.U]::GetWindowRect($h, [ref]$r)
    $r
}

function Get-ClientSize([IntPtr]$h) {
    $r = New-Object ProbeLib.U+RECT
    [void][ProbeLib.U]::GetClientRect($h, [ref]$r)
    [pscustomobject]@{ W = $r.Right - $r.Left; H = $r.Bottom - $r.Top }
}

# Where the app's absolute coordinate space starts. shot reads this once, into
# a->vsx / a->vsy (src\shot\surface.c) and adds it to every mouse message as
# it unpacks it, so the rectangles it lays out and logs are in that space
# while the messages a probe posts are in client space. On a machine whose
# primary monitor is not at 0,0 the two are not the same point, and posting a
# logged rectangle back unconverted lands next to the button instead of on it.
function Get-VirtualOrigin() {
    [pscustomobject]@{
        X = [ProbeLib.U]::GetSystemMetrics(76)   # SM_XVIRTUALSCREEN
        Y = [ProbeLib.U]::GetSystemMetrics(77)   # SM_YVIRTUALSCREEN
        W = [ProbeLib.U]::GetSystemMetrics(78)   # SM_CXVIRTUALSCREEN
        H = [ProbeLib.U]::GetSystemMetrics(79)   # SM_CYVIRTUALSCREEN
    }
}

# --- finding windows -----------------------------------------------------

# One enumeration pass, returning every top level window of a process. The
# EnumProc callback cannot return a value, so the result is accumulated into a
# script-scope list - the same idiom every probe in this folder already uses.
function Get-AppWindows([int]$ProcId) {
    $script:foundWins = @()
    $cb = [ProbeLib.U+EnumProc] {
        param($h, $x)
        $p2 = 0
        [void][ProbeLib.U]::GetWindowThreadProcessId($h, [ref]$p2)
        if ($p2 -eq $ProcId) { $script:foundWins += [IntPtr]$h }
        return $true
    }
    [void][ProbeLib.U]::EnumWindows($cb, [IntPtr]::Zero)
    $script:foundWins
}

# Every descendant of a window, with the class name, control id and text
# already read. Probes address controls by id, and the ids live in the app's
# own #defines, so this is what lets a probe say "the value box" instead of
# "the third child".
function Get-ChildMap([IntPtr]$Parent) {
    $script:childMap = @()
    $cb = [ProbeLib.U+EnumProc] {
        param($h, $x)
        $script:childMap += [pscustomobject]@{
            H     = [IntPtr]$h
            Class = Get-ClassName $h
            Id    = [ProbeLib.U]::GetDlgCtrlID($h)
            Text  = Get-WinText $h 512
            Vis   = [ProbeLib.U]::IsWindowVisible($h)
        }
        return $true
    }
    [void][ProbeLib.U]::EnumChildWindows($Parent, $cb, [IntPtr]::Zero)
    $script:childMap
}

function Find-AppWindow([int]$ProcId, [string]$Class, [int]$TimeoutMs = 6000) {
    $end = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $end) {
        foreach ($h in @(Get-AppWindows $ProcId)) {
            if ((Get-ClassName $h) -eq $Class) { return $h }
        }
        Start-Sleep -Milliseconds 100
    }
    [IntPtr]::Zero
}

# Same as Find-AppWindow but scoped to a parent, for the overlay/prompt/about
# windows that are children of nothing and belong to no particular class list.
function Find-Child([IntPtr]$Parent, [string]$Class) {
    foreach ($c in @(Get-ChildMap $Parent)) {
        if ($c.Class -eq $Class) { return $c.H }
    }
    [IntPtr]::Zero
}

# --- talking to the app --------------------------------------------------

# LPARAM for a mouse message, the way the window proc unpacks it.
function Mouse-LParam([int]$X, [int]$Y) {
    [IntPtr]((($Y -band 0xFFFF) -shl 16) -bor ($X -band 0xFFFF))
}

function Send-Move([IntPtr]$h, [int]$X, [int]$Y, [int]$W = 0) {
    [void][ProbeLib.U]::PostMessageW($h, $script:WM_MOUSEMOVE, [IntPtr]$W, (Mouse-LParam $X $Y))
}

# Press, and only press. Split from the release so a probe can hold the button
# down across a window-state change - the interesting cases are all about what
# happens between the two halves.
function Send-Down([IntPtr]$h, [int]$X, [int]$Y, [switch]$Right) {
    if ($Right) {
        [void][ProbeLib.U]::PostMessageW($h, $script:WM_RBUTTONDOWN, [IntPtr]$script:MK_RBUTTON, (Mouse-LParam $X $Y))
    } else {
        [void][ProbeLib.U]::PostMessageW($h, $script:WM_LBUTTONDOWN, [IntPtr]$script:MK_LBUTTON, (Mouse-LParam $X $Y))
    }
}

function Send-Up([IntPtr]$h, [int]$X, [int]$Y, [switch]$Right) {
    if ($Right) {
        [void][ProbeLib.U]::PostMessageW($h, $script:WM_RBUTTONUP, [IntPtr]::Zero, (Mouse-LParam $X $Y))
    } else {
        [void][ProbeLib.U]::PostMessageW($h, $script:WM_LBUTTONUP, [IntPtr]::Zero, (Mouse-LParam $X $Y))
    }
}

# A whole click: down, let the app settle, up, settle. The pauses are the
# existing probes' too - a down/up pair with nothing between them can be
# coalesced by the app's own idle work and the button then never sees a press.
# Hands a window of another thread the keyboard focus, the way real input
# would. A posted click cannot do this: focus moves in the window manager when
# genuine input activates a control, and a posted message is delivered to the
# window it names without ever passing through that path - which is why a
# probe asserting "clicking X focuses X" from synthetic messages measures the
# message queue, not the app. Attaching to the target's input queue first (the
# same trick Activate-Window uses for the foreground) is what makes SetFocus
# across the thread boundary legal.
function Give-AppFocus([IntPtr]$Hwnd, [uint32]$AppTid) {
    if ($Hwnd -eq [IntPtr]::Zero -or -not $AppTid) { return $false }
    $me = [ProbeLib.U]::GetCurrentThreadId()
    if ($me -eq $AppTid) { [void][ProbeLib.U]::SetFocus($Hwnd); return $true }
    $attached = [ProbeLib.U]::AttachThreadInput($me, $AppTid, $true)
    try { [void][ProbeLib.U]::SetFocus($Hwnd) }
    finally { if ($attached) { [void][ProbeLib.U]::AttachThreadInput($me, $AppTid, $false) } }
    return $true
}

function Send-Click([IntPtr]$h, [int]$X, [int]$Y, [switch]$Right, [switch]$Double) {
    Send-Down $h $X $Y -Right:$Right
    Start-Sleep -Milliseconds 120
    Send-Up $h $X $Y -Right:$Right
    Start-Sleep -Milliseconds 120
    if ($Double) {
        Send-Down $h $X $Y -Right:$Right
        Start-Sleep -Milliseconds 60
        Send-Up $h $X $Y -Right:$Right
        Start-Sleep -Milliseconds 120
    }
}

# Wheel messages carry screen coordinates, not client ones, which is why this
# one does not use Mouse-LParam.
function Send-Wheel([IntPtr]$h, [int]$X, [int]$Y, [int]$Delta) {
    $p = New-Object ProbeLib.U+POINT
    $p.X = $X; $p.Y = $Y
    [void][ProbeLib.U]::ClientToScreen($h, [ref]$p)
    $w = [IntPtr](($Delta -shl 16) -band 0xFFFFFFFF)
    [void][ProbeLib.U]::PostMessageW($h, $script:WM_MOUSEWHEEL, $w, (Mouse-LParam $p.X $p.Y))
}

function Send-Key([IntPtr]$h, [int]$Vk, [int]$Char = 0) {
    [void][ProbeLib.U]::PostMessageW($h, $script:WM_KEYDOWN, [IntPtr]$Vk, [IntPtr]::Zero)
    if ($Char) {
        [void][ProbeLib.U]::PostMessageW($h, $script:WM_CHAR, [IntPtr]$Char, [IntPtr]::Zero)
    }
    [void][ProbeLib.U]::PostMessageW($h, 0x0101, [IntPtr]$Vk, [IntPtr]::Zero)
}

# A REAL keystroke, through the input queue, to whichever window owns the
# focus. This is the whole reason probe-focus.ps1 exists: PostMessage reaches
# a window regardless of focus, so it cannot answer "does Esc work for a user".
# Use it only where the app is supposed to own the keyboard.
function Send-RealKey([int]$Vk) {
    [ProbeLib.U]::keybd_event([byte]$Vk, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [ProbeLib.U]::keybd_event([byte]$Vk, 0, $script:KEYEVENTF_KEYUP, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 120
}

# A real chord, held the way a hand holds it.
#
# This is the only way to reach a handler that asks GetKeyState(VK_CONTROL)
# instead of reading the modifier byte out of lParam - shot's Ctrl+Z / Ctrl+Y
# undo and picker's Ctrl+Shift+C are both that shape, so a posted key message
# would be silently ignored and the shortcut would look untested while never
# running.
#
# The part that took three attempts to get right: the key and the modifiers must
# not be released in the same breath as they are pressed. SendInput injects its
# whole batch into the queue at once, and the queue is drained by the system
# faster than the app's own message pump gets scheduled. picker reads both
# modifiers inside WM_CHAR, and WM_CHAR is a second hop behind the key-down
# through TranslateMessage - so in a single batch the character arrives with the
# modifiers already back up, the test sees them released, and the shortcut
# quietly does nothing. Batching made it no better: it removed the gap between
# the calls, not the gap between the events.
#
# So it is two batches with real time between them: modifiers down and the key
# down, wait long enough for the app to be handed the character while the
# modifiers are still physically held, then everything up. That is the same
# sequence a person produces, and it is the one the handler is written against.
function Send-RealCombo([int]$Vk, [switch]$Ctrl, [switch]$Shift, [switch]$Alt) {
    $held = New-Object System.Collections.ArrayList
    if ($Ctrl)  { [void]$held.Add(0x11) }          # VK_CONTROL
    if ($Alt)   { [void]$held.Add(0x12) }          # VK_MENU
    if ($Shift) { [void]$held.Add(0x10) }          # VK_SHIFT

    $KEYEVENTF_KEYUP = 2
    $INPUT_KEYBOARD  = 1
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type][ProbeLib.U+INPUT])

    function New-KeyInput([int]$Vk, [uint32]$Flags) {
        # Build the union through locals. "$i.u.ki.wVk = x" looks like it
        # fills the nested struct, but structs are value types: the chain
        # dereferences copies, the write lands in a temporary, and the INPUT
        # reaches SendInput with every key field zero - "keys" with no key
        # code, which the system injects as vk=0 and no window ever sees as
        # a character. That is how Ctrl+Shift+C came to look broken in the
        # app while the probe was sending nothing at all.
        $ki = New-Object ProbeLib.U+KEYBDINPUT
        $ki.wVk = [uint16]$Vk
        $ki.dwFlags = $Flags
        $un = New-Object ProbeLib.U+INPUTUNION
        $un.ki = $ki
        $i = New-Object ProbeLib.U+INPUT
        $i.type = $INPUT_KEYBOARD
        $i.u = $un
        return $i
    }

    # Half one: modifiers down, then the key down. The key stays down.
    $down = New-Object System.Collections.ArrayList
    foreach ($m in $held) { [void]$down.Add((New-KeyInput $m 0)) }
    [void]$down.Add((New-KeyInput $Vk 0))
    # Half two: the key up, then the modifiers up in reverse order.
    $up = New-Object System.Collections.ArrayList
    [void]$up.Add((New-KeyInput $Vk $KEYEVENTF_KEYUP))
    for ($i = $held.Count - 1; $i -ge 0; $i--) { [void]$up.Add((New-KeyInput $held[$i] $KEYEVENTF_KEYUP)) }

    $aDown = $down.ToArray([type][ProbeLib.U+INPUT])
    $aUp   = $up.ToArray([type][ProbeLib.U+INPUT])
    $sent = [ProbeLib.U]::SendInput([uint32]$aDown.Length, $aDown, $size)
    if ($sent -ne $aDown.Length) {
        # The input queue is full or blocked. Fall back to the old per-key
        # call so the shortcut is still attempted, and let the caller see a
        # real result rather than a silent nothing.
        Send-RealKey $Vk
        return
    }

    # Long enough for the app to be woken, dequeue the key-down, run
    # TranslateMessage and reach the handler - with the modifiers still down.
    Start-Sleep -Milliseconds 220

    [void][ProbeLib.U]::SendInput([uint32]$aUp.Length, $aUp, $size)
    Start-Sleep -Milliseconds 350
}

# Is the app's thread answering, and how long did it take? The thread is
# single, so a slow round trip is a slow paint or a wedged message loop -
# either way it is the number a "feels frozen" report turns into.
function Wait-Idle([IntPtr]$h, [int]$Ms = 2000) {
    # SendMessageTimeoutW reports success through its own return value, not
    # through the out parameter: for WM_NULL the message result is 0 whether
    # the window answered or not, so a probe that reads lpdwResult sees "not
    # answered" on a perfectly healthy window. The declaration returns IntPtr
    # because the ABI is LRESULT-sized, so the test is "non-zero", not "true".
    $res = [IntPtr]::Zero
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $rv = [ProbeLib.U]::SendMessageTimeoutW($h, $script:WM_NULL, [IntPtr]::Zero, [IntPtr]::Zero,
              ($script:SMTO_ABORTIFHUNG -bor $script:SMTO_BLOCK), [uint32]$Ms, [ref]$res)
    $sw.Stop()
    [pscustomobject]@{ Ok = ($rv -ne [IntPtr]::Zero); Ms = $sw.Elapsed.TotalMilliseconds }
}

# Force the app down its normal repaint path. RDW_ALLCHILDREN matters: the
# parent-paint -> child-re-place path is where a flicker bug lives, and
# repainting the parent alone does not walk it.
function Pump-Repaint([IntPtr]$h, [int]$Times = 3, [int]$PauseMs = 150) {
    for ($k = 0; $k -lt $Times; $k++) {
        [void][ProbeLib.U]::RedrawWindow($h, [IntPtr]::Zero, [IntPtr]::Zero,
            ($script:RDW_INVALIDATE -bor $script:RDW_UPDATENOW -bor $script:RDW_ALLCHILDREN))
        Start-Sleep -Milliseconds $PauseMs
    }
}

function Focus-GuiInfo() {
    $gti = New-Object ProbeLib.U+GUITHREADINFO
    $gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($gti)
    $ok = [ProbeLib.U]::GetGUIThreadInfo(0, [ref]$gti)
    [pscustomobject]@{
        Ok       = $ok
        Fore     = [ProbeLib.U]::GetForegroundWindow()
        Active   = $gti.hwndActive
        Focus    = $gti.hwndFocus
        Capture  = $gti.hwndCapture
    }
}

# Put the keyboard in front of a window, the way a user clicking it would.
#
# SetForegroundWindow on its own is refused by the foreground lock for any
# process that is not already in front, and a probe is by definition not in
# front - it is a console started from a terminal. Tapping ALT first is the
# documented way round it: a process that has just been given input is allowed
# to take the foreground. Without this the probe ends up typing into whatever
# the shell had in front, and a shortcut that works perfectly looks dead.
#
# Returns what the foreground and the focus actually are afterwards, because
# "the app owns the keyboard" is the precondition for every real-key check and
# a probe that cannot establish it has to say so rather than quietly fail.
function Activate-Window([IntPtr]$h) {
    [ProbeLib.U]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)          # ALT down
    Start-Sleep -Milliseconds 60
    [ProbeLib.U]::keybd_event(0x12, 0, $script:KEYEVENTF_KEYUP, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [void][ProbeLib.U]::SetForegroundWindow($h)

    # If that was refused, borrow the foreground thread's input queue. A
    # process cannot steal the foreground, but a process that is temporarily
    # attached to the thread which owns it is treated as part of that thread
    # and may. The attach has to be undone again, or the two input queues stay
    # wedged together and keystrokes stop being delivered anywhere.
    $fg0 = [ProbeLib.U]::GetForegroundWindow()
    $me = [ProbeLib.U]::GetCurrentThreadId()
    $fgPid = 0
    $fgThread = [ProbeLib.U]::GetWindowThreadProcessId($fg0, [ref]$fgPid)
    $attached = $false
    if ($fgThread -ne 0 -and $fgThread -ne $me) {
        $attached = [ProbeLib.U]::AttachThreadInput($me, $fgThread, $true)
    }
    [void][ProbeLib.U]::BringWindowToTop($h)
    [void][ProbeLib.U]::SetForegroundWindow($h)
    if ($attached) { [void][ProbeLib.U]::AttachThreadInput($me, $fgThread, $false) }

    # Last resort, and the one that works when a system window is sitting in
    # front: SwitchToThisWindow is what Explorer itself uses to switch
    # windows, and unlike SetForegroundWindow it is not subject to the
    # foreground lock. It is undocumented, so the result is verified by
    # reading the foreground back rather than trusted.
    [ProbeLib.U]::SwitchToThisWindow($h, $true)
    Start-Sleep -Milliseconds 200
    if ([ProbeLib.U]::GetForegroundWindow() -ne $h) {
        [ProbeLib.U]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0003)   # HWND_TOPMOST, NOSIZE|NOMOVE
        Start-Sleep -Milliseconds 150
        [ProbeLib.U]::SwitchToThisWindow($h, $false)
        Start-Sleep -Milliseconds 150
        [ProbeLib.U]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x0003)  # HWND_NOTOPMOST
    }

    for ($k = 0; $k -lt 10; $k++) {
        Start-Sleep -Milliseconds 120
        $g = Focus-GuiInfo
        if ($g.Fore -eq $h -and $g.Focus -eq $h) { return $g }
        if ($k -eq 4) {
            $g2 = [ProbeLib.U]::GetForegroundWindow()
            $p2 = 0
            $t2 = [ProbeLib.U]::GetWindowThreadProcessId($g2, [ref]$p2)
            if ($t2 -ne 0 -and $t2 -ne $me) {
                [void][ProbeLib.U]::AttachThreadInput($me, $t2, $true)
                [void][ProbeLib.U]::SetForegroundWindow($h)
                [void][ProbeLib.U]::AttachThreadInput($me, $t2, $false)
            }
        }
    }
    Focus-GuiInfo
}

# Ask a window how small it is willing to be. Both envtool and slice answer
# WM_GETMINMAXINFO with the size below which their panel of controls no longer
# fits, and both do it because without it a user can drag the frame smaller
# than the content and leave the bottom controls outside the client area.
#
# MoveWindow cannot check this: it sets the size outright and the message is
# never sent. The message only arrives on a real drag, so the only way to
# ask the question from outside is to ask the window itself - which is
# exactly what a check here is worth. Returns $null if the window does not
# answer at all, which is the failure this is looking for.
function Get-MinTrackSize([IntPtr]$h) {
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type][ProbeLib.U+MINMAXINFO])
    $mem = [System.Runtime.InteropServices.Marshal]::AllocHGlobal($size)
    try {
        for ($i = 0; $i -lt $size; $i += 4) {
            [System.Runtime.InteropServices.Marshal]::WriteInt32($mem, $i, 0)
        }
        [void][ProbeLib.U]::SendMessageW($h, $script:WM_GETMINMAXINFO, [IntPtr]::Zero, $mem)
        $mm = [System.Runtime.InteropServices.Marshal]::PtrToStructure($mem, [type][ProbeLib.U+MINMAXINFO])
        if ($mm.ptMinTrackSize.X -le 0 -and $mm.ptMinTrackSize.Y -le 0) { return $null }
        [pscustomobject]@{ W = $mm.ptMinTrackSize.X; H = $mm.ptMinTrackSize.Y }
    } finally {
        [System.Runtime.InteropServices.Marshal]::FreeHGlobal($mem)
    }
}

# --- the app under test --------------------------------------------------

# Start a program with its own log files under build\. Every probe in this
# folder does this, and every one of them wanted to read the app's [dbg]
# trace afterwards, so it lives here.
function Start-ProbeApp([string]$Exe, [string[]]$AppArgs = @(), [string]$LogTag = 'probe') {
    if (-not (Test-Path $Exe)) { throw "$Exe not found (build it first)" }
    $Exe = (Resolve-Path $Exe).Path
    $logDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build'
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force -Path $logDir | Out-Null }
    $err = Join-Path $logDir "$LogTag-stderr.txt"
    $out = Join-Path $logDir "$LogTag-stdout.txt"
    # Start-Process rejects an empty -ArgumentList outright under 5.1, so the
    # switch is only passed when there is something to pass.
    if ($AppArgs -and $AppArgs.Count) {
        $p = Start-Process -FilePath $Exe -ArgumentList $AppArgs -PassThru `
                -RedirectStandardError $err -RedirectStandardOutput $out
    } else {
        $p = Start-Process -FilePath $Exe -PassThru `
                -RedirectStandardError $err -RedirectStandardOutput $out
    }
    # Start-Process -PassThru hands back a Process whose exit code is not
    # always retrievable afterwards: the underlying handle is not kept, and
    # once the process is gone ExitCode reads back as null instead of a number.
    # Touching .Handle once while the process is alive caches it, which is what
    # makes "did it exit cleanly" answerable at all.
    $null = $p.Handle
    [pscustomobject]@{ Proc = $p; Exe = $Exe; Err = $err; Out = $out }
}

function Stop-ProbeApp($App) {
    if ($App -and $App.Proc -and -not $App.Proc.HasExited) {
        Stop-Process -Id $App.Proc.Id -Force -ErrorAction SilentlyContinue
        # give the OS a moment to actually tear the process down, so a leak
        # count taken right after is not counting a half-dead process
        for ($k = 0; $k -lt 40 -and -not $App.Proc.HasExited; $k++) { Start-Sleep -Milliseconds 50 }
    }
}

# Read what an app has written to its log since the last call, and remember
# where to look next time. The apps' own state query (WM_APP + 2 in shot and
# envtool) answers by writing one line to stderr, so this is how a probe reads
# a value the app owns instead of guessing at it.
function Read-LogSince([string]$Path) {
    $script:logMark = 0
    if (-not $script:logSeen) { $script:logSeen = @{} }
    if ($script:logSeen.ContainsKey($Path)) { $script:logMark = $script:logSeen[$Path] }
    $script:logSeen[$Path] = 0
    if (-not (Test-Path $Path)) { return @() }
    $all = @(Get-Content $Path -ErrorAction SilentlyContinue)
    $script:logSeen[$Path] = $all.Count
    if ($all.Count -le $script:logMark) { return @() }
    $all[$script:logMark..($all.Count - 1)]
}

# The whole log, for state the app writes once at a change rather than on
# demand. shot dumps its toolbar geometry when it lays the row out, not when
# it is asked a question, so a probe that reads only new lines will find
# nothing unless it happened to be listening at that exact moment. Callers
# should take the last match, since an earlier dump describes an earlier
# layout.
function Read-LogAll([string]$Path) {
    if (-not (Test-Path $Path)) { return @() }
    @(Get-Content $Path -ErrorAction SilentlyContinue)
}

# --- crash log -----------------------------------------------------------
#
# All four programs install a vectored exception handler that appends to
# %LOCALAPPDATA%\MuskTools\crash.log (app_install_crash_log in
# src\common\winutil.c). A probe that ends with the app still alive proves
# little on its own: a handled access violation on a worker thread does not
# take the process down. The log is what turns "the window is still there"
# into "nothing went wrong", so every event probe in this folder brackets its
# run with one of these.

function Get-CrashLogPath() {
    Join-Path $env:LOCALAPPDATA 'MuskTools\crash.log'
}

function Get-CrashLogSize() {
    $p = Get-CrashLogPath
    if (Test-Path $p) { return (Get-Item $p).Length }
    0
}

# Take one of these before touching the app, and hand it to Get-CrashRecords
# afterwards. A byte count on its own is not enough: the log is shared by all
# four programs, so a slice.exe the test plan killed a moment earlier can still
# land a record in it - RPC teardown on a forced kill raises 0x6BA through the
# same vectored handler - and a plain size delta reports that as a shot crash.
# Filtering on the record header is what makes the answer about the program
# actually under test.
function Get-CrashMark() {
    Get-CrashLogSize
}

# Records appended since $Mark whose header names $Tag. The file is opened
# shared for read as well, which is what lets this read a log the app still
# holds open for append.
function Get-CrashRecords([int]$Mark, [string]$Tag) {
    $p = Get-CrashLogPath
    if (-not (Test-Path $p)) { return @() }
    $size = (Get-Item $p).Length
    if ($size -le $Mark) { return @() }
    $fs = [System.IO.File]::Open($p, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$fs.Seek($Mark, 'Begin')
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        $text = $sr.ReadToEnd()
        $sr.Close()
    } finally { $fs.Close() }
    if (-not $Tag) { return @($text -split "`r?`n" | Where-Object { $_ -match '^---- .* crash:' }) }
    @($text -split "`r?`n" | Where-Object { $_ -match ('^----\s+' + [regex]::Escape($Tag) + '\s+crash:') })
}

# --- resource counts -----------------------------------------------------
#
# Handle and GDI leaks do not fail a probe on their own; they show up twenty
# open/close cycles later as a program that cannot start. Counting them is
# cheap and turns "it closed" into "it closed and gave everything back".

function Get-ProcResources([int]$ProcId) {    $h = [ProbeLib.U]::OpenProcess($script:PROCESS_QUERY_INFO, $false, $ProcId)
    if ($h -eq [IntPtr]::Zero) { return $null }
    $handles = [uint32]0
    $ok = [ProbeLib.U]::GetProcessHandleCount($h, [ref]$handles)
    [void][ProbeLib.U]::CloseHandle($h)
    if (-not $ok) { return $null }
    [pscustomobject]@{
        Handles = $handles
        Gdi     = [ProbeLib.U]::GetGuiResources($h, $script:GAUSAGE_HANDLES)
    }
}

# The closing check every event probe makes. "The window was still there" is
# not the same claim as "nothing went wrong": a handled access violation on a
# worker thread leaves the window up and the process alive, and the only trace
# is a record in the shared log. The detail carries the header line of each new
# record, so a failure says which program and which exception code it was.
function Add-CrashCheck([int]$Mark, [string]$Tag, [string]$Label = '') {
    if (-not $Label) { $Label = "$Tag logged no crash" }
    $recs = @(Get-CrashRecords $Mark $Tag)
    Add-Check $Label ($recs.Count -eq 0) `
        $(if ($recs.Count) { ($recs | Select-Object -First 2) -join ' | ' } else { 'no new record' })
}

# --- check bookkeeping ---------------------------------------------------
#
# A probe that fails by reaching the end of the script has failed silently,
# which is the one outcome worse than a red result. Everything a probe
# asserts goes through Add-Check, and the run ends in Write-Checks, which
# prints the tally and sets the exit code. test.ps1 reads the exit code and
# also scans for FAIL, so both have to agree.

$script:checks = @()

function Add-Check([string]$Name, [bool]$Ok, [string]$Detail = '') {
    $script:checks += [pscustomobject]@{ Name = $Name; Ok = $Ok; Skipped = $false; Detail = $Detail }
    $mark = if ($Ok) { '  ok  ' } else { '  FAIL' }
    $tone = if ($Ok) { 'DarkGray' } else { 'Red' }
    $line = if ($Detail) { "$mark  $Name  ($Detail)" } else { "$mark  $Name" }
    Write-Host $line -ForegroundColor $tone
    # Nothing is emitted: a probe calls Add-Check for its effect, and a stray
    # True on the pipeline ends up in the harness's captured output where it
    # reads like an extra result line.
}

# A check that could not be made, with the reason. Reserved for the case where
# the test itself is blocked by the machine rather than by the program - a
# system window holding the foreground so no keystroke can be delivered to an
# ordinary window, say.
#
# It is deliberately not a pass. The reason has to be written down, because
# "the shortcut was not exercised" and "the shortcut works" are otherwise the
# same green tick, and only one of them is true.
function Add-Skip([string]$Name, [string]$Reason) {
    $script:checks += [pscustomobject]@{ Name = $Name; Ok = $true; Skipped = $true; Detail = $Reason }
    Write-Host ("  skip  {0}  ({1})" -f $Name, $Reason) -ForegroundColor Yellow
}

function Write-Checks([string]$Title) {
    $bad  = @($script:checks | Where-Object { -not $_.Ok })
    $skip = @($script:checks | Where-Object { $_.Skipped })
    $ran  = $script:checks.Count - $skip.Count
    Write-Host ""
    if ($bad.Count) {
        Write-Host ("RESULT: {0} - {1} of {2} checks failed" -f $Title, $bad.Count, $ran) -ForegroundColor Red
        foreach ($b in $bad) { Write-Host ("  FAILED: {0} {1}" -f $b.Name, $b.Detail) -ForegroundColor Red }
    } elseif ($skip.Count) {
        Write-Host ("RESULT: {0} - {1} checks passed, {2} skipped" -f $Title, $ran, $skip.Count) -ForegroundColor Yellow
        foreach ($s in $skip) { Write-Host ("  SKIPPED: {0} - {1}" -f $s.Name, $s.Detail) -ForegroundColor Yellow }
    } else {
        Write-Host ("RESULT: {0} - {1} checks passed" -f $Title, $ran) -ForegroundColor Green
    }
    exit $(if ($bad.Count) { 1 } else { 0 })
}
