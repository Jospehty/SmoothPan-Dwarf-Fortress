# Rebuild smoothpan and copy to the Steam Dwarf Fortress install.
param(
    [string]$Config = "Release",
    [string]$DfPath = "C:\Program Files (x86)\Steam\steamapps\common\Dwarf Fortress"
)

$ErrorActionPreference = "Stop"
$BuildRoot = "D:\dfhack\build\VC2022"
$BuiltDll = Join-Path $BuildRoot "plugins\smoothpan\$Config\smoothpan.plug.dll"
$TargetDll = Join-Path $DfPath "hack\plugins\smoothpan.plug.dll"

Write-Host "Building smoothpan ($Config)..."
cmake --build $BuildRoot --target smoothpan --config $Config

if (-not (Test-Path $BuiltDll)) {
    throw "Build output not found: $BuiltDll"
}

$version = "3.11.46"
$versionBytes = [System.Text.Encoding]::ASCII.GetBytes($version)
$dllBytes = [IO.File]::ReadAllBytes($BuiltDll)
$found = $false
for ($i = 0; $i -le $dllBytes.Length - $versionBytes.Length; $i++) {
    $match = $true
    for ($j = 0; $j -lt $versionBytes.Length; $j++) {
        if ($dllBytes[$i + $j] -ne $versionBytes[$j]) { $match = $false; break }
    }
    if ($match) { $found = $true; break }
}
if (-not $found) {
    throw "Built DLL missing version string $version - wrong binary?"
}

Write-Host "Copying to $TargetDll"
Copy-Item $BuiltDll $TargetDll -Force

Write-Host "Done. Restart DF or run: plugin unload smoothpan / plugin load smoothpan"
Write-Host "Enable message should say: SmoothPan $version enabled"
