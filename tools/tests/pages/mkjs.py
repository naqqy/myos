import os

os.makedirs('/tmp/www', exist_ok=True)
os.chdir('/tmp/www')

open('redir.html', 'w').write('''<title>Redirecting</title>
<p>You should not stay on this page.</p>
<script>location.href = 'js.html';</script>
''')

open('app.js', 'w').write('''
document.getElementById('ext').textContent = 'external script ran (app.js)';
window.APP = { greet: function (who) { return 'hello ' + who; } };
''')

open('data.json', 'w').write('{"planets": ["Mercury", "Venus", "Earth", "Mars"]}')

open('js.html', 'w').write('''<!DOCTYPE html>
<html><head><title>Script test</title>
<style>.warn { color: #c00000; font-weight: bold } .gone { display: none }</style>
<script src="app.js" defer></script>
</head><body>
<form action="find.html"><input name="q" size="20" placeholder="search"> <input type="hidden" name="from" value="js test"> <button>Search</button></form>
<p id="ext">FAIL: external script did not run</p>
<p id="inline">FAIL: inline script did not run</p>
<p id="dcl">FAIL: DOMContentLoaded did not fire</p>
<p id="fetch">FAIL: fetch did not finish</p>
<p id="timer">FAIL: timer did not fire</p>
<p id="hide" >FAIL: this should be hidden by a class set from script</p>
<p id="cls">This turns red and bold through classList.</p>
<script>document.write('<p>Written by document.write: 6 * 7 = ' + (6 * 7) + '</p>');</script>
<p><a href="find.html?q=plain" id="l1">plain link</a> | <a href="find.html?q=never" id="l2">link a script cancels</a> | <span id="n">0</span> clicks</p>
<noscript><p>FAIL: noscript content is showing</p></noscript>
<script>
document.getElementById('inline').textContent = 'inline script ran; APP says ' + APP.greet('NAQ OS');
document.addEventListener('DOMContentLoaded', function () {
  document.getElementById('dcl').textContent = 'DOMContentLoaded fired, readyState ' + document.readyState;
});
fetch('data.json').then(function (r) { return r.json(); }).then(function (d) {
  document.getElementById('fetch').innerHTML = 'fetch + JSON: <b>' + d.planets.length + '</b> planets, third is <i>' + d.planets[2] + '</i>';
});
setTimeout(function () { document.getElementById('timer').textContent = 'timer fired after 300 ms'; }, 300);
document.getElementById('hide').classList.add('gone');
document.getElementById('cls').classList.add('warn');
var clicks = 0;
document.body.addEventListener('click', function (e) {
  clicks++; document.getElementById('n').textContent = clicks;
  if (e.target.id === 'l2') { e.preventDefault(); e.target.textContent = 'cancelled by script'; }
});
console.log('page script finished', { clicks: clicks, list: [1, 2, 3] });
console.warn('this is a warning');
thisFunctionDoesNotExist();
</script>
<script>document.title = 'Script test (title set by script)'; null.oops;</script>
</body></html>
''')

open('find.html', 'w').write('''<title>Find</title>
<h2>Search results</h2>
<p id="out">FAIL: script did not run</p>
<p><button onclick="history.back()">Go back</button> <button onclick="alert('The answer is ' + 6 * 7)">Alert</button></p>
<script>
var p = new URLSearchParams(location.search);
document.getElementById('out').textContent = 'You searched for "' + p.get('q') + '" (from: ' + p.get('from') + ') at ' + location.pathname;
</script>
''')

print('made js test pages')
