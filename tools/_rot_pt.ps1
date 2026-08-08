param([int]$Metre=16,[int]$Frames=64,[string]$Out="renders\rot-pt")
$ErrorActionPreference='Continue'
$exe=".\build\bin\WorldShaper.exe"; $s=$Metre/32.0
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$W=640;$H=480
$views=@(
 @{n="vault";  p=@(0.0,3.50,-2.60); yaw=90;  pit=60},
 @{n="niche";  p=@(0.0,3.50,0.0);   yaw=45;  pit=4},
 @{n="room";   p=@(0.0,3.50,-3.20); yaw=90;  pit=8},
 @{n="floor";  p=@(0.0,7.20,0.0);   yaw=90;  pit=-64}
)
$common=@("--clip-file","clips\facility.clip","--clip-metre","$Metre","--no-vsync","--no-update-check",
          "--pathtrace","--width","$W","--height","$H","--screenshot-frame","$Frames")
$made=@();$labels=@()
foreach($v in $views){
  $cam=("{0:0.###},{1:0.###},{2:0.###},{3},{4}" -f ($v.p[0]*$s),($v.p[1]*$s),($v.p[2]*$s),$v.yaw,$v.pit)
  $png=Join-Path $Out ($v.n+".png")
  $a=$common+@("--screenshot",$png,"--cam",$cam)
  try{ & $exe @a | Out-Null }catch{}
  if(Test-Path $png){$made+=$png;$labels+=$v.n;Write-Host ("  ok  "+$v.n)}else{Write-Host ("  FAIL "+$v.n)}
}
Add-Type -AssemblyName System.Drawing
$cols=2;$rows=[int][Math]::Ceiling($made.Count/$cols);$lab=18
$sheet=[System.Drawing.Bitmap]::new([int]($cols*$W),[int]($rows*($H+$lab)))
$g=[System.Drawing.Graphics]::FromImage($sheet);$g.Clear([System.Drawing.Color]::FromArgb(24,24,26))
$font=[System.Drawing.Font]::new("Consolas",[single]11)
$ink=[System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(230,230,235))
for($i=0;$i -lt $made.Count;$i++){
  $t=[System.Drawing.Image]::FromFile($made[$i])
  $cx=[int](($i % $cols)*$W);$cy=[int]([Math]::Floor($i/$cols)*($H+$lab))
  $g.DrawString($labels[$i],$font,$ink,[single]($cx+4),[single]($cy+2))
  $g.DrawImage($t,$cx,$cy+$lab,$W,$H);$t.Dispose()
}
$sheet.Save((Join-Path $Out "contact-sheet.png"))
Write-Host ("contact sheet: "+(Join-Path $Out "contact-sheet.png"))
