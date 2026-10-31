# src_drive.ps1 - 驱动「书源搜索」对话框做端到端验证（开发期工具）
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\src_drive.ps1 -Step open
#   ... -Step search -Kw 斗破苍穹
#   ... -Step count
#   ... -Step download -WaitSec 40
# 说明：全部走 < 0x400 的窗口消息（WM_COMMAND / LB_* / WM_SETTEXT 带编组），
#       不做跨进程指针读取，避免把探测脚本自己搞崩。
param(
  [string]$Step = "open",
  [string]$Kw = "斗破苍穹",
  [int]$WaitSec = 30,
  [int]$Index = 0
)

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class SD {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
}
"@

function Find-ByClass([string]$cls) {
  $script:found = [IntPtr]::Zero
  $script:foundHidden = [IntPtr]::Zero
  $script:wantCls = $cls
  $cb = [SD+EnumProc]{
    param($h, $p)
    $sb = New-Object System.Text.StringBuilder 256
    [void][SD]::GetClassNameW($h, $sb, 256)
    if ($sb.ToString() -eq $script:wantCls) {
      if ([SD]::IsWindowVisible($h)) { $script:found = $h; return $false }
      if ($script:foundHidden -eq [IntPtr]::Zero) { $script:foundHidden = $h }
    }
    return $true
  }
  [void][SD]::EnumWindows($cb, [IntPtr]::Zero)
  if ($script:found -ne [IntPtr]::Zero) { return $script:found }
  # 本环境会时不时隐藏窗口，退回到"存在即用"，并顺手显示出来
  if ($script:foundHidden -ne [IntPtr]::Zero) { [void][SD]::ShowWindow($script:foundHidden, 5) }
  return $script:foundHidden
}

$main = Find-ByClass "NRMain"
if ($main -eq [IntPtr]::Zero) { Write-Output "NO NRMain"; exit 1 }

$dlg = Find-ByClass "NRSourceBox"

switch ($Step) {
  "open" {
    [void][SD]::PostMessageW($main, 0x0111, [IntPtr]1293, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    $dlg = Find-ByClass "NRSourceBox"
    Write-Output ("dialog=" + $dlg)
  }
  "search" {
    if ($dlg -eq [IntPtr]::Zero) { Write-Output "NO DIALOG"; exit 1 }
    $ed = [SD]::GetDlgItem($dlg, 101)
    [void][SD]::SendMessageText($ed, 0x000C, [IntPtr]::Zero, $Kw)
    Write-Output ("edit=" + $ed + " 文字已写入")
    [void][SD]::PostMessageW($dlg, 0x0111, [IntPtr]103, [IntPtr]::Zero)
    Start-Sleep -Seconds 6
    $lb = [SD]::GetDlgItem($dlg, 102)
    $n = [SD]::SendMessageW($lb, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Output ("结果条数=" + $n)
  }
  "count" {
    if ($dlg -eq [IntPtr]::Zero) { Write-Output "NO DIALOG"; exit 1 }
    $lb = [SD]::GetDlgItem($dlg, 102)
    $n = [SD]::SendMessageW($lb, 0x018B, [IntPtr]::Zero, [IntPtr]::Zero)
    Write-Output ("结果条数=" + $n)
  }
  "download" {
    if ($dlg -eq [IntPtr]::Zero) { Write-Output "NO DIALOG"; exit 1 }
    $lb = [SD]::GetDlgItem($dlg, 102)
    [void][SD]::SendMessageW($lb, 0x0186, [IntPtr]$Index, [IntPtr]::Zero)   # LB_SETCURSEL
    [void][SD]::PostMessageW($dlg, 0x0111, [IntPtr]104, [IntPtr]::Zero)
    Write-Output ("开始下载，等 " + $WaitSec + " 秒…")
    Start-Sleep -Seconds $WaitSec
    # 发一个真实的 ESC（GetAsyncKeyState 只认底层键状态，PostMessage 不行）
    [SD]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [SD]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
    Write-Output "已发 ESC"
    Start-Sleep -Seconds 2
    Write-Output ("对话框还在吗: " + [SD]::IsWindow($dlg))
  }
  "shot" {
    Write-Output "用 nr.ps1 -Cmd shot 截图"
  }
}
