param([string]$OutDir = "$env:TEMP\sf-snaps")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
$g.Dispose()
$name = Join-Path $OutDir ("snap-" + (Get-Date -Format 'HHmmss-fff') + ".png")
$bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output $name
