$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$sourceFiles = Get-ChildItem (Join-Path $root 'src'), (Join-Path $root 'include') `
    -Recurse -File -Include '*.cpp', '*.h'
$sourceText = ($sourceFiles | ForEach-Object {
    Get-Content -Raw -Encoding UTF8 $_.FullName
}) -join "`n"
$characters = [regex]::Matches($sourceText, '[\u3400-\u9FFF\u3000-\u303F\uFF00-\uFFEF]') |
    ForEach-Object Value | Sort-Object -Unique

$output = Join-Path $root 'include\font\chinese_fallback_font.h'
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('#pragma once')
$lines.Add('')
$lines.Add('#include <stdint.h>')
$lines.Add('')
$lines.Add('namespace ChineseFallbackFont {')
$lines.Add('')
$lines.Add('struct Glyph { uint32_t codepoint; uint8_t bitmap[128]; };')
$lines.Add('')
$lines.Add('static const Glyph GLYPHS[] = {')

$font = [System.Drawing.Font]::new('SimHei', 28, [System.Drawing.FontStyle]::Regular,
                                  [System.Drawing.GraphicsUnit]::Pixel)
$format = [System.Drawing.StringFormat]::GenericTypographic
$format.FormatFlags = $format.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

foreach ($character in $characters) {
    # Render generously first, measure the actual ink bounds, then normalize
    # every character into a centered 14x14 cell. Drawing directly into 16x16
    # clipped descenders and left medium-alpha edges that the firmware drops.
    $sourceBitmap = [System.Drawing.Bitmap]::new(40, 40, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($sourceBitmap)
    $graphics.Clear([System.Drawing.Color]::White)
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.DrawString($character, $font, [System.Drawing.Brushes]::Black, 0.0, 0.0, $format)
    $graphics.Dispose()

    $left = 40; $top = 40; $right = -1; $bottom = -1
    for ($y = 0; $y -lt 40; $y++) {
        for ($x = 0; $x -lt 40; $x++) {
            if ($sourceBitmap.GetPixel($x, $y).R -lt 235) {
                $left = [Math]::Min($left, $x); $right = [Math]::Max($right, $x)
                $top = [Math]::Min($top, $y); $bottom = [Math]::Max($bottom, $y)
            }
        }
    }

    $bitmap = [System.Drawing.Bitmap]::new(16, 16, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $targetGraphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $targetGraphics.Clear([System.Drawing.Color]::White)
    $targetGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $targetGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    if ($right -ge $left -and $bottom -ge $top) {
        $sourceRect = [System.Drawing.Rectangle]::new($left, $top, $right - $left + 1, $bottom - $top + 1)
        $targetRect = [System.Drawing.Rectangle]::new(1, 1, 14, 14)
        $targetGraphics.DrawImage($sourceBitmap, $targetRect, $sourceRect, [System.Drawing.GraphicsUnit]::Pixel)
    }
    $targetGraphics.Dispose()
    $sourceBitmap.Dispose()

    $bytes = [System.Collections.Generic.List[byte]]::new()
    for ($y = 0; $y -lt 16; $y++) {
        for ($x = 0; $x -lt 16; $x += 2) {
            $left = $bitmap.GetPixel($x, $y)
            $right = $bitmap.GetPixel($x + 1, $y)
            # Existing page renderers intentionally keep only alpha >= 11.
            # Emit binary 0/15 coverage so fallback strokes are never crippled.
            $leftAlpha = if ($left.R -lt 150) { 15 } else { 0 }
            $rightAlpha = if ($right.R -lt 150) { 15 } else { 0 }
            $bytes.Add([byte](($leftAlpha -shl 4) -bor $rightAlpha))
        }
    }
    $bitmap.Dispose()

    $codepoint = [int][char]$character
    $encoded = ($bytes | ForEach-Object { '0x{0:X2}' -f $_ }) -join ','
    $lines.Add(('    {{0x{0:X4}, {{{1}}}}},' -f $codepoint, $encoded))
}

$font.Dispose()
$lines.Add('};')
$lines.Add('')
$lines.Add('constexpr uint16_t GLYPH_COUNT = sizeof(GLYPHS) / sizeof(GLYPHS[0]);')
$lines.Add('')
$lines.Add('} // namespace ChineseFallbackFont')
[System.IO.File]::WriteAllLines($output, $lines, [System.Text.UTF8Encoding]::new($false))
Write-Host "Generated $($characters.Count) fallback glyphs at $output"