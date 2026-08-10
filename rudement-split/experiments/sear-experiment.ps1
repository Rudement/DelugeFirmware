<#
    Sear "too tame" — discriminating experiment.

    THESE ARE THROWAWAY BUILDS. Nothing here should ever be committed. Every mode
    edits src/deluge/dsp/sear.hpp in place; `revert` restores it with git checkout.

    Background
    ----------
    searBuffer contains a deliberate partial correction. As committed at 82029181:

        const q31_t lift   = (level < (1 << 29)) ? (level << 2) : ONE_Q31;
        const q31_t target = add_saturation(raw, 2 * multiply_32x32_rshift32(raw, lift));

    `raw` is the pure ratio envIn/envOut, which corrects loudness to 0.00 dB. `lift`
    adds an allowance back on top, now reaching 2.0x (+6.0 dB) at full knob. Its own
    comment says correcting all the way "makes drive read as nothing is happening".

    Two structural facts that constrain every mode below:

      1. lift scales LINEARLY with level, so the bottom of the knob gets very little
         allowance in absolute terms regardless of the ceiling.
      2. target = raw * (1 + lift/2^31) and lift saturates at ONE_Q31, so 2.0x is a
         HARD CEILING. "Just make it bigger" is no longer available.

    Where this stands, 2026-08-09
    -----------------------------
    `bypass` established that the leveller, not softClipCubic, causes "tame".
    `lift2x` was then built, auditioned, accepted, and COMMITTED as 82029181, so
    2.0x is now the baseline this script patches against - not 1.5x.

    But 2.0x was then reported as STILL NOT AS AGGRESSIVE as the pre-leveller
    version. That is a different complaint from "tame", and it splits the search:

      - If the deficit is LEVEL, only reshaping is left -> `liftsteep`.
      - If the deficit is DYNAMICS, allowance cannot fix it at any size, because
        `lift` is a static multiplier that raises attack and sustain together.
        -> `slowglide` or `noseed`.

    The discriminating listen: hold a SUSTAINED note at fixed level. If that alone
    sounds adequately driven and only note ONSETS feel soft, it is dynamics, and
    liftsteep is the wrong road.

    Why dynamics is the live suspicion: kSearGainShift = 7 moves the gain 1/128 per
    sample, a 2.9 ms time constant at 44.1 kHz, and voice.cpp seeds each new Voice
    from the Sound's converged state. So the correction is already ducking from
    sample zero of every note and can chase a change within a few ms - which is the
    attack. The pre-leveller version used a static makeup, so every attack passed
    uncorrected. A leveller that tracks in 2.9 ms is behaving like a compressor,
    which sear.hpp's own design notes say it must not.

    Modes
    -----
    bypass     Correction removed entirely: target = unity, gain never corrects.
               The diagnostic extreme, not a candidate fix. Loud, and it pumps on
               dynamics - that is the point. Already run; kept for re-reference.

    liftsteep  READ THIS BEFORE ASSUMING IT MEANS "MORE THAN 2x". It does not, and
               cannot. target = raw * (1 + lift/2^31) and lift saturates at ONE_Q31,
               so 2.0x (+6.0 dB) is the STRUCTURAL CEILING of the current expression.
               Exceeding it needs the `2 *` in the target line changed, and that
               overflows int32 as written - real surgery, not a patch.

               What this mode does instead is SHAPE the curve: lift reaches the 2.0x
               ceiling by roughly half knob and holds it, rather than arriving there
               only at full knob. So the bottom and middle of the range get much
               more allowance, the top gets none. That directly tests HANDOFF's
               "shape it, do not enlarge it" hypothesis, which is the only thing
               still available on the LEVEL axis.

                 more aggressive low/mid  -> shaping was right; tune the curve.
                 no change, or just early loudness -> level axis is exhausted.
                                                     It is dynamics: slowglide/noseed.

    slowglide  kSearGainShift 7 -> 11, so the gain glide goes from 2.9 ms to 46 ms
               and physically cannot move during a note attack. Keeps the per-note
               seed. The controlled half of the dynamics hypothesis.

    noseed     Drops the per-note seed in voice.cpp: each note starts at unity gain
               and settles over ~23 ms, so every onset passes uncorrected. This is
               structurally what the pre-leveller version did. NOTE this is the one
               mode that patches voice.cpp rather than sear.hpp - revert restores
               both.

    revert     git checkout both files. Run this before committing anything, ever.

    Usage
    -----
        .\rudement-split\experiments\sear-experiment.ps1 liftsteep
        .\rudement-split\experiments\sear-experiment.ps1 slowglide
        .\rudement-split\experiments\sear-experiment.ps1 noseed
        .\rudement-split\experiments\sear-experiment.ps1 revert

    Each build mode patches, builds chopin, and copies the binary to the Desktop
    "Rude Claude" folder with an -EXPERIMENT-<mode> suffix so it cannot be confused
    with a real build.
#>

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('bypass', 'liftsteep', 'slowglide', 'noseed', 'revert')]
    [string]$Mode
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
Set-Location $repo
$file  = 'src/deluge/dsp/sear.hpp'
$vfile = 'src/deluge/model/voice/voice.cpp'

# BASELINE IS NOW 2.0x, COMMITTED AS 82029181. The old `lift2x` mode searched for the
# 1.5x line and would now throw, because that line no longer exists - which is the
# correct failure, but a useless one. It is replaced by `liftsteep`.
$LIFT_ORIG   = 'const q31_t lift = (level < (1 << 29)) ? (level << 2) : ONE_Q31;'
$LIFT_STEEP  = 'const q31_t lift = (level < (1 << 28)) ? (level << 3) : ONE_Q31; // EXPERIMENT'
$TARGET_ORIG = 'const q31_t target = add_saturation(raw, 2 * multiply_32x32_rshift32(raw, lift));'
$TARGET_BYP  = 'const q31_t discarded = add_saturation(raw, 2 * multiply_32x32_rshift32(raw, lift)); (void)discarded; const q31_t target = ONE_Q31; // EXPERIMENT'
$GLIDE_ORIG  = 'constexpr int32_t kSearGainShift = 7;'
$GLIDE_SLOW  = 'constexpr int32_t kSearGainShift = 11; // EXPERIMENT'
$SEED_ORIG   = 'searLevelState = sound->searLevelSeed;'
$SEED_NONE   = 'dsp::searLevelReset(searLevelState); // EXPERIMENT'

function Restore-Sear {
    git checkout -- $file
    git checkout -- $vfile
    Write-Host "Restored $file and $vfile" -ForegroundColor Green
}

if ($Mode -eq 'revert') {
    Restore-Sear
    git status --short -uno
    exit 0
}

# Always start from a clean copy so modes cannot stack on each other.
Restore-Sear

$text = Get-Content $file -Raw

switch ($Mode) {
    'bypass' {
        if ($text -notmatch [regex]::Escape($TARGET_ORIG)) {
            throw "Could not find the target line. sear.hpp has changed - re-read it before trusting this script."
        }
        $text = $text.Replace($TARGET_ORIG, $TARGET_BYP)
        Write-Host "Patched: correction bypassed (target forced to unity)" -ForegroundColor Yellow
    }
    'liftsteep' {
        if ($text -notmatch [regex]::Escape($LIFT_ORIG)) {
            throw "Could not find the lift line. sear.hpp has changed - re-read it before trusting this script."
        }
        $text = $text.Replace($LIFT_ORIG, $LIFT_STEEP)
        Write-Host "Patched: lift hits the 2.0x ceiling by ~half knob (shape, not size)" -ForegroundColor Yellow
    }
    'slowglide' {
        if ($text -notmatch [regex]::Escape($GLIDE_ORIG)) {
            throw "Could not find kSearGainShift. sear.hpp has changed - re-read it before trusting this script."
        }
        $text = $text.Replace($GLIDE_ORIG, $GLIDE_SLOW)
        Write-Host "Patched: gain glide 2.9 ms -> 46 ms (cannot track a note attack)" -ForegroundColor Yellow
    }
    'noseed' {
        $vtext = Get-Content $vfile -Raw
        if ($vtext -notmatch [regex]::Escape($SEED_ORIG)) {
            throw "Could not find the seed line in voice.cpp - re-read it before trusting this script."
        }
        $vtext = $vtext.Replace($SEED_ORIG, $SEED_NONE)
        Set-Content -Path $vfile -Value $vtext -NoNewline
        Write-Host "Patched: per-note seed dropped, each note starts uncorrected" -ForegroundColor Yellow
    }
}

Set-Content -Path $file -Value $text -NoNewline

Write-Host ""
Write-Host "=== Diff ===" -ForegroundColor Cyan
git --no-pager diff -- $file $vfile

# g_menus.inc gets rewritten with different line endings by every build; discard it
# if that is the only difference, or the branch guard trips later.
git diff --quiet --ignore-cr-at-eol -- 'src/deluge/gui/menu_item/generate/g_menus.inc'
if ($LASTEXITCODE -eq 0) { git checkout -- 'src/deluge/gui/menu_item/generate/g_menus.inc' 2>$null }

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
$req    = (Get-Content 'toolchain/REQUIRED_VERSION' -Raw).Trim()
Write-Host ""
Write-Host "Branch: $branch   toolchain: v$req" -ForegroundColor Cyan
if ($req -ne '16') {
    Write-Host "Expected the 1.2.1 line (v16). Switch to chopin-rudement first." -ForegroundColor Red
    Restore-Sear
    exit 1
}

Write-Host ""
Write-Host "=== Building (no nuke - same toolchain, incremental) ===" -ForegroundColor Cyan
& cmd /c "dbt build release -m"
if ($LASTEXITCODE -ne 0) {
    Write-Host "BUILD FAILED - reverting sear.hpp" -ForegroundColor Red
    Restore-Sear
    exit 1
}

$desktop = [Environment]::GetFolderPath('Desktop')
$dest    = Join-Path $desktop 'Rude Claude'
if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Path $dest | Out-Null }

# ONLY the binary this run just produced. build/Release keeps stale .bin files from
# previous builds, and copying all of them labels an untouched build as an experiment -
# which is worse than useless, because you cannot tell by listening that you flashed
# the wrong one.
$fresh = Get-ChildItem 'build/Release/*.bin' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $fresh) { throw "No .bin produced." }

$age = (Get-Date) - $fresh.LastWriteTime
if ($age.TotalMinutes -gt 10) {
    Write-Host "Newest .bin is $([int]$age.TotalMinutes) min old - the build may not have produced one." -ForegroundColor Red
    Restore-Sear
    exit 1
}

$out = Join-Path $dest ("EXPERIMENT-$Mode-" + $fresh.Name)
Copy-Item $fresh.FullName $out -Force
Write-Host "  -> $out" -ForegroundColor Green

# Clear mislabelled copies from earlier runs of this script.
Get-ChildItem (Join-Path $dest "EXPERIMENT-*.bin") | Where-Object { $_.FullName -ne $out } | ForEach-Object {
    Remove-Item $_.FullName -Force
    Write-Host "  removed stale experiment binary: $($_.Name)" -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "Flash it, listen, then run:" -ForegroundColor Yellow
Write-Host "    .\rudement-split\experiments\sear-experiment.ps1 revert" -ForegroundColor Yellow
Write-Host ""
Write-Host "sear.hpp is currently MODIFIED. Do not commit until you have reverted." -ForegroundColor Red
