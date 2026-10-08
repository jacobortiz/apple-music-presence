[CmdletBinding()]
param(
    [string]$DestinationDirectory,
    [string]$ArchivePath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Net.Http
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not $DestinationDirectory) { $DestinationDirectory = $PSScriptRoot }

# Immutable public provider release; no GitHub sign-in or app credentials are used.
$releaseUrl = 'https://github.com/GyanD/codexffmpeg/releases/download/9.0.2/ffmpeg-9.0.2-essentials_build.zip'
$archiveSha256 = '60F467265B1E312373DBCD92200C2618A74850F98D3D078E94296BB3FA2047BA'
$binarySha256 = '3256173F3F8BFFD7DF12227C68ADF68025EDB1832273A9530688A7BB1ED8EDEC'
$archiveLength = 114768076L
$archiveRoot = 'ffmpeg-9.0.2-essentials_build/'

function Assert-DownloadUri([uri]$Uri, [bool]$Initial) {
    $hosts = @('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com')
    if ($Uri.Scheme -ne 'https' -or $Uri.Port -ne 443 -or $Uri.UserInfo -or $Uri.Fragment -or
        $hosts -notcontains $Uri.DnsSafeHost) {
        throw 'The provider returned an unsupported download address.'
    }
    if ($Uri.DnsSafeHost -eq 'github.com' -and $Uri.AbsolutePath -ne ([uri]$releaseUrl).AbsolutePath) {
        throw 'The provider returned an unexpected release address.'
    }
    if ($Initial -and $Uri.AbsoluteUri -ne $releaseUrl) { throw 'Unexpected initial download address.' }
}

function Download-Release([string]$OutputPath) {
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $handler.UseCookies = $false
    $handler.UseDefaultCredentials = $false
    $handler.Credentials = $null
    $handler.UseProxy = $false
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(180)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('AppleMusicPresence-MotionInstaller/1.0')
    $cancel = [System.Threading.CancellationTokenSource]::new()
    $cancel.CancelAfter(180000)
    $response = $null
    try {
        $downloadUri = [uri]$releaseUrl
        for ($redirect = 0; $redirect -le 4; ++$redirect) {
            Assert-DownloadUri $downloadUri ($redirect -eq 0)
            $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Get, $downloadUri)
            try {
                $response = $client.SendAsync($request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead,
                    $cancel.Token).GetAwaiter().GetResult()
            } finally { $request.Dispose() }
            if ([int]$response.StatusCode -in @(301, 302, 303, 307, 308)) {
                $location = $response.Headers.Location
                if ($null -eq $location -or $redirect -eq 4) { throw 'The provider redirected too many times.' }
                $downloadUri = [uri]::new($downloadUri, $location)
                $response.Dispose()
                $response = $null
                continue
            }
            if ([int]$response.StatusCode -ne 200) { throw 'The provider download was unavailable.' }
            $declaredLength = $response.Content.Headers.ContentLength
            if ($null -ne $declaredLength -and $declaredLength -ne $archiveLength) {
                throw 'The provider archive size changed; nothing was installed.'
            }
            $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
            $output = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $buffer = [byte[]]::new(81920)
                $downloaded = 0L
                while (($count = $stream.ReadAsync($buffer, 0, $buffer.Length, $cancel.Token).GetAwaiter().GetResult()) -gt 0) {
                    $downloaded += $count
                    if ($downloaded -gt $archiveLength) { throw 'The provider archive exceeded its size limit.' }
                    $output.Write($buffer, 0, $count)
                }
                if ($downloaded -ne $archiveLength) { throw 'The provider archive was incomplete.' }
            } finally {
                $output.Dispose()
                $stream.Dispose()
            }
            return
        }
        throw 'The provider download did not complete.'
    } catch {
        # Avoid displaying signed CDN query strings or underlying network details.
        throw 'The verified FFmpeg download failed or timed out. Try again later, or use -ArchivePath with the pinned provider ZIP.'
    } finally {
        if ($null -ne $response) { $response.Dispose() }
        $cancel.Dispose()
        $client.Dispose()
        $handler.Dispose()
    }
}

function Copy-ZipEntry($Zip, [string]$Name, [string]$Target, [long]$Limit, [long]$ExpectedLength = 0) {
    $entry = $Zip.GetEntry($Name)
    if ($null -eq $entry -or $entry.Length -le 0 -or $entry.Length -gt $Limit -or
        ($ExpectedLength -gt 0 -and $entry.Length -ne $ExpectedLength)) {
        throw 'The pinned archive is missing an expected file.'
    }
    # Extract only exact allowlisted entries to generated local names; never extract ZIP paths.
    $inputStream = $entry.Open()
    $outputStream = [IO.File]::Open($Target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
    if ((Get-Item -LiteralPath $Target).Length -ne $entry.Length) { throw 'Archive extraction was incomplete.' }
}

function Assert-X64Binary([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'The converter is not a Windows executable.' }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 92) { throw 'Invalid converter executable header.' }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw 'The converter is not a Windows x64 executable.'
        }
    } finally { $reader.Dispose(); $stream.Dispose() }
}

function Get-ConverterOutput([string]$Path, [string]$Arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Path
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables.Clear()
    foreach ($name in @('SystemRoot', 'WINDIR', 'TEMP', 'TMP')) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($value) { $info.EnvironmentVariables[$name] = $value }
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'The converter could not be checked.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'The converter check timed out.'
        }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -or $text.Length -gt 1048576) { throw 'The converter check failed.' }
        return $text
    } finally { $process.Dispose() }
}

function Install-VerifiedFile([string]$Source, [string]$Destination) {
    if ([IO.File]::Exists($Destination)) {
        # PowerShell 5.1 converts a null backup name to an invalid empty path.
        # Keep the backup inside our generated temporary directory instead.
        $backup = $Source + '.previous'
        [IO.File]::Replace($Source, $Destination, $backup)
        [IO.File]::Delete($backup)
    }
    else { [IO.File]::Move($Source, $Destination) }
}

$destination = [IO.Path]::GetFullPath($DestinationDirectory)
$null = [IO.Directory]::CreateDirectory($destination)
$targetBinary = Join-Path $destination 'ffmpeg.exe'
$notices = Join-Path $destination 'ffmpeg-notices'
if ([IO.File]::Exists($targetBinary) -and (Get-FileHash -LiteralPath $targetBinary -Algorithm SHA256).Hash -eq $binarySha256 -and
    [IO.File]::Exists((Join-Path $notices 'LICENSE')) -and [IO.File]::Exists((Join-Path $notices 'README.txt')) -and
    [IO.File]::Exists((Join-Path $notices 'PROVENANCE.txt'))) {
    Write-Host 'Verified motion support is already installed.'
    return
}
if ([IO.File]::Exists($targetBinary) -and (Get-FileHash -LiteralPath $targetBinary -Algorithm SHA256).Hash -ne $binarySha256 -and -not $Force) {
    throw 'A different ffmpeg.exe already exists. Keep it, or close the app and rerun this installer with -Force to replace it.'
}

$temporary = Join-Path $destination ('.motion-install-' + [guid]::NewGuid().ToString('N'))
$null = [IO.Directory]::CreateDirectory($temporary)
try {
    if ($ArchivePath) {
        $verifiedArchive = (Get-Item -LiteralPath $ArchivePath).FullName
    } else {
        Write-Host 'Downloading optional FFmpeg 9.0.2 from its public Windows build provider (about 109 MB)...'
        $verifiedArchive = Join-Path $temporary 'provider.zip'
        Download-Release $verifiedArchive
    }
    if ((Get-Item -LiteralPath $verifiedArchive).Length -ne $archiveLength -or
        (Get-FileHash -LiteralPath $verifiedArchive -Algorithm SHA256).Hash -ne $archiveSha256) {
        throw 'The FFmpeg archive checksum did not match. Nothing was installed.'
    }
    $zip = [IO.Compression.ZipFile]::OpenRead($verifiedArchive)
    try {
        Copy-ZipEntry $zip ($archiveRoot + 'bin/ffmpeg.exe') (Join-Path $temporary 'ffmpeg.exe') 120000000 105423872
        Copy-ZipEntry $zip ($archiveRoot + 'LICENSE') (Join-Path $temporary 'LICENSE') 131072 35147
        Copy-ZipEntry $zip ($archiveRoot + 'README.txt') (Join-Path $temporary 'README.txt') 1048576
    } finally { $zip.Dispose() }
    $temporaryBinary = Join-Path $temporary 'ffmpeg.exe'
    if ((Get-FileHash -LiteralPath $temporaryBinary -Algorithm SHA256).Hash -ne $binarySha256) {
        throw 'The FFmpeg executable checksum did not match. Nothing was installed.'
    }
    Assert-X64Binary $temporaryBinary
    if ((Get-ConverterOutput $temporaryBinary '-hide_banner -version') -notmatch 'ffmpeg version 9\.0\.2-essentials_build-www\.gyan\.dev') {
        throw 'The converter version check failed.'
    }
    if ((Get-ConverterOutput $temporaryBinary '-hide_banner -encoders') -notmatch '\blibwebp_anim\b') {
        throw 'The converter does not support animated WebP.'
    }
    $null = [IO.Directory]::CreateDirectory($notices)
    Install-VerifiedFile (Join-Path $temporary 'LICENSE') (Join-Path $notices 'LICENSE')
    Install-VerifiedFile (Join-Path $temporary 'README.txt') (Join-Path $notices 'README.txt')
    $provenance = @"
FFmpeg 9.0.2-essentials_build-www.gyan.dev (Windows x64, GPL v3)
Downloaded directly from the provider; this binary is not part of the app ZIP.
Release: https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2
Archive: $releaseUrl
Archive SHA256: $archiveSha256
ffmpeg.exe SHA256: $binarySha256
FFmpeg source: https://github.com/FFmpeg/FFmpeg/commit/946fcce07b
The provider README lists its configuration, dependency versions, and source links.
Keep this folder with the executable. The provider controls download availability.
"@
    [IO.File]::WriteAllText((Join-Path $temporary 'PROVENANCE.txt'), $provenance, [Text.UTF8Encoding]::new($false))
    Install-VerifiedFile (Join-Path $temporary 'PROVENANCE.txt') (Join-Path $notices 'PROVENANCE.txt')
    if (-not [IO.File]::Exists($targetBinary) -or (Get-FileHash -LiteralPath $targetBinary -Algorithm SHA256).Hash -ne $binarySha256) {
        Install-VerifiedFile $temporaryBinary $targetBinary
    }
    Write-Host 'Motion support installed. Restart Apple Music Presence, then enable animated covers in its settings.'
} finally {
    # Only remove the generated child directory after verifying its absolute scope.
    $resolvedTemporary = [IO.Path]::GetFullPath($temporary)
    $safePrefix = $destination.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if ($resolvedTemporary.StartsWith($safePrefix, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedTemporary) -match '^\.motion-install-[0-9a-f]{32}$' -and
        [IO.Directory]::Exists($resolvedTemporary)) {
        Remove-Item -LiteralPath $resolvedTemporary -Recurse -Force
    }
}
