/* Host test for userapps/browser/dom.js: run with  qjs --std domtest.js <dom.js> */
const P = globalThis.print;
let clock = 1000;
const files = {
    'http://test.local/dir/lib.js': 'var LIB = { twice: x => x * 2 }; document.getElementById("lib").textContent = "lib loaded";',
    'http://test.local/data.json': '{"items":["alpha","beta"]}',
};

globalThis.__native = {
    log(l, t) { P('  [' + l + '] ' + t); },
    load(u) { return u in files ? files[u] : null; },
    eval(code, name) {
        try { std.evalScript(code); return true; }
        catch (e) { P('  [error] ' + name + ': ' + e + '\n' + (e && e.stack)); return false; }
    },
    navigate(u) { P('  NAVIGATE ' + u); },
    url() { return 'http://test.local/dir/page.html?x=1'; },
    now() { return clock; },
    alert(t) { P('  ALERT ' + t); },
    confirm() { return true; },
    title(t) { P('  TITLE ' + t); },
    status(t) { P('  STATUS ' + t); },
    size() { return [760, 460]; },
    back() { },
};

std.evalScript(std.loadFile(scriptArgs[1]));

const html = `<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>Test &amp; page</title>
<meta name="description" content="hello meta">
<style>.x { color: red }</style>
<script>var early = document.body ? 'body exists' : 'no body';</script>
</head>
<body class="main" onload="document.getElementById('onload').textContent='body onload ran'">
<h1 id="h">Heading</h1>
<p id="p1" class="a b">One <b>bold</b> &lt;tag&gt; &copy; text<p id="p2">Two (implied close)
<ul id="list"><li>first<li class="sel">second<li>third</ul>
<div id="lib"></div><div id="onload"></div><div id="out"></div>
<button id="btn">Count: 0</button>
<span id="inl" onclick="this.textContent = 'inline ' + event.type">inline handler</span>
<a id="lnk" href="/other?q=1">a link</a> <a id="stop" href="/blocked">blocked link</a>
<form id="f" action="/search"><input id="q" name="q" placeholder="Search"><input type="hidden" name="h" value="1">
<input type="checkbox" id="cb" name="c" value="yes"> <select id="sel" name="s"><option value="1">One<option value="2" selected>Two</select>
<button type="submit" id="go">Go</button></form>
<table><tr><td>a<td>b<tr><td>c</table>
<noscript><p>NOSCRIPT TEXT</p></noscript>
<script src="lib.js"></script>
<script>
var log = [];
var out = document.getElementById('out');
log.push('early=' + early);
log.push('title=' + document.title);
log.push('meta=' + document.querySelector('meta[name=description]').content);
log.push('p count=' + document.querySelectorAll('p').length + ' p2 parent=' + document.getElementById('p2').parentNode.tagName);
log.push('li=' + [...document.querySelectorAll('#list > li')].map(e => e.textContent).join('|'));
log.push('sel=' + document.querySelector('ul li.sel + li').textContent + ' nth=' + document.querySelector('li:nth-child(2)').textContent
         + ' not=' + document.querySelectorAll('li:not(.sel)').length + ' last=' + document.querySelector('li:last-child').textContent);
log.push('td=' + document.querySelectorAll('td').length + ' tr=' + document.querySelectorAll('tr').length);
log.push('lib=' + LIB.twice(21) + ' href=' + document.getElementById('lnk').href + ' host=' + location.hostname + ' search=' + location.search);
log.push('inner=' + document.getElementById('p1').innerHTML);
var n = 0;
var btn = document.getElementById('btn');
btn.addEventListener('click', function (e) { n++; this.textContent = 'Count: ' + n; this.classList.toggle('on'); this.style.fontWeight = 'bold'; });
document.addEventListener('click', e => { if (e.target.id === 'stop') { e.preventDefault(); out.textContent = 'blocked by delegate'; } });
document.getElementById('q').addEventListener('input', e => { document.getElementById('h').textContent = 'typed: ' + e.target.value; });
document.getElementById('cb').addEventListener('change', e => log.push('cb=' + e.target.checked));
document.getElementById('f').addEventListener('submit', e => log.push('submit q=' + new FormDataLike(e.target)));
function FormDataLike(f) { this.toString = () => f.elements.length + ' controls'; }
var d = document.createElement('div'); d.id = 'made'; d.innerHTML = '<i>made</i> by <b class="k">script</b>'; document.body.appendChild(d);
d.dataset.fooBar = '7'; log.push('data=' + d.getAttribute('data-foo-bar') + ' closest=' + d.querySelector('.k').closest('div').id);
setTimeout(() => { out.textContent += ' +timeout'; }, 50);
var iv = setInterval(() => { out.textContent += ' +iv'; if (out.textContent.split('+iv').length > 3) clearInterval(iv); }, 20);
fetch('/data.json').then(r => r.json()).then(j => { document.getElementById('lib').textContent += ' fetch:' + j.items.join(','); });
document.addEventListener('DOMContentLoaded', () => log.push('DCL readyState=' + document.readyState));
window.addEventListener('load', () => { log.push('load'); });
Promise.resolve().then(() => log.push('microtask'));
new URL('a/b?x=1#h', 'https://ex.com/dir/');
log.push('url=' + new URL('../a/b?x=1#h', 'https://ex.com/dir/sub/').href + ' sp=' + new URLSearchParams('a=1&b=two+words').get('b'));
localStorage.setItem('k', 'v'); log.push('ls=' + localStorage.getItem('k') + ' ua=' + (navigator.userAgent.length > 5));
undefinedFunctionCall();
</script>
<script>log.push('next script still runs');</script>
</body></html>`;

function pending() { while (os && false) { } }
function show(label) { const h = __flush(0); P(label + ': ' + (h === null ? '(no change)' : h.replace(/\n/g, ' '))); return h; }
function idOf(h, marker) {
    const at = h.indexOf(marker);
    const m = [...h.slice(0, at + marker.length).matchAll(/<nq (\d+)>/g)];
    return +m[m.length - 1][1];
}
function drain() { std.evalScript('0'); }

let t0 = Date.now();
__boot(html);
P('boot ms', Date.now() - t0);
P('scripts run:', __runScripts());
let h = show('first');
P(log.join('\n'));

P('click button ->', __click(idOf(h, 'Count: 0')));
h = show('after click');
P('click inline ->', __click(idOf(h, 'inline handler')));
P('click link ->', __click(idOf(h, 'a link')), '(0 = browser follows it)');
P('click blocked ->', __click(idOf(h, 'blocked link')));
h = show('after 3 clicks');
P('click checkbox ->', __click(idOf(h, 'type="checkbox"')));
P('click select ->', __click(idOf(h, 'type="select"')));
P('click input ->', __click(idOf(h, 'placeholder="Search"')));
for (const ch of 'naq os') P('key', ch, __key(ch.toUpperCase().charCodeAt(0), ch));
P('backspace', __key(8, ''));
h = show('after typing');
clock += 30; P('tick ->', __tick());
clock += 30; P('tick ->', __tick());
clock += 100; P('tick ->', __tick()); P('tick ->', __tick());
h = show('after timers');
P('enter ->', __key(13, ''));
P(log.join('\n'));

/* a big page, for timing */
let big = '<html><body>';
for (let i = 0; i < 20000; i++) big += '<div class="row r' + (i % 7) + '" id="r' + i + '"><a href="/p/' + i + '">link ' + i + '</a> some text &amp; more <span>span</span></div>\n';
t0 = Date.now(); __boot(big); const t1 = Date.now();
const count = document.querySelectorAll('div.r3 > span').length; const t2 = Date.now();
const g = document.getElementById('r19999').textContent; const t3 = Date.now();
const out2 = __flush(1); const t4 = Date.now();
P('big: parse', t1 - t0, 'ms, query', t2 - t1, 'ms (' + count + '), byId', t3 - t2, 'ms, render', t4 - t3, 'ms,', out2.length, 'bytes');
