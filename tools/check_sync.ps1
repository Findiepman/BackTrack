# Development check for audio/video sync (milestone 4). Needs ffmpeg on PATH (only for this check;
# Backtrack itself does not use it). Record while tools\sync_beacon.exe runs, save a clip, then:
#
#   powershell -ExecutionPolicy Bypass -File tools\check_sync.ps1 <clip.mp4>
#
# Finds every white flash of the beacon square (top-left corner) and every 1 kHz tone onset, pairs
# them and prints audio time minus video time for each pair. Positive = sound later than picture.

param([Parameter(Mandatory = $true)][string]$Clip)

$ErrorActionPreference = 'Continue'

# Video: average brightness of the middle of the beacon square, per frame.
$video = & ffmpeg -hide_banner -nostats -i $Clip -an -vf "crop=200:200:50:50,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-" -f null - 2>$null
$flashes = @()
$time = $null
$wasBright = $false
foreach ($line in $video) {
    if ($line -match 'pts_time:([0-9.]+)') { $time = [double]$Matches[1] }
    elseif ($line -match 'YAVG=([0-9.]+)') {
        $bright = [double]$Matches[1] -gt 128
        if ($bright -and -not $wasBright) { $flashes += $time }
        $wasBright = $bright
    }
}

# Audio: band-pass around 1 kHz, then the end of each silence is a tone onset.
$audio = & ffmpeg -hide_banner -nostats -i $Clip -vn -af "highpass=f=800,lowpass=f=1250,silencedetect=n=-40dB:d=0.1" -f null - 2>&1
# silencedetect also reports the end of the file as the end of a silence; that is no tone.
$duration = [double](& ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 $Clip)
$tones = @()
foreach ($line in $audio) {
    if ($line -match 'silence_end: ([0-9.]+)') {
        $time = [double]$Matches[1]
        if ($duration - $time -gt 0.1) { $tones += $time }
    }
}

"flashes: $($flashes.Count), tones: $($tones.Count)"
$offsets = @()
foreach ($flash in $flashes) {
    $nearest = $tones | Sort-Object { [math]::Abs($_ - $flash) } | Select-Object -First 1
    # Beacon flash and tone start together; anything further apart is something else on screen.
    if ($null -ne $nearest -and [math]::Abs($nearest - $flash) -lt 0.25) {
        $offset = [math]::Round(($nearest - $flash) * 1000)
        $offsets += $offset
        "flash at {0,7:N3} s  tone at {1,7:N3} s  offset {2,5} ms" -f $flash, $nearest, $offset
    }
}
if ($offsets.Count -gt 0) {
    $measure = $offsets | Measure-Object -Average -Minimum -Maximum
    "pairs: {0}, offset average {1:N0} ms, min {2} ms, max {3} ms" -f $offsets.Count, $measure.Average, $measure.Minimum, $measure.Maximum
}
