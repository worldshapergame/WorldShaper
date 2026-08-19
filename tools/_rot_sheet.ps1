param([string]$Out="renders\rot-room",[int]$Metre=12,[int]$Frames=14,[switch]$PT,[int]$W=480,[int]$H=360)
$ErrorActionPreference='Continue'
$exe=".\build\bin\WorldShaper.exe"
$s=$Metre/32.0
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$eye=@(0.0,3.50,0.0)
$views=@()
foreach($p in @(@{n="in-east";y=0},@{n="in-northeast";y=45},@{n="in-north";y=90},@{n="in-northwest";y=135},
                @{n="in-west";y=180},@{n="in-southwest";y=225},@{n="in-south";y=270},@{n="in-southeast";y=315})){
  $views+=@{n=$p.n;cam=("{0:0.###},{1:0.###},{2:0.###},{3},{4}" -f ($eye[0]*$s),($eye[1]*$s),($eye[2]*$s),$p.y,6)}
}
$views+=@{n="in-up";cam=("{0:0.###},{1:0.###},{2:0.###},90,62" -f 0,(3.50*$s),(-2.60*$s))}
$common=@("--clip-file","clips\facility.clip","--clip-metre","$Metre","--no-vsync","--no-update-check",
          "--width","$W","--height","$H","--screenshot-frame","$Frames")
# R3d deleted the reference path tracer (D517, D518). Refused rather than ignored: an unknown
# argument is warned about and dropped, so asking for the tracer would return a clean-looking
# measurement of the real-time path. Trap 15, and baseline.ps1 refuses it the same way.
if($PT){throw "the reference path tracer was deleted by R3d; there is no --pathtrace to measure"}
$made=@();$labels=@()
foreach($v in $views){
  $png=Join-Path $Out ($v.n+".png")
  $a=$common+@("--screenshot",$png,"--cam",$v.cam)
  try{ & $exe @a | Out-Null }catch{}
  if(Test-Path $png){$made+=$png;$labels+=$v.n;Write-Host ("  ok  "+$v.n)}else{Write-Host ("  FAIL "+$v.n)}
}
Add-Type -AssemblyName System.Drawing
$cols=3;$rows=[int][Math]::Ceiling($made.Count/$cols);$lab=18
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
