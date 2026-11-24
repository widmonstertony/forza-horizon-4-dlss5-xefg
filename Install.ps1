[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $GamePath,
    [Parameter(Mandatory)] [string] $RuntimeSource
)

$ErrorActionPreference = 'Stop'
$game = (Resolve-Path -LiteralPath $GamePath).Path
$runtime = (Resolve-Path -LiteralPath $RuntimeSource).Path
$manifest = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'runtime-manifest.json') | ConvertFrom-Json
$verifiedGame = 'B838A79A65C6D1E12F46DEABA3584E4C88EBCCDFDC4214B4535ECE4BA3C0ABBE'
if ((Get-FileHash -LiteralPath (Join-Path $game 'ForzaHorizon4.exe')).Hash -ne $verifiedGame) {
    throw 'This package targets the verified Steam build 1.478.564.0.'
}
if (Get-Process -Name ForzaHorizon4 -ErrorAction SilentlyContinue) {
    throw 'Close Forza Horizon 4 before installing.'
}

$copies = foreach ($entry in $manifest) {
    $source = Join-Path (Join-Path $PSScriptRoot 'payload') $entry.Path
    if ($entry.Path -in 'FH4Dlss5\nvngx_dlss.dll', 'FH4Dlss5\nvngx_dlssnr.dll') {
        $source = Join-Path $runtime ([System.IO.Path]::GetFileName($entry.Path))
    }
    if ((Get-FileHash -LiteralPath $source).Hash -ne $entry.SHA256) {
        throw "Required file does not match the verified build: $($entry.Path)"
    }
    $target = Join-Path $game $entry.Path
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHash -LiteralPath $target).Hash -ne $entry.SHA256) {
            throw "Existing file has different contents: $target. Back up and move that file before installing."
        }
    } else {
        [pscustomobject]@{ Source = $source; Target = $target; Hash = $entry.SHA256 }
    }
}

New-Item -ItemType Directory -Path (Join-Path $game 'FH4Dlss5') -Force | Out-Null
$created = [System.Collections.Generic.List[string]]::new()
try {
    foreach ($copy in $copies) {
        [System.IO.File]::Copy($copy.Source, $copy.Target, $false)
        $created.Add($copy.Target)
        if ((Get-FileHash -LiteralPath $copy.Target).Hash -ne $copy.Hash) {
            throw "Copied file failed verification: $($copy.Target)"
        }
    }
} catch {
    foreach ($file in $created) {
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
    }
    throw
}
Write-Output 'FH4 graphics add-on installed. Launch the Steam edition normally.'
