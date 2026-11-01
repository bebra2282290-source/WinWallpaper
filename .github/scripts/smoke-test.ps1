# Smoke test for WinWallpaper on a real Windows desktop (used by CI).
#
# 1. Generates a test video that alternates between magenta and cyan.
# 2. Starts WinWallpaper with it and checks that those colours appear on the
#    desktop (i.e. the video plays behind the icons).
# 3. Opens a maximized window and checks that the app stops using CPU.
# 4. Closes the window and checks that the video comes back.
# 5. Sends --exit and checks that the app quits and the video disappears.

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$OutDir = "smoke"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Exe = (Resolve-Path $Exe).Path
$OutDir = (Resolve-Path $OutDir).Path

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class Native {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr param);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hwnd, int index);

    public static string ClassOf(IntPtr hwnd) {
        var sb = new StringBuilder(256);
        GetClassName(hwnd, sb, sb.Capacity);
        return sb.ToString();
    }

    public static string Describe(IntPtr hwnd) {
        RECT r;
        GetWindowRect(hwnd, out r);
        return string.Format("0x{0:X} {1} visible={2} exstyle=0x{3:X8} rect=({4},{5})-({6},{7})",
            hwnd.ToInt64(), ClassOf(hwnd), IsWindowVisible(hwnd), GetWindowLong(hwnd, -20), r.Left, r.Top, r.Right, r.Bottom);
    }

    public static List<string> DesktopWindows() {
        var list = new List<string>();
        EnumWindows((h, p) => {
            string c = ClassOf(h);
            if (c == "Progman" || c == "WorkerW") list.Add(Describe(h));
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static List<string> Children(IntPtr parent) {
        var list = new List<string>();
        if (parent == IntPtr.Zero) return list;
        for (IntPtr c = GetWindow(parent, 5 /* GW_CHILD */); c != IntPtr.Zero; c = GetWindow(c, 2 /* GW_HWNDNEXT */)) {
            list.Add(Describe(c));
        }
        return list;
    }

    public static List<string> WallpaperWindows() {
        var list = new List<string>();
        EnumWindows((top, p) => {
            EnumChildWindows(top, (child, p2) => {
                if (ClassOf(child) == "WinWallpaper.Surface") {
                    list.Add(Describe(child) + " parent=" + ClassOf(GetParent(child)));
                }
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

[Native]::SetProcessDPIAware() | Out-Null
$failures = New-Object System.Collections.Generic.List[string]
$summary = New-Object System.Collections.Generic.List[string]

function Section([string]$title) {
    Write-Host ""
    Write-Host "=== $title ==="
}

function Get-ScreenColor([int]$x, [int]$y) {
    $bmp = New-Object System.Drawing.Bitmap 1, 1
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size 1, 1))
        return $bmp.GetPixel(0, 0)
    } finally {
        $g.Dispose()
        $bmp.Dispose()
    }
}

function Get-ColorName($c) {
    if ($c.R -gt 170 -and $c.G -lt 100 -and $c.B -gt 170) { return 'magenta' }
    if ($c.R -lt 100 -and $c.G -gt 170 -and $c.B -gt 170) { return 'cyan' }
    return "other"
}

function Save-Screenshot([string]$name) {
    try {
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Left, $b.Top, 0, 0, $bmp.Size)
        $bmp.Save((Join-Path $OutDir "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose()
        $bmp.Dispose()
    } catch {
        Write-Host "Screenshot failed: $_"
    }
}

# Samples a few desktop points for $seconds; returns how often each colour was seen.
function Watch-Desktop([int]$seconds) {
    $seen = @{ magenta = 0; cyan = 0; other = 0 }
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) {
        foreach ($p in $script:points) {
            try {
                $c = Get-ScreenColor $p[0] $p[1]
                $seen[(Get-ColorName $c)]++
            } catch {
                $seen['other']++
            }
        }
        Start-Sleep -Milliseconds 150
    }
    return $seen
}

function Format-Seen($seen) {
    return "magenta=$($seen.magenta) cyan=$($seen.cyan) other=$($seen.other)"
}

function Measure-CpuPercent([System.Diagnostics.Process]$p, [int]$seconds) {
    $p.Refresh()
    $t0 = $p.TotalProcessorTime
    Start-Sleep -Seconds $seconds
    $p.Refresh()
    $t1 = $p.TotalProcessorTime
    return [math]::Round(($t1 - $t0).TotalMilliseconds / ($seconds * 10.0) / [Environment]::ProcessorCount, 2)
}

Section "System"
$os = Get-CimInstance Win32_OperatingSystem
Write-Host "$($os.Caption), build $($os.BuildNumber)"
$summary.Add("OS: $($os.Caption), build $($os.BuildNumber)")
Write-Host ("mfplat.dll present: {0}; MFMediaEngine.dll present: {1}" -f `
    (Test-Path "$env:windir\System32\mfplat.dll"), (Test-Path "$env:windir\System32\MFMediaEngine.dll"))
Write-Host "Session of this script: $((Get-Process -Id $PID).SessionId)"
Get-Process explorer -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "explorer.exe pid=$($_.Id) session=$($_.SessionId)" }
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
Write-Host "Primary screen: $($screen.Width)x$($screen.Height)"
# Points away from the icon column on the left.
$script:points = @(
    @([int]($screen.Width * 0.75), [int]($screen.Height * 0.5)),
    @([int]($screen.Width * 0.5), [int]($screen.Height * 0.35)),
    @([int]($screen.Width * 0.6), [int]($screen.Height * 0.7))
)

Section "Test video"
# Native tools write progress to stderr; Windows PowerShell must not treat that as a failure.
$ErrorActionPreference = 'Continue'
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Host "Installing ffmpeg..."
    choco install ffmpeg -y --no-progress | Out-Null
}
$video = Join-Path $OutDir 'test.mp4'
& ffmpeg -hide_banner -loglevel error -y `
    -f lavfi -i "color=c=0xFF00FF:s=640x360:r=30:d=1" `
    -f lavfi -i "color=c=0x00FFFF:s=640x360:r=30:d=1" `
    -filter_complex "[0:v][1:v]concat=n=2:v=1:a=0[v]" -map "[v]" `
    -c:v libx264 -pix_fmt yuv420p -movflags +faststart $video
$ErrorActionPreference = 'Stop'
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $video)) { throw "ffmpeg could not create the test video" }
Write-Host "Created $video ($((Get-Item $video).Length) bytes)"

Section "Before start"
(New-Object -ComObject Shell.Application).MinimizeAll()
Start-Sleep -Seconds 2
$before = Watch-Desktop 2
Write-Host "Desktop colours before start: $(Format-Seen $before)"

Section "Start WinWallpaper"
# Portable mode: settings go next to the copied exe, not into %APPDATA%.
$appDir = Join-Path $OutDir 'app'
New-Item -ItemType Directory -Force $appDir | Out-Null
Copy-Item $Exe (Join-Path $appDir 'WinWallpaper.exe') -Force
New-Item -ItemType File -Force (Join-Path $appDir 'WinWallpaper.ini') | Out-Null
$proc = Start-Process (Join-Path $appDir 'WinWallpaper.exe') -ArgumentList "`"$video`"" -PassThru
Start-Sleep -Seconds 8
if ($proc.HasExited) {
    $failures.Add("WinWallpaper exited right after start (exit code $($proc.ExitCode))")
}

Section "Window tree"
foreach ($line in [Native]::DesktopWindows()) { Write-Host "top-level: $line" }
foreach ($line in [Native]::Children([Native]::FindWindow('Progman', $null))) { Write-Host "Progman child: $line" }
$surfaces = [Native]::WallpaperWindows()
foreach ($line in $surfaces) { Write-Host "wallpaper window: $line" }
$summary.Add("Wallpaper windows: $($surfaces.Count)")
if ($surfaces.Count -eq 0) { $failures.Add("No wallpaper window was attached to the desktop") }

Section "Playing"
Save-Screenshot 'playing'
$playing = Watch-Desktop 4
Write-Host "Desktop colours while playing: $(Format-Seen $playing)"
$summary.Add("Playing: $(Format-Seen $playing)")
if ($playing.magenta -eq 0 -and $playing.cyan -eq 0) { $failures.Add("The video is not visible on the desktop") }
if ($playing.magenta -eq 0 -or $playing.cyan -eq 0) { Write-Host "Only one of the two colours was seen: the video may not be advancing" }

$cpuPlaying = -1
if (-not $proc.HasExited) {
    $cpuPlaying = Measure-CpuPercent $proc 5
    $proc.Refresh()
    $mem = [math]::Round($proc.WorkingSet64 / 1MB, 1)
    Write-Host "CPU while playing: $cpuPlaying% of all cores, working set: $mem MB (CI has no GPU: decoding and drawing run in software)"
    $summary.Add("CPU while playing: $cpuPlaying% (software rendering), working set $mem MB")
}

Section "Covered by a maximized window"
$cover = Start-Process notepad.exe -WindowStyle Maximized -PassThru
Start-Sleep -Seconds 3
Save-Screenshot 'covered'
if (-not $proc.HasExited) {
    $cpuCovered = Measure-CpuPercent $proc 5
    Write-Host "CPU while covered: $cpuCovered%"
    $summary.Add("CPU while covered: $cpuCovered%")
    if ($cpuPlaying -gt 2 -and $cpuCovered -gt [math]::Max(0.5, $cpuPlaying / 4)) {
        $failures.Add("The app kept working while the desktop was covered ($cpuCovered% vs $cpuPlaying%)")
    }
}
Stop-Process -Id $cover.Id -Force -ErrorAction SilentlyContinue
Get-Process notepad -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

Section "Uncovered again"
Start-Sleep -Seconds 3
$resumed = Watch-Desktop 4
Write-Host "Desktop colours after the window closed: $(Format-Seen $resumed)"
$summary.Add("Resumed: $(Format-Seen $resumed)")
if ($resumed.magenta -eq 0 -and $resumed.cyan -eq 0) { $failures.Add("The video did not come back after the window closed") }

Section "Exit"
Start-Process (Join-Path $appDir 'WinWallpaper.exe') -ArgumentList '--exit' -Wait
if (-not $proc.WaitForExit(10000)) {
    $failures.Add("WinWallpaper did not exit after --exit")
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2
Save-Screenshot 'after-exit'
$after = Watch-Desktop 2
Write-Host "Desktop colours after exit: $(Format-Seen $after)"
$summary.Add("After exit: $(Format-Seen $after)")
if ($after.magenta + $after.cyan -gt 0) { $failures.Add("The video frame stayed on the desktop after exit") }
Write-Host "Settings file:"
Get-Content (Join-Path $appDir 'WinWallpaper.ini') -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "  $_" }

Section "Result"
if ($env:GITHUB_STEP_SUMMARY) {
    $lines = @("### Smoke test: $($os.Caption)", "") + ($summary | ForEach-Object { "- $_" })
    if ($failures.Count -gt 0) { $lines += @("", "**Failures:**") + ($failures | ForEach-Object { "- $_" }) }
    $lines | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append -Encoding utf8
}
if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host "All checks passed"
exit 0
