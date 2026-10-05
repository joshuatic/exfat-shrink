param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z]$')]
    [string]$DriveLetter,
    [int]$Cycles = 2
)

# Preserves existing files. Never formats, creates, or deletes a partition.
$ErrorActionPreference = 'Stop'
$workspace = $PSScriptRoot
$status = Join-Path $workspace 'live-physical-status.txt'
$report = Join-Path $workspace 'live-physical-verification.log'
$exe = Join-Path $workspace 'build\exfat_shrink.exe'
$root = $DriveLetter.ToUpper() + ':\'
$created = Join-Path $root ('exfat-shrink-test-' + [guid]::NewGuid().ToString('N'))

function Manifest {
    $result = @{}
    $files = @(Get-ChildItem -LiteralPath $root -File -Force)
    Get-ChildItem -LiteralPath $root -Directory -Force |
        Where-Object { $_.Name -notin @('System Volume Information', '$RECYCLE.BIN') } |
        ForEach-Object { $files += @(Get-ChildItem -LiteralPath $_.FullName -File -Recurse -Force) }
    $files | ForEach-Object {
        $result[$_.FullName.Substring($root.Length)] = @(
            $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)
    }
    return $result
}

function Verify($Expected) {
    $present = Manifest
    if ($present.Count -ne $Expected.Count) { throw 'File count changed.' }
    foreach ($name in $Expected.Keys) {
        if (-not $present.ContainsKey($name) -or
            $present[$name][0] -ne $Expected[$name][0] -or
            $present[$name][1] -ne $Expected[$name][1]) { throw "File mismatch: $name" }
    }
}

try {
    if ($Cycles -lt 1 -or $Cycles -gt 4) { throw 'Cycles must be 1 through 4.' }
    $selected = Get-Partition -DriveLetter $DriveLetter
    $disk = Get-Disk -Number $selected.DiskNumber
    $volume = Get-Volume -DriveLetter $DriveLetter
    if ($disk.IsBoot -or $disk.IsSystem -or $disk.PartitionStyle -ne 'GPT' -or
        $volume.FileSystem -ne 'exFAT' -or $volume.FileSystemLabel -ne 'EXFATTEST' -or
        $selected.Size -gt 2147483648 -or $selected.Size -lt 1073741824 -or
        (Get-Partition -DriveLetter D).DiskNumber -ne $disk.Number -or
        $volume.SizeRemaining -lt 128MB) { throw 'Disposable same-SSD partition guard failed.' }
    $identity = $selected | Select-Object DiskNumber, PartitionNumber, Offset, Guid
    $neighbors = @($disk | Get-Partition | Where-Object PartitionNumber -ne $selected.PartitionNumber |
        Select-Object PartitionNumber, Offset, Size, Guid)
    'Hashing existing files before test writes.' | Set-Content -LiteralPath $status
    $original = Manifest
    $original | Export-Clixml -LiteralPath (Join-Path $workspace 'live-physical-original.xml')
    New-Item -ItemType Directory -Path $created | Out-Null
    $deep = $created
    foreach ($level in 1..20) {
        $deep = Join-Path $deep ('level-' + $level)
        New-Item -ItemType Directory -Path $deep | Out-Null
    }
    $unicodeName = 'unicode-' + [char]0x4e2d + [char]0x6587 + '.bin'
    [IO.File]::WriteAllBytes((Join-Path $deep $unicodeName), [byte[]](1, 2, 3))
    $rng = New-Object Random 7301
    $buffer = New-Object byte[] 65536
    $rng.NextBytes($buffer)
    foreach ($index in 1..512) {
        [IO.File]::WriteAllBytes((Join-Path $created ('small-' + $index + '.bin')), $buffer[0..4095])
    }
    $stream = [IO.File]::Open((Join-Path $created 'large.bin'), [IO.FileMode]::CreateNew)
    try { foreach ($block in 1..512) { $stream.Write($buffer, 0, $buffer.Length) }; $stream.Flush($true) }
    finally { $stream.Dispose() }
    $expected = Manifest
    foreach ($cycle in 1..$Cycles) {
        $selected = Get-Partition -DriveLetter $DriveLetter
        $target = $selected.Size - 16MB
        $recovery = Join-Path $workspace ('live-recovery-physical-' + [guid]::NewGuid().ToString('N'))
        "Physical cycle $cycle, target $target, $($expected.Count) files." | Set-Content -LiteralPath $status
        $ErrorActionPreference = 'Continue'
        & $exe shrink-live $DriveLetter $target $recovery 2>&1 | Add-Content -LiteralPath $report
        $code = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($code) { throw "Physical shrink refused/failed; preserved recovery: $recovery" }
        # Explicit drive-letter removal and reassignment exercises a fresh mount.
        # Reattach even if an intermediate step throws.
        $removed = $false
        try {
            $selected = Get-Partition -DiskNumber $disk.Number -PartitionNumber $identity.PartitionNumber
            $selected | Remove-PartitionAccessPath -AccessPath $root
            $removed = $true
            $selected | Add-PartitionAccessPath -AccessPath $root
            $removed = $false
        }
        finally {
            if ($removed) {
                Get-Partition -DiskNumber $disk.Number -PartitionNumber $identity.PartitionNumber |
                    Add-PartitionAccessPath -AccessPath $root
            }
        }
        Verify $expected
        $ErrorActionPreference = 'Continue'
        & chkdsk ($DriveLetter + ':') 2>&1 | Add-Content -LiteralPath $report
        $check = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($check) { throw 'Physical CHKDSK failed.' }
        $current = Get-Partition -DriveLetter $DriveLetter
        if ($current.Size -ne $target -or $current.Guid -ne $identity.Guid -or
            $current.Offset -ne $identity.Offset) { throw 'Selected partition identity/size mismatch.' }
        foreach ($neighbor in $neighbors) {
            $now = Get-Partition -DiskNumber $disk.Number -PartitionNumber $neighbor.PartitionNumber
            if ($now.Size -ne $neighbor.Size -or $now.Offset -ne $neighbor.Offset -or
                $now.Guid -ne $neighbor.Guid) { throw 'Neighbor geometry changed.' }
        }
        "Cycle $cycle PASS: hashes, CHKDSK, size and neighbors." | Add-Content -LiteralPath $report
        # Retain the small journal until all cycles and original-file verification pass.
    }
    $checked = [IO.Path]::GetFullPath($created)
    if (-not $checked.StartsWith($root + 'exfat-shrink-test-', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Generated-file cleanup guard failed.'
    }
    Remove-Item -LiteralPath $checked -Recurse
    Verify $original
    "PASS: $Cycles physical shrinks; $($original.Count) existing files unchanged; generated files removed." |
        Set-Content -LiteralPath $status
}
catch {
    "FAILED: $($_.Exception.Message); generated test directory: $created" | Set-Content -LiteralPath $status
    exit 1
}
