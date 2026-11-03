# -*- coding: utf-8 -*-
# 用 Windows SAPI 生成 J280 手势语音（16 kHz / 单声道 / 16 bit）
# 用法：双击 make_voice_wavs.bat，或在本目录执行
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\make_voice_wavs.ps1
# 生成目录：.\voice\  —— 整个文件夹拷到 K230 的 /sdcard/j280_gesture/voice/

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Speech

$out = Join-Path $PSScriptRoot "voice"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
    16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)

$phrases = [ordered]@{
    "at_0"              = "已经在零度"
    "at_p90"            = "已经在正九十度"
    "at_m90"            = "已经在负九十度"
    "at_m45"            = "已经在负四十五度"

    "from_0_to_p90"     = "正在从零度转到正九十度"
    "from_p90_to_0"     = "正在从正九十度回到零度"
    "from_0_to_m90"     = "正在从零度转到负九十度"
    "from_m90_to_0"     = "正在从负九十度回到零度"
    "from_p90_to_m90"   = "正在从正九十度转到负九十度"
    "from_m90_to_p90"   = "正在从负九十度转到正九十度"

    "from_0_to_m45"     = "正在从零度转到负四十五度"
    "from_m45_to_0"     = "正在从负四十五度回到零度"
    "from_p90_to_m45"   = "正在从正九十度转到负四十五度"
    "from_m45_to_p90"   = "正在从负四十五度转到正九十度"
    "from_m90_to_m45"   = "正在从负九十度转到负四十五度"
    "from_m45_to_m90"   = "正在从负四十五度转到负九十度"
}

$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.Rate = 0
$synth.Volume = 100

# 优先选中文语音（微软慧慧），否则用默认
$zh = $synth.GetInstalledVoices() | Where-Object { $_.VoiceInfo.Culture.Name -like "zh*" } | Select-Object -First 1
if ($zh) {
    $synth.SelectVoice($zh.VoiceInfo.Name)
    Write-Host ("使用语音: " + $zh.VoiceInfo.Name)
} else {
    Write-Host "警告: 未找到中文语音，生成的 WAV 可能读不出中文"
}

foreach ($key in $phrases.Keys) {
    $path = Join-Path $out ($key + ".wav")
    $synth.SetOutputToWaveFile($path, $fmt)
    $synth.Speak($phrases[$key])
    $synth.SetOutputToNull()
    Write-Host ("OK  " + $key + ".wav")
}

$synth.Dispose()
Write-Host ""
Write-Host ("输出目录: " + $out)
Write-Host "把整个 voice 文件夹拷贝到 K230 的 /sdcard/j280_gesture/voice/"
