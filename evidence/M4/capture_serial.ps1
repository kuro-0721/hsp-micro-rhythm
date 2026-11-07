# capture_serial.ps1 —— 抓 COM9 串口日志到文件，跑满指定秒数后自动退出
# 用法：powershell -NoProfile -File capture_serial.ps1 -Seconds 700 -Out "xxx.log"
param(
    [int]$Seconds = 700,
    [string]$Out = "capture.log",
    [string]$Port = "COM9",
    [int]$Baud = 1000000
)

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = $Baud
$sp.DataBits = 8
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.ReadTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false

$sp.Open()

# 抓取开始前先拉一次 RTS 复位脉冲，让日志里带上启动横幅
# （这样能证明采到的这段里没有发生过重启：[AUTO]/[HEAP] 时间戳必须单调，
#  且 "SiFli Corporation" 横幅只应出现一次）
$sp.RtsEnable = $true
Start-Sleep -Milliseconds 150
$sp.RtsEnable = $false
Start-Sleep -Milliseconds 100

$sw = New-Object System.IO.StreamWriter($Out, $false, (New-Object System.Text.UTF8Encoding($false)))
$sw.AutoFlush = $true

$deadline = (Get-Date).AddSeconds($Seconds)
$buf = New-Object char[] 4096

while ((Get-Date) -lt $deadline) {
    try {
        $n = $sp.Read($buf, 0, $buf.Length)
        if ($n -gt 0) {
            $sw.Write($buf, 0, $n)
        }
    } catch [TimeoutException] {
        # 正常：这段时间没有数据
    } catch {
        $sw.WriteLine("")
        $sw.WriteLine("[capture] ERROR: " + $_.Exception.Message)
        break
    }
}

$sw.Flush()
$sw.Close()
$sp.Close()
$sp.Dispose()
Write-Output ("capture done: " + $Out + "  " + (Get-Item $Out).Length + " bytes")
