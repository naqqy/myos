import os

os.makedirs('/tmp/www', exist_ok=True)
os.chdir('/tmp/www')

LOREM = ('Text that should run beside the floated box and then carry on underneath it once the box '
         'has ended, the way a newspaper column runs round a picture. ') * 3

open('flo.html', 'w').write('''<!DOCTYPE html>
<html><head><title>Floats and flex</title>
<style>
body { margin: 0; font-family: sans-serif; color: #222; }
.bar { display: flex; justify-content: space-between; align-items: center; background: #1a4f7a; color: #fff; padding: 6px 12px; }
.bar .logo { font-size: 20px; font-weight: bold; }
.bar .links a { color: #cfe6ff; margin-left: 14px; text-decoration: none; }
.page { padding: 8px 14px; }
.info { float: right; width: 170px; margin: 0 0 8px 12px; border: 1px solid #99a; background: #f3f6fa; padding: 6px; font-size: 13px; }
.info b { display: block; text-align: center; background: #c8d8ea; padding: 2px; }
.pic { float: left; width: 70px; height: 50px; background: #d98; margin: 3px 10px 4px 0; }
.clear { clear: both; border-top: 1px solid #ccc; margin-top: 6px; padding-top: 6px; }
.row { display: flex; gap: 8px; margin: 8px 0; }
.row div { background: #e6f2d8; border: 1px solid #8a6; padding: 6px; }
.grow { flex: 1; }
.fixed { width: 90px; }
.tall { height: 60px; }
.mid { display: flex; align-items: center; justify-content: center; background: #fbeed0; border: 1px solid #ca6; height: 50px; }
.push { display: flex; background: #eee; padding: 4px; }
.push .last { margin-left: auto; }
.wrap { display: flex; flex-wrap: wrap; gap: 6px; }
.wrap span { display: block; background: #dde; padding: 4px 22px; }
</style></head>
<body>
<div class="bar"><span class="logo">Site name</span><span class="links"><a href="lay.html">Layout</a><a href="js.html">Scripts</a><a href="flo.html">Floats</a></span></div>
<div class="page">
<div class="info"><b>Info box</b>Floated to the right, 170 pixels wide, with a border. The paragraph runs down its left side.</div>
<p><span class="pic"></span>''' + LOREM + '''</p>
<p>A second paragraph, still beside the info box if it is long enough to reach this far down the page.</p>
<div class="clear">This line has <b>clear: both</b>, so it starts below both floats, across the full width.</div>
<div class="row"><div class="fixed">Fixed 90</div><div class="grow">Grows to fill the rest of the row</div><div class="tall">Tall</div></div>
<div class="row"><div class="grow">One third</div><div class="grow">One third</div><div class="grow">One third</div></div>
<div class="mid">Centred both ways in a 50 pixel box</div>
<div class="push"><span>Left</span><span class="last">Pushed right by an auto margin</span></div>
<p>Wrapping row:</p>
<div class="wrap"><span>alpha</span><span>beta</span><span>gamma</span><span>delta</span><span>epsilon</span><span>zeta</span><span>eta</span><span>theta</span><span>iota</span><span>kappa</span></div>
</div>
</body></html>
''')
print('made float/flex test page')
