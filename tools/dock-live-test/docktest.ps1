# Live check of the dock fork: drives real test windows with synthetic mouse and keyboard input.
param([string]$Exe = "$PSScriptRoot\..\..\x64\Debug\FanciestZones.exe", [switch]$NoInput,
    # 0: the primary monitor, 1: the other one
    [int]$Monitor = 0)

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [StructLayout(LayoutKind.Sequential)] public struct MI { public int cb; public RECT mon; public RECT work; public uint flags; }
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromPoint(POINT p, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr h, ref MI mi);
    delegate bool MonCb(IntPtr h, IntPtr dc, ref RECT r, IntPtr d);
    [DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonCb cb, IntPtr d);
    // Work areas of the monitors, the primary one first
    public static RECT[] WorkAreas() { var list = new System.Collections.Generic.List<RECT>(); EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr h, IntPtr dc, ref RECT r, IntPtr d) => { MI mi = new MI(); mi.cb = Marshal.SizeOf(typeof(MI)); GetMonitorInfo(h, ref mi); if ((mi.flags & 1) != 0) list.Insert(0, mi.work); else list.Add(mi.work); return true; }, IntPtr.Zero); return list.ToArray(); }
    public static RECT PrimaryWorkArea() { POINT p; p.X = 0; p.Y = 0; MI mi = new MI(); mi.cb = Marshal.SizeOf(typeof(MI)); GetMonitorInfo(MonitorFromPoint(p, 1), ref mi); return mi.work; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    // A background process can't put a window above the foreground one: an input event lifts that restriction
    public static void Raise(IntPtr h) { mouse_event(0x0001, 0, 0, 0, UIntPtr.Zero); SetForegroundWindow(h); SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002); }
    public static IntPtr RootAt(int x, int y) { POINT p; p.X = x; p.Y = y; return GetAncestor(WindowFromPoint(p), 2); }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
    public static void Move(int x, int y) {
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        int nx = (int)Math.Round((x - vx) * 65535.0 / (vw - 1)), ny = (int)Math.Round((y - vy) * 65535.0 / (vh - 1));
        mouse_event(0x0001 | 0x8000 | 0x4000, nx, ny, 0, UIntPtr.Zero);
    }
}
'@
# Physical pixels, like FancyZones
[void][W]::SetThreadDpiAwarenessContext([IntPtr](-4))

function Frame($h) { $r = New-Object W+RECT; [void][W]::DwmGetWindowAttribute($h, 9, [ref]$r, 16); "{0},{1},{2},{3}" -f $r.L, $r.T, $r.R, $r.B }
function FrameRect($h) { $r = New-Object W+RECT; [void][W]::DwmGetWindowAttribute($h, 9, [ref]$r, 16); $r }

# Input is only sent when the grab point is on the expected test window
function Drag($h, $x1, $y1, $x2, $y2, [switch]$Shift) {
    $at = [W]::RootAt($x1, $y1)
    if ($at -ne $h) {
        $owner = Get-Process | Where-Object { $_.MainWindowHandle -eq $at } | Select-Object -First 1
        throw "Grab point $x1,$y1 is not on the test window $h but on $at ($($owner.Name) '$($owner.MainWindowTitle)')"
    }
    [W]::Move($x1, $y1); Start-Sleep -Milliseconds 200
    $cur = New-Object W+POINT; [void][W]::GetCursorPos([ref]$cur)
    if ([math]::Abs($cur.X - $x1) -gt 4 -or [math]::Abs($cur.Y - $y1) -gt 4) { throw "Cursor is at $($cur.X),$($cur.Y) instead of $x1,${y1}: the mouse is in use, or the mapping is off" }
    if ($Shift) { [W]::keybd_event(0x10, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 100 }
    [W]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 200
    1..25 | ForEach-Object {
        [W]::Move([int]($x1 + ($x2 - $x1) * $_ / 25), [int]($y1 + ($y2 - $y1) * $_ / 25)); Start-Sleep -Milliseconds 20
    }
    Start-Sleep -Milliseconds 500
    [W]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 100
    if ($Shift) { [W]::keybd_event(0x10, 0, 2, [UIntPtr]::Zero) }
    Start-Sleep -Milliseconds 900
}

function RaiseAt($h, $x, $y) {
    foreach ($try in 1..5) { [W]::Raise($h); Start-Sleep -Milliseconds 300; if ([W]::RootAt($x, $y) -eq $h) { return } }
}

function DragWindow($h, $x2, $y2, [switch]$Shift) {
    $r = FrameRect $h
    RaiseAt $h ($r.L + 200) ($r.T + 15)
    Drag $h ($r.L + 200) ($r.T + 15) $x2 $y2 -Shift:$Shift
}

function Chord([byte[]]$keys) {
    foreach ($k in $keys) { [W]::keybd_event($k, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
    [array]::Reverse($keys)
    foreach ($k in $keys) { [W]::keybd_event($k, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
    Start-Sleep -Milliseconds 900
}

function NewWindow($title, $x, $y) {
    $p = Start-Process pwsh -WindowStyle Hidden -ArgumentList '-NoProfile', '-File', "`"$PSScriptRoot\testwindow.ps1`"", $title, $x, $y -PassThru
    $h = [IntPtr]::Zero
    foreach ($i in 1..50) { Start-Sleep -Milliseconds 200; $h = [W]::FindWindow([NullString]::Value, $title); if ($h -ne [IntPtr]::Zero -and [W]::IsWindowVisible($h)) { break } }
    [pscustomobject]@{ Process = $p; Handle = $h; Title = $title }
}

$results = @()
function Check($name, $actual, $expected) {
    $script:results += [pscustomobject]@{ Check = $name; Ok = ($actual -eq $expected); Actual = $actual; Expected = $expected }
}
# Frames are compared with a tolerance: windows that aren't per-monitor DPI aware keep their invisible border
# on screen when monitors have different scaling (stock FancyZones behavior), and sizes are rounded by scaling.
# Frames given as "l,t,r,b" strings. Windows scaled by Windows are sized in steps, so they can be off by a pixel or two.
function CheckSame($name, $actual, $expected, $tolerance = 3) {
    $x = $actual -split ','; $y = $expected -split ','
    $ok = $x.Count -eq $y.Count
    for ($i = 0; $ok -and $i -lt $x.Count; $i++) { $ok = [math]::Abs([int]$x[$i] - [int]$y[$i]) -le $tolerance }
    $script:results += [pscustomobject]@{ Check = $name; Ok = $ok; Actual = $actual; Expected = $expected }
}
function CheckFrame($name, $h, $l, $t, $r, $b) {
    $f = FrameRect $h
    $ok = ([math]::Abs($f.L - $l) -le 12) -and ([math]::Abs($f.T - $t) -le 12) -and ([math]::Abs($f.R - $r) -le 12) -and ([math]::Abs($f.B - $b) -le 12)
    $script:results += [pscustomobject]@{ Check = $name; Ok = $ok; Actual = (Frame $h); Expected = "$l,$t,$r,$b" }
}

$areas = [W]::WorkAreas()
if ($Monitor -ge $areas.Count) { "No monitor $Monitor"; return }
$wa = $areas[$Monitor]
$other = if ($areas.Count -gt 1) { $areas[1 - $Monitor] } else { $null }
$waL = $wa.L; $waT = $wa.T; $waR = $wa.R; $waB = $wa.B; $Wd = $waR - $waL; $Ht = $waB - $waT
$midX = $waL + [int]($Wd / 2); $midY = $waT + [int]($Ht / 2)
"monitor ${Monitor}: work area $waL,$waT,$waR,$waB"

$a = NewWindow 'DockTestA' ($waL + 700) ($waT + 300)
$b = NewWindow 'DockTestB' ($waL + 800) ($waT + 350)
$c = NewWindow 'DockTestC' ($waL + 900) ($waT + 400)
$c2 = $null
Start-Sleep -Milliseconds 500
# The windows are created by a process that isn't per-monitor DPI aware: put them where they are meant to be
$i = 0
foreach ($w in $a, $b, $c) { [void][W]::SetWindowPos($w.Handle, [IntPtr]::Zero, $waL + 700 + 100 * $i, $waT + 300 + 50 * $i, 900, 600, 0x0004 -bor 0x0010); $i++ }
Start-Sleep -Milliseconds 500
# Never send input unless every test window is really there
$missing = $a, $b, $c | Where-Object { $_.Handle -eq [IntPtr]::Zero -or ![W]::IsWindowVisible($_.Handle) -or (Frame $_.Handle) -eq '0,0,0,0' }
if ($missing -or $NoInput) {
    "windows: " + (($a, $b, $c | ForEach-Object { "$($_.Title)=$($_.Handle) [$(Frame $_.Handle)]" }) -join '; ')
    $a, $b, $c | ForEach-Object { if (!$_.Process.HasExited) { Stop-Process -Id $_.Process.Id -Force } }
    if ($missing) { "ABORT: test window missing" }
    return
}

$fz = Start-Process $Exe -PassThru
Start-Sleep -Seconds 4
if ($fz.HasExited) { "FancyZones exited at start, code $($fz.ExitCode)"; return }
try {
    # Gap between docked windows: 8
    DragWindow $a.Handle ($waL + 20) $midY -Shift
    CheckFrame 'A docked on left screen edge' $a.Handle $waL $waT ($midX - 4) $waB

    DragWindow $b.Handle ($waL + [int]($Wd * 0.75)) $midY -Shift
    CheckFrame 'B filled the empty right half' $b.Handle ($midX + 4) $waT $waR $waB
    CheckFrame 'A stayed put' $a.Handle $waL $waT ($midX - 4) $waB

    DragWindow $c.Handle ($waL + [int]($Wd * 0.75)) ($waB - 130) -Shift
    CheckFrame 'C split B at its bottom edge' $c.Handle ($midX + 4) ($midY + 4) $waR $waB
    CheckFrame 'B kept the top' $b.Handle ($midX + 4) $waT $waR ($midY - 4)

    [void][W]::ShowWindow($c.Handle, 6); Start-Sleep -Milliseconds 1200
    CheckFrame 'B took the space of minimized C' $b.Handle ($midX + 4) $waT $waR $waB
    [void][W]::ShowWindow($c.Handle, 9); Start-Sleep -Milliseconds 1500
    CheckFrame 'C restored to its place' $c.Handle ($midX + 4) ($midY + 4) $waR $waB
    CheckFrame 'B back to the top' $b.Handle ($midX + 4) $waT $waR ($midY - 4)

    # Resize A by its right border: the neighbors follow
    RaiseAt $a.Handle ((FrameRect $a.Handle).R + 2) ($waT + 300)
    $newRight = $waL + [int]($Wd * 0.4)
    Drag $a.Handle ((FrameRect $a.Handle).R + 2) ($waT + 300) $newRight ($waT + 300)
    $ra = FrameRect $a.Handle; $rb = FrameRect $b.Handle; $rc = FrameRect $c.Handle
    CheckSame 'Resizing A moved B and C (gap kept)' ("{0},{1}" -f ($rb.L - $ra.R), ($rc.L - $ra.R)) '8,8' 2
    Check 'A right edge follows the drag' ([math]::Abs($ra.R - $newRight) -lt 14) $true

    # Resize B by its bottom border: only the split between B and C moves
    RaiseAt $b.Handle ($rb.L + 300) ($rb.B + 2)
    $aBefore = Frame $a.Handle
    Drag $b.Handle ($rb.L + 300) ($rb.B + 2) ($rb.L + 300) ($waT + [int]($Ht * 0.3))
    CheckSame 'Resizing B bottom left A alone' (Frame $a.Handle) $aBefore
    CheckSame 'C follows B bottom (gap kept)' "$((FrameRect $c.Handle).T - (FrameRect $b.Handle).B)" '8' 2

    # Save layout 1, undock B by a drag without Shift, recall layout 1
    [W]::Move($midX, $midY); Start-Sleep -Milliseconds 200
    Chord @(0x5B, 0x11, 0x12, 0x10, 0x31)
    $bDocked = Frame $b.Handle
    DragWindow $b.Handle ($waL + [int]($Wd * 0.6)) ($waT + [int]($Ht * 0.4))
    Check 'C took the space of undocked B' ([math]::Abs((FrameRect $c.Handle).T - $waT) -le 12) $true
    [W]::Move($midX, $midY); Start-Sleep -Milliseconds 200
    Chord @(0x5B, 0x11, 0x12, 0x31)
    CheckSame 'Recall put B back' (Frame $b.Handle) $bDocked

    # The layout survives a restart of FancyZones
    Stop-Process -Id $fz.Id -Force; Start-Sleep -Milliseconds 800
    [void][W]::SetWindowPos($b.Handle, [IntPtr]::Zero, $waL + 900, $waT + 400, 900, 600, 0x0004 -bor 0x0010)
    $fz = Start-Process $Exe -PassThru; Start-Sleep -Seconds 4
    CheckSame 'Restart put B back' (Frame $b.Handle) $bDocked

    # A slot is kept for the app of a window that is gone, the next window of that app takes it
    $cDocked = Frame $c.Handle
    Stop-Process -Id $fz.Id -Force; Start-Sleep -Milliseconds 800
    Stop-Process -Id $c.Process.Id -Force; Start-Sleep -Milliseconds 800
    $fz = Start-Process $Exe -PassThru; Start-Sleep -Seconds 4
    CheckSame 'B keeps its place next to the kept slot' (Frame $b.Handle) $bDocked
    $c2 = NewWindow 'DockTestC2' ($waL + 900) ($waT + 400); Start-Sleep -Milliseconds 1500
    CheckSame 'New window of the app took the kept slot' (Frame $c2.Handle) $cDocked

    if ($other) {
        # A docked window moved to the other monitor without a drag leaves the layout instead of being pulled back
        [void][W]::SetWindowPos($c2.Handle, [IntPtr]::Zero, $other.L + 300, $other.T + 300, 900, 600, 0x0004 -bor 0x0010); Start-Sleep -Milliseconds 1200
        $moved = Frame $c2.Handle
        [void][W]::ShowWindow($a.Handle, 6); Start-Sleep -Milliseconds 1200
        [void][W]::ShowWindow($a.Handle, 9); Start-Sleep -Milliseconds 1800
        CheckSame 'Window moved to the other monitor was not pulled back' (Frame $c2.Handle) $moved
        Check 'B took the space it left' ([math]::Abs((FrameRect $b.Handle).B - $waB) -le 12) $true

        # A docked window dragged to the other monitor docks there, and leaves the layout of this one
        $otherMidX = $other.L + [int](($other.R - $other.L) / 2); $otherMidY = $other.T + [int](($other.B - $other.T) / 2)
        DragWindow $b.Handle ($other.L + 20) $otherMidY -Shift
        CheckFrame 'B docked on the left edge of the other monitor' $b.Handle $other.L $other.T ($otherMidX - 4) $other.B
        CheckFrame 'A took the whole monitor it was left alone on' $a.Handle $waL $waT $waR $waB

        # And back: docked on this monitor again, next to A
        DragWindow $b.Handle ($waR - 20) $midY -Shift
        CheckFrame 'B docked back on the right edge of this monitor' $b.Handle ($midX + 4) $waT $waR $waB
        CheckFrame 'A gave half back' $a.Handle $waL $waT ($midX - 4) $waB

        # Win+arrows: swap with the neighbor on that side, or go on to the monitor on that side
        $VkLeft = 0x25; $VkRight = 0x27
        $otherIsRight = $other.L -ge $waR
        [W]::Raise($b.Handle); Start-Sleep -Milliseconds 600
        if ([W]::GetForegroundWindow() -eq $b.Handle) {
            Chord @(0x5B, $VkLeft)
            CheckFrame 'Win+Left swapped B with A' $b.Handle $waL $waT ($midX - 4) $waB
            CheckFrame 'A is on the right now' $a.Handle ($midX + 4) $waT $waR $waB
            Chord @(0x5B, $VkRight)
            CheckFrame 'Win+Right swapped them back' $b.Handle ($midX + 4) $waT $waR $waB

            # Shift+Win+arrows: the focus goes to the docked window on that side, nothing moves
            Chord @(0x5B, 0x10, $VkLeft)
            Check 'Shift+Win+Left focused A' ([W]::GetForegroundWindow() -eq $a.Handle) $true
            Chord @(0x5B, 0x10, $VkRight)
            Check 'Shift+Win+Right focused B again' ([W]::GetForegroundWindow() -eq $b.Handle) $true
            Chord @(0x5B, 0x10, 0x26)
            Check 'Shift+Win+Up with nothing above kept the focus on B' ([W]::GetForegroundWindow() -eq $b.Handle) $true
            CheckFrame 'A did not move' $a.Handle $waL $waT ($midX - 4) $waB
            CheckFrame 'B did not move' $b.Handle ($midX + 4) $waT $waR $waB

            if ($otherIsRight) {
                Chord @(0x5B, $VkRight)
                CheckFrame 'Win+Right at the edge moved B to the monitor on the right' $b.Handle $other.L $other.T ($otherMidX - 4) $other.B
                CheckFrame 'A took the whole monitor' $a.Handle $waL $waT $waR $waB
                Chord @(0x5B, $VkLeft)
                CheckFrame 'Win+Left brought B back, on the right edge' $b.Handle ($midX + 4) $waT $waR $waB
            }
            else {
                Chord @(0x5B, $VkLeft); Chord @(0x5B, $VkLeft)
                CheckFrame 'Win+Left at the edge moved B to the monitor on the left' $b.Handle ($otherMidX + 4) $other.T $other.R $other.B
                CheckFrame 'A took the whole monitor' $a.Handle $waL $waT $waR $waB
                Chord @(0x5B, $VkRight)
                CheckFrame 'Win+Right brought B back, on the left edge' $b.Handle $waL $waT ($midX - 4) $waB
            }

            # A window that isn't docked gets docked on that side
            [W]::Raise($c2.Handle); Start-Sleep -Milliseconds 600
            if ([W]::GetForegroundWindow() -eq $c2.Handle) {
                Chord @(0x5B, $VkLeft)
                CheckFrame 'Win+Left docked a floating window on the left of its monitor' $c2.Handle $other.L $other.T ($otherMidX - 4) $other.B
                [void][W]::PostMessage($c2.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Milliseconds 1000
            }
        }
        else { "skipped the Win+arrow checks: could not focus the test window" }
    }
    else { "skipped the two-monitor checks: one monitor only" }

    # Closing a window on the layout gives its space to the others
    [void][W]::PostMessage($a.Handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Milliseconds 1500
    Check 'B took the space of closed A' ([math]::Abs((FrameRect $b.Handle).L - $waL) -le 12) $true
}
catch { "ABORTED: $_" }
finally {
    0x10, 0x11, 0x12, 0x5B | ForEach-Object { [W]::keybd_event($_, 0, 2, [UIntPtr]::Zero) }
    $a, $b, $c, $c2 | Where-Object { $_ } | ForEach-Object { if (!$_.Process.HasExited) { Stop-Process -Id $_.Process.Id -Force } }
    Start-Sleep -Milliseconds 800
    "FancyZones still running: $(!$fz.HasExited)"
    if (!$fz.HasExited) { Stop-Process -Id $fz.Id -Force }
}
$results | Format-Table -AutoSize | Out-String -Width 220
