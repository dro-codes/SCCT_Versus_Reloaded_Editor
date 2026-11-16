param(
    [string]$OutDirectory = (Join-Path ([System.IO.Path]::GetTempPath()) 'scct-wiki-raw'),
    [switch]$Compare
)
# Refetches the SCCT Versus Reloaded Editor wiki for reviewing the MCP wiki
# digests (Reloaded.Editor.Mcp/Resources/wiki/<slug>.md, served as
# scct://wiki/<slug>).
#
# How the digests are made. They are curated by hand from the raw wiki
# markdown, not generated:
# - Each digest keeps every class, property and enum name verbatim, including
#   the wiki's own spellings (TriggerMethode, strenght, bInitialyOn). It also
#   keeps every number, rule, gotcha and step.
# - Values the wiki only shows in screenshots are marked "image-only". The
#   images themselves are not copied.
# - Each digest starts with "# <title>" and "Source: <wiki URL>".
# - Each digest ends with "## How the MCP tools apply this". That section
#   names the tools that write the page's actors and a "- Rules:" line of the
#   map_validate rule ids that check them.
# - tests/PromptsTests.cpp checks those ids against LevelRulesModel.h, and
#   checks that every rule's wiki page has a digest.
#
# To update after the wiki changes:
# 1. Run this script with -Compare. It fetches every page into OutDirectory
#    and writes manifest.json there, with the wiki commit, sizes and SHA-256
#    hashes. It then lists, per page, the backticked names and the numbers
#    that the digest no longer contains.
# 2. Edit the digests to match. Diff the manifest against the previous run's
#    to see which pages changed.
# 3. Run: python3 tools/mcp/embed_resources.py
# 4. Rebuild and run tests/PromptsTests.cpp.
#
# The downloaded pages are untrusted text: nothing from them is executed.
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$digests = Join-Path $repository 'Reloaded.Editor.Mcp/Resources/wiki'
$wiki = 'https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki'
$raw = 'https://raw.githubusercontent.com/wiki/AllyPal/SCCT_Versus_Reloaded_Editor'
# Wiki page => digest slug. Home and _Sidebar list the pages; Sound-Browser is
# linked from Map Packaging rather than the sidebar.
$pages = [ordered]@{
    'Home' = 'home'; 'Spawn-Points' = 'spawn-points'; 'Game-Modes' = 'game-modes'; 'Map-Settings' = 'map-settings'
    'Level-Properties' = 'level-properties'; 'Vision-Mode-Properties' = 'vision-mode-properties'; 'Geometric-Event-(GE)' = 'geometric-events'
    'Events' = 'events'; 'Alarms' = 'alarms'; 'Movers' = 'movers'; 'Elevators' = 'elevators'; 'Lighting' = 'lighting'
    'Material-Classes' = 'material-classes'; 'Static-Meshes' = 'static-meshes'; 'Sound-Actors' = 'sound-actors'; 'Sound-Browser' = 'sound-browser'
    'Gameplay-Objects' = 'gameplay-objects'; 'Buttons-and-Hacking-Panels' = 'buttons-and-hacking-panels'; 'Level-Ingredients' = 'level-ingredients'
    'Zones-and-Volumes' = 'zones-and-volumes'; 'SoftBody-Actors' = 'softbody-actors'; 'Animated-Meshes' = 'animated-meshes'; 'Fish' = 'fish'
    'Map-Tutorials' = 'map-tutorials'; 'Map-Packaging' = 'map-packaging'
}

New-Item -ItemType Directory -Force -Path $OutDirectory | Out-Null
$commit = $null
if (Get-Command git -ErrorAction SilentlyContinue) {
    $head = & git ls-remote 'https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor.wiki.git' HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $head) { $commit = ($head -split '\s+')[0] }
}
$sha = [System.Security.Cryptography.SHA256]::Create()
$manifest = [ordered]@{ fetched = (Get-Date).ToUniversalTime().ToString('o'); wikiCommit = $commit; pages = @() }
$fetched = @{}
foreach ($page in @('_Sidebar') + @($pages.Keys)) {
    # Parentheses in a page name are percent-encoded in the raw URL.
    $url = "$raw/$($page.Replace('(', '%28').Replace(')', '%29')).md"
    $file = Join-Path $OutDirectory "$page.md"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file
    $bytes = [System.IO.File]::ReadAllBytes($file)
    $hash = ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join ''
    $slug = if ($pages.Contains($page)) { $pages[$page] } else { $null }
    $manifest.pages += [ordered]@{ page = $page; slug = $slug; url = "$wiki/$page"; raw = $url; bytes = $bytes.Length; sha256 = $hash }
    $fetched[$page] = [System.Text.Encoding]::UTF8.GetString($bytes)
    Write-Host ("{0,-28} {1,7} bytes  {2}" -f $page, $bytes.Length, $hash.Substring(0, 12))
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDirectory 'manifest.json') -Encoding UTF8
Write-Host "Fetched $($manifest.pages.Count) pages into $OutDirectory (wiki commit $(if ($commit) { $commit } else { 'unknown' }))."

# Sidebar pages the digests do not cover yet.
foreach ($link in [regex]::Matches($fetched['_Sidebar'], '\]\(([^()#]+(?:\([^()]*\))?)\)')) {
    $name = $link.Groups[1].Value
    if (!$pages.Contains($name)) { Write-Warning "The sidebar links $name, which has no digest slug here." }
}

if ($Compare) {
    $missingTotal = 0
    foreach ($page in $pages.Keys) {
        $digestPath = Join-Path $digests "$($pages[$page]).md"
        if (!(Test-Path -LiteralPath $digestPath)) { Write-Warning "No digest for $page ($digestPath)."; $missingTotal++; continue }
        $digest = Get-Content -LiteralPath $digestPath -Raw -Encoding UTF8
        $text = $fetched[$page] -replace '<img[^>]*>', ''
        $names = [regex]::Matches($text, '`([^`\r\n]+)`') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
        $numbers = [regex]::Matches($text, '(?<![\w.#-])(\d+(?:\.\d+)?)(?!\w)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
        $missing = @($names | Where-Object { !$digest.Contains($_) })
        $missingNumbers = @($numbers | Where-Object { !$digest.Contains($_) })
        if (!$digest.Contains("Source: $wiki/$page")) { Write-Warning "$($pages[$page]).md has no 'Source: $wiki/$page' line." }
        if ($missing.Count -or $missingNumbers.Count) {
            Write-Host "$($pages[$page]): names not in the digest: $($missing -join ', ')"
            if ($missingNumbers.Count) { Write-Host "$($pages[$page]): numbers not in the digest: $($missingNumbers -join ', ')" }
            $missingTotal += $missing.Count
        }
    }
    Write-Host "$missingTotal wiki names are missing from the digests (a name the wiki itself misspells inside backticks, such as 'SFlaque.', is expected)."
}
