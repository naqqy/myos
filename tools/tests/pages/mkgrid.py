import os

os.makedirs('/tmp/www', exist_ok=True)
os.chdir('/tmp/www')

open('grid.html', 'w').write('''<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Grid, columns, generated text</title>
<style>
body { font-family: sans-serif; margin: 10px; color: #223; }
.g3 { display: grid; grid-template-columns: 120px 1fr 2fr; gap: 8px; }
.g3 div, .auto div, .rep div { background: #e3ecf7; border: 1px solid #7a94b8; padding: 6px; }
.g3 .wide { grid-column: span 2; background: #f7e9d0; }
.g3 .full { grid-column: 1 / -1; background: #dff0d8; }
.auto { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 6px 10px; margin-top: 10px; }
.rep { display: grid; grid-template-columns: repeat(4, 1fr); gap: 4px; margin-top: 10px; }
.col { display: flex; flex-direction: column; gap: 6px; align-items: center; background: #f2f2f2; padding: 8px; margin-top: 10px; }
.col span { display: block; background: #cfe0ff; padding: 3px 14px; }
.pill { display: inline-block; background: #2a7ab8; color: #fff; padding: 3px 12px; border-radius: 12px; }
.card { border: 2px solid #c08a2a; border-radius: 10px; background: #fff7e6; padding: 8px 12px; margin-top: 10px; }
.round { display: inline-block; width: 40px; height: 40px; border-radius: 50%; background: #c0392b; }
ul.crumbs { list-style: none; padding: 0; }
ul.crumbs li { display: inline; }
ul.crumbs li + li::before { content: " \\203A "; color: #888; }
q::before { content: "\\201C"; } q::after { content: "\\201D"; }
.req::after { content: " *"; color: #c00; }
.note::before { content: "Note: "; font-weight: bold; }
.icon::before { content: "\\f101"; }
.sep a + a::before { content: " | "; }
</style></head>
<body>
<div class="g3"><div>120px</div><div>1fr</div><div>2fr</div><div class="wide">spans two columns</div><div>third</div><div class="full">whole row (1 / -1)</div></div>
<div class="auto"><div>auto-fill a</div><div>auto-fill b</div><div>auto-fill c</div><div>auto-fill d</div><div>auto-fill e</div><div>auto-fill f</div></div>
<div class="rep"><div>1</div><div>2</div><div>3</div><div>4</div><div>5</div><div>6</div></div>
<div class="col"><span>column item one</span><span>two</span><span>a third, longer item</span></div>
<p><span class="pill">a pill</span> <span class="pill">another</span> <span class="round"></span> rounded things.</p>
<div class="card">A card with a rounded 2 pixel border.</div>
<ul class="crumbs"><li>Home</li><li>Library</li><li>Books</li></ul>
<p>She said <q>generated quotes</q>. <span class="req">Required field</span>. <span class="icon">icon font (nothing before)</span></p>
<p class="note">text added before a paragraph.</p>
<p class="sep"><a href="lay.html">Layout</a><a href="flo.html">Floats</a><a href="pos.html">Position</a></p>
</body></html>
''')
print('made grid test page')
