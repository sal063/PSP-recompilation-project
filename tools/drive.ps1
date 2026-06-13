# Debug driver: screenshot the ACX window and/or send keys to it.
# Usage: drive.ps1 -Shot out.png            (capture window)
#        drive.ps1 -Key RETURN|X|UP|...     (tap a key globally, 120 ms)
param([string]$Shot, [string]$Key)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
    [DllImport("user32.dll")] public static extern IntPtr FindWindowA(string c, string t);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);
}
"@
$h = [W]::FindWindowA("acx_recomp", $null)
if ($h -eq [IntPtr]::Zero) { $h = [W]::FindWindowA($null, "Ace Combat X: Skies of Deception") }
if ($h -eq [IntPtr]::Zero) { $h = [W]::FindWindowA($null, "Ace Combat X") }
if ($h -eq [IntPtr]::Zero) { Write-Error "window not found"; exit 1 }

if ($Key) {
    $vk = switch ($Key.ToUpper()) {
        "RETURN" { 0x0D } "X" { 0x58 } "Z" { 0x5A } "A" { 0x41 } "S" { 0x53 }
        "UP" { 0x26 } "DOWN" { 0x28 } "LEFT" { 0x25 } "RIGHT" { 0x27 }
        default { [int][char]$Key.ToUpper() }
    }
    [W]::SetForegroundWindow($h) | Out-Null
    [W]::keybd_event([byte]$vk, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [W]::keybd_event([byte]$vk, 0, 2, [UIntPtr]::Zero)  # KEYEVENTF_KEYUP
}

if ($Shot) {
    $r = New-Object W+RECT
    [W]::GetClientRect($h, [ref]$r) | Out-Null
    $p = New-Object W+POINT
    [W]::ClientToScreen($h, [ref]$p) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { Write-Error "empty client rect"; exit 1 }
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($p.X, $p.Y, 0, 0, $bmp.Size)
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "saved $Shot ($w x $ht)"
}
