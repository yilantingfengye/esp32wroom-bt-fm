# 后台抓串口日志：连接手机放歌后，用这个文件看蓝牙 PCM 有没有进来
$out = "f:\ESP32-WROOM\esp32-fm\esp32_max98357_fm\build\bt_capture.txt"
$secs = 300
if ($args.Count -gt 0) { $secs = [int]$args[0] }
try { Remove-Item $out -Force -ErrorAction SilentlyContinue } catch {}
$sp = New-Object System.IO.Ports.SerialPort "COM4",115200,"None",8,"One"
$sp.ReadTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sb = New-Object System.Text.StringBuilder
try {
    $sp.Open()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $secs) {
        $s = $sp.ReadExisting()
        if ($s) { [void]$sb.Append($s) }
        # 每 20 秒把当前计数器和关键日志刷一次到文件
        if ((([int]$sw.Elapsed.TotalSeconds) % 20) -eq 0) {
            $sp.Write("bt`r`n")
            Start-Sleep -Milliseconds 600
            [void]$sb.Append($sp.ReadExisting())
            [System.IO.File]::WriteAllText($out, $sb.ToString())
        }
        Start-Sleep -Milliseconds 120
    }
    [System.IO.File]::WriteAllText($out, $sb.ToString())
} catch {
    [System.IO.File]::WriteAllText($out, "capture error: $_`r`n" + $sb.ToString())
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
