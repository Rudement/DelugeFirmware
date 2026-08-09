@echo off
REM ============================================================================
REM  Create PRs for the three branches create-trail.cmd could not.
REM
REM  WHY THEY FAILED: feat/gristle-13 (6c02c5d6) and feat/sear-13 (a0da0808) are
REM  original commits, not cherry-picks, so they are direct ANCESTORS of
REM  beta-1.3. "git log beta-1.3..feat/sear-13" is empty - there is genuinely
REM  nothing to compare, and GitHub refuses. The other 1.3 branches were
REM  cherry-picked onto the base and got new SHAs, which is why they worked.
REM
REM  Fix: target base-13, a branch sitting at the 1.3 base commit 134d000f.
REM  Then the PR shows exactly the feature diff and nothing else.
REM
REM  Run from the repo root:  rudement-split\gh\fix-remaining-prs.cmd
REM ============================================================================

setlocal
cd /d "%~dp0..\.."
set "BODY=%~dp0"

echo.
echo === Base branches ===

git rev-parse --verify --quiet base-12 >nul || (
  git branch base-12 a2e333b9 && echo   created base-12 at a2e333b9
)
git rev-parse --verify --quiet base-13 >nul || (
  git branch base-13 134d000f && echo   created base-13 at 134d000f
)

git push origin base-12 base-13
if errorlevel 1 (
  echo Push failed. Cannot open PRs against bases the remote does not have.
  exit /b 1
)

echo.
echo === The two ancestor branches, against base-13 ===

gh pr create --draft --head feat/gristle-13 --base base-13 ^
  --title "The Gristleizer: standalone 9-parameter effect on 1.3" ^
  --body-file "%BODY%pr\gristle-13.md"

gh pr create --draft --head feat/sear-13 --base base-13 ^
  --title "Sear: rename from Heat, automatic level matching (1.3)" ^
  --body-file "%BODY%pr\sear-13.md"

echo.
echo === feat/ci-chopin-12 ===
echo Retrying against chopin-rudement. If it fails again the error is printed
echo in full below rather than swallowed.

gh pr create --draft --head feat/ci-chopin-12 --base chopin-rudement ^
  --title "CI: build this fork's chopin branch on push" ^
  --body-file "%BODY%pr\ci-chopin-12.md"
if errorlevel 1 (
  echo.
  echo Still failing. Falling back to base-12:
  gh pr create --draft --head feat/ci-chopin-12 --base base-12 ^
    --title "CI: build this fork's chopin branch on push" ^
    --body-file "%BODY%pr\ci-chopin-12.md"
)

echo.
echo === All PRs ===
gh pr list --limit 20
exit /b 0
