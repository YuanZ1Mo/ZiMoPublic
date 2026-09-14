# Split drogon.lib into <100MB parts (GitHub single-file 100MB limit).
# Usage : powershell -ExecutionPolicy Bypass -File .\drogon_lib_split.ps1
# Output: drogon.lib.part01 / part02 / part03 (90MB each)
# Merge : .\drogon_lib_merge.ps1
# NOTE  : ASCII-only script (PS 5.1 reads .ps1 as ANSI; Chinese comments corrupt parsing).

$ErrorActionPreference = 'Stop'

$src = Join-Path $PSScriptRoot 'drogon.lib'
$partSize = 90MB
$chunk = 1MB

if (-not (Test-Path $src)) {
    Write-Error "source not found: $src"
}

$total = (Get-Item $src).Length
$partNum = 0
$partWritten = 0

$fsIn = [System.IO.File]::OpenRead($src)
$buf = New-Object byte[] $chunk

while ($partWritten -lt $total) {
    $partNum++
    $partName = '{0}.part{1:D2}' -f $src, $partNum
    $fsOut = [System.IO.File]::Create($partName)
    $written = 0
    while ($written -lt $partSize -and $partWritten -lt $total) {
        $remain = $partSize - $written
        $toRead = [Math]::Min($chunk, [Math]::Min($remain, $total - $partWritten))
        $n = $fsIn.Read($buf, 0, $toRead)
        if ($n -eq 0) { break }
        $fsOut.Write($buf, 0, $n)
        $written += $n
        $partWritten += $n
    }
    $fsOut.Close()
    Write-Host ("{0}  {1:N2} MB" -f (Split-Path $partName -Leaf), ($written / 1MB))
}

$fsIn.Close()

Write-Host ""
Write-Host ("done: {0:N2} MB -> {1} parts ({2:N2} MB each)" -f ($total / 1MB), $partNum, ($partSize / 1MB))
Write-Host "merge with drogon_lib_merge.ps1"
