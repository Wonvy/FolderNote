# Rebuild the deterministic vector-style app icon at native Windows icon sizes.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
function New-NoteImage([int]$size) {
    $canvas = [Drawing.Bitmap]::new($size * 4, $size * 4, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($canvas)
    $g.Clear([Drawing.Color]::Transparent)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.ScaleTransform($size / 4.0, $size / 4.0)
    $brushes = @('#c58a12','#f3bb36','#ffdb73','#246bc5') | ForEach-Object {
        [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($_))
    }
    $folder = [Drawing.Drawing2D.GraphicsPath]::new()
    $folder.AddPolygon([Drawing.PointF[]]@([Drawing.PointF]::new(1,3),[Drawing.PointF]::new(6.5,3),[Drawing.PointF]::new(8,4.5),[Drawing.PointF]::new(14,4.5),[Drawing.PointF]::new(14,13),[Drawing.PointF]::new(1,13)))
    $g.FillPath($brushes[0],$folder)
    $g.FillRectangle($brushes[1],1,6,13,7.5)
    $g.FillRectangle($brushes[2],1,6,13,1)
    $note = [Drawing.Drawing2D.GraphicsPath]::new()
    $note.AddArc(7,5,2,2,180,90); $note.AddArc(13,5,2,2,270,90)
    $note.AddArc(13,13,2,2,0,90); $note.AddArc(7,13,2,2,90,90); $note.CloseFigure()
    $g.FillPath($brushes[3],$note)
    $pen = [Drawing.Pen]::new([Drawing.Color]::White,1)
    $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen,[single]9,[single]8,[single]13,[single]8)
    $g.DrawLine($pen,[single]9,[single]10,[single]13,[single]10)
    $g.DrawLine($pen,[single]9,[single]12,[single]11.5,[single]12)
    $g.Dispose(); $pen.Dispose(); $folder.Dispose(); $note.Dispose()
    foreach($brush in $brushes){$brush.Dispose()}
    $image = [Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $output = [Drawing.Graphics]::FromImage($image)
    $output.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $output.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $output.DrawImage($canvas,0,0,$size,$size)
    $output.Dispose(); $canvas.Dispose()
    return $image
}
$sizes = @(16,20,24,32,40,48,64,128,256)
$entries = @()
foreach($size in $sizes){
    $image = New-NoteImage $size
    $stream = [IO.MemoryStream]::new()
    $image.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $entries += ,$stream.ToArray()
    if($size -eq 256){$image.Save((Join-Path $PSScriptRoot 'FolderNote.png'),[Drawing.Imaging.ImageFormat]::Png)}
    $image.Dispose(); $stream.Dispose()
}
$file = [IO.File]::Create((Join-Path $PSScriptRoot 'FolderNote.ico'))
$writer = [IO.BinaryWriter]::new($file)
$writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for($i=0;$i -lt $sizes.Count;$i++){
    $dimension = if($sizes[$i] -eq 256){0}else{$sizes[$i]}
    $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
    $writer.Write([byte]0); $writer.Write([byte]0)
    $writer.Write([uint16]1); $writer.Write([uint16]32)
    $writer.Write([uint32]$entries[$i].Length); $writer.Write([uint32]$offset)
    $offset += $entries[$i].Length
}
foreach($entry in $entries){$writer.Write([byte[]]$entry)}
$writer.Dispose()
foreach($item in @(@('Logo.png',150),@('SmallLogo.png',44))){
    $image = New-NoteImage ([int]$item[1])
    $image.Save((Join-Path $PSScriptRoot $item[0]),[Drawing.Imaging.ImageFormat]::Png)
    $image.Dispose()
}
