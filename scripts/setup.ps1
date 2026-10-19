<#
.SYNOPSIS
  Downloads everything Tony needs next to tony.exe: llama.cpp, whisper.cpp, stable-diffusion.cpp, Piper, aria2,
  a Whisper speech model and a Piper voice. Optionally a chat model too.

.DESCRIPTION
  Uses the official prebuilt Windows binaries from the upstream GitHub releases (no compiler needed) and
  curl.exe, which ships with Windows 10/11 (resumes interrupted downloads).
  Safe to re-run: files that are already present are skipped.

.EXAMPLE
  .\scripts\setup.ps1                                   # Vulkan engines (Intel Arc, AMD, NVIDIA), Whisper + voice
.EXAMPLE
  .\scripts\setup.ps1 -Gpu sycl                         # Intel oneAPI/SYCL build of llama.cpp (needs the Intel oneAPI runtime)
.EXAMPLE
  .\scripts\setup.ps1 -ChatModelRepo ggml-org/gpt-oss-20b-GGUF -ChatModelPattern 'MXFP4'
#>
[CmdletBinding()]
param(
  [string]$Root = '',                                   # folder that contains tony.exe (default: parent of this script)
  [ValidateSet('vulkan', 'sycl', 'openvino', 'cpu')]
  [string]$Gpu = 'vulkan',
  [string]$LlamaTag = 'b11690',                         # the version Tony was developed and tested against; 'latest' also works
  [string]$WhisperTag = 'v1.9.2',                       # newest whisper.cpp release that ships Windows binaries
  [string]$SdTag = 'master-ff4fdbb',
  [string]$WhisperModel = 'ggml-large-v3-turbo-q5_0.bin',
  [string]$PiperVoice = 'en/en_US/lessac/medium/en_US-lessac-medium',   # path inside rhasspy/piper-voices
  [string]$ChatModelRepo = '',                          # e.g. ggml-org/gpt-oss-20b-GGUF
  [string]$ChatModelPattern = 'MXFP4|Q4_K_M',           # regex that selects the .gguf file in that repo
  [string]$MmprojPattern = '',                          # for vision models, e.g. 'mmproj.*f16'
  [switch]$SkipLlama, [switch]$SkipWhisper, [switch]$SkipImage, [switch]$SkipVoice, [switch]$SkipTorrent
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest progress makes downloads 10x slower in Windows PowerShell 5.1
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
$Root = (Resolve-Path $Root).Path
Write-Host "Tony folder: $Root" -ForegroundColor Cyan
if ($Root -match '[^\x00-\x7F]') {
  Write-Warning "The path contains non-ASCII characters. Some engines (stable-diffusion.cpp, Piper) cannot open such paths. Consider moving Tony to e.g. C:\Tony."
}

$engines = Join-Path $Root 'engines'
$models = Join-Path $Root 'models'
$tts = Join-Path $Root 'tts'
$tools = Join-Path $Root 'tools'
foreach ($d in @($engines, "$engines\llama", "$engines\whisper", "$engines\sd", $tts, $tools, "$models\llm", "$models\stt", "$models\tts", "$models\image")) {
  New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$tmp = Join-Path $env:TEMP ('tony-setup-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

$results = [ordered]@{}

function Get-File([string]$Url, [string]$Dest) {
  Write-Host "  downloading $(Split-Path -Leaf $Dest) ..." -ForegroundColor DarkGray
  & curl.exe -L --fail --silent --show-error --retry 3 --retry-delay 2 -C - -o $Dest $Url
  if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
}

# Release asset names come from the public GitHub pages: no API key, no rate limit.
function Get-ReleaseTags([string]$Repo) {
  $html = (Invoke-WebRequest -UseBasicParsing "https://github.com/$Repo/releases").Content
  [regex]::Matches($html, 'releases/expanded_assets/([^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique -First 8
}

function Get-ReleaseAssets([string]$Repo, [string]$Tag) {
  $html = (Invoke-WebRequest -UseBasicParsing "https://github.com/$Repo/releases/expanded_assets/$Tag").Content
  [regex]::Matches($html, 'releases/download/[^/]+/([^"]+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
}

# $Tag may be 'latest': then the newest release that actually contains a matching asset is used
# (llama.cpp, for example, keeps a marker release called "latest" that has no binaries).
function Install-Release([string]$Repo, [string]$Tag, [string]$Pattern, [string]$Dest, [string]$MustHave) {
  $tags = if ($Tag -eq 'latest') { @(Get-ReleaseTags $Repo) } else { @($Tag) }
  $asset = $null; $usedTag = $null; $seen = @()
  foreach ($t in $tags) {
    $names = @(Get-ReleaseAssets $Repo $t)
    $seen += $names
    $asset = $names | Where-Object { $_ -match $Pattern } | Select-Object -First 1
    if ($asset) { $usedTag = $t; break }
  }
  if (-not $asset) { throw "No asset matching '$Pattern' in $Repo ($($tags -join ', ')). Seen: $((($seen | Select-Object -Unique) -join ', '))" }
  $zip = Join-Path $tmp $asset
  Get-File "https://github.com/$Repo/releases/download/$usedTag/$asset" $zip
  $x = Join-Path $tmp ([IO.Path]::GetFileNameWithoutExtension($asset))
  if (Test-Path $x) { Remove-Item -Recurse -Force $x }
  Expand-Archive -Path $zip -DestinationPath $x -Force
  # some archives wrap everything in one folder (whisper: Release\)
  $top = Get-ChildItem $x
  $src = if ($top.Count -eq 1 -and $top[0].PSIsContainer) { $top[0].FullName } else { $x }
  Copy-Item -Path (Join-Path $src '*') -Destination $Dest -Recurse -Force
  if ($MustHave -and -not (Test-Path (Join-Path $Dest $MustHave))) { throw "$MustHave is missing after extracting $asset" }
  return "$usedTag  ($asset)"
}

function Step([string]$Name, [scriptblock]$Do) {
  Write-Host "`n== $Name" -ForegroundColor Cyan
  try { $results[$Name] = & $Do; Write-Host "   OK" -ForegroundColor Green }
  catch { $results[$Name] = "FAILED: $($_.Exception.Message)"; Write-Host "   $($results[$Name])" -ForegroundColor Red }
}

# ------------------------------------------------------------------ engines
if (-not $SkipLlama) {
  Step 'llama.cpp (language + vision models)' {
    if (Test-Path "$engines\llama\llama-server.exe") { return 'already installed' }
    $pat = switch ($Gpu) {
      'vulkan'   { '^llama-.*-bin-win-vulkan-x64\.zip$' }
      'sycl'     { '^llama-.*-bin-win-sycl-x64\.zip$' }
      'openvino' { '^llama-.*-bin-win-openvino-.*-x64\.zip$' }
      default    { '^llama-.*-bin-win-cpu-x64\.zip$' }
    }
    Install-Release 'ggml-org/llama.cpp' $LlamaTag $pat "$engines\llama" 'llama-server.exe'
  }
}
if (-not $SkipWhisper) {
  Step 'whisper.cpp (speech recognition, CPU)' {
    if (Test-Path "$engines\whisper\whisper-server.exe") { return 'already installed' }
    Install-Release 'ggml-org/whisper.cpp' $WhisperTag '^whisper-bin-x64\.zip$' "$engines\whisper" 'whisper-server.exe'
  }
  Step "Whisper model $WhisperModel" {
    $dest = "$models\stt\$WhisperModel"
    if (-not (Test-Path $dest)) { Get-File "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$WhisperModel" $dest }
    $dest
  }
}
if (-not $SkipImage) {
  Step 'stable-diffusion.cpp (image generation and editing)' {
    if (Test-Path "$engines\sd\sd.exe") { return 'already installed' }
    $pat = if ($Gpu -eq 'cpu') { '^sd-.*-bin-win-avx2-x64\.zip$' } else { '^sd-.*-bin-win-vulkan-x64\.zip$' }
    Install-Release 'leejet/stable-diffusion.cpp' $SdTag $pat "$engines\sd" 'sd.exe'
  }
}
if (-not $SkipVoice) {
  Step 'Piper (offline neural voices)' {
    if (Test-Path "$tts\piper\piper.exe") { return 'already installed' }
    $zip = Join-Path $tmp 'piper.zip'
    Get-File 'https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip' $zip
    Expand-Archive -Path $zip -DestinationPath $tts -Force
    if (-not (Test-Path "$tts\piper\piper.exe")) { throw 'piper.exe missing after extraction' }
    '2023.11.14-2'
  }
  Step "Piper voice $(Split-Path -Leaf $PiperVoice)" {
    $leaf = Split-Path -Leaf $PiperVoice
    foreach ($ext in '.onnx', '.onnx.json') {
      $dest = "$models\tts\$leaf$ext"
      if (-not (Test-Path $dest)) { Get-File "https://huggingface.co/rhasspy/piper-voices/resolve/main/$PiperVoice$ext" $dest }
    }
    "$models\tts\$leaf.onnx"
  }
}
if (-not $SkipTorrent) {
  Step 'aria2 (optional, for the torrent tool)' {
    if (Test-Path "$tools\aria2c.exe") { return 'already installed' }
    $zip = Join-Path $tmp 'aria2.zip'
    Get-File 'https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip' $zip
    $x = Join-Path $tmp 'aria2'
    Expand-Archive -Path $zip -DestinationPath $x -Force
    Copy-Item (Get-ChildItem $x -Recurse -Filter aria2c.exe | Select-Object -First 1).FullName "$tools\aria2c.exe" -Force
    '1.37.0'
  }
}

# ------------------------------------------------------------------ optional chat model
$chatPath = ''; $mmprojPath = ''
if ($ChatModelRepo) {
  Step "Chat model from $ChatModelRepo" {
    $tree = Invoke-RestMethod "https://huggingface.co/api/models/$ChatModelRepo/tree/main"
    $file = $tree | Where-Object { $_.type -eq 'file' -and $_.path -like '*.gguf' -and $_.path -notmatch 'mmproj' -and $_.path -match $ChatModelPattern } | Select-Object -First 1
    if (-not $file) { throw "No .gguf file matching '$ChatModelPattern'. Files: $(($tree | ForEach-Object path) -join ', ')" }
    $script:chatPath = "$models\llm\$(Split-Path -Leaf $file.path)"
    if (-not (Test-Path $script:chatPath)) {
      Write-Host "  this is a large download ($([math]::Round($file.lfs.size / 1GB, 1)) GB); it resumes if interrupted" -ForegroundColor Yellow
      Get-File "https://huggingface.co/$ChatModelRepo/resolve/main/$($file.path)?download=true" $script:chatPath
    }
    if ($MmprojPattern) {
      $mm = $tree | Where-Object { $_.type -eq 'file' -and $_.path -match 'mmproj' -and $_.path -match $MmprojPattern } | Select-Object -First 1
      if ($mm) {
        $script:mmprojPath = "$models\llm\$(Split-Path -Leaf $mm.path)"
        if (-not (Test-Path $script:mmprojPath)) { Get-File "https://huggingface.co/$ChatModelRepo/resolve/main/$($mm.path)?download=true" $script:mmprojPath }
      }
    }
    $script:chatPath
  }
}

# ------------------------------------------------------------------ write what we found into Tony's settings
Step 'Tony settings' {
  $dir = Join-Path $env:APPDATA 'Tony'
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $file = Join-Path $dir 'settings.json'
  $utf8 = New-Object Text.UTF8Encoding($false)   # Tony reads UTF-8 without BOM; PowerShell 5.1 defaults differ, so be explicit
  $s = if (Test-Path $file) { [IO.File]::ReadAllText($file, $utf8) | ConvertFrom-Json } else { [pscustomobject]@{} }
  function Ensure($obj, $name, $default) { if (-not ($obj.PSObject.Properties.Name -contains $name)) { $obj | Add-Member -NotePropertyName $name -NotePropertyValue $default }; $obj.$name }
  $whisper = Ensure $s 'whisper' ([pscustomobject]@{})
  $wm = "$models\stt\$WhisperModel"
  if ((Test-Path $wm) -and -not $whisper.modelPath) { Ensure $whisper 'modelPath' '' | Out-Null; $whisper.modelPath = $wm }
  $ttsS = Ensure $s 'tts' ([pscustomobject]@{})
  $voice = "$models\tts\$(Split-Path -Leaf $PiperVoice).onnx"
  if ((Test-Path $voice) -and -not $ttsS.piperVoice) { Ensure $ttsS 'piperVoice' '' | Out-Null; $ttsS.piperVoice = $voice }
  if ($chatPath) {
    $llama = Ensure $s 'llama' ([pscustomobject]@{})
    $list = @(Ensure $llama 'models' @())
    $name = [IO.Path]::GetFileNameWithoutExtension($chatPath)
    if (-not ($list | Where-Object { $_.name -eq $name })) {
      $entry = [pscustomobject]@{ name = $name; role = 'chat'; path = $chatPath; mmproj = $mmprojPath }
      $llama.models = @($list) + @($entry)
    }
    Ensure $llama 'activeChat' '' | Out-Null
    if (-not $llama.activeChat) { $llama.activeChat = $name }
  }
  [IO.File]::WriteAllText($file, ($s | ConvertTo-Json -Depth 20), $utf8)
  $file
}

Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

Write-Host "`n================ summary ================" -ForegroundColor Cyan
foreach ($k in $results.Keys) {
  $ok = -not ("$($results[$k])" -like 'FAILED*')
  Write-Host ("{0,-46} {1}" -f $k, $results[$k]) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
if (-not $ChatModelRepo) {
  Write-Host "`nNext: start Tony, open Settings > Models and download or select a chat model (a GGUF file)." -ForegroundColor Yellow
  Write-Host "Example for your hardware:  .\scripts\setup.ps1 -ChatModelRepo ggml-org/gpt-oss-20b-GGUF" -ForegroundColor Yellow
}
