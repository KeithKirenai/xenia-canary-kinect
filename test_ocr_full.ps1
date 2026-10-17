Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap(400, 100)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.Clear([System.Drawing.Color]::White)
$font = New-Object System.Drawing.Font("Arial", 24)
$brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)
$gfx.DrawString("IS ANYONE THERE?", $font, $brush, 10.0, 30.0)
$gfx.Dispose()
$bmp.Save("test_img.png")
$bmp.Dispose()

.\winrt_ocr.ps1 -ImagePath "test_img.png"
