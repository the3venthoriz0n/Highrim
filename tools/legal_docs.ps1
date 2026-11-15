# Adds the licence, credits and source pointer to a mod zip (dot-sourced by the packaging script).
#   Add-LegalDocs -Zip <zip> -Prefix "Data/Docs/Highrim" -Version <version>
# Entries: COPYING.txt, CREDITS.txt (from CREDITS.md), THIRD_PARTY_NOTICES.txt, SOURCE.txt. Entry names use
# forward slashes. Throws when a file is missing or COPYING is not the full GPL text.
function Add-LegalDocs {
    param(
        [Parameter(Mandatory)][string]$Zip,
        [Parameter(Mandatory)][string]$Prefix,
        [Parameter(Mandatory)][string]$Version
    )
    $root = Split-Path -Parent $PSScriptRoot
    $files = [ordered]@{
        "COPYING.txt"             = "COPYING"
        "CREDITS.txt"             = "CREDITS.md"
        "THIRD_PARTY_NOTICES.txt" = "THIRD_PARTY_NOTICES.txt"
    }
    $docs = [ordered]@{}
    foreach ($name in $files.Keys) {
        $path = Join-Path $root $files[$name]
        if (-not (Test-Path -LiteralPath $path)) { throw "Licence file missing: $path" }
        $docs[$name] = [System.IO.File]::ReadAllBytes($path)
    }
    if ($docs["COPYING.txt"].Length -lt 30000) { throw "COPYING is not the complete GPL-3.0 text." }
    $text = [System.IO.File]::ReadAllText((Join-Path $root "COPYING"))
    if (-not $text.Contains("GNU GENERAL PUBLIC LICENSE") -or -not $text.Contains("Version 3, 29 June 2007")) { throw "COPYING is not GPL version 3." }
    $source = @"
Highrim $Version is free software, licensed GPL-3.0-or-later (the full text is in COPYING.txt next to this file).

Source code: https://github.com/the3venthoriz0n/Highrim
Every GitHub release of this mod also has a source archive attached (Highrim_<version>_source.zip). It contains
the repository at the release commit plus the pinned sources of the git submodules (CommonLibSSE-NG).
The build also downloads the libraries listed in vcpkg.json (pinned by vcpkg-configuration.json) and the MinHook
v1.3.4 instruction decoder (CMake FetchContent); their licences are in THIRD_PARTY_NOTICES.txt.

Credits: CREDITS.txt. Third-party licences: THIRD_PARTY_NOTICES.txt.
"@
    $docs["SOURCE.txt"] = (New-Object System.Text.UTF8Encoding($false)).GetBytes($source.Replace("`r`n", "`n"))

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::Open($Zip, [System.IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($name in $docs.Keys) {
            $entryName = "$Prefix/$name"
            $old = $archive.GetEntry($entryName)
            if ($old) { $old.Delete() }
            $entry = $archive.CreateEntry($entryName, [System.IO.Compression.CompressionLevel]::Optimal)
            $out = $entry.Open()
            try { $out.Write($docs[$name], 0, $docs[$name].Length) } finally { $out.Dispose() }
        }
    } finally {
        $archive.Dispose()
    }
    Write-Host ("Added {0}/ (COPYING.txt, CREDITS.txt, THIRD_PARTY_NOTICES.txt, SOURCE.txt)" -f $Prefix)
}
