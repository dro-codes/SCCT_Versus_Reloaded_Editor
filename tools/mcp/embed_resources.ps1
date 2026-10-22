param(
    [string]$Output,
    [string]$Resources
)
# Compiles Reloaded.Editor.Mcp/Resources/** into Reloaded.Editor.Mcp/Embedded.gen.h.
#
# The PowerShell twin of embed_resources.py, for the Visual Studio build: the
# Reloaded.Editor.Mcp project runs it before compiling, on Windows PowerShell
# 5.1, so building needs no Python. The two write byte-identical headers (LF
# line endings on every OS) and stop on the same problems; change them
# together. tools/test_mcp.sh and CI compare their output.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/mcp/embed_resources.ps1 [-Output <header>] [-Resources <folder>]
#
# The header is written only when its content changed, so an unchanged
# resource set does not rebuild the project. See embed_resources.py for the
# format.
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$Resources) { $Resources = Join-Path $repository 'Reloaded.Editor.Mcp/Resources' }
if (!$Output) { $Output = Join-Path $repository 'Reloaded.Editor.Mcp/Embedded.gen.h' }
# [System.IO.File] resolves relative paths against the process directory, not
# PowerShell's current location.
$Resources = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Resources)
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)

$mime = @{
    '.md' = 'text/markdown'
    '.json' = 'application/json'
    '.png' = 'image/png'
    '.txt' = 'text/plain'
    '.csv' = 'text/csv'
    '.svg' = 'image/svg+xml'
    '.jpg' = 'image/jpeg'
    '.jpeg' = 'image/jpeg'
    '.bmp' = 'image/bmp'
    '.tga' = 'image/x-tga'
}
$text = @('.md', '.json', '.txt', '.csv', '.svg')
$bytesPerLine = 24
$utf8 = New-Object System.Text.UTF8Encoding($false, $true)

function Stop-Embedding([string]$message) {
    [Console]::Error.WriteLine($message)
    exit 1
}

# Adds (relative path, mime, bytes) for every file under $folder to $files.
# Hidden files count, as they do for the Python script; names starting with a
# dot, and backups ending in ~, do not.
function Get-ResourceFiles([string]$folder, [string]$prefix, [System.Collections.Generic.List[object]]$files) {
    $names = [System.IO.Directory]::GetFiles($folder) | ForEach-Object { [System.IO.Path]::GetFileName($_) }
    foreach ($name in $names) {
        if ($name.StartsWith('.') -or $name.EndsWith('~')) { continue }
        $relative = $prefix + $name
        $dot = $name.LastIndexOf('.')
        $extension = ''
        if ($dot -gt 0) { $extension = $name.Substring($dot).ToLowerInvariant() }
        if (!$mime.ContainsKey($extension)) {
            $what = $extension
            if (!$what) { $what = 'files without an extension' }
            Stop-Embedding "${relative}: no MIME type for $what; add it to MIME in embed_resources.py and embed_resources.ps1."
        }
        $data = [System.IO.File]::ReadAllBytes((Join-Path $folder $name))
        if ($text -contains $extension) {
            try { $content = $utf8.GetString($data) }
            catch { Stop-Embedding "${relative}: not UTF-8 ($($_.Exception.InnerException.Message))." }
            if ($content.Contains("`r`n")) {
                $content = $content.Replace("`r`n", "`n")
                $data = $utf8.GetBytes($content)
            }
            if ($extension -eq '.json') {
                # ConvertFrom-Json accepts a BOM and an empty file; json.loads does not.
                # It is laxer elsewhere (a trailing comma), and stricter about keys
                # differing only in case; CI compares the two scripts' output.
                if ($content.Length -gt 0 -and $content[0] -eq [char]0xFEFF) { Stop-Embedding "${relative}: invalid JSON (Unexpected UTF-8 BOM)." }
                if (!$content.Trim()) { Stop-Embedding "${relative}: invalid JSON (no value)." }
                try { $null = ConvertFrom-Json -InputObject $content }
                catch { Stop-Embedding "${relative}: invalid JSON ($($_.Exception.Message))." }
            }
        }
        foreach ($character in $relative.ToCharArray()) {
            if ([int]$character -gt 127 -or $character -eq '"' -or $character -eq '\') { Stop-Embedding "${relative}: resource paths must be plain ASCII." }
        }
        $files.Add(@($relative, $mime[$extension], $data))
    }
    $directories = [string[]]@([System.IO.Directory]::GetDirectories($folder) | ForEach-Object { [System.IO.Path]::GetFileName($_) })
    [System.Array]::Sort($directories, [System.StringComparer]::Ordinal)
    foreach ($name in $directories) {
        if ($name.StartsWith('.')) { continue }
        Get-ResourceFiles (Join-Path $folder $name) ($prefix + $name + '/') $files
    }
}

$files = New-Object System.Collections.Generic.List[object]
Get-ResourceFiles $Resources '' $files
# Byte order of the UTF-8 paths: the paths are ASCII, so ordinal order.
$files.Sort([System.Comparison[object]] { param($left, $right) [string]::CompareOrdinal($left[0], $right[0]) })
$sorted = $files.ToArray()

$hex = New-Object 'string[]' 256
for ($value = 0; $value -lt 256; $value++) { $hex[$value] = '0x' + $value.ToString('x2') }
$total = 0
foreach ($file in $sorted) { $total += $file[2].Length }

$out = New-Object System.Text.StringBuilder (6 * $total + 4096)
$null = $out.Append("#pragma once`n")
$null = $out.Append("// Generated by tools/mcp/embed_resources.ps1 (the build) or embed_resources.py from Reloaded.Editor.Mcp/Resources; do not edit.`n")
$null = $out.Append("// $($sorted.Count) files, $total bytes. Not committed: the Reloaded.Editor.Mcp build writes it before compiling.`n")
$null = $out.Append("#include `"Embedded.h`"`n`nnamespace Mcp`n{`n    inline const std::vector<EmbeddedFile>& EmbeddedFiles()`n    {`n")
for ($index = 0; $index -lt $sorted.Count; $index++) {
    # A trailing zero, not counted in size, keeps empty files legal and text usable as a C string.
    $data = $sorted[$index][2]
    $payload = New-Object 'byte[]' ($data.Length + 1)
    [System.Array]::Copy($data, $payload, $data.Length)
    # One lookup for the whole file, then one Join per line: a statement per
    # byte is too slow on Windows PowerShell. The last line has no comma.
    $bytes = [string[]]$hex[$payload]
    $null = $out.Append("        // $($sorted[$index][0])`n        static const unsigned char f$index[] = {`n")
    $last = $payload.Length - 1 - ($payload.Length - 1) % $bytesPerLine
    for ($start = 0; $start -lt $last; $start += $bytesPerLine) {
        $null = $out.Append('            ').Append([string]::Join(',', $bytes, $start, $bytesPerLine)).Append(",`n")
    }
    $null = $out.Append('            ').Append([string]::Join(',', $bytes, $last, $payload.Length - $last)).Append("`n")
    $null = $out.Append("        };`n")
}
$null = $out.Append("        static const std::vector<EmbeddedFile> files{`n")
for ($index = 0; $index -lt $sorted.Count; $index++) {
    $null = $out.Append("            {`"$($sorted[$index][0])`", `"$($sorted[$index][1])`", f$index, $($sorted[$index][2].Length)},`n")
}
$null = $out.Append("        };`n        return files;`n    }`n}`n")
$header = (New-Object System.Text.UTF8Encoding($false)).GetBytes($out.ToString())

$shown = $Output
if ($Output.StartsWith($repository + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::Ordinal)) { $shown = $Output.Substring($repository.Length + 1) }
# Compared as bytes, so a header with CRLF line endings (an old checkout's) is
# rewritten; as Base64 strings, since Windows PowerShell has no one-call
# comparison of two byte arrays.
if ([System.IO.File]::Exists($Output)) {
    $current = [System.IO.File]::ReadAllBytes($Output)
    if ($current.Length -eq $header.Length -and [System.Convert]::ToBase64String($current) -ceq [System.Convert]::ToBase64String($header)) {
        Write-Output "$shown unchanged ($($sorted.Count) files)."
        exit 0
    }
}
[System.IO.File]::WriteAllBytes($Output, $header)
Write-Output "Wrote ${shown}: $($sorted.Count) files, $total bytes."
