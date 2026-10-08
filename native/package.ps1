[CmdletBinding()]
param(
    [ValidatePattern('^native-v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9]+(?:\.[A-Za-z0-9]+)*)?$')]
    [string]$Version = 'native-v0.1.0-preview.1',
    [string]$AppPath,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
if (-not $AppPath) { $AppPath = Join-Path $PSScriptRoot '..\build\native\Release\AppleMusicPresenceNative.exe' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $PSScriptRoot '..\dist\native' }

function Assert-PublicText([string]$Value) {
    if ($Value -match '(?i)[a-z]:[\\/](?:Users|Documents and Settings)[\\/]') {
        throw 'Packaging refused: a private Windows profile path was found. Rebuild without local paths.'
    }
}

function Assert-NativeBinary([string]$Path) {
    if ([IO.Path]::GetFileName($Path) -cne 'AppleMusicPresenceNative.exe') {
        throw 'Package only the named native app executable, not a test program or converter.'
    }
    if ((Get-Item -LiteralPath $Path).Length -gt 20000000) { throw 'The native app executable exceeds its size limit.' }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256 -or $bytes.Length -gt 20000000 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) {
        throw 'The native app executable has an invalid size or Windows header.'
    }
    $peOffset = [BitConverter]::ToUInt32($bytes, 0x3C)
    if ($peOffset -lt 64 -or $peOffset -gt $bytes.Length - 94 -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($bytes, $peOffset + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $peOffset + 24) -ne 0x020B -or
        [BitConverter]::ToUInt16($bytes, $peOffset + 24 + 68) -ne 2) {
        throw 'The package requires the Windows x64 GUI app build.'
    }
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $unicode = [Text.Encoding]::Unicode.GetString($bytes, 0, $bytes.Length - ($bytes.Length % 2))
    $unicodeOdd = [Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1 - (($bytes.Length - 1) % 2))
    Assert-PublicText $ascii
    Assert-PublicText $unicode
    Assert-PublicText $unicodeOdd
    if (-not $unicode.Contains('AppleMusicPresence.Native.Window') -and
        -not $unicodeOdd.Contains('AppleMusicPresence.Native.Window')) {
        throw 'The executable does not contain the native tray application marker.'
    }
    return ,$bytes
}

$resolvedApp = (Get-Item -LiteralPath $AppPath).FullName
$validatedApp = Assert-NativeBinary $resolvedApp
$output = [IO.Path]::GetFullPath($OutputDirectory)
$null = [IO.Directory]::CreateDirectory($output)
$archiveName = 'AppleMusicPresence-' + $Version + '-windows-x64.zip'
$archivePath = Join-Path $output $archiveName
$temporaryArchive = Join-Path $output ('.package-' + [guid]::NewGuid().ToString('N') + '.tmp')

$readme = @"
Apple Music Presence - $Version (Windows x64 preview)

SETUP
1. Extract this ZIP into a permanent folder. Open AppleMusicPresenceNative.exe.
2. Open the music-note icon in the Windows notification area for Settings.
   Enter your Discord Application ID. Keep Apple Music and Discord desktop open.
   Create an Apple Music application at https://discord.com/developers/applications
   and copy its Application ID from General Information. No Discord token is needed.
3. Enable normal artwork if wanted, then Save. Closing Settings keeps sharing.
   Right-click the tray icon to pause/resume sharing or exit.

Use Windows 10 1809 or newer, x64. No Python, compiler, or admin install is needed.
The icon may be in the hidden-icons menu. Run only one presence app for your ID.
Start with Windows is optional. Disable it before moving the extracted folder.

OPTIONAL NEW ANIMATED COVERS
Close the app, then double-click Install-MotionSupport.cmd once. It downloads
and verifies FFmpeg 9.0.2 (about 109 MB) directly from the public Windows build
provider. Restart the app, enable Prefer animated covers, and enter a public
owner/repository plus write access. Existing Git for Windows credential-helper
sign-in works; otherwise use APPLE_MUSIC_PRESENCE_GITHUB_TOKEN with Contents:
read and write. Never put a token in settings, this folder, or Git.
Normal covers and already hosted animations do not require the converter.

PRIVACY
Preferences and artwork cache stay in %LOCALAPPDATA%\AppleMusicPresence.
Artwork opt-in sends track tags to Apple's public catalog. Animated-cover opt-in
uploads cover images and album IDs to the chosen public repository's
motion-artwork branch. Prepared albums and upload times become public.
No Apple password, Discord token, cookies, local music files, telemetry, or
per-song history are collected. This ZIP contains no personal settings or cache.

The app uses Discord's local Rich Presence; Discord controls image caching and
animation display. Keep THIRD_PARTY_NOTICES.txt and licenses with the app.
If installed, keep ffmpeg-notices with the optional converter.

Docs and source: https://github.com/jacobortiz/apple-music-presence/tree/$Version/native
FFmpeg provider: https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2
"@
Assert-PublicText $readme

# This explicit allowlist deliberately excludes build folders, PDBs, tests,
# converters, settings, caches, credential files, and local development output.
$files = [ordered]@{
    'AppleMusicPresenceNative.exe' = $resolvedApp
    'Install-MotionSupport.ps1' = Join-Path $PSScriptRoot 'Install-MotionSupport.ps1'
    'Install-MotionSupport.cmd' = Join-Path $PSScriptRoot 'Install-MotionSupport.cmd'
    'THIRD_PARTY_NOTICES.txt' = Join-Path $PSScriptRoot '..\THIRD_PARTY_NOTICES.txt'
    'licenses/nlohmann-json-MIT.txt' = Join-Path $PSScriptRoot 'vendor\nlohmann\LICENSE.MIT'
}
foreach ($file in $files.Values) {
    if (-not [IO.File]::Exists($file)) { throw 'A required public packaging resource is missing.' }
}

try {
    $zipStream = [IO.File]::Open($temporaryArchive, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $zip = [IO.Compression.ZipArchive]::new($zipStream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        foreach ($name in $files.Keys) {
            if ($name -eq 'AppleMusicPresenceNative.exe') {
                # Archive the exact bytes checked above, even if another build starts.
                $binaryEntry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
                $binaryStream = $binaryEntry.Open()
                try { $binaryStream.Write($validatedApp, 0, $validatedApp.Length) } finally { $binaryStream.Dispose() }
            } else {
                $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $files[$name], $name,
                    [IO.Compression.CompressionLevel]::Optimal)
            }
        }
        $entry = $zip.CreateEntry('README.txt', [IO.Compression.CompressionLevel]::Optimal)
        $writer = [IO.StreamWriter]::new($entry.Open(), [Text.UTF8Encoding]::new($false))
        try { $writer.Write($readme) } finally { $writer.Dispose() }
    } finally { $zip.Dispose(); $zipStream.Dispose() }
    $verifiedZip = [IO.Compression.ZipFile]::OpenRead($temporaryArchive)
    try {
        $expectedEntries = @($files.Keys) + @('README.txt')
        if ($verifiedZip.Entries.Count -ne $expectedEntries.Count) { throw 'Unexpected package entries.' }
        foreach ($entry in $verifiedZip.Entries) {
            if ($expectedEntries -cnotcontains $entry.FullName) { throw 'Unexpected package entry.' }
        }
    } finally { $verifiedZip.Dispose() }
    if ([IO.File]::Exists($archivePath)) {
        [IO.File]::Replace($temporaryArchive, $archivePath, ($temporaryArchive + '.previous'))
        [IO.File]::Delete($temporaryArchive + '.previous')
    }
    else { [IO.File]::Move($temporaryArchive, $archivePath) }
    $digest = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText((Join-Path $output 'SHA256SUMS.txt'), "$digest  $archiveName`n", [Text.UTF8Encoding]::new($false))
    Write-Host ('Created ' + $archiveName + ' with ' + $expectedEntries.Count + ' allowlisted entries.')
    Write-Host ('SHA256 ' + $digest)
} finally {
    if ([IO.File]::Exists($temporaryArchive)) { [IO.File]::Delete($temporaryArchive) }
    if ([IO.File]::Exists($temporaryArchive + '.previous')) { [IO.File]::Delete($temporaryArchive + '.previous') }
}
