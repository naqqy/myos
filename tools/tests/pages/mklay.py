import os

os.makedirs('/tmp/www', exist_ok=True)
os.chdir('/tmp/www')

open('lay.css', 'w').write(''':root { --accent: #1a6fb5; --pad: 12px; --soft: #eef5fb; }
body { font-family: sans-serif; margin: 0; background: #f4f6f8; color: #222; }
.top { background: var(--accent); color: white; padding: 10px 16px; }
.top h1 { margin: 0; font-size: 22px; }
.top a { color: #ffe9a8; margin-left: 14px; text-decoration: none; }
.wrap { max-width: 640px; margin: 16px auto; background: #fff; border: 1px solid #c8d2dc; padding: var(--pad) 18px; }
h2 { color: var(--accent); border-bottom: 2px solid var(--accent, red); padding-bottom: 3px; }
.note { background: var(--soft); border-left: 4px solid #1a6fb5; padding: 8px 12px; margin: 10px 0; }
.note b { color: #a03000; }
code { background: #eee; padding: 0 3px; }
table.data { border: 1px solid #99a; width: 100%; }
table.data th { background: #dde6ee; text-align: left; padding: 4px 8px; }
table.data td { padding: 4px 8px; border-top: 1px solid #ccd; }
.right { text-align: right; }
.badge { display: inline-block; background: #2a8a3a; color: #fff; padding: 1px 8px; font-size: 12px; }
.cols { display: flex; }
.cols div { background: #fde9c8; border: 1px solid #d9a441; padding: 6px 10px; margin: 4px; }
.small { font-size: 12px; color: #667; }
.hidden-thing { display: none; }
@media (max-width: 300px) { .wrap { background: red; } }
@media print { body { background: red; } }
ul.plain { list-style: none; padding-left: 0; }
.footer { text-align: center; font-size: 12px; color: #778; margin: 14px 0; }
''')

open('lay.html', 'w').write('''<!DOCTYPE html>
<html><head><title>Layout test</title><link rel="stylesheet" href="lay.css"></head>
<body>
<div class="top"><h1>Layout test <a href="js.html">scripts</a><a href="lay.html">again</a></h1></div>
<div class="wrap">
<h2>Boxes and text</h2>
<p>This paragraph sits in a white box that is centred, has a border and is at most 640 pixels wide.
It has <b>bold</b>, <i>italic</i>, <a href="find.html">a link</a>, some <code>inline code</code> and
a <span class="badge">badge</span>. Curly &ldquo;quotes&rdquo;, a dash &mdash; and caf&eacute; na&iuml;ve &euro;5.</p>
<div class="note"><b>Note:</b> a box with a tinted background, a thick left border, and colours taken from CSS variables.</div>
<p class="hidden-thing">FAIL: this is display none</p>
<h2>Lists</h2>
<ul><li>First item</li><li>Second item with a longer line of text that should wrap neatly under itself rather than under the bullet when the box is narrow enough</li>
<li>Third<ul><li>Nested one</li><li>Nested two</li></ul></li></ul>
<ol><li>Step one</li><li>Step two</li><li>Step three</li></ol>
<h2>Table</h2>
<table class="data"><tr><th>Planet</th><th>Moons</th><th class="right">Day (hours)</th></tr>
<tr><td>Earth</td><td>1</td><td class="right">24</td></tr>
<tr><td>Mars</td><td>2</td><td class="right">24.6</td></tr>
<tr><td>Jupiter, the largest planet</td><td>95</td><td class="right">9.9</td></tr></table>
<h2>Side by side</h2>
<div class="cols"><div>One</div><div>Two is wider</div><div>Three</div></div>
<pre>preformatted   text
  keeps   its spacing</pre>
<p class="small">Small grey text. <button>A button</button> <input size="12" placeholder="a text box"> <input type="checkbox" checked> ticked</p>
</div>
<div class="footer">Footer, centred and small</div>
</body></html>
''')
print('made layout test page')
