param([string]$Title, [int]$X, [int]$Y)
Add-Type -AssemblyName System.Windows.Forms
Add-Type 'using System; using System.Runtime.InteropServices; public static class S { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd); }'
$f = New-Object Windows.Forms.Form
$f.Text = $Title
$f.TopMost = $true
$f.StartPosition = 'Manual'
$f.Location = New-Object Drawing.Point($X, $Y)
$f.Size = New-Object Drawing.Size(600, 400)
# The process is started hidden, which also hides the first show of the form
$f.Add_Shown({ [void][S]::ShowWindow($f.Handle, 1) })
[Windows.Forms.Application]::Run($f)
