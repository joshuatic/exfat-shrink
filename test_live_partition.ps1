param(
    [string]$Python = 'python',
    [ValidateSet('GPT', 'MBR')]
    [string]$PartitionStyle = 'GPT',
    [uint32]$OffsetSectors = 2048
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$python = (Get-Command $Python -ErrorAction Stop).Source
$status = Join-Path $PSScriptRoot 'live-test-status.txt'
$vhd = Join-Path $PSScriptRoot ('live-gpt-test-' + [guid]::NewGuid().ToString('N') + '.vhd')
$mounted = $false
$recoveryVhd = $null
$recoveryMounted = $false
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
    $wrapperArgs = @((Join-Path $PSScriptRoot 'make_test_vhd.py'), $seed, $vhd)
    $wrapperArgs += @('--offset-sectors', [string]$OffsetSectors)
    if ($PartitionStyle -eq 'GPT') { $wrapperArgs += '--gpt' }
    & $python @wrapperArgs 2>&1 |
        Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-test-create.log')
    $createExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($createExit -ne 0) { throw 'Test VHD creation failed.' }

    Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
    $mounted = $true
    $disk = Get-DiskImage -ImagePath $vhd | Get-Disk
    if ($disk.PartitionStyle -ne $PartitionStyle -or $disk.IsBoot -or $disk.IsSystem) {
        throw 'Unexpected disposable disk identity.'
    }
    $partitions = @($disk | Get-Partition)
    $selected = $partitions | Where-Object { $_.Size -eq $sourceSize }
    $neighbor = $partitions | Where-Object { $_.PartitionNumber -ne $selected.PartitionNumber }
    if (-not $selected -or ($PartitionStyle -eq 'GPT' -and -not $neighbor)) { throw 'Missing selected/sentinel partition.' }
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
    $cycleExpected = @{}
    foreach ($cycle in 0..199) {
        $relative = 'nested\reuse-' + ($cycle % 64) + '.bin'
        $path = Join-Path $root $relative
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        $bytes = New-Object byte[] ($rng.Next(1, 8193))
        $rng.NextBytes($bytes)
        $hash = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
        [IO.File]::WriteAllBytes($path, $bytes)
        $cycleExpected[$relative] = [pscustomobject]@{ Path = $relative; Hash = $hash }
    }
    $rows += @($cycleExpected.Values)
    $sha.Dispose()

    $neighbors = @($neighbor | Select-Object PartitionNumber, Size, Offset, Guid)
    $phases = @('before-journal', 'after-journal', 'after-dirty', 'after-first-write', 'halfway-writes', 'after-filesystem', 'after-partition')
    if ($PartitionStyle -eq 'GPT') {
        $phases += @('crash-after-dirty', 'crash-after-filesystem', 'crash-after-partition', 'crash-during-rollback')
    }
    foreach ($rejectedDrive in @($env:SystemDrive.Substring(0,1), '\\.\PhysicalDrive0')) {
        $rejectDirectory = Join-Path $PSScriptRoot ('live-recovery-system-' + [guid]::NewGuid().ToString('N'))
        $ErrorActionPreference = 'Continue'
        & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') shrink-live $rejectedDrive $target $rejectDirectory 2>&1 |
            Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-reject-system.log')
        $rejectCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($rejectCode -eq 0) { throw 'System or whole-disk input was accepted.' }
    }
    if ($PartitionStyle -eq 'GPT') {
        $selected | Set-Partition -IsReadOnly $true
        try {
            $rejectDirectory = Join-Path $PSScriptRoot ('live-recovery-readonly-' + [guid]::NewGuid().ToString('N'))
            $ErrorActionPreference = 'Continue'
            & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') shrink-live $letter $target $rejectDirectory 2>&1 |
                Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-reject-special-attribute.log')
            $rejectCode = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($rejectCode -eq 0) { throw 'Special GPT attributes were accepted.' }
        } finally { $selected | Set-Partition -IsReadOnly $false }
    }
    if ($PartitionStyle -eq 'GPT') {
        $recoveryVhd = Join-Path $PSScriptRoot ('live-recovery-disk-' + [guid]::NewGuid().ToString('N') + '.vhd')
        & $python (Join-Path $PSScriptRoot 'make_test_vhd.py') $seed $recoveryVhd | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Recovery test disk creation failed.' }
        Mount-DiskImage -ImagePath $recoveryVhd -StorageType VHD -Access ReadWrite -NoDriveLetter | Out-Null
        $recoveryMounted = $true
        $recoveryDisk = Get-DiskImage -ImagePath $recoveryVhd | Get-Disk
        if ($recoveryDisk.IsSystem -or $recoveryDisk.IsBoot -or $recoveryDisk.Number -eq $disk.Number) { throw 'Wrong recovery test disk.' }
        $recoveryPartition = $recoveryDisk | Get-Partition | Where-Object { $_.Size -eq $sourceSize }
        $recoveryPartition | Format-Volume -FileSystem NTFS -NewFileSystemLabel 'RECOVERY_TEST' -Confirm:$false | Out-Null
        $recoveryPartition | Add-PartitionAccessPath -AssignDriveLetter
        $recoveryVolume = $recoveryPartition | Get-Volume
        $recoveryRoot = $recoveryVolume.DriveLetter + ':\'
        $filler = [IO.File]::Open((Join-Path $recoveryRoot 'filler.bin'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try {
            $bytes = New-Object byte[] 1048576
            for ($fill = 0; $fill -lt 100; $fill++) {
                $free = (Get-Volume -DriveLetter $recoveryVolume.DriveLetter).SizeRemaining
                if ($free -le 65536) { break }
                $take = [int][Math]::Min($bytes.Length, $free - 65536)
                $filler.Write($bytes, 0, $take)
                $filler.Flush($true)
            }
        } finally { $filler.Dispose() }
        $ErrorActionPreference = 'Continue'
        & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') shrink-live $letter $target (Join-Path $recoveryRoot 'full') 2>&1 |
            Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-reject-full-recovery.log')
        $rejectCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($rejectCode -eq 0) { throw 'Full recovery disk was accepted.' }
        Remove-Item -LiteralPath (Join-Path $recoveryRoot 'filler.bin')
        Dismount-DiskImage -ImagePath $recoveryVhd | Out-Null
        $recoveryMounted = $false
        Mount-DiskImage -ImagePath $recoveryVhd -StorageType VHD -Access ReadOnly -NoDriveLetter | Out-Null
        $recoveryMounted = $true
        $recoveryDisk = Get-DiskImage -ImagePath $recoveryVhd | Get-Disk
        $recoveryPartition = $recoveryDisk | Get-Partition | Where-Object { $_.Size -eq $sourceSize }
        $recoveryPartition | Add-PartitionAccessPath -AssignDriveLetter
        $recoveryVolume = $recoveryPartition | Get-Volume
        $ErrorActionPreference = 'Continue'
        & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') shrink-live $letter $target ($recoveryVolume.DriveLetter + ':\readonly') 2>&1 |
            Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-reject-readonly-recovery.log')
        $rejectCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($rejectCode -eq 0) { throw 'Read-only recovery disk was accepted.' }
        Dismount-DiskImage -ImagePath $recoveryVhd | Out-Null
        $recoveryMounted = $false
        Remove-Item -LiteralPath $recoveryVhd
        $recoveryVhd = $null
        if ((Get-Partition -DiskNumber $disk.Number -PartitionNumber $selected.PartitionNumber).Size -ne $sourceSize) { throw 'Recovery storage refusal changed partition.' }
        $checked = Check-OriginalFiles $root
    }
    $held = [IO.File]::Open((Join-Path $root 'nested\file-512.bin'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $lockedRecovery = Join-Path $PSScriptRoot ('live-recovery-held-' + [guid]::NewGuid().ToString('N'))
        $ErrorActionPreference = 'Continue'
        & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') shrink-live $letter $target $lockedRecovery 2>&1 |
            Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-reject-open-handle.log')
        $lockedCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($lockedCode -eq 0) { throw 'Open file handle did not prevent shrink.' }
    } finally { $held.Dispose() }
    $phases += 'commit'
    foreach ($phase in $phases) {
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
        $commandLetter = if ($phase -eq 'after-dirty') { $letter.ToLowerInvariant() } else { $letter }
        & $program shrink-live $commandLetter $target $recovery 2>&1 | Set-Content -LiteralPath $log
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        for ($retry = 0; $retry -lt 3 -and $code -ne 0; $retry++) {
            $preparing = Get-Content -LiteralPath (Join-Path $recovery 'state.txt') -Raw
            if (-not $preparing.StartsWith('PREPARING') -or -not (Test-Path -LiteralPath $log)) { break }
            $failure = Get-Content -LiteralPath $log -Raw
            if (-not $failure.Contains('cannot lock and dismount')) { break }
            Start-Sleep -Milliseconds 500
            $recovery = Join-Path $PSScriptRoot ('live-recovery-' + $phase + '-retry-' + [guid]::NewGuid().ToString('N'))
            $ErrorActionPreference = 'Continue'
            & $program shrink-live $commandLetter $target $recovery 2>&1 | Set-Content -LiteralPath $log
            $code = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
        }
        Remove-Item Env:\EXFAT_TEST_FAULT -ErrorAction SilentlyContinue

        if (($phase -eq 'commit' -and $code -ne 0) -or ($phase -ne 'commit' -and $code -eq 0)) {
            throw "Unexpected live command result for $phase ($code). See $log"
        }
        if ($phase.StartsWith('crash-')) {
            $production = Join-Path $PSScriptRoot 'build\exfat_shrink.exe'
            $journalPath = Join-Path $recovery 'metadata-journal.bin'
            $pristineJournal = [IO.File]::ReadAllBytes($journalPath)
            foreach ($damage in @('checksum', 'header', 'truncated')) {
                $journalBytes = [byte[]]$pristineJournal.Clone()
                if ($damage -eq 'checksum') { $journalBytes[$journalBytes.Length - 1] = $journalBytes[$journalBytes.Length - 1] -bxor 1 }
                if ($damage -eq 'header') { $journalBytes[0] = $journalBytes[0] -bxor 1 }
                if ($damage -eq 'truncated') { $journalBytes = $journalBytes[0..($journalBytes.Length - 2)] }
                [IO.File]::WriteAllBytes($journalPath, $journalBytes)
                $ErrorActionPreference = 'Continue'
                & $production recover-live $letter $recovery 2>&1 | Set-Content -LiteralPath (Join-Path $PSScriptRoot ('live-reject-' + $phase + '-' + $damage + '.log'))
                $rejectCode = $LASTEXITCODE
                $ErrorActionPreference = 'Stop'
                if ($rejectCode -eq 0) { throw "Corrupt $damage journal was accepted." }
            }
            [IO.File]::WriteAllBytes($journalPath, $pristineJournal)
            # Bind digest-valid journals to the disk GUID: a forged alternate
            # layout with a recomputed checksum must still be refused.
            $layoutPath = Join-Path $recovery 'layout-before.bin'
            $pristineLayout = [IO.File]::ReadAllBytes($layoutPath)
            $wrongLayout = [byte[]]$pristineLayout.Clone()
            $wrongLayout[8] = $wrongLayout[8] -bxor 1
            [IO.File]::WriteAllBytes($layoutPath, $wrongLayout)
            $hashInput = New-Object byte[] ($wrongLayout.Length + $pristineJournal.Length - 32)
            [Array]::Copy($wrongLayout, 0, $hashInput, 0, $wrongLayout.Length)
            [Array]::Copy($pristineJournal, 0, $hashInput, $wrongLayout.Length, $pristineJournal.Length - 32)
            $journalBytes = [byte[]]$pristineJournal.Clone()
            $identityHash = [Security.Cryptography.SHA256]::Create()
            [Array]::Copy($identityHash.ComputeHash($hashInput), 0, $journalBytes, $journalBytes.Length - 32, 32)
            $identityHash.Dispose()
            [IO.File]::WriteAllBytes($journalPath, $journalBytes)
            $ErrorActionPreference = 'Continue'
            & $production recover-live $letter $recovery 2>&1 | Set-Content -LiteralPath (Join-Path $PSScriptRoot ('live-reject-' + $phase + '-wrong-disk.log'))
            $rejectCode = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($rejectCode -eq 0) { throw 'Wrong disk journal was accepted.' }
            [IO.File]::WriteAllBytes($layoutPath, $pristineLayout)
            [IO.File]::WriteAllBytes($journalPath, $pristineJournal)
            foreach ($replay in @(1, 2)) {
                $ErrorActionPreference = 'Continue'
                & $production recover-live $letter $recovery 2>&1 | Set-Content -LiteralPath (Join-Path $PSScriptRoot ('live-recover-' + $phase + '-' + $replay + '.log'))
                $recoverCode = $LASTEXITCODE
                $ErrorActionPreference = 'Stop'
                if ($recoverCode -ne 0) { throw "Recovery replay $replay failed for $phase." }
            }
        }
        $state = Get-Content -LiteralPath (Join-Path $recovery 'state.txt') -Raw
        if ($phase -notin @('commit', 'before-journal', 'after-journal') -and -not ($state.StartsWith('ROLLED_BACK') -or $state.StartsWith('RECOVERED'))) {
            throw "Rollback did not complete for $phase. See $log"
        }
        $current = Get-Partition -DiskNumber $disk.Number -PartitionNumber $selected.PartitionNumber
        $expectedSize = if ($phase -eq 'commit') { $target } else { $sourceSize }
        if ($current.Size -ne $expectedSize -or $current.Offset -ne $selected.Offset -or
            $current.Guid -ne $selected.Guid) {
            throw "Partition identity/size mismatch after $phase."
        }
        foreach ($oldNeighbor in $neighbors) {
            $currentNeighbor = Get-Partition -DiskNumber $disk.Number -PartitionNumber $oldNeighbor.PartitionNumber
            if ($currentNeighbor.Size -ne $oldNeighbor.Size -or $currentNeighbor.Offset -ne $oldNeighbor.Offset -or
                $currentNeighbor.Guid -ne $oldNeighbor.Guid) { throw "Neighbor partition changed after $phase." }
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

    foreach ($repeatTarget in @(20971520, 16777216)) {
        $repeatRecovery = Join-Path $PSScriptRoot ('live-recovery-repeat-' + [guid]::NewGuid().ToString('N'))
        $ErrorActionPreference = 'Continue'
        & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') shrink-live $letter $repeatTarget $repeatRecovery 2>&1 |
            Set-Content -LiteralPath (Join-Path $PSScriptRoot ('live-repeat-' + $repeatTarget + '.log'))
        $repeatCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($repeatCode -ne 0 -or (Get-Partition -DiskNumber $disk.Number -PartitionNumber $selected.PartitionNumber).Size -ne $repeatTarget) {
            throw 'Repeated shrink failed.'
        }
        & chkdsk ($letter + ':') 2>&1 | Set-Content -LiteralPath (Join-Path $PSScriptRoot ('live-chkdsk-repeat-' + $repeatTarget + '.log'))
        if ($LASTEXITCODE -ne 0) { throw 'Repeated shrink CHKDSK failed.' }
        $checked = Check-OriginalFiles $root
        foreach ($oldNeighbor in $neighbors) {
            $currentNeighbor = Get-Partition -DiskNumber $disk.Number -PartitionNumber $oldNeighbor.PartitionNumber
            if ($currentNeighbor.Size -ne $oldNeighbor.Size -or $currentNeighbor.Offset -ne $oldNeighbor.Offset -or
                $currentNeighbor.Guid -ne $oldNeighbor.Guid) { throw 'Repeated shrink changed a neighbor.' }
        }
        $ErrorActionPreference = 'Continue'
        & (Join-Path $PSScriptRoot 'build\exfat_shrink.exe') recover-live $letter $repeatRecovery 2>&1 |
            Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-reject-committed.log')
        $committedCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($committedCode -eq 0) { throw 'Committed recovery journal was accepted.' }
    }
    Dismount-DiskImage -ImagePath $vhd | Out-Null
    $mounted = $false
    Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly -NoDriveLetter | Out-Null
    $mounted = $true
    $disk = Get-DiskImage -ImagePath $vhd | Get-Disk
    $remounted = Get-Partition -DiskNumber $disk.Number -PartitionNumber $selected.PartitionNumber
    if ($remounted.Size -ne 16777216 -or $remounted.Offset -ne $selected.Offset -or $remounted.Guid -ne $selected.Guid) {
        throw 'Remounted partition identity or size changed.'
    }
    $remounted | Add-PartitionAccessPath -AssignDriveLetter
    $volume = $remounted | Get-Volume
    $checked = Check-OriginalFiles ($volume.DriveLetter + ':\')
    & chkdsk ($volume.DriveLetter + ':') 2>&1 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'live-chkdsk-remount.log')
    if ($LASTEXITCODE -ne 0) { throw 'Remounted CHKDSK failed.' }
    Dismount-DiskImage -ImagePath $vhd | Out-Null
    $mounted = $false
    Remove-Item -LiteralPath $vhd
    Remove-Item -LiteralPath $seed
    "PASS: $PartitionStyle failure phases, recovery checks when GPT, plus actual partition shrink, CHKDSK, $($rows.Count) independent hashes at each phase, neighbor unchanged; detached. VHD: $vhd" |
        Set-Content -LiteralPath $status
}
catch {
    "FAILED: $($_.Exception.Message). VHD: $vhd" | Set-Content -LiteralPath $status
    exit 1
}
finally {
    Remove-Item Env:\EXFAT_TEST_FAULT -ErrorAction SilentlyContinue
    if ($mounted) { Dismount-DiskImage -ImagePath $vhd | Out-Null }
    if ($recoveryMounted) { Dismount-DiskImage -ImagePath $recoveryVhd | Out-Null }
}
