param(
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$data = Join-Path $root "package\Data"
$dll  = Join-Path $data "SKSE\Plugins\Highrim.dll"
$esp  = Join-Path $data "Highrim.esp"
$dist = Join-Path $root "dist"
$zip  = Join-Path $dist ("Highrim-{0}.zip" -f $Version)

if (-not (Test-Path -LiteralPath $dll)) { throw "DLL not found: $dll. Build first (build_release.bat)." }
if (-not (Test-Path -LiteralPath $esp)) { throw "Plugin not found: $esp. Build first (build_release.bat)." }

# Regenerate the plugin and run its tests; a stale or broken plugin must not be packaged.
Push-Location $root
try {
    & python -I (Join-Path $root "tools\make_esp.py")
    if ($LASTEXITCODE -ne 0) { throw "tools\make_esp.py failed." }
    & python -I (Join-Path $root "tests\test_esp.py")
    if ($LASTEXITCODE -ne 0) { throw "tests\test_esp.py failed; not packaging." }
} finally {
    Pop-Location
}

# The DLL must be the current build (it contains the version marker).
$bytes = [System.IO.File]::ReadAllBytes($dll)
$ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
if (-not $ascii.Contains("Highrim $Version loading")) { throw "Version marker for $Version not found in DLL." }

New-Item -ItemType Directory -Path $dist -Force | Out-Null
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

# Build the zip with System.IO.Compression so entry names use forward slashes
# (Compress-Archive on Windows PowerShell 5.1 writes backslashes).
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($zip, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $prefix = (Resolve-Path -LiteralPath $data).Path.TrimEnd('\') + '\'
    foreach ($file in Get-ChildItem -LiteralPath $data -Recurse -File) {
        $name  = "Data/" + $file.FullName.Substring($prefix.Length).Replace('\', '/')
        $entry = $archive.CreateEntry($name, [System.IO.Compression.CompressionLevel]::Optimal)
        $in    = [System.IO.File]::OpenRead($file.FullName)
        $out   = $entry.Open()
        try { $in.CopyTo($out) } finally { $out.Dispose(); $in.Dispose() }
    }
} finally {
    $archive.Dispose()
}
Write-Host "Created $zip"
