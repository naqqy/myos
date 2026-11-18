import os

os.makedirs('/tmp/www', exist_ok=True)
os.chdir('/tmp/www')

open('pos.html', 'w').write('''<!DOCTYPE html>
<html><head><title>Position and selectors</title>
<style>
body { font-family: sans-serif; margin: 10px; }
.card { position: relative; border: 1px solid #99a; background: #f6f8fb; padding: 14px; height: 110px; width: 420px; }
.badge { position: absolute; top: 6px; right: 8px; background: #c0392b; color: #fff; padding: 1px 8px; font-size: 12px; }
.corner { position: absolute; bottom: 4px; left: 8px; font-size: 12px; color: #567; }
.stretch { position: absolute; left: 150px; right: 10px; top: 60px; background: #dfe9c8; border: 1px solid #8a6; padding: 2px 6px; font-size: 13px; }
.sr-only { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0,0,0,0); }
.offscreen { position: absolute; left: -9999px; }
.invisible { opacity: 0; }
.nudge { position: relative; left: 40px; top: 4px; background: #fde9c8; }
.stamp { position: absolute; top: 14px; right: 20px; border: 2px solid #1a6fb5; color: #1a6fb5; padding: 4px 10px; font-weight: bold; }
ul.z li:nth-child(odd) { background: #e8eef5; }
ul.z li:nth-child(3) { font-weight: bold; }
ul.z li:not(.plain) { color: #1a4f7a; }
ul.z li:last-child { border-bottom: 2px solid #1a4f7a; }
h3 + p { color: #a03000; }
h3 ~ p.later { font-style: italic; }
a[href^="https"] { color: #1a8a3a; }
a[href$=".pdf"] { color: #a02020; font-weight: bold; }
a[href*="example"] { background: #ffef9a; }
span[data-kind~="two"] { text-decoration: underline; }
p:first-of-type { margin-top: 4px; }
</style></head>
<body>
<div class="stamp">absolute, page corner</div>
<div class="card">A relative card, 420 by 110.
  <span class="badge">top right</span><span class="corner">bottom left</span>
  <div class="stretch">left and right given: stretched</div>
  <span class="sr-only">FAIL: screen-reader-only text is showing</span>
  <span class="offscreen">FAIL: off-screen text is showing</span>
  <span class="invisible">FAIL: opacity 0 text is showing</span>
</div>
<p>Normal text, <span class="nudge">nudged inline (ignored)</span> and after.</p>
<div class="nudge">A block nudged 40 right and 4 down by position: relative.</div>
<ul class="z"><li>one (shaded, blue)</li><li class="plain">two (plain: not blue)</li><li>three (shaded, bold)</li><li>four</li><li>five (shaded, underlined below)</li></ul>
<h3>Sibling rules</h3>
<p>Right after the heading: red-brown (h3 + p).</p>
<p class="later">Later sibling with class: italic (h3 ~ p.later).</p>
<p><a href="https://secure.test/">https link: green</a> | <a href="/files/paper.pdf">PDF link: red bold</a> |
<a href="http://www.example.org/x">contains "example": yellow</a> | <span data-kind="one two three">word match: underlined</span></p>
</body></html>
''')
print('made position test page')
