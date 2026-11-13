# Drive a running Vita3K window for testing the port on the PC.
#   powershell -File tools/v3k.ps1 shot <out.png>             capture the Vita3K window
#   powershell -File tools/v3k.ps1 key <Key> [hold_ms]         press a key (Vita3K's keyboard map:
#        Enter START, X cross, C circle, Z square, V triangle, ShiftRight SELECT, arrows d-pad,
#        W/A/S/D left stick, Q/E L/R)
#   powershell -File tools/v3k.ps1 keys "<Key> <Key> ..." [gap_ms]
param([string]$Cmd, [string]$Arg, [int]$Ms = 120)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class V3K {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process Vita3K -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no Vita3K window"; exit 1 }
[V3K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 150
$vk = @{ Enter = 0x0D; ShiftRight = 0xA1; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27 }
function Press([string]$k, [int]$hold) {
    $code = if ($vk.ContainsKey($k)) { $vk[$k] } else { [int][char]$k.ToUpper() }
    $ext = if ($k -in 'Up', 'Down', 'Left', 'Right') { 1 } else { 0 }
    $scan = [byte][V3K]::MapVirtualKey($code, 0)
    [V3K]::keybd_event([byte]$code, $scan, $ext, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $hold
    [V3K]::keybd_event([byte]$code, $scan, ($ext -bor 2), [UIntPtr]::Zero)
}
switch ($Cmd) {
    'shot' {
        $r = New-Object V3K+RECT
        [V3K]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        $b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($b)
        $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
        $b.Save($Arg)
        Write-Output $Arg
    }
    'key' { Press $Arg $Ms }
    'keys' { foreach ($k in $Arg.Split(' ')) { Press $k 120; Start-Sleep -Milliseconds $Ms } }
}
