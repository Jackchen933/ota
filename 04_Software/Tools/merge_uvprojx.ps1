# Merge screen-related groups (BSP/LVGL/UI/GUI Guider/UI/Assets) from the
# source project's uvprojx into the target project's uvprojx, and append
# include paths. NO BOM allowed in uvprojx (UV4 refuses to open it),
# so we write with UTF8Encoding($false).
$ErrorActionPreference = 'Stop'
$src = 'd:\lixin\2\jiemian1\MDK-ARM\jiemian1.uvprojx'
$dst = 'D:\lixin\1\OTA\04_Software\MDK-ARM\04_Software.uvprojx'

$srcText = [IO.File]::ReadAllText($src)
$dstText = [IO.File]::ReadAllText($dst)

$dstGroups = [regex]::Matches($dstText, '<GroupName>([^<]+)</GroupName>') |
             ForEach-Object { $_.Groups[1].Value }

$want = @('BSP','LVGL/core','LVGL/draw','LVGL/font','LVGL/misc','LVGL/widgets',
          'LVGL/themes','LVGL/others','UI','GUI Guider','UI/Assets')

$blocks = ''
$merged = @()
$groupRegex = [regex]'(?s)<Group>\s*<GroupName>([^<]+)</GroupName>.*?</Group>'
foreach ($m in $groupRegex.Matches($srcText)) {
    $name = $m.Groups[1].Value
    if ($want -contains $name -and ($dstGroups -notcontains $name)) {
        $blocks += "`r`n" + $m.Value
        $merged += $name
    }
}

if ($merged.Count -eq 0) { Write-Output 'nothing to merge (already merged?)'; exit 0 }

$dstText = $dstText -replace '(?s)(\s*</Groups>)', ($blocks + '$1')

$addPath = ';../Middlewares/lvgl;../UI;../UI/custom;../UI/generated;../UI/generated/screens;../UI/generated/events;../UI/assets/fonts;../UI/assets/images;../BSP/Inc'
$dstText = $dstText -replace '(\.\./Drivers/CMSIS/Include)</IncludePath>', ('$1' + $addPath + '</IncludePath>')

[IO.File]::WriteAllText($dst, $dstText, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ('merged groups: ' + ($merged -join ', '))
