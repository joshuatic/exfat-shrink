param(
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$python = (Get-Command $Python -ErrorAction Stop).Source
$status = Join-Path $PSScriptRoot 'live-test-status.txt'
$vhd = Join-Path $PSScriptRoot ('live-gpt-test-' + [guid]::NewGuid().ToString('N') + '.vhd')
$mounted = $false
$target = 25165824
$sourceSize = 33554432
$seed = Join-Path $PSScriptRoot ('live-seed-' + [guid]::NewGuid().ToString('N') + '.img')
$rows = @()

function Check-OriginalFiles([string]$Root) {

    foreach ($row in $rows) {
        $path = Join-Path $Root $row.Path
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $row.Hash) {
            throw "Original hash mismatch: $($row.Path)"
        }
    }
    return $rows.Count
}

try {
    'Creating disposable GPT VHD...' | Set-Content -LiteralPath $status
    & $python -c "from test_images import fixture; from pathlib import Path; p=Path(r'$seed'); f=p.open('xb'); f.write(fixture()); f.seek(33554431); f.write(bytes(1)); f.close()"
    if ($LASTEXITCODE -ne 0) { throw 'Seed creation failed.' }
    $ErrorActionPreference = 'Continue'
    & $python (Join-Path $PSScriptRoot 'make_test_vhd.py') $seed $vhd --gpt 2>&1 |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-test-create.log')
    $createExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($createExit -ne 0) { throw 'Test VHD creation failed.' }

    Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
    $mounted = $true
    $disk = Get-DiskImage -ImagePath $vhd | Get-Disk
    if ($disk.PartitionStyle -ne 'GPT' -or $disk.IsBoot -or $disk.IsSystem) {
        throw 'Unexpected disposable disk identity.'
    }
    $partitions = @($disk | Get-Partition)
    $selected = $partitions | Where-Object { $_.Size -eq $sourceSize }
    $neighbor = $partitions | Where-Object { $_.PartitionNumber -ne $selected.PartitionNumber }
    if (-not $selected -or -not $neighbor) { throw 'Missing selected/sentinel partition.' }
    $selected | Format-Volume -FileSystem exFAT -NewFileSystemLabel 'EXFAT_TEST' -Confirm:$false | Out-Null
    $selected | Add-PartitionAccessPath -AssignDriveLetter
    $volume = $selected | Get-Volume
    if ($volume.FileSystem -ne 'exFAT' -or -not $volume.DriveLetter) {
        throw 'Unexpected test volume.'
    }
    $letter = [string]$volume.DriveLetter
    $root = $letter + ':\'
    New-Item -ItemType Directory -Path (Join-Path $root 'nested') | Out-Null
    $rng = New-Object Random 42017
    $sha = [Security.Cryptography.SHA256]::Create()
    foreach ($length in @(0, 1, 511, 512, 513, 4095, 4096, 4097, 1048576, 3145728)) {
        $bytes = New-Object byte[] $length
        $rng.NextBytes($bytes)
        $relative = 'nested\file-' + $length + '.bin'
        $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
        [IO.File]::WriteAllBytes((Join-Path $root $relative), $bytes)
        $rows += [pscustomobject]@{ Path = $relative; Hash = $hash }
    }
    $sha.Dispose()

    $neighborSize = $neighbor.Size
    $neighborOffset = $neighbor.Offset
    $neighborGuid = $neighbor.Guid

    foreach ($phase in @('after-dirty', 'after-filesystem', 'after-partition', 'commit')) {
        "Testing live partition phase: $phase" | Set-Content -LiteralPath $status
        $recovery = Join-Path $PSScriptRoot ('live-recovery-' + $phase + '-' + [guid]::NewGuid().ToString('N'))
        $log = Join-Path $PSScriptRoot ('live-test-' + $phase + '.log')
        $program = Join-Path $PSScriptRoot 'build\exfat_fault_test.exe'
        if ($phase -eq 'commit') {
            $program = Join-Path $PSScriptRoot 'build\exfat_shrink.exe'
            Remove-Item Env:\EXFAT_TEST_FAULT -ErrorAction SilentlyContinue
        } else {
            $env:EXFAT_TEST_FAULT = $phase
        }
        $ErrorActionPreference = 'Continue'
        & $program shrink-live $letter $target $recovery 2>&1 | Set-Content -LiteralPath $log
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        Remove-Item Env:\EXFAT_TEST_FAULT -ErrorAction SilentlyContinue

        if (($phase -eq 'commit' -and $code -ne 0) -or ($phase -ne 'commit' -and $code -eq 0)) {
            throw "Unexpected live command result for $phase ($code). See $log"
        }
        $state = Get-Content -LiteralPath (Join-Path $recovery 'state.txt') -Raw
        if ($phase -ne 'commit' -and -not $state.StartsWith('ROLLED_BACK')) {
            throw "Rollback did not complete for $phase. See $log"
        }
        $current = Get-Partition -DiskNumber $disk.Number -PartitionNumber $selected.PartitionNumber
        $expectedSize = if ($phase -eq 'commit') { $target } else { $sourceSize }
        if ($current.Size -ne $expectedSize -or $current.Offset -ne $selected.Offset -or
            $current.Guid -ne $selected.Guid) {
            throw "Partition identity/size mismatch after $phase."
        }
        $currentNeighbor = Get-Partition -DiskNumber $disk.Number -PartitionNumber $neighbor.PartitionNumber
        if ($currentNeighbor.Size -ne $neighborSize -or $currentNeighbor.Offset -ne $neighborOffset -or
            $currentNeighbor.Guid -ne $neighborGuid) {
            throw "Neighbor partition changed after $phase."
        }
        & chkdsk ($letter + ':') 2>&1 | Set-Content -LiteralPath (Join-Path $PSScriptRoot ('live-chkdsk-' + $phase + '.log'))
        if ($LASTEXITCODE -ne 0) { throw "CHKDSK failed after $phase." }
        $checked = Check-OriginalFiles $root
        "Phase $phase passed: partition $expectedSize bytes; $checked hashes; neighbor unchanged." |
            Add-Content -LiteralPath (Join-Path $PSScriptRoot 'live-test-summary.txt')

        $recoveryRoot = [IO.Path]::GetFullPath($recovery)
        $workspaceRoot = [IO.Path]::GetFullPath($PSScriptRoot) + '\'
        if (-not $recoveryRoot.StartsWith($workspaceRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Recovery cleanup target outside workspace.'
        }
        foreach ($name in @('original.img', 'prepared.img', 'readback.img')) {
            $temporaryImage = Join-Path $recoveryRoot $name
            if (Test-Path -LiteralPath $temporaryImage) {
                Remove-Item -LiteralPath $temporaryImage
            }
        }
    }

    Dismount-DiskImage -ImagePath $vhd | Out-Null
    $mounted = $false
    Remove-Item -LiteralPath $vhd
    Remove-Item -LiteralPath $seed
    "PASS: 3 rollback phases plus actual GPT partition shrink, CHKDSK, 10 independent hashes at each phase, neighbor unchanged; detached. VHD: $vhd" |
        Set-Content -LiteralPath $status
}
catch {
    "FAILED: $($_.Exception.Message). VHD: $vhd" | Set-Content -LiteralPath $status
    exit 1
}
finally {
    Remove-Item Env:\EXFAT_TEST_FAULT -ErrorAction SilentlyContinue
    if ($mounted) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
}
