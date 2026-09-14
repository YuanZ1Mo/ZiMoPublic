# Merge drogon.lib parts back into drogon.lib.
# Usage : powershell -ExecutionPolicy Bypass -File .\drogon_lib_merge.ps1
# Input : drogon.lib.part01 / part02 / ...
# Output: drogon.lib (byte-identical to the pre-split original)
# NOTE  : ASCII-only script (PS 5.1 reads .ps1 as ANSI).

$ErrorActionPreference = 'Stop'

$parts = Get-ChildItem (Join-Path $PSScriptRoot 'drogon.lib.part*') -File |
    Sort-Object { [int]([regex]::Match($_.Name, 'part(\d+)').Groups[1].Value) }

if ($parts.Count -eq 0) {
    Write-Error 'no drogon.lib.part* files found'
}

$dst = Join-Path $PSScriptRoot 'drogon.lib'
$fsOut = [System.IO.File]::Create($dst)
$chunk = 1MB
$buf = New-Object byte[] $chunk
$total = 0

foreach ($p in $parts) {
    $fsIn = [System.IO.File]::OpenRead($p.FullName)
    while (($n = $fsIn.Read($buf, 0, $chunk)) -gt 0) {
        $fsOut.Write($buf, 0, $n)
        $total += $n
    }
    $fsIn.Close()
    Write-Host ("merged: {0}" -f $p.Name)
}

$fsOut.Close()

Write-Host ""
Write-Host ("done: {0} parts -> drogon.lib ({1:N2} MB)" -f $parts.Count, ($total / 1MB))

$expected = ($parts | Measure-Object Length -Sum).Sum
if ($total -eq $expected) {
    Write-Host 'verify: size OK'
}
else {
    Write-Warning ("verify FAILED: merged {0} != sum {1}" -f $total, $expected)
}
