[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$LogPath,

    [ValidateRange(1, 40)]
    [int]$MaxLines = 40,

    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
    throw "Log file not found: $LogPath"
}

# Keep transition/fallback milestones and only the most recent slow delivery warnings.
# At the default setting the extracted output cannot exceed 40 lines.
$milestonePattern = '\[(?:NewSegment|BeginFlush|EndFlush)\]|\[(?:SeekFallback|SeekTrace)/(?:Armed|Start|FirstFrameDelivered|WarmupReady|Handoff|FirstReceive|FirstVisibleReceive)\]|\[Engine/DeferredSeekReset\]'
$slowStagePattern = '\[WARN\] \[Output/Allocator\] Slow GetDeliveryBuffer|\[WARN\] \[Output/Downstream\]'
$milestones = @(Select-String -LiteralPath $LogPath -Pattern $milestonePattern | Select-Object -Last 30)
$slowStages = @(Select-String -LiteralPath $LogPath -Pattern $slowStagePattern | Select-Object -Last 10)
$selectedMatches = @($milestones + $slowStages | Sort-Object LineNumber | Select-Object -Last $MaxLines)
$selected = @($selectedMatches | ForEach-Object { $_.Line })

if ($selected.Count -eq 0) {
    Write-Output 'No seek-trace events found. Confirm DirectMotion file logging was enabled before the seek.'
    exit 0
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $selected | Set-Content -LiteralPath $OutputPath -Encoding UTF8
    Write-Output "Wrote $($selected.Count) filtered seek events to $OutputPath"
} else {
    $selected
}
