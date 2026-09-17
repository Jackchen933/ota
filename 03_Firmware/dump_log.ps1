$b = [System.IO.File]::ReadAllBytes('d:\lixin\1\OTA\03_Firmware\session.log')
Write-Output ("TotalBytes: " + $b.Length)
# 输出为 hex, 每行32字节
$sb = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $b.Length; $i++) {
    [void]$sb.Append($b[$i].ToString('X2'))
    [void]$sb.Append(' ')
    if (($i + 1) % 32 -eq 0) { [void]$sb.Append("`n") }
}
[System.IO.File]::WriteAllText('d:\lixin\1\OTA\03_Firmware\session_hex.txt', $sb.ToString())
Write-Output "Hex written to session_hex.txt"
