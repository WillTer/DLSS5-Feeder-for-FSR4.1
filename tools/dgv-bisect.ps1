param(
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][string]$GameDir
)
# Swap in one dgVoodoo2 version's x86 DirectX DLL set and test-launch the game.
$ErrorActionPreference = 'Stop'
$cache = "$PSScriptRoot\..\deploy\dgv-bisect"
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$verDir = Join-Path $cache $Version
if (-not (Test-Path "$verDir\MS\x86\D3D9.dll")) {
    $tag = if ($Version -eq '2.87.6') { $Version } else { "v$Version" }
    # tag naming: 2.87.6 has no 'v' prefix, the rest do; asset name underscores
    $asset = "dgVoodoo2_$($Version.Replace('.','_')).zip"
    $url = "https://github.com/dege-diosg/dgVoodoo2/releases/download/$tag/$asset"
    $zip = Join-Path $cache "$Version.zip"
    [Net.ServicePointManager]::SecurityProtocol = 'Tls12'
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $verDir -Force
    Remove-Item $zip
}
foreach ($dll in 'D3D9.dll','D3D8.dll','D3DImm.dll','DDraw.dll') {
    Copy-Item (Join-Path $verDir "MS\x86\$dll") (Join-Path $GameDir $dll) -Force
}
$p = Start-Process -FilePath (Join-Path $GameDir 'SpellForce.exe') -WorkingDirectory $GameDir -PassThru
if ($p.WaitForExit(25000)) { Write-Output ("$Version : EXITED code=" + $p.ExitCode) }
else {
    Write-Output "$Version : ALIVE"
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
