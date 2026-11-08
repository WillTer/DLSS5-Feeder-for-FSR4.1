param([string]$ProcName = 'SpellForce')
$p = Get-Process -Name $ProcName -ErrorAction Stop
"pid: $($p.Id)  bits: $(if ($p.ProcessorArchitecture -eq 'x86') {'32'} else {'64'})"
$p.Modules | Where-Object { $_.ModuleName -match '(?i)d3d|dxgi|ddraw|voodoo|reshade' } |
    ForEach-Object { "{0,-22} {1}" -f $_.ModuleName, $_.FileName }
