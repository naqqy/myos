/*
 * NAQ OS Browser: the page interface for scripts (the "DOM").
 *
 * This file runs inside the QuickJS engine before any script of a page.
 * It builds the document tree from the page's HTML and gives scripts the
 * objects they expect: window, document, elements, events, timers,
 * console, fetch and so on.
 *
 * The browser itself (browser.c) draws pages from HTML text. So after
 * scripts have changed the tree, __flush() writes the tree back out as
 * HTML and the browser lays that out again. Each piece of text carries
 * the number of its element (<nq 12>), which is how a click on the
 * screen finds its way back to the right element here.
 *
 * `native` is the small set of functions browser.c provides:
 *   log(level, text)   load(url) -> text or null     eval(code, name) -> ok
 *   navigate(url)      url() -> page address         now() -> milliseconds
 *   alert(text)        confirm(text) -> bool         title(text)
 *   status(text)       size() -> [width, height]     back()
 */
(function (global, native) {
'use strict';

const EMPTY = Object.freeze([]);
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input',
                      'link', 'meta', 'param', 'source', 'track', 'wbr']);
const RAW_TEXT = new Set(['script', 'style', 'textarea', 'title', 'noscript',
                          'xmp', 'iframe', 'noembed', 'noframes']);
const HEAD_TAGS = new Set(['title', 'meta', 'link', 'style', 'script', 'base',
                           'noscript', 'template']);
const P_CLOSERS = new Set(['address', 'article', 'aside', 'blockquote', 'center',
    'details', 'div', 'dl', 'fieldset', 'figcaption', 'figure', 'footer', 'form',
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'header', 'hr', 'main', 'menu', 'nav',
    'ol', 'p', 'pre', 'section', 'table', 'ul']);
const CONTROLS = new Set(['input', 'button', 'select', 'textarea']);
const NOT_SHOWN = new Set(['script', 'style', 'noscript', 'template', 'head',
                           'title', 'meta', 'link', 'base', 'svg', 'iframe',
                           'object', 'embed', 'audio', 'video', 'canvas', 'datalist']);
const TEXT_TYPES = new Set(['text', 'search', 'email', 'url', 'tel', 'password',
                            'number', 'date', 'time', 'datetime-local', 'month', 'week']);

let doc = null;             /* the page's document */
let dirty = false;          /* the tree changed since the last __flush */
let interactive = false;    /* the page has controls or event listeners */
let rendered = false;
let treeVersion = 0;        /* bumped when elements or ids change */
let scriptsLive = false;    /* the page's own scripts have finished */
let focused = null;
let scriptsRun = 0;

const MAX_SCRIPTS = 40;

function touch() {
    dirty = true;
}

function report(e) {
    let text;

    try {
        text = (e && e.stack) ? String(e) + '\n' + e.stack : String(e);
    } catch (x) {
        text = 'error';
    }

    native.log('error', 'Uncaught ' + text);
}

class DOMException extends Error {
    constructor(message, name) {
        super(message);
        this.name = name || 'Error';
    }
}

/* ---------------------------------------------------------------------- */
/* Events                                                                  */
/* ---------------------------------------------------------------------- */

class Event {
    constructor(type, init) {
        this.type = String(type);
        this.bubbles = !!(init && init.bubbles);
        this.cancelable = !!(init && init.cancelable);
        this.composed = false;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = 0;
        this.isTrusted = false;
        this.timeStamp = native.now();
        this._stop = false;
        this._stopNow = false;
        this._path = null;
    }

    preventDefault() {
        if (this.cancelable) {
            this.defaultPrevented = true;
        }
    }

    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopNow = true; }

    initEvent(type, bubbles, cancelable) {
        this.type = String(type);
        this.bubbles = !!bubbles;
        this.cancelable = !!cancelable;
    }

    composedPath() { return this._path ? this._path.slice() : []; }

    get srcElement() { return this.target; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (v === false) this.preventDefault(); }
    get cancelBubble() { return this._stop; }
    set cancelBubble(v) { if (v) this._stop = true; }
}

Event.NONE = 0;
Event.CAPTURING_PHASE = 1;
Event.AT_TARGET = 2;
Event.BUBBLING_PHASE = 3;

class CustomEvent extends Event {
    constructor(type, init) {
        super(type, init);
        this.detail = (init && 'detail' in init) ? init.detail : null;
    }

    initCustomEvent(type, bubbles, cancelable, detail) {
        this.initEvent(type, bubbles, cancelable);
        this.detail = detail;
    }
}

class UIEvent extends Event {
    constructor(type, init) {
        super(type, init);
        this.view = global;
        this.detail = (init && init.detail) || 0;
    }
}

class MouseEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        this.clientX = this.pageX = this.screenX = this.x = this.offsetX = init.clientX || 0;
        this.clientY = this.pageY = this.screenY = this.y = this.offsetY = init.clientY || 0;
        this.button = init.button || 0;
        this.buttons = init.buttons || 0;
        this.ctrlKey = !!init.ctrlKey;
        this.shiftKey = !!init.shiftKey;
        this.altKey = !!init.altKey;
        this.metaKey = !!init.metaKey;
        this.relatedTarget = init.relatedTarget || null;
    }
}

class KeyboardEvent extends UIEvent {
    constructor(type, init) {
        super(type, init);
        init = init || {};
        this.key = init.key || '';
        this.code = init.code || '';
        this.keyCode = this.which = init.keyCode || 0;
        this.charCode = init.charCode || 0;
        this.ctrlKey = !!init.ctrlKey;
        this.shiftKey = !!init.shiftKey;
        this.altKey = !!init.altKey;
        this.metaKey = !!init.metaKey;
        this.repeat = false;
        this.isComposing = false;
    }

    getModifierState() { return false; }
}

class EventTarget {
    addEventListener(type, fn, options) {
        if (!fn) {
            return;
        }

        const capture = typeof options === 'boolean' ? options : !!(options && options.capture);
        const once = !!(options && typeof options === 'object' && options.once);
        const map = this._ev || (this._ev = new Map());
        let list = map.get(type);

        if (!list) {
            map.set(type, list = []);
        }

        for (const l of list) {
            if (l.fn === fn && l.capture === capture) {
                return;
            }
        }

        list.push({ fn, capture, once });
        interactive = true;
    }

    removeEventListener(type, fn, options) {
        const capture = typeof options === 'boolean' ? options : !!(options && options.capture);
        const list = this._ev && this._ev.get(type);

        if (list) {
            for (let i = 0; i < list.length; i++) {
                if (list[i].fn === fn && list[i].capture === capture) {
                    list.splice(i, 1);
                    break;
                }
            }
        }
    }

    dispatchEvent(ev) {
        return dispatch(this, ev);
    }
}

/* The window's listeners live on this object ("window" is the global). */
const winTarget = new EventTarget();

/* The on<event> handler of a node: a property, or an attribute's code. */
function handlerOf(node, type) {
    const name = 'on' + type;

    if (node === winTarget) {
        const h = global[name];
        return typeof h === 'function' ? h : null;
    }

    const h = node[name];

    if (typeof h === 'function') {
        return h;
    }

    if (node.nodeType === 1 && node._a) {
        const code = node.getAttribute(name);

        if (code !== null) {
            const cache = node._inline || (node._inline = {});

            if (!(name in cache) || cache[name].code !== code) {
                let fn = null;

                try {
                    fn = new Function('event', code);
                } catch (e) {
                    report(e);
                }

                cache[name] = { code, fn };
            }

            return cache[name].fn;
        }
    }

    return null;
}

function invoke(node, ev, phase) {
    const current = node === winTarget ? global : node;

    ev.currentTarget = current;
    ev.eventPhase = phase;

    if (phase !== 1) {
        const h = handlerOf(node, ev.type);

        if (h) {
            try {
                if (h.call(current, ev) === false) {
                    ev.preventDefault();
                }
            } catch (e) {
                report(e);
            }
        }
    }

    const list = node._ev && node._ev.get(ev.type);

    if (!list || !list.length) {
        return;
    }

    for (const l of list.slice()) {
        if ((phase === 1 && !l.capture) || (phase === 3 && l.capture)) {
            continue;
        }

        if (l.once) {
            const i = list.indexOf(l);

            if (i >= 0) {
                list.splice(i, 1);
            }
        }

        try {
            if (typeof l.fn === 'function') {
                l.fn.call(current, ev);
            } else if (l.fn && typeof l.fn.handleEvent === 'function') {
                l.fn.handleEvent(ev);
            }
        } catch (e) {
            report(e);
        }

        if (ev._stopNow) {
            break;
        }
    }
}

function dispatch(target, ev) {
    const path = [];

    if (target instanceof Node) {
        for (let n = target; n; n = n.parentNode) {
            path.push(n);
        }

        if (path[path.length - 1] === doc) {
            path.push(winTarget);
        }
    } else {
        path.push(target);
    }

    ev.target = target === winTarget ? global : target;
    ev._path = path.map(n => n === winTarget ? global : n);
    ev._stop = ev._stopNow = false;

    for (let i = path.length - 1; i > 0 && !ev._stop; i--) {
        invoke(path[i], ev, 1);
    }

    if (!ev._stop) {
        invoke(path[0], ev, 2);
    }

    if (ev.bubbles) {
        for (let i = 1; i < path.length && !ev._stop; i++) {
            invoke(path[i], ev, 3);
        }
    }

    ev.currentTarget = null;
    ev.eventPhase = 0;

    return !ev.defaultPrevented;
}

/* ---------------------------------------------------------------------- */
/* Text escaping                                                           */
/* ---------------------------------------------------------------------- */

const ENTITIES = {
    __proto__: null,
    amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ',
    copy: '©', reg: '®', trade: '™', deg: '°', plusmn: '±',
    micro: 'µ', para: '¶', middot: '·', sect: '§',
    laquo: '«', raquo: '»', iexcl: '¡', iquest: '¿',
    cent: '¢', pound: '£', yen: '¥', euro: '€', curren: '¤',
    frac12: '½', frac14: '¼', frac34: '¾', sup1: '¹',
    sup2: '²', sup3: '³', times: '×', divide: '÷',
    ndash: '–', mdash: '—', lsquo: '‘', rsquo: '’',
    sbquo: '‚', ldquo: '“', rdquo: '”', bdquo: '„',
    dagger: '†', Dagger: '‡', bull: '•', hellip: '…',
    permil: '‰', prime: '′', Prime: '″', lsaquo: '‹',
    rsaquo: '›', larr: '←', uarr: '↑', rarr: '→',
    darr: '↓', harr: '↔', rArr: '⇒', hArr: '⇔',
    minus: '−', ne: '≠', le: '≤', ge: '≥', asymp: '≈',
    infin: '∞', radic: '√', sum: '∑', prod: '∏',
    part: '∂', deg2: '°', ensp: ' ', emsp: ' ',
    thinsp: ' ', zwnj: '‌', zwj: '‍', shy: '­',
    lrm: '‎', rlm: '‏', hearts: '♥', spades: '♠',
    clubs: '♣', diams: '♦', check: '✓', star: '☆',
    agrave: 'à', aacute: 'á', acirc: 'â', atilde: 'ã',
    auml: 'ä', aring: 'å', aelig: 'æ', ccedil: 'ç',
    egrave: 'è', eacute: 'é', ecirc: 'ê', euml: 'ë',
    igrave: 'ì', iacute: 'í', icirc: 'î', iuml: 'ï',
    ntilde: 'ñ', ograve: 'ò', oacute: 'ó', ocirc: 'ô',
    otilde: 'õ', ouml: 'ö', oslash: 'ø', ugrave: 'ù',
    uacute: 'ú', ucirc: 'û', uuml: 'ü', yacute: 'ý',
    yuml: 'ÿ', szlig: 'ß', Agrave: 'À', Aacute: 'Á',
    Acirc: 'Â', Atilde: 'Ã', Auml: 'Ä', Aring: 'Å',
    AElig: 'Æ', Ccedil: 'Ç', Egrave: 'È', Eacute: 'É',
    Ecirc: 'Ê', Euml: 'Ë', Iacute: 'Í', Ntilde: 'Ñ',
    Oacute: 'Ó', Ouml: 'Ö', Oslash: 'Ø', Uacute: 'Ú',
    Uuml: 'Ü', alpha: 'α', beta: 'β', gamma: 'γ',
    delta: 'δ', epsilon: 'ε', lambda: 'λ', mu: 'μ',
    pi: 'π', sigma: 'σ', tau: 'τ', phi: 'φ',
    omega: 'ω', Delta: 'Δ', Sigma: 'Σ', Omega: 'Ω', Pi: 'Π'
};

function decodeEntities(s) {
    if (s.indexOf('&') < 0) {
        return s;
    }

    return s.replace(/&(#[xX][0-9a-fA-F]+|#[0-9]+|[a-zA-Z][a-zA-Z0-9]*);?/g, (m, body) => {
        if (body.charCodeAt(0) === 35) {
            const hex = body[1] === 'x' || body[1] === 'X';
            const cp = hex ? parseInt(body.slice(2), 16) : parseInt(body.slice(1), 10);

            return (cp > 0 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff))
                ? String.fromCodePoint(cp) : '�';
        }

        const v = ENTITIES[body];

        return v !== undefined ? v : m;
    });
}

function escapeText(s) {
    if (s.indexOf('&') < 0 && s.indexOf('<') < 0 && s.indexOf('>') < 0) {
        return s;
    }

    return s.replace(/[&<>]/g, c => c === '&' ? '&amp;' : (c === '<' ? '&lt;' : '&gt;'));
}

function escapeAttr(s) {
    if (s.indexOf('&') < 0 && s.indexOf('"') < 0) {
        return s;
    }

    return s.replace(/[&"]/g, c => c === '&' ? '&amp;' : '&quot;');
}

function kebab(name) {
    if (name === 'cssFloat') {
        return 'float';
    }

    return name.replace(/[A-Z]/g, c => '-' + c.toLowerCase());
}

function camel(name) {
    return name.replace(/-([a-z])/g, (m, c) => c.toUpperCase());
}

/* ---------------------------------------------------------------------- */
/* Addresses                                                               */
/* ---------------------------------------------------------------------- */

const URL_PARTS = /^([a-zA-Z][a-zA-Z0-9+.\-]*:)?(?:\/\/([^\/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/;

function splitURL(href) {
    const m = URL_PARTS.exec(String(href)) || [];

    return {
        protocol: m[1] || '', host: m[2] || '', pathname: m[3] || '',
        search: m[4] || '', hash: m[5] || '', hasHost: m[2] !== undefined
    };
}

function cleanPath(path) {
    const out = [];

    for (const part of path.split('/')) {
        if (part === '..') {
            if (out.length > 1) {
                out.pop();
            }
        } else if (part !== '.') {
            out.push(part);
        }
    }

    const last = path.split('/').pop();

    if (last === '.' || last === '..') {
        out.push('');
    }

    return out.join('/') || '/';
}

function resolveURL(rel, base) {
    rel = String(rel).trim();

    if (base === undefined) {
        base = native.url();
    }

    const r = splitURL(rel);

    if (r.protocol) {
        if (!/^https?:$/i.test(r.protocol) || r.hasHost) {
            return rel;
        }
    }

    const b = splitURL(base);

    if (r.hasHost) {
        return b.protocol + '//' + r.host + (r.pathname ? cleanPath(r.pathname) : '/') + r.search + r.hash;
    }

    const origin = b.protocol + '//' + b.host;

    if (r.pathname === '') {
        return origin + (b.pathname || '/') + (r.search || (rel === '' || r.hash ? b.search : '')) + r.hash;
    }

    if (r.pathname[0] === '/') {
        return origin + cleanPath(r.pathname) + r.search + r.hash;
    }

    const dir = (b.pathname || '/').replace(/[^\/]*$/, '');

    return origin + cleanPath(dir + r.pathname) + r.search + r.hash;
}

function formEncode(s) {
    return encodeURIComponent(s).replace(/%20/g, '+');
}

class URLSearchParams {
    constructor(init) {
        this._p = [];

        if (typeof init === 'string') {
            if (init[0] === '?') {
                init = init.slice(1);
            }

            for (const part of init.split('&')) {
                if (part) {
                    const i = part.indexOf('=');
                    const k = i < 0 ? part : part.slice(0, i);
                    const v = i < 0 ? '' : part.slice(i + 1);

                    this._p.push([this._dec(k), this._dec(v)]);
                }
            }
        } else if (init && typeof init[Symbol.iterator] === 'function') {
            for (const [k, v] of init) {
                this._p.push([String(k), String(v)]);
            }
        } else if (init && typeof init === 'object') {
            for (const k of Object.keys(init)) {
                this._p.push([k, String(init[k])]);
            }
        }
    }

    _dec(s) {
        try {
            return decodeURIComponent(s.replace(/\+/g, ' '));
        } catch (e) {
            return s;
        }
    }

    get(k) { const p = this._p.find(e => e[0] === k); return p ? p[1] : null; }
    getAll(k) { return this._p.filter(e => e[0] === k).map(e => e[1]); }
    has(k) { return this._p.some(e => e[0] === k); }
    append(k, v) { this._p.push([String(k), String(v)]); }
    delete(k) { this._p = this._p.filter(e => e[0] !== k); }

    set(k, v) {
        const p = this._p.find(e => e[0] === k);

        if (p) {
            p[1] = String(v);
            this._p = this._p.filter(e => e[0] !== k || e === p);
        } else {
            this.append(k, v);
        }
    }

    forEach(fn, self) { for (const [k, v] of this._p) fn.call(self, v, k, this); }
    keys() { return this._p.map(e => e[0])[Symbol.iterator](); }
    values() { return this._p.map(e => e[1])[Symbol.iterator](); }
    entries() { return this._p.map(e => [e[0], e[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this._p.map(e => formEncode(e[0]) + '=' + formEncode(e[1])).join('&'); }
}

class URL {
    constructor(url, base) {
        const href = base !== undefined ? resolveURL(url, String(base)) : String(url);
        const p = splitURL(href);

        if (!p.protocol) {
            throw new TypeError('Invalid URL: ' + url);
        }

        this._set(href);
    }

    _set(href) {
        const p = splitURL(href);
        const at = p.host.lastIndexOf('@');
        const host = at >= 0 ? p.host.slice(at + 1) : p.host;
        const colon = host.lastIndexOf(':');

        this.protocol = p.protocol.toLowerCase();
        this.host = host;
        this.hostname = colon >= 0 ? host.slice(0, colon) : host;
        this.port = colon >= 0 ? host.slice(colon + 1) : '';
        this.pathname = p.pathname || (p.hasHost ? '/' : '');
        this.search = p.search === '?' ? '' : p.search;
        this.hash = p.hash === '#' ? '' : p.hash;
        this.username = '';
        this.password = '';
        this.searchParams = new URLSearchParams(this.search);
    }

    get origin() { return this.protocol + '//' + this.host; }

    get href() {
        const q = this.searchParams.toString();

        return this.protocol + (this.host || this.protocol === 'file:' ? '//' : '') + this.host +
               this.pathname + (q ? '?' + q : '') + this.hash;
    }

    set href(v) { this._set(String(v)); }
    toString() { return this.href; }
    toJSON() { return this.href; }
}

/* ---------------------------------------------------------------------- */
/* Selectors                                                               */
/* ---------------------------------------------------------------------- */

const selectorCache = new Map();

function selectorError(text) {
    return new DOMException("'" + text + "' is not a valid selector", 'SyntaxError');
}

function hasClass(all, name) {
    if (all === name) {
        return true;
    }

    if (all.length <= name.length) {
        return false;
    }

    return (' ' + all.replace(/[\t\n\f\r]/g, ' ') + ' ').indexOf(' ' + name + ' ') >= 0;
}

function elementIndex(el, sameType, fromEnd) {
    const p = el.parentNode;

    if (!p) {
        return 1;
    }

    const kids = p.childNodes;
    let n = 0;

    if (fromEnd) {
        for (let i = kids.length - 1; i >= 0; i--) {
            const k = kids[i];

            if (k.nodeType === 1 && (!sameType || k.localName === el.localName)) {
                n++;

                if (k === el) {
                    return n;
                }
            }
        }
    } else {
        for (let i = 0; i < kids.length; i++) {
            const k = kids[i];

            if (k.nodeType === 1 && (!sameType || k.localName === el.localName)) {
                n++;

                if (k === el) {
                    return n;
                }
            }
        }
    }

    return n;
}

/* "2n+1", "odd", "3" ... -> a test on a 1-based position */
function nthTest(arg, text) {
    arg = arg.replace(/\s+/g, '').toLowerCase();

    let a = 0;
    let b = 0;

    if (arg === 'odd') {
        a = 2; b = 1;
    } else if (arg === 'even') {
        a = 2; b = 0;
    } else {
        const m = /^([+-]?\d*)?n([+-]\d+)?$/.exec(arg);

        if (m) {
            a = m[1] === undefined || m[1] === '' || m[1] === '+' ? 1 : (m[1] === '-' ? -1 : parseInt(m[1], 10));
            b = m[2] ? parseInt(m[2], 10) : 0;
        } else if (/^[+-]?\d+$/.test(arg)) {
            b = parseInt(arg, 10);
        } else {
            throw selectorError(text);
        }
    }

    return pos => a === 0 ? pos === b : ((pos - b) / a >= 0 && (pos - b) % a === 0);
}

function makePseudo(name, arg, text) {
    switch (name) {
        case 'first-child': return el => elementIndex(el, false, false) === 1;
        case 'last-child': return el => elementIndex(el, false, true) === 1;
        case 'only-child':
            return el => elementIndex(el, false, false) === 1 && elementIndex(el, false, true) === 1;
        case 'first-of-type': return el => elementIndex(el, true, false) === 1;
        case 'last-of-type': return el => elementIndex(el, true, true) === 1;
        case 'only-of-type':
            return el => elementIndex(el, true, false) === 1 && elementIndex(el, true, true) === 1;
        case 'nth-child': { const t = nthTest(arg || '', text); return el => t(elementIndex(el, false, false)); }
        case 'nth-last-child': { const t = nthTest(arg || '', text); return el => t(elementIndex(el, false, true)); }
        case 'nth-of-type': { const t = nthTest(arg || '', text); return el => t(elementIndex(el, true, false)); }
        case 'nth-last-of-type': { const t = nthTest(arg || '', text); return el => t(elementIndex(el, true, true)); }
        case 'not': { const list = parseSelector(arg || ''); return el => !matchesList(el, list); }
        case 'is':
        case 'where':
        case 'matches':
        case '-webkit-any': { const list = parseSelector(arg || ''); return el => matchesList(el, list); }
        case 'has': {
            const inner = arg || '';
            return el => {
                try {
                    return queryAll(el, /^\s*[>+~]/.test(inner) ? '*' : inner, true).length > 0;
                } catch (e) {
                    return false;
                }
            };
        }
        case 'checked': return el => el.checked === true || (el.localName === 'option' && el.selected);
        case 'disabled': return el => el.hasAttribute('disabled');
        case 'enabled': return el => CONTROLS.has(el.localName) && !el.hasAttribute('disabled');
        case 'empty': return el => el.childNodes.length === 0;
        case 'root': return el => el.parentNode === doc;
        case 'link':
        case 'any-link': return el => el.localName === 'a' && el.hasAttribute('href');
        case 'focus':
        case 'focus-visible': return el => el === focused;
        case 'focus-within': return el => !!focused && el.contains(focused);
        case 'hover':
        case 'active':
        case 'visited':
        case 'target':
        case 'before':
        case 'after':
        case 'first-line':
        case 'first-letter':
        case 'selection':
        case 'placeholder':
        case 'placeholder-shown':
        case 'required':
        case 'optional':
        case 'read-only':
        case 'invalid':
        case 'indeterminate':
            return () => false;
        case 'valid':
        case 'read-write':
        case 'scope':
            return () => true;
        default:
            throw selectorError(text);
    }
}

function compileSelector(src) {
    const n = src.length;
    const list = [];
    let i = 0;

    function space(c) {
        return c === 32 || (c >= 9 && c <= 13);
    }

    function skip() {
        while (i < n && space(src.charCodeAt(i))) {
            i++;
        }
    }

    function identStart(c) {
        return (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || c === 95 || c === 45 || c >= 128 || c === 92;
    }

    function ident() {
        const start = i;

        while (i < n) {
            const c = src.charCodeAt(i);

            if (c === 92 && i + 1 < n) {
                i += 2;
            } else if (identStart(c) || (c >= 48 && c <= 57)) {
                i++;
            } else {
                break;
            }
        }

        if (i === start) {
            throw selectorError(src);
        }

        const raw = src.slice(start, i);

        return raw.indexOf('\\') < 0 ? raw : raw.replace(/\\(.)/g, '$1');
    }

    function compound() {
        const c = { comb: ' ', tag: null, id: null, cls: null, attrs: null, pseudo: null };
        let any = false;

        if (src[i] === '*') {
            i++;
            any = true;
        } else if (i < n && identStart(src.charCodeAt(i))) {
            c.tag = ident().toLowerCase();
            any = true;
        }

        for (;;) {
            const ch = src[i];

            if (ch === '#') {
                i++;
                c.id = ident();
            } else if (ch === '.') {
                i++;
                (c.cls || (c.cls = [])).push(ident());
            } else if (ch === '[') {
                i++;
                skip();

                const name = ident().toLowerCase();
                let op = null;
                let val = null;

                skip();

                if (src[i] !== ']') {
                    const m = /^[~|^$*]?=/.exec(src.slice(i, i + 2));

                    if (!m) {
                        throw selectorError(src);
                    }

                    op = m[0];
                    i += op.length;
                    skip();

                    if (src[i] === '"' || src[i] === "'") {
                        const q = src[i++];
                        const end = src.indexOf(q, i);

                        if (end < 0) {
                            throw selectorError(src);
                        }

                        val = src.slice(i, end);
                        i = end + 1;
                    } else {
                        const start = i;

                        while (i < n && src[i] !== ']' && !space(src.charCodeAt(i))) {
                            i++;
                        }

                        val = src.slice(start, i);
                    }

                    skip();

                    if (src[i] === 'i' || src[i] === 's') {
                        i++;
                        skip();
                    }
                }

                if (src[i] !== ']') {
                    throw selectorError(src);
                }

                i++;
                (c.attrs || (c.attrs = [])).push({ name, op, val });
            } else if (ch === ':') {
                i++;

                if (src[i] === ':') {
                    i++;
                }

                const name = ident().toLowerCase();
                let arg = null;

                if (src[i] === '(') {
                    let depth = 1;
                    const start = ++i;

                    while (i < n && depth) {
                        if (src[i] === '(') depth++;
                        else if (src[i] === ')') depth--;
                        i++;
                    }

                    if (depth) {
                        throw selectorError(src);
                    }

                    arg = src.slice(start, i - 1).trim();
                }

                (c.pseudo || (c.pseudo = [])).push(makePseudo(name, arg, src));
            } else {
                break;
            }

            any = true;
        }

        if (!any) {
            throw selectorError(src);
        }

        return c;
    }

    for (;;) {
        skip();

        const chain = [compound()];

        for (;;) {
            const before = i;

            skip();

            const ch = src[i];

            if (i >= n || ch === ',') {
                break;
            }

            let comb = ' ';

            if (ch === '>' || ch === '+' || ch === '~') {
                comb = ch;
                i++;
                skip();
            } else if (before === i) {
                throw selectorError(src);
            }

            const c = compound();

            c.comb = comb;
            chain.push(c);
        }

        list.push(chain);

        if (src[i] === ',') {
            i++;
            continue;
        }

        if (i >= n) {
            break;
        }

        throw selectorError(src);
    }

    return list;
}

function parseSelector(text) {
    text = String(text);

    let list = selectorCache.get(text);

    if (!list) {
        list = compileSelector(text);

        if (selectorCache.size > 400) {
            selectorCache.clear();
        }

        selectorCache.set(text, list);
    }

    return list;
}

function matchAttr(el, a) {
    const v = el.getAttribute(a.name);

    if (v === null) {
        return false;
    }

    switch (a.op) {
        case null: return true;
        case '=': return v === a.val;
        case '~=': return a.val !== '' && hasClass(v, a.val);
        case '|=': return v === a.val || v.startsWith(a.val + '-');
        case '^=': return a.val !== '' && v.startsWith(a.val);
        case '$=': return a.val !== '' && v.endsWith(a.val);
        case '*=': return a.val !== '' && v.indexOf(a.val) >= 0;
    }

    return false;
}

function matchCompound(el, c) {
    if (c.tag !== null && el.localName !== c.tag) {
        return false;
    }

    if (c.id !== null && el.getAttribute('id') !== c.id) {
        return false;
    }

    if (c.cls) {
        const all = el.getAttribute('class');

        if (!all) {
            return false;
        }

        for (let i = 0; i < c.cls.length; i++) {
            if (!hasClass(all, c.cls[i])) {
                return false;
            }
        }
    }

    if (c.attrs) {
        for (let i = 0; i < c.attrs.length; i++) {
            if (!matchAttr(el, c.attrs[i])) {
                return false;
            }
        }
    }

    if (c.pseudo) {
        for (let i = 0; i < c.pseudo.length; i++) {
            if (!c.pseudo[i](el)) {
                return false;
            }
        }
    }

    return true;
}

function previousElement(el) {
    const p = el.parentNode;

    if (!p) {
        return null;
    }

    const kids = p.childNodes;

    for (let i = kids.indexOf(el) - 1; i >= 0; i--) {
        if (kids[i].nodeType === 1) {
            return kids[i];
        }
    }

    return null;
}

function matchChain(el, chain, k) {
    if (!matchCompound(el, chain[k])) {
        return false;
    }

    if (k === 0) {
        return true;
    }

    switch (chain[k].comb) {
        case '>': {
            const p = el.parentNode;
            return !!p && p.nodeType === 1 && matchChain(p, chain, k - 1);
        }

        case ' ':
            for (let p = el.parentNode; p && p.nodeType === 1; p = p.parentNode) {
                if (matchChain(p, chain, k - 1)) {
                    return true;
                }
            }

            return false;

        case '+': {
            const s = previousElement(el);
            return !!s && matchChain(s, chain, k - 1);
        }

        case '~':
            for (let s = previousElement(el); s; s = previousElement(s)) {
                if (matchChain(s, chain, k - 1)) {
                    return true;
                }
            }

            return false;
    }

    return false;
}

function matchesList(el, list) {
    for (let i = 0; i < list.length; i++) {
        if (matchChain(el, list[i], list[i].length - 1)) {
            return true;
        }
    }

    return false;
}

/* Calls fn for every element below root, in document order; stops when
   fn returns true. */
function eachElement(root, fn) {
    const kids = root.childNodes;

    for (let i = 0; i < kids.length; i++) {
        const k = kids[i];

        if (k.nodeType === 1) {
            if (fn(k) === true || eachElement(k, fn) === true) {
                return true;
            }
        }
    }

    return false;
}

function queryAll(root, selector, firstOnly) {
    const list = parseSelector(selector);
    const out = new NodeList();

    eachElement(root, el => {
        if (matchesList(el, list)) {
            out.push(el);
            return firstOnly;
        }

        return false;
    });

    return out;
}

/* ---------------------------------------------------------------------- */
/* Nodes                                                                   */
/* ---------------------------------------------------------------------- */

class NodeList extends Array {
    item(i) { return this[i] || null; }
    namedItem(name) {
        return this.find(e => e.getAttribute && (e.getAttribute('id') === name ||
                                                e.getAttribute('name') === name)) || null;
    }
}

function toNode(v) {
    return v instanceof Node ? v : new Text(String(v));
}

function insert(parent, child, ref) {
    if (!(child instanceof Node)) {
        throw new TypeError('The value to insert is not a node');
    }

    if (parent.childNodes === EMPTY) {
        throw new DOMException('This node cannot have children', 'HierarchyRequestError');
    }

    if (child.nodeType === 11) {
        for (const k of child.childNodes.slice()) {
            insert(parent, k, ref);
        }

        return child;
    }

    if (child === ref) {
        return child;
    }

    if (child.childNodes !== EMPTY && child.contains(parent)) {
        throw new DOMException('A node cannot be put inside itself', 'HierarchyRequestError');
    }

    if (child.parentNode) {
        const old = child.parentNode.childNodes;

        old.splice(old.indexOf(child), 1);
    }

    const kids = parent.childNodes;

    if (ref) {
        const i = kids.indexOf(ref);

        if (i < 0) {
            throw new DOMException('The node to insert before is not a child', 'NotFoundError');
        }

        kids.splice(i, 0, child);
    } else {
        kids.push(child);
    }

    child.parentNode = parent;
    dirty = true;

    if (child.nodeType === 1) {
        treeVersion++;

        if (CONTROLS.has(child.localName)) {
            interactive = true;
        }

        if (scriptsLive && parent.isConnected) {
            scheduleScripts(child);
        }
    }

    return child;
}

class Node extends EventTarget {
    get firstChild() { const c = this.childNodes; return c.length ? c[0] : null; }
    get lastChild() { const c = this.childNodes; return c.length ? c[c.length - 1] : null; }

    get nextSibling() {
        const p = this.parentNode;

        if (!p) {
            return null;
        }

        const c = p.childNodes;
        const i = c.indexOf(this);

        return i + 1 < c.length ? c[i + 1] : null;
    }

    get previousSibling() {
        const p = this.parentNode;

        if (!p) {
            return null;
        }

        const c = p.childNodes;
        const i = c.indexOf(this);

        return i > 0 ? c[i - 1] : null;
    }

    get parentElement() {
        const p = this.parentNode;
        return p && p.nodeType === 1 ? p : null;
    }

    get ownerDocument() { return this.nodeType === 9 ? null : doc; }

    get isConnected() {
        for (let n = this; n; n = n.parentNode) {
            if (n === doc) {
                return true;
            }
        }

        return false;
    }

    get nodeValue() { return null; }
    set nodeValue(v) { }
    get baseURI() { return native.url(); }

    get textContent() {
        const parts = [];

        (function walk(n) {
            const kids = n.childNodes;

            for (let i = 0; i < kids.length; i++) {
                const k = kids[i];

                if (k.nodeType === 3) {
                    parts.push(k._d);
                } else if (k.nodeType === 1) {
                    walk(k);
                }
            }
        })(this);

        return parts.join('');
    }

    set textContent(v) {
        clearChildren(this);

        v = (v === null || v === undefined) ? '' : String(v);

        if (v !== '') {
            const t = new Text(v);

            t.parentNode = this;
            this.childNodes.push(t);
        }

        dirty = true;
    }

    hasChildNodes() { return this.childNodes.length > 0; }

    contains(other) {
        for (; other; other = other.parentNode) {
            if (other === this) {
                return true;
            }
        }

        return false;
    }

    getRootNode() {
        let n = this;

        while (n.parentNode) {
            n = n.parentNode;
        }

        return n;
    }

    appendChild(child) { return insert(this, child, null); }
    insertBefore(child, ref) { return insert(this, child, ref || null); }

    removeChild(child) {
        const kids = this.childNodes;
        const i = kids.indexOf(child);

        if (i < 0) {
            throw new DOMException('The node to remove is not a child of this node', 'NotFoundError');
        }

        kids.splice(i, 1);
        child.parentNode = null;
        dirty = true;

        if (child.nodeType === 1) {
            treeVersion++;
        }

        if (focused && (child === focused || child.contains(focused))) {
            focused = null;
        }

        return child;
    }

    replaceChild(fresh, old) {
        if (fresh === old) {
            return old;
        }

        insert(this, fresh, old);
        this.removeChild(old);

        return old;
    }

    isSameNode(other) { return other === this; }
    isEqualNode(other) { return other === this; }
    compareDocumentPosition(other) { return other === this ? 0 : (this.contains(other) ? 20 : (other && other.contains(this) ? 10 : 1)); }
    normalize() { }
    lookupNamespaceURI() { return null; }

    /* ChildNode / ParentNode conveniences */
    remove() { if (this.parentNode) this.parentNode.removeChild(this); }

    before(...nodes) {
        const p = this.parentNode;

        if (p) {
            for (const n of nodes) insert(p, toNode(n), this);
        }
    }

    after(...nodes) {
        const p = this.parentNode;

        if (p) {
            const ref = this.nextSibling;

            for (const n of nodes) insert(p, toNode(n), ref);
        }
    }

    replaceWith(...nodes) {
        const p = this.parentNode;

        if (p) {
            for (const n of nodes) insert(p, toNode(n), this);
            p.removeChild(this);
        }
    }

    append(...nodes) { for (const n of nodes) insert(this, toNode(n), null); }

    prepend(...nodes) {
        const first = this.firstChild;

        for (const n of nodes) insert(this, toNode(n), first);
    }

    replaceChildren(...nodes) {
        clearChildren(this);

        for (const n of nodes) insert(this, toNode(n), null);
    }

    get children() {
        const out = new NodeList();
        const kids = this.childNodes;

        for (let i = 0; i < kids.length; i++) {
            if (kids[i].nodeType === 1) {
                out.push(kids[i]);
            }
        }

        return out;
    }

    get childElementCount() { return this.children.length; }

    get firstElementChild() {
        const kids = this.childNodes;

        for (let i = 0; i < kids.length; i++) {
            if (kids[i].nodeType === 1) return kids[i];
        }

        return null;
    }

    get lastElementChild() {
        const kids = this.childNodes;

        for (let i = kids.length - 1; i >= 0; i--) {
            if (kids[i].nodeType === 1) return kids[i];
        }

        return null;
    }

    querySelector(selector) { return queryAll(this, selector, true)[0] || null; }
    querySelectorAll(selector) { return queryAll(this, selector, false); }

    getElementsByTagName(name) {
        name = String(name).toLowerCase();

        const out = new NodeList();

        eachElement(this, el => {
            if (name === '*' || el.localName === name) {
                out.push(el);
            }
        });

        return out;
    }

    getElementsByClassName(names) {
        const want = String(names).split(/\s+/).filter(Boolean);
        const out = new NodeList();

        if (want.length) {
            eachElement(this, el => {
                const all = el.getAttribute('class');

                if (all && want.every(w => hasClass(all, w))) {
                    out.push(el);
                }
            });
        }

        return out;
    }
}

Node.ELEMENT_NODE = 1;
Node.TEXT_NODE = 3;
Node.COMMENT_NODE = 8;
Node.DOCUMENT_NODE = 9;
Node.DOCUMENT_TYPE_NODE = 10;
Node.DOCUMENT_FRAGMENT_NODE = 11;

for (const k of ['ELEMENT_NODE', 'TEXT_NODE', 'COMMENT_NODE', 'DOCUMENT_NODE',
                 'DOCUMENT_TYPE_NODE', 'DOCUMENT_FRAGMENT_NODE']) {
    Node.prototype[k] = Node[k];
}

function clearChildren(node) {
    const kids = node.childNodes;

    if (kids === EMPTY) {
        return;
    }

    for (let i = 0; i < kids.length; i++) {
        kids[i].parentNode = null;

        if (kids[i].nodeType === 1) {
            treeVersion++;
        }
    }

    if (focused && node.contains(focused) && node !== focused) {
        focused = null;
    }

    kids.length = 0;
    dirty = true;
}

class CharacterData extends Node {
    constructor(type, name, data) {
        super();
        this.nodeType = type;
        this.nodeName = name;
        this.parentNode = null;
        this._d = data;
    }

    get childNodes() { return EMPTY; }
    get data() { return this._d; }
    set data(v) { this._d = String(v); dirty = true; }
    get nodeValue() { return this._d; }
    set nodeValue(v) { this.data = v; }
    get textContent() { return this._d; }
    set textContent(v) { this.data = v; }
    get length() { return this._d.length; }
    get wholeText() { return this._d; }
    appendData(s) { this.data = this._d + s; }
    substringData(o, c) { return this._d.substr(o, c); }
}

class Text extends CharacterData {
    constructor(data) { super(3, '#text', data === undefined ? '' : String(data)); }
    cloneNode() { return new Text(this._d); }
}

class Comment extends CharacterData {
    constructor(data) { super(8, '#comment', data === undefined ? '' : String(data)); }
    cloneNode() { return new Comment(this._d); }
}

class DocumentFragment extends Node {
    constructor() {
        super();
        this.nodeType = 11;
        this.nodeName = '#document-fragment';
        this.parentNode = null;
        this.childNodes = [];
    }

    getElementById(id) { return findById(this, String(id)); }

    cloneNode(deep) {
        const f = new DocumentFragment();

        if (deep) {
            for (const k of this.childNodes) copyInto(f, k.cloneNode(true));
        }

        return f;
    }
}

function copyInto(parent, child) {
    child.parentNode = parent;
    parent.childNodes.push(child);
}

function findById(root, id) {
    let found = null;

    eachElement(root, el => {
        if (el.getAttribute('id') === id) {
            found = el;
            return true;
        }

        return false;
    });

    return found;
}

/* ---- style, classList, dataset ---- */

function parseStyle(text) {
    const map = new Map();

    if (text) {
        for (const part of text.split(';')) {
            const i = part.indexOf(':');

            if (i > 0) {
                const k = part.slice(0, i).trim().toLowerCase();
                const v = part.slice(i + 1).trim();

                if (k) {
                    map.set(k, v);
                }
            }
        }
    }

    return map;
}

function writeStyle(el, map) {
    const parts = [];

    for (const [k, v] of map) {
        parts.push(k + ': ' + v);
    }

    if (parts.length) {
        el.setAttribute('style', parts.join('; '));
    } else {
        el.removeAttribute('style');
    }
}

function styleOf(el) {
    if (el._style) {
        return el._style;
    }

    const api = {
        getPropertyValue(name) {
            return parseStyle(el.getAttribute('style')).get(String(name).toLowerCase()) || '';
        },
        getPropertyPriority() { return ''; },
        setProperty(name, value) {
            const map = parseStyle(el.getAttribute('style'));

            name = String(name).toLowerCase();

            if (value === null || value === undefined || value === '') {
                map.delete(name);
            } else {
                map.set(name, String(value));
            }

            writeStyle(el, map);
        },
        removeProperty(name) {
            const map = parseStyle(el.getAttribute('style'));
            const old = map.get(String(name).toLowerCase()) || '';

            map.delete(String(name).toLowerCase());
            writeStyle(el, map);

            return old;
        },
        item(i) { return [...parseStyle(el.getAttribute('style')).keys()][i] || ''; }
    };

    return el._style = new Proxy(api, {
        get(t, k) {
            if (typeof k !== 'string') {
                return undefined;
            }

            if (k in t) {
                return t[k];
            }

            if (k === 'cssText') {
                return el.getAttribute('style') || '';
            }

            if (k === 'length') {
                return parseStyle(el.getAttribute('style')).size;
            }

            return t.getPropertyValue(kebab(k));
        },
        set(t, k, v) {
            if (typeof k !== 'string') {
                return true;
            }

            if (k === 'cssText') {
                el.setAttribute('style', String(v));
            } else {
                t.setProperty(kebab(k), v);
            }

            return true;
        }
    });
}

function classListOf(el) {
    if (el._classes) {
        return el._classes;
    }

    const read = () => {
        const c = el.getAttribute('class');
        return c ? c.split(/\s+/).filter(Boolean) : [];
    };

    const write = list => el.setAttribute('class', list.join(' '));

    return el._classes = {
        add(...names) {
            const list = read();
            let changed = false;

            for (const n of names) {
                const s = String(n);

                if (s && list.indexOf(s) < 0) {
                    list.push(s);
                    changed = true;
                }
            }

            if (changed) write(list);
        },
        remove(...names) {
            const list = read();
            const kept = list.filter(c => names.indexOf(c) < 0);

            if (kept.length !== list.length) write(kept);
        },
        toggle(name, force) {
            const list = read();
            const has = list.indexOf(name) >= 0;
            const want = force === undefined ? !has : !!force;

            if (want && !has) {
                list.push(String(name));
                write(list);
            } else if (!want && has) {
                write(list.filter(c => c !== name));
            }

            return want;
        },
        contains(name) { return read().indexOf(String(name)) >= 0; },
        replace(a, b) {
            const list = read();
            const i = list.indexOf(a);

            if (i < 0) return false;

            list[i] = String(b);
            write(list);
            return true;
        },
        item(i) { return read()[i] || null; },
        forEach(fn, self) { read().forEach(fn, self); },
        get length() { return read().length; },
        get value() { return el.getAttribute('class') || ''; },
        set value(v) { el.setAttribute('class', String(v)); },
        toString() { return el.getAttribute('class') || ''; },
        [Symbol.iterator]() { return read()[Symbol.iterator](); }
    };
}

function datasetOf(el) {
    return el._dataset || (el._dataset = new Proxy({}, {
        get(t, k) {
            if (typeof k !== 'string') return undefined;
            const v = el.getAttribute('data-' + kebab(k));
            return v === null ? undefined : v;
        },
        set(t, k, v) {
            if (typeof k === 'string') el.setAttribute('data-' + kebab(k), String(v));
            return true;
        },
        has(t, k) { return typeof k === 'string' && el.hasAttribute('data-' + kebab(k)); },
        deleteProperty(t, k) {
            if (typeof k === 'string') el.removeAttribute('data-' + kebab(k));
            return true;
        },
        ownKeys() {
            return el.getAttributeNames().filter(n => n.startsWith('data-')).map(n => camel(n.slice(5)));
        },
        getOwnPropertyDescriptor(t, k) {
            if (typeof k === 'string' && el.hasAttribute('data-' + kebab(k))) {
                return { value: el.getAttribute('data-' + kebab(k)), writable: true,
                         enumerable: true, configurable: true };
            }

            return undefined;
        }
    }));
}

const ZERO_RECT = Object.freeze({
    x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0,
    toJSON() { return this; }
});

class Element extends Node {
    constructor(name) {
        super();
        this.nodeType = 1;
        this.localName = name;
        this.parentNode = null;
        this.childNodes = [];
        this._a = null;         /* attributes: [name, value, name, value, ...] */
    }

    get tagName() { return this.localName.toUpperCase(); }
    get nodeName() { return this.localName.toUpperCase(); }
    get namespaceURI() { return 'http://www.w3.org/1999/xhtml'; }

    /* ---- attributes ---- */

    getAttribute(name) {
        const a = this._a;

        if (a) {
            for (let i = 0; i < a.length; i += 2) {
                if (a[i] === name) {
                    return a[i + 1];
                }
            }

            if (typeof name !== 'string' || /[A-Z]/.test(name)) {
                name = String(name).toLowerCase();

                for (let i = 0; i < a.length; i += 2) {
                    if (a[i] === name) {
                        return a[i + 1];
                    }
                }
            }
        }

        return null;
    }

    setAttribute(name, value) {
        name = String(name).toLowerCase();
        value = String(value);

        const a = this._a || (this._a = []);

        for (let i = 0; i < a.length; i += 2) {
            if (a[i] === name) {
                if (a[i + 1] !== value) {
                    a[i + 1] = value;
                    this._changed(name);
                }

                return;
            }
        }

        a.push(name, value);
        this._changed(name);
    }

    removeAttribute(name) {
        name = String(name).toLowerCase();

        const a = this._a;

        if (a) {
            for (let i = 0; i < a.length; i += 2) {
                if (a[i] === name) {
                    a.splice(i, 2);
                    this._changed(name);
                    return;
                }
            }
        }
    }

    _changed(name) {
        dirty = true;

        if (name === 'id') {
            treeVersion++;
        } else if (name.charCodeAt(0) === 111 && name.charCodeAt(1) === 110) {
            interactive = true;     /* on... */
        }
    }

    hasAttribute(name) { return this.getAttribute(name) !== null; }
    hasAttributes() { return !!this._a && this._a.length > 0; }

    toggleAttribute(name, force) {
        const has = this.hasAttribute(name);
        const want = force === undefined ? !has : !!force;

        if (want && !has) this.setAttribute(name, '');
        else if (!want && has) this.removeAttribute(name);

        return want;
    }

    getAttributeNames() {
        const out = [];
        const a = this._a;

        if (a) {
            for (let i = 0; i < a.length; i += 2) out.push(a[i]);
        }

        return out;
    }

    getAttributeNS(ns, name) { return this.getAttribute(name); }
    setAttributeNS(ns, name, value) { this.setAttribute(String(name).replace(/^.*:/, ''), value); }
    removeAttributeNS(ns, name) { this.removeAttribute(name); }
    hasAttributeNS(ns, name) { return this.hasAttribute(name); }

    getAttributeNode(name) {
        const v = this.getAttribute(name);
        return v === null ? null : { name: String(name).toLowerCase(), value: v, specified: true };
    }

    get attributes() {
        const out = [];
        const a = this._a;

        if (a) {
            for (let i = 0; i < a.length; i += 2) {
                out.push({ name: a[i], value: a[i + 1], nodeName: a[i], nodeValue: a[i + 1], specified: true });
            }
        }

        out.getNamedItem = name => out.find(e => e.name === name) || null;
        out.item = i => out[i] || null;

        return out;
    }

    /* ---- content ---- */

    get innerHTML() {
        const out = [];

        for (const k of this.childNodes) serialize(k, out);

        return out.join('');
    }

    set innerHTML(html) {
        clearChildren(this);
        parseFragment(this, String(html === null ? '' : html));
        dirty = true;
        treeVersion++;

        if (scriptsLive && this.isConnected) {
            /* scripts added this way do not run, as in other browsers */
            eachElement(this, el => { if (el.localName === 'script') el._ran = true; });
        }
    }

    get outerHTML() {
        const out = [];

        serialize(this, out);

        return out.join('');
    }

    set outerHTML(html) {
        const p = this.parentNode;

        if (p) {
            const holder = new Element(p.nodeType === 1 ? p.localName : 'div');

            parseFragment(holder, String(html));

            for (const k of holder.childNodes.slice()) insert(p, k, this);

            p.removeChild(this);
        }
    }

    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    get outerText() { return this.textContent; }
    get text() { return this.textContent; }
    set text(v) { this.textContent = v; }

    insertAdjacentHTML(where, html) {
        const holder = new Element('div');

        parseFragment(holder, String(html));

        const nodes = holder.childNodes.slice();

        switch (String(where).toLowerCase()) {
            case 'beforebegin': this.before(...nodes); break;
            case 'afterbegin': this.prepend(...nodes); break;
            case 'beforeend': this.append(...nodes); break;
            case 'afterend': this.after(...nodes); break;
        }
    }

    insertAdjacentElement(where, el) {
        switch (String(where).toLowerCase()) {
            case 'beforebegin': this.before(el); break;
            case 'afterbegin': this.prepend(el); break;
            case 'beforeend': this.append(el); break;
            case 'afterend': this.after(el); break;
        }

        return el;
    }

    insertAdjacentText(where, text) {
        this.insertAdjacentElement(where, new Text(String(text)));
    }

    cloneNode(deep) {
        const el = new Element(this.localName);

        if (this._a) el._a = this._a.slice();
        if (this._v !== undefined) el._v = this._v;
        if (this._c !== undefined) el._c = this._c;

        if (deep) {
            for (const k of this.childNodes) copyInto(el, k.cloneNode(true));
        }

        return el;
    }

    matches(selector) { return matchesList(this, parseSelector(selector)); }
    webkitMatchesSelector(selector) { return this.matches(selector); }

    closest(selector) {
        const list = parseSelector(selector);

        for (let el = this; el && el.nodeType === 1; el = el.parentNode) {
            if (matchesList(el, list)) {
                return el;
            }
        }

        return null;
    }

    get nextElementSibling() {
        for (let n = this.nextSibling; n; n = n.nextSibling) {
            if (n.nodeType === 1) return n;
        }

        return null;
    }

    get previousElementSibling() { return previousElement(this); }

    /* ---- reflected properties ---- */

    get style() { return styleOf(this); }
    set style(v) { this.setAttribute('style', String(v)); }
    get classList() { return classListOf(this); }
    get dataset() { return datasetOf(this); }

    get value() {
        switch (this.localName) {
            case 'select': {
                const o = selectedOption(this);
                return o ? o.value : '';
            }

            case 'option': {
                const v = this.getAttribute('value');
                return v !== null ? v : this.textContent.trim();
            }

            case 'textarea':
                return this._v !== undefined ? this._v : this.textContent;

            default:
                if (this._v !== undefined) return this._v;

                const v = this.getAttribute('value');

                if (v !== null) return v;

                return (this.localName === 'input' && /^(checkbox|radio)$/i.test(this.getAttribute('type') || '')) ? 'on' : '';
        }
    }

    set value(v) {
        v = (v === null || v === undefined) ? '' : String(v);

        if (this.localName === 'select') {
            for (const o of this.options) o._s = (o.value === v);
        } else {
            this._v = v;
        }

        dirty = true;
    }

    get defaultValue() { return this.getAttribute('value') || ''; }
    set defaultValue(v) { this.setAttribute('value', v); }

    get checked() { return this._c !== undefined ? this._c : this.hasAttribute('checked'); }
    set checked(v) { this._c = !!v; dirty = true; }

    get selected() {
        if (this._s !== undefined) return this._s;
        return this.hasAttribute('selected');
    }

    set selected(v) {
        const sel = this.closest('select');

        if (v && sel && !sel.hasAttribute('multiple')) {
            for (const o of sel.options) o._s = false;
        }

        this._s = !!v;
        dirty = true;
    }

    get options() { return this.localName === 'select' ? this.getElementsByTagName('option') : undefined; }
    get selectedIndex() { return this.options ? this.options.indexOf(selectedOption(this)) : -1; }

    set selectedIndex(i) {
        const opts = this.options;

        if (opts) {
            opts.forEach((o, k) => { o._s = (k === i); });
            dirty = true;
        }
    }

    get selectedOptions() { return this.options ? this.options.filter(o => o.selected) : undefined; }

    get type() {
        const t = this.getAttribute('type');

        if (this.localName === 'input') return (t || 'text').toLowerCase();
        if (this.localName === 'button') return (t || 'submit').toLowerCase();
        if (this.localName === 'select') return 'select-one';
        if (this.localName === 'textarea') return 'textarea';

        return t || '';
    }

    set type(v) { this.setAttribute('type', v); }

    get form() { return this.closest('form'); }

    get elements() {
        return this.localName === 'form'
            ? queryAll(this, 'input,select,textarea,button', false) : undefined;
    }

    get labels() { return new NodeList(); }
    get tabIndex() { const v = parseInt(this.getAttribute('tabindex'), 10); return isNaN(v) ? -1 : v; }
    set tabIndex(v) { this.setAttribute('tabindex', v); }

    submit() { if (this.localName === 'form') submitForm(this, null, false); }
    requestSubmit(by) { if (this.localName === 'form') submitForm(this, by || null, true); }

    reset() {
        if (this.localName === 'form') {
            for (const c of this.elements) {
                c._v = undefined;
                c._c = undefined;
            }

            eachElement(this, el => { if (el.localName === 'option') el._s = undefined; });
            dirty = true;
        }
    }

    checkValidity() { return true; }
    reportValidity() { return true; }
    setCustomValidity() { }

    /* ---- things the screen would decide; this browser has no boxes ---- */

    getBoundingClientRect() { return ZERO_RECT; }
    getClientRects() { return []; }
    get offsetParent() { return null; }
    get offsetWidth() { return 0; }
    get offsetHeight() { return 0; }
    get offsetTop() { return 0; }
    get offsetLeft() { return 0; }
    get clientTop() { return 0; }
    get clientLeft() { return 0; }
    get clientWidth() { return this.parentNode === doc ? native.size()[0] : 0; }
    get clientHeight() { return this.parentNode === doc ? native.size()[1] : 0; }
    get scrollWidth() { return 0; }
    get scrollHeight() { return 0; }
    get scrollTop() { return 0; }
    set scrollTop(v) { }
    get scrollLeft() { return 0; }
    set scrollLeft(v) { }
    scrollIntoView() { }
    scrollTo() { }
    scrollBy() { }
    scroll() { }
    animate() { return { cancel() { }, finish() { }, play() { }, pause() { }, finished: Promise.resolve() }; }
    getAnimations() { return []; }
    getContext() { return null; }
    setPointerCapture() { }
    releasePointerCapture() { }
    select() { }
    setSelectionRange() { }
    get complete() { return true; }
    get naturalWidth() { return 0; }
    get naturalHeight() { return 0; }
    get isContentEditable() { return false; }
    get shadowRoot() { return null; }
    get sheet() { return null; }
    get content() {
        if (this.localName !== 'template') {
            return this.getAttribute('content') || '';
        }

        return this._content || (this._content = new DocumentFragment());
    }

    set content(v) { this.setAttribute('content', v); }

    focus() { setFocus(this); }
    blur() { if (focused === this) setFocus(null); }
    click() { clickNode(this, false); }
}

/* Properties that simply mirror an attribute. */
for (const [prop, attr] of [
    ['id', 'id'], ['className', 'class'], ['title', 'title'], ['lang', 'lang'],
    ['dir', 'dir'], ['name', 'name'], ['alt', 'alt'], ['placeholder', 'placeholder'],
    ['rel', 'rel'], ['target', 'target'], ['method', 'method'], ['htmlFor', 'for'],
    ['accessKey', 'accesskey'], ['role', 'role'], ['charset', 'charset'],
    ['media', 'media'], ['enctype', 'enctype'], ['download', 'download'],
    ['slot', 'slot'], ['nonce', 'nonce'], ['integrity', 'integrity'],
    ['crossOrigin', 'crossorigin'], ['loading', 'loading'], ['srcset', 'srcset'],
    ['sizes', 'sizes'], ['label', 'label'], ['pattern', 'pattern'],
    ['min', 'min'], ['max', 'max'], ['step', 'step'], ['autocomplete', 'autocomplete'],
    ['httpEquiv', 'http-equiv'], ['as', 'as'], ['coords', 'coords'], ['scope', 'scope']]) {
    Object.defineProperty(Element.prototype, prop, {
        get() { return this.getAttribute(attr) || ''; },
        set(v) { this.setAttribute(attr, v); },
        configurable: true
    });
}

/* Properties that are true when the attribute is present. */
for (const [prop, attr] of [
    ['disabled', 'disabled'], ['hidden', 'hidden'], ['readOnly', 'readonly'],
    ['required', 'required'], ['multiple', 'multiple'], ['defer', 'defer'],
    ['async', 'async'], ['autofocus', 'autofocus'], ['noModule', 'nomodule'],
    ['open', 'open'], ['draggable', 'draggable'], ['controls', 'controls'],
    ['autoplay', 'autoplay'], ['loop', 'loop'], ['muted', 'muted'],
    ['defaultChecked', 'checked'], ['defaultSelected', 'selected'],
    ['noValidate', 'novalidate'], ['inert', 'inert']]) {
    Object.defineProperty(Element.prototype, prop, {
        get() { return this.hasAttribute(attr); },
        set(v) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); },
        configurable: true
    });
}

/* Properties holding an address: reading gives the full address. */
for (const prop of ['href', 'src', 'action', 'poster', 'cite', 'formAction']) {
    const attr = prop.toLowerCase();

    Object.defineProperty(Element.prototype, prop, {
        get() {
            const v = this.getAttribute(attr);

            if (v === null) {
                return prop === 'action' ? native.url() : '';
            }

            return resolveURL(v);
        },
        set(v) { this.setAttribute(attr, v); },
        configurable: true
    });
}

/* Numbers. */
for (const prop of ['width', 'height', 'size', 'maxLength', 'minLength', 'rows',
                    'cols', 'colSpan', 'rowSpan', 'start']) {
    const attr = prop.toLowerCase();

    Object.defineProperty(Element.prototype, prop, {
        get() { const v = parseInt(this.getAttribute(attr), 10); return isNaN(v) ? 0 : v; },
        set(v) { this.setAttribute(attr, v); },
        configurable: true
    });
}

/* Parts of a link's address (a.hostname and friends). */
for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
    Object.defineProperty(Element.prototype, part, {
        get() {
            try {
                return new URL(this.href)[part];
            } catch (e) {
                return '';
            }
        },
        configurable: true
    });
}

function selectedOption(select) {
    const opts = select.options;
    let chosen = null;

    for (const o of opts) {
        if (o._s === true) return o;
        if (o._s === undefined && o.hasAttribute('selected') && !chosen) chosen = o;
    }

    return chosen || opts.find(o => o._s !== false) || opts[0] || null;
}

/* ---------------------------------------------------------------------- */
/* Document                                                                */
/* ---------------------------------------------------------------------- */

let idMap = null;
let idMapVersion = -1;

class Document extends Node {
    constructor() {
        super();
        this.nodeType = 9;
        this.nodeName = '#document';
        this.parentNode = null;
        this.childNodes = [];
        this.readyState = 'loading';
        this.currentScript = null;
        this._cookie = '';
    }

    get documentElement() { return this.firstElementChild; }
    get head() { const r = this.documentElement; return r ? r.childNodes.find(n => n.localName === 'head') || null : null; }
    get body() { const r = this.documentElement; return r ? r.childNodes.find(n => n.localName === 'body') || null : null; }
    get scrollingElement() { return this.documentElement; }
    get defaultView() { return global; }
    get ownerDocument() { return null; }
    get doctype() { return { name: 'html', publicId: '', systemId: '', nodeType: 10 }; }
    get compatMode() { return 'CSS1Compat'; }
    get characterSet() { return 'UTF-8'; }
    get charset() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get URL() { return native.url(); }
    get documentURI() { return native.url(); }
    get referrer() { return ''; }
    get domain() { return splitURL(native.url()).host; }
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get activeElement() { return focused || this.body; }
    get styleSheets() { return []; }
    get fonts() { return { ready: Promise.resolve(), load() { return Promise.resolve([]); }, check() { return true; } }; }
    get implementation() {
        return {
            hasFeature() { return true; },
            createHTMLDocument(title) { return makeDocument('<title>' + escapeText(title || '') + '</title>'); }
        };
    }

    get location() { return location; }
    set location(v) { location.href = v; }

    get cookie() { return this._cookie; }

    set cookie(v) {
        /* kept only for this page: "name=value" pairs, attributes dropped */
        const pair = String(v).split(';')[0].trim();
        const name = pair.split('=')[0];
        const rest = this._cookie.split('; ').filter(c => c && c.split('=')[0] !== name);

        if (pair) rest.push(pair);

        this._cookie = rest.join('; ');
    }

    get title() {
        const t = this.getElementsByTagName('title')[0];
        return t ? t.textContent.replace(/\s+/g, ' ').trim() : '';
    }

    set title(v) {
        let t = this.getElementsByTagName('title')[0];

        if (!t && this.head) {
            t = this.head.appendChild(new Element('title'));
        }

        if (t) t.textContent = v;
    }

    get forms() { return this.getElementsByTagName('form'); }
    get images() { return this.getElementsByTagName('img'); }
    get scripts() { return this.getElementsByTagName('script'); }
    get links() { return queryAll(this, 'a[href],area[href]', false); }
    get all() { return this.getElementsByTagName('*'); }

    createElement(name) { return new Element(String(name).toLowerCase()); }
    createElementNS(ns, name) { return new Element(String(name).replace(/^.*:/, '').toLowerCase()); }
    createTextNode(data) { return new Text(String(data)); }
    createComment(data) { return new Comment(String(data)); }
    createDocumentFragment() { return new DocumentFragment(); }

    createEvent(kind) {
        const k = String(kind).toLowerCase();
        const ev = k.startsWith('mouse') ? new MouseEvent('')
                 : (k.startsWith('key') ? new KeyboardEvent('')
                 : (k.startsWith('custom') ? new CustomEvent('') : new Event('')));

        return ev;
    }

    createRange() {
        return {
            setStart() { }, setEnd() { }, selectNode() { }, selectNodeContents() { },
            collapse() { }, getBoundingClientRect() { return ZERO_RECT; }, getClientRects() { return []; },
            createContextualFragment(html) {
                const f = new DocumentFragment();
                parseFragment(f, String(html));
                return f;
            }
        };
    }

    createTreeWalker(root) {
        const nodes = [];

        (function walk(n) { for (const k of n.childNodes) { nodes.push(k); walk(k); } })(root);

        let i = -1;

        return { currentNode: root, nextNode() { i++; return this.currentNode = nodes[i] || null; } };
    }

    getElementById(id) {
        if (this !== doc) {
            return findById(this, String(id));
        }

        if (idMapVersion !== treeVersion) {
            idMap = new Map();

            eachElement(this, el => {
                const v = el.getAttribute('id');

                if (v !== null && !idMap.has(v)) {
                    idMap.set(v, el);
                }
            });

            idMapVersion = treeVersion;
        }

        return idMap.get(String(id)) || null;
    }

    getElementsByName(name) { return queryAll(this, '[name="' + String(name).replace(/"/g, '\\"') + '"]', false); }

    hasFocus() { return true; }
    getSelection() { return global.getSelection(); }
    execCommand() { return false; }
    queryCommandSupported() { return false; }
    elementFromPoint() { return this.body; }
    elementsFromPoint() { return []; }
    adoptNode(n) { return n; }
    importNode(n, deep) { return n.cloneNode(deep); }
    open() { return this; }
    close() { }

    write(...parts) {
        const html = parts.join('');
        const script = this.currentScript;

        if (scriptsLive || !script || !script.parentNode) {
            native.log('warn', 'document.write() after the page has loaded was ignored');
            return;
        }

        let parent = script.parentNode;
        let ref = script.nextSibling;

        if (parent.localName === 'head') {
            parent = this.body;
            ref = null;
        }

        const holder = new Element(parent.localName);

        parseFragment(holder, html);

        for (const k of holder.childNodes.slice()) {
            insert(parent, k, ref);
        }
    }

    writeln(...parts) { this.write(...parts, '\n'); }

    cloneNode() { return makeDocument(''); }
}

/* ---------------------------------------------------------------------- */
/* HTML -> tree                                                            */
/* ---------------------------------------------------------------------- */

const NAME_RE = /[a-zA-Z][^\s\/>]*/y;
const ATTR_RE = /[\s\/]*([^\s"'<>\/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]*)))?/y;
const TAG_END_RE = /[\s\/]*>/y;
const closeTagRe = {};

function addText(parent, data) {
    const t = new Text(data);

    t.parentNode = parent;
    parent.childNodes.push(t);
}

/*
 * Parses HTML into `root`. With `d` set, this is a whole page: content
 * goes to d.head or d.body as appropriate. Otherwise it is a fragment and
 * everything goes into `root`.
 */
function parseInto(root, html, d) {
    const n = html.length;
    const head = d ? d.head : null;
    const body = d ? d.body : null;
    let inHead = !!d;
    let stack = [d ? head : root];
    let cur = stack[0];
    let i = 0;
    let foreign = 0;        /* depth inside <svg> or <math> */

    function leaveHead() {
        if (inHead) {
            inHead = false;
            stack = [body];
            cur = body;
        }
    }

    function popTo(k) {
        for (let j = stack.length - 1; j >= k; j--) {
            const name = stack[j].localName;

            if (name === 'svg' || name === 'math') {
                foreign--;
            }
        }

        stack.length = k;
        cur = stack[k - 1];
    }

    function closeOpen(names, stops) {
        for (let k = stack.length - 1; k > 0; k--) {
            const name = stack[k].localName;

            if (names.indexOf(name) >= 0) {
                popTo(k);
                return;
            }

            if (stops.indexOf(name) >= 0) {
                return;
            }
        }
    }

    while (i < n) {
        let lt = html.indexOf('<', i);

        if (lt < 0) {
            lt = n;
        }

        if (lt > i) {
            const raw = html.slice(i, lt);

            if (inHead) {
                if (/\S/.test(raw)) {
                    leaveHead();
                    addText(cur, decodeEntities(raw));
                }
            } else {
                addText(cur, decodeEntities(raw));
            }
        }

        if (lt >= n) {
            break;
        }

        const c = html.charCodeAt(lt + 1);

        /* comment, doctype, processing instruction */
        if (c === 33 || c === 63) {
            if (html.startsWith('<!--', lt)) {
                let end = html.indexOf('-->', lt + 4);

                if (end < 0) {
                    end = n;
                }

                const cm = new Comment(html.slice(lt + 4, end));

                cm.parentNode = cur;
                cur.childNodes.push(cm);
                i = end + 3;
            } else {
                const end = html.indexOf('>', lt);

                i = end < 0 ? n : end + 1;
            }

            continue;
        }

        /* end tag */
        if (c === 47) {
            const end = html.indexOf('>', lt);

            if (end < 0) {
                break;
            }

            const name = html.slice(lt + 2, end).trim().toLowerCase();

            i = end + 1;

            if (d && name === 'head') {
                leaveHead();
            } else if (d && (name === 'body' || name === 'html')) {
                /* later content still belongs to the body */
            } else {
                for (let k = stack.length - 1; k > 0; k--) {
                    if (stack[k].localName === name) {
                        popTo(k);
                        break;
                    }
                }
            }

            continue;
        }

        /* a "<" that does not start a tag is text */
        NAME_RE.lastIndex = lt + 1;

        const nm = NAME_RE.exec(html);

        if (!nm) {
            leaveHead();
            addText(cur, '<');
            i = lt + 1;
            continue;
        }

        const name = foreign ? nm[0] : nm[0].toLowerCase();
        const lname = nm[0].toLowerCase();
        let attrs = null;
        let pos = NAME_RE.lastIndex;
        let selfClose = false;

        for (;;) {
            TAG_END_RE.lastIndex = pos;

            const te = TAG_END_RE.exec(html);

            if (te) {
                selfClose = te[0].indexOf('/') >= 0;
                pos = TAG_END_RE.lastIndex;
                break;
            }

            ATTR_RE.lastIndex = pos;

            const am = ATTR_RE.exec(html);

            if (!am || ATTR_RE.lastIndex === pos) {
                /* malformed: give up on this tag at the next ">" */
                const end = html.indexOf('>', pos);

                pos = end < 0 ? n : end + 1;
                break;
            }

            pos = ATTR_RE.lastIndex;

            const an = am[1].toLowerCase();
            const av = am[2] !== undefined ? am[2] : (am[3] !== undefined ? am[3] : (am[4] || ''));

            if (!attrs) {
                attrs = [];
            }

            let seen = false;

            for (let k = 0; k < attrs.length; k += 2) {
                if (attrs[k] === an) {
                    seen = true;
                    break;
                }
            }

            if (!seen) {
                attrs.push(an, decodeEntities(av));

                if (an.charCodeAt(0) === 111 && an.charCodeAt(1) === 110) {
                    interactive = true;
                }
            }
        }

        i = pos;

        /* the page's skeleton already exists */
        if (d && (lname === 'html' || lname === 'head' || lname === 'body')) {
            const target = lname === 'html' ? d.documentElement : (lname === 'head' ? head : body);

            if (attrs) {
                for (let k = 0; k < attrs.length; k += 2) {
                    if (!target.hasAttribute(attrs[k])) {
                        (target._a || (target._a = [])).push(attrs[k], attrs[k + 1]);
                    }
                }
            }

            if (lname === 'body') {
                leaveHead();
            }

            continue;
        }

        if (inHead && !HEAD_TAGS.has(lname)) {
            leaveHead();
        }

        /* elements that end the one before them */
        if (!foreign) {
            if (cur.localName === 'p' && P_CLOSERS.has(lname)) {
                popTo(stack.length - 1);
            }

            switch (lname) {
                case 'li': closeOpen(['li'], ['ul', 'ol', 'menu']); break;
                case 'dt':
                case 'dd': closeOpen(['dt', 'dd'], ['dl']); break;
                case 'tr': closeOpen(['tr'], ['table', 'thead', 'tbody', 'tfoot']); break;
                case 'td':
                case 'th': closeOpen(['td', 'th'], ['tr', 'table']); break;
                case 'thead':
                case 'tbody':
                case 'tfoot': closeOpen(['thead', 'tbody', 'tfoot'], ['table']); break;
                case 'option': closeOpen(['option'], ['select', 'datalist', 'optgroup']); break;
                case 'optgroup': closeOpen(['optgroup'], ['select']); break;
            }

            if (CONTROLS.has(lname)) {
                interactive = true;
            }
        }

        const el = new Element(name);

        el._a = attrs;
        el.parentNode = cur;
        cur.childNodes.push(el);

        if (!foreign && RAW_TEXT.has(lname)) {
            let re = closeTagRe[lname];

            if (!re) {
                re = closeTagRe[lname] = new RegExp('</' + lname + '[\\s>/]', 'ig');
            }

            re.lastIndex = i;

            const m = re.exec(html);
            const stop = m ? m.index : n;

            if (stop > i) {
                const raw = html.slice(i, stop);

                addText(el, (lname === 'textarea' || lname === 'title') ? decodeEntities(raw) : raw);
            }

            if (m) {
                const end = html.indexOf('>', m.index);

                i = end < 0 ? n : end + 1;
            } else {
                i = n;
            }

            continue;
        }

        if (VOID.has(lname) || (foreign && selfClose)) {
            continue;
        }

        if (lname === 'svg' || lname === 'math') {
            foreign++;
        }

        stack.push(el);
        cur = el;
    }
}

function parseFragment(parent, html) {
    parseInto(parent, html, null);
}

function makeDocument(html) {
    const d = new Document();
    const root = new Element('html');
    const head = new Element('head');
    const body = new Element('body');

    copyInto(d, root);
    copyInto(root, head);
    copyInto(root, body);

    parseInto(body, html, d);

    return d;
}

/* ---------------------------------------------------------------------- */
/* Tree -> HTML                                                            */
/* ---------------------------------------------------------------------- */

function openTag(el, out) {
    out.push('<', el.localName);

    const a = el._a;

    if (a) {
        for (let i = 0; i < a.length; i += 2) {
            out.push(' ', a[i]);

            if (a[i + 1] !== '') {
                out.push('="', escapeAttr(a[i + 1]), '"');
            }
        }
    }

    out.push('>');
}

/* For innerHTML / outerHTML: everything, as the script would expect. */
function serialize(node, out) {
    switch (node.nodeType) {
        case 3: {
            const p = node.parentNode;

            out.push(p && (p.localName === 'script' || p.localName === 'style')
                ? node._d : escapeText(node._d));
            break;
        }

        case 8:
            out.push('<!--', node._d, '-->');
            break;

        case 1: {
            openTag(node, out);

            if (!VOID.has(node.localName)) {
                const kids = node.childNodes;

                for (let i = 0; i < kids.length; i++) {
                    serialize(kids[i], out);
                }

                out.push('</', node.localName, '>');
            }

            break;
        }

        default: {
            const kids = node.childNodes;

            for (let i = 0; i < kids.length; i++) {
                serialize(kids[i], out);
            }
        }
    }
}

/*
 * For the screen: what the browser's own HTML reader needs, with element
 * numbers in front of text and controls, the live state of form controls
 * written into attributes, and everything invisible left out.
 */
let nodeById = new Map();
let nextNodeId = 0;

function render() {
    const out = [];
    let last = 0;

    nodeById = new Map();

    function mark(el) {
        const id = el._id || (el._id = ++nextNodeId);

        nodeById.set(id, el);

        if (id !== last) {
            out.push('<nq ', String(id), '>');
            last = id;
        }
    }

    function attrsFor(el, skipValue) {
        const a = el._a;

        if (!a) {
            return;
        }

        for (let i = 0; i < a.length; i += 2) {
            const name = a[i];
            const c0 = name.charCodeAt(0);

            /* handlers and bulky data the screen has no use for */
            if ((c0 === 111 && name.charCodeAt(1) === 110) ||
                (c0 === 100 && name.startsWith('data-') && name !== 'data-src') ||
                (c0 === 97 && name.startsWith('aria-')) ||
                name === 'srcset' || name === 'sizes' ||
                (skipValue && (name === 'value' || name === 'checked' || name === 'type'))) {
                continue;
            }

            out.push(' ', name);

            if (a[i + 1] !== '') {
                out.push('="', escapeAttr(a[i + 1]), '"');
            }
        }
    }

    function control(el, type, value, extra) {
        mark(el);
        out.push('<input');
        attrsFor(el, true);
        out.push(' type="', type, '" value="', escapeAttr(value), '"', extra || '');

        if (el === focused) {
            out.push(' data-nqfocus="1"');
        }

        out.push('>');
    }

    function walk(node) {
        if (node.nodeType === 3) {
            const data = node._d;

            if (data !== '') {
                if (/\S/.test(data)) {
                    mark(node.parentNode);
                }

                out.push(escapeText(data));
            }

            return;
        }

        if (node.nodeType !== 1) {
            return;
        }

        const name = node.localName;

        if (NOT_SHOWN.has(name) || node.hasAttribute('hidden')) {
            return;
        }

        if (name === 'input') {
            const type = node.type;

            if (type !== 'hidden') {
                control(node, type, node.value, node.checked ? ' checked="1"' : '');
            }

            return;
        }

        if (name === 'textarea') {
            control(node, 'text', node.value.replace(/\s+/g, ' '), '');
            return;
        }

        if (name === 'select') {
            const o = selectedOption(node);

            control(node, 'select', o ? o.textContent.replace(/\s+/g, ' ').trim() : '', '');
            return;
        }

        if (name === 'img' || name === 'button') {
            mark(node);
        }

        out.push('<', name);
        attrsFor(node, false);
        out.push('>');

        if (VOID.has(name)) {
            return;
        }

        const kids = node.childNodes;

        for (let i = 0; i < kids.length; i++) {
            walk(kids[i]);
        }

        out.push('</', name, '>');
    }

    if (doc.body) {
        walk(doc.body);
    }

    return out.join('');
}

/* ---------------------------------------------------------------------- */
/* Controls, clicks and keys                                               */
/* ---------------------------------------------------------------------- */

function isTextInput(el) {
    if (!el || el.nodeType !== 1 || el.hasAttribute('disabled')) {
        return false;
    }

    return el.localName === 'textarea' || (el.localName === 'input' && TEXT_TYPES.has(el.type));
}

function setFocus(el) {
    if (el === focused) {
        return;
    }

    const old = focused;

    focused = el;
    dirty = true;

    if (old) {
        dispatch(old, new Event('blur'));
        dispatch(old, new Event('focusout', { bubbles: true }));

        if (old._before !== undefined && old._before !== old.value) {
            dispatch(old, new Event('change', { bubbles: true }));
        }
    }

    if (el) {
        el._before = el.value;
        dispatch(el, new Event('focus'));
        dispatch(el, new Event('focusin', { bubbles: true }));
    }
}

function submitForm(form, submitter, announce) {
    if (announce) {
        const ev = new Event('submit', { bubbles: true, cancelable: true });

        ev.submitter = submitter;

        if (!dispatch(form, ev)) {
            return;
        }
    }

    const pairs = [];

    for (const c of form.elements) {
        const name = c.getAttribute('name');

        if (!name || c.hasAttribute('disabled')) {
            continue;
        }

        const type = c.type;

        if (c.localName === 'button' || type === 'submit' || type === 'button' ||
            type === 'reset' || type === 'image' || type === 'file') {
            if (c === submitter) {
                pairs.push([name, c.value]);
            }
        } else if (type === 'checkbox' || type === 'radio') {
            if (c.checked) {
                pairs.push([name, c.value]);
            }
        } else {
            pairs.push([name, c.value]);
        }
    }

    const method = (form.getAttribute('method') || 'get').toLowerCase();
    const query = pairs.map(p => formEncode(p[0]) + '=' + formEncode(p[1])).join('&');
    let action = form.action;

    if (method !== 'get') {
        native.log('warn', 'This form sends its data with ' + method.toUpperCase() +
                           ', which the browser cannot do yet');
        native.status('This form uses POST, which the browser cannot send yet.');
        return;
    }

    action = action.replace(/#.*$/, '').replace(/\?.*$/, '');
    native.navigate(action + (query ? '?' + query : ''));
}

function controlFor(el) {
    for (let n = el; n && n.nodeType === 1; n = n.parentNode) {
        const name = n.localName;

        if (CONTROLS.has(name) || name === 'a' || name === 'label' || name === 'summary') {
            return n;
        }
    }

    return null;
}

/*
 * A click on an element. Returns 1 if the browser must not follow the
 * link under the pointer (a script handled the click, or it was a control).
 */
function clickNode(el, fromUser) {
    let c = controlFor(el);

    if (c && c.localName === 'label') {
        const id = c.getAttribute('for');
        const target = id ? doc.getElementById(id) : queryAll(c, 'input,select,textarea,button', true)[0];

        if (target) {
            /* the label's own click first, then one on its control */
            const ev = new MouseEvent('click', { bubbles: true, cancelable: true });

            if (dispatch(el, ev)) {
                clickNode(target, fromUser);
            }

            return 1;
        }

        c = null;
    }

    if (c && c.hasAttribute('disabled')) {
        return 1;
    }

    /* check boxes change before their click event, and change back if a
       handler cancels it */
    let box = null;
    let wasChecked = false;
    let radioBefore = null;

    if (c && c.localName === 'input' && (c.type === 'checkbox' || c.type === 'radio')) {
        box = c;
        wasChecked = c.checked;

        if (c.type === 'checkbox') {
            c.checked = !wasChecked;
        } else {
            const name = c.getAttribute('name');
            const scope = c.form || doc;

            if (name) {
                for (const r of queryAll(scope, 'input', false)) {
                    if (r.type === 'radio' && r.getAttribute('name') === name && r.checked && r !== c) {
                        radioBefore = r;
                        r.checked = false;
                    }
                }
            }

            c.checked = true;
        }
    }

    if (fromUser) {
        dispatch(el, new MouseEvent('mousedown', { bubbles: true, cancelable: true, buttons: 1 }));
        dispatch(el, new MouseEvent('mouseup', { bubbles: true, cancelable: true }));
    }

    const ev = new MouseEvent('click', { bubbles: true, cancelable: true, detail: 1 });

    ev.isTrusted = !!fromUser;

    if (!dispatch(el, ev)) {
        if (box) {
            box.checked = wasChecked;

            if (radioBefore) {
                radioBefore.checked = true;
            }
        }

        return 1;
    }

    if (!c) {
        if (fromUser && focused) {
            setFocus(null);
        }

        return 0;
    }

    if (box) {
        if (box.checked !== wasChecked) {
            dispatch(box, new Event('input', { bubbles: true }));
            dispatch(box, new Event('change', { bubbles: true }));
        }

        return 1;
    }

    switch (c.localName) {
        case 'a': {
            const href = c.getAttribute('href');

            if (fromUser && focused) {
                setFocus(null);
            }

            if (href === null) {
                return 0;
            }

            if (/^\s*javascript:/i.test(href)) {
                native.eval(decodeURIComponent(href.replace(/^\s*javascript:/i, '')), 'javascript: link');
                return 1;
            }

            if (!fromUser) {
                if (href[0] !== '#') {
                    native.navigate(resolveURL(href));
                }

                return 1;
            }

            return href[0] === '#' ? 1 : 0;
        }

        case 'summary': {
            const details = c.parentNode;

            if (details && details.localName === 'details') {
                details.toggleAttribute('open');
            }

            return 1;
        }

        case 'select': {
            const opts = c.options.filter(o => !o.hasAttribute('disabled'));

            if (opts.length > 1) {
                const now = opts.indexOf(selectedOption(c));

                c.options.forEach(o => { o._s = false; });
                opts[(now + 1) % opts.length]._s = true;
                dirty = true;

                dispatch(c, new Event('input', { bubbles: true }));
                dispatch(c, new Event('change', { bubbles: true }));
            }

            return 1;
        }

        case 'textarea':
            setFocus(c);
            return 1;

        case 'button':
        case 'input': {
            const type = c.type;

            if (isTextInput(c)) {
                setFocus(c);
                return 1;
            }

            if (fromUser && focused) {
                setFocus(null);
            }

            const form = c.form;

            if (form && (type === 'submit' || type === 'image')) {
                submitForm(form, c, true);
            } else if (form && type === 'reset') {
                if (dispatch(form, new Event('reset', { bubbles: true, cancelable: true }))) {
                    form.reset();
                }
            }

            return 1;
        }
    }

    return 0;
}

const KEY_NAMES = {
    8: 'Backspace', 9: 'Tab', 13: 'Enter', 27: 'Escape', 32: ' ', 33: 'PageUp',
    34: 'PageDown', 35: 'End', 36: 'Home', 37: 'ArrowLeft', 38: 'ArrowUp',
    39: 'ArrowRight', 40: 'ArrowDown', 46: 'Delete'
};

function keyCodeName(code, ch) {
    if (KEY_NAMES[code]) {
        return code === 32 ? 'Space' : KEY_NAMES[code];
    }

    if (code >= 65 && code <= 90) {
        return 'Key' + String.fromCharCode(code);
    }

    if (code >= 48 && code <= 57) {
        return 'Digit' + String.fromCharCode(code);
    }

    return ch || '';
}

/* ---------------------------------------------------------------------- */
/* Timers                                                                  */
/* ---------------------------------------------------------------------- */

const timers = new Map();
let timerSeq = 0;

function addTimer(fn, delay, args, repeat) {
    if (typeof fn === 'string') {
        const code = fn;

        fn = () => { native.eval(code, 'timer'); };
    } else if (typeof fn !== 'function') {
        return 0;
    }

    delay = Math.max(0, +delay || 0);

    if (repeat && delay < 10) {
        delay = 10;
    }

    const id = ++timerSeq;

    timers.set(id, { fn, args, at: native.now() + delay, every: repeat ? delay : 0 });

    return id;
}

/* ---------------------------------------------------------------------- */
/* Scripts                                                                 */
/* ---------------------------------------------------------------------- */

function runScript(el) {
    if (el._ran) {
        return;
    }

    el._ran = true;

    const type = (el.getAttribute('type') || '').trim().toLowerCase();

    if (type && !/^(text|application)\/(x-)?(java|ecma)script$/.test(type)) {
        if (type === 'module') {
            native.log('info', 'A module script was skipped (modules are not supported)');
        }

        return;
    }

    const src = el.getAttribute('src');
    let code;

    if (src !== null) {
        if (scriptsRun >= MAX_SCRIPTS) {
            native.log('warn', 'Too many scripts on this page; skipped ' + src);
            return;
        }

        code = native.load(resolveURL(src));

        if (code === null) {
            native.log('warn', 'Could not load script ' + src);
            dispatch(el, new Event('error'));
            return;
        }
    } else {
        code = el.textContent;
    }

    scriptsRun++;

    const before = doc.currentScript;

    doc.currentScript = el;

    const ok = native.eval(code, src !== null ? src : 'script in page');

    doc.currentScript = before;

    if (src !== null) {
        dispatch(el, new Event(ok ? 'load' : 'error'));
    }
}

/* Scripts added to the page by other scripts run a moment later. */
function scheduleScripts(el) {
    const found = [];

    if (el.localName === 'script') {
        found.push(el);
    } else if (el.childNodes.length) {
        eachElement(el, k => { if (k.localName === 'script') found.push(k); });
    }

    for (const s of found) {
        if (!s._ran) {
            addTimer(() => { if (s.isConnected) runScript(s); }, 0, EMPTY, false);
        }
    }

    if (el.localName === 'img' || el.localName === 'link') {
        /* nothing is fetched for these here, but pages wait for "load" */
        addTimer(() => dispatch(el, new Event('load')), 0, EMPTY, false);
    }
}

/* ---------------------------------------------------------------------- */
/* window                                                                  */
/* ---------------------------------------------------------------------- */

const location = {
    get href() { return native.url(); },
    set href(v) { location.assign(v); },
    assign(v) {
        const target = resolveURL(v);

        if (target.replace(/#.*$/, '') === native.url().replace(/#.*$/, '') && target.indexOf('#') >= 0) {
            return;     /* a jump inside the same page */
        }

        native.navigate(target);
    },
    replace(v) { location.assign(v); },
    reload() { native.navigate(native.url()); },
    toString() { return native.url(); },
    get ancestorOrigins() { return []; }
};

for (const part of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin']) {
    Object.defineProperty(location, part, {
        get() {
            try {
                return new URL(native.url())[part];
            } catch (e) {
                return '';
            }
        },
        set(v) {
            if (part === 'search') {
                location.assign(native.url().replace(/[?#].*$/, '') + (String(v)[0] === '?' ? v : '?' + v));
            } else if (part === 'pathname') {
                location.assign(v);
            }
        },
        enumerable: true
    });
}

class Storage {
    constructor() { this._m = new Map(); }
    get length() { return this._m.size; }
    key(i) { return [...this._m.keys()][i] || null; }
    getItem(k) { const v = this._m.get(String(k)); return v === undefined ? null : v; }
    setItem(k, v) { this._m.set(String(k), String(v)); }
    removeItem(k) { this._m.delete(String(k)); }
    clear() { this._m.clear(); }
}

class Headers {
    constructor(init) {
        this._m = new Map();

        if (init) {
            const entries = init instanceof Headers ? [...init._m]
                : (Array.isArray(init) ? init : Object.entries(init));

            for (const [k, v] of entries) this.set(k, v);
        }
    }

    get(k) { const v = this._m.get(String(k).toLowerCase()); return v === undefined ? null : v; }
    set(k, v) { this._m.set(String(k).toLowerCase(), String(v)); }
    append(k, v) { const old = this.get(k); this.set(k, old === null ? v : old + ', ' + v); }
    has(k) { return this._m.has(String(k).toLowerCase()); }
    delete(k) { this._m.delete(String(k).toLowerCase()); }
    forEach(fn, self) { for (const [k, v] of this._m) fn.call(self, v, k, this); }
    entries() { return this._m.entries(); }
    keys() { return this._m.keys(); }
    values() { return this._m.values(); }
    [Symbol.iterator]() { return this._m.entries(); }
}

class Response {
    constructor(body, init) {
        this._body = (body === null || body === undefined) ? '' : String(body);
        this.status = (init && init.status) || 200;
        this.statusText = (init && init.statusText) || '';
        this.ok = this.status >= 200 && this.status < 300;
        this.headers = new Headers(init && init.headers);
        this.url = (init && init.url) || '';
        this.type = 'basic';
        this.redirected = false;
        this.bodyUsed = false;
    }

    text() { this.bodyUsed = true; return Promise.resolve(this._body); }
    json() { this.bodyUsed = true; return new Promise(resolve => resolve(JSON.parse(this._body))); }
    clone() { return new Response(this._body, this); }
    arrayBuffer() { return Promise.resolve(new TextEncoder().encode(this._body).buffer); }
    blob() { return Promise.reject(new TypeError('Blobs are not supported')); }
}

function fetchText(url, init) {
    const method = ((init && init.method) || 'GET').toUpperCase();

    if (method !== 'GET') {
        throw new TypeError('Only GET requests are supported by this browser');
    }

    const full = resolveURL(url);
    const text = native.load(full);

    if (text === null) {
        throw new TypeError('Failed to fetch ' + full);
    }

    return [full, text];
}

class XMLHttpRequest extends EventTarget {
    constructor() {
        super();
        this.readyState = 0;
        this.status = 0;
        this.statusText = '';
        this.responseText = '';
        this.response = null;
        this.responseType = '';
        this.responseURL = '';
        this.timeout = 0;
        this.withCredentials = false;
        this.upload = new EventTarget();
        this._async = true;
        this._done = false;
    }

    open(method, url, async) {
        this._method = String(method || 'GET').toUpperCase();
        this._url = String(url);
        this._async = async !== false;
        this.readyState = 1;
        this._fire('readystatechange');
    }

    setRequestHeader() { }
    overrideMimeType() { }
    getResponseHeader() { return null; }
    getAllResponseHeaders() { return ''; }
    abort() { this._done = true; }

    _fire(type) {
        dispatch(this, new Event(type));
    }

    send() {
        const finish = () => {
            if (this._done) {
                return;
            }

            this._done = true;

            let ok = true;

            try {
                const [full, text] = fetchText(this._url, { method: this._method });

                this.responseURL = full;
                this.responseText = text;
                this.status = 200;
                this.statusText = 'OK';
                this.response = text;

                if (this.responseType === 'json') {
                    try {
                        this.response = JSON.parse(text);
                    } catch (e) {
                        this.response = null;
                    }
                }
            } catch (e) {
                ok = false;
            }

            this.readyState = 4;
            this._fire('readystatechange');
            this._fire(ok ? 'load' : 'error');
            this._fire('loadend');
        };

        if (this._async) {
            addTimer(finish, 0, EMPTY, false);
        } else {
            finish();
        }
    }
}

XMLHttpRequest.UNSENT = 0;
XMLHttpRequest.OPENED = 1;
XMLHttpRequest.HEADERS_RECEIVED = 2;
XMLHttpRequest.LOADING = 3;
XMLHttpRequest.DONE = 4;

class TextEncoder {
    get encoding() { return 'utf-8'; }

    encode(s) {
        const text = unescape(encodeURIComponent(String(s === undefined ? '' : s)));
        const out = new Uint8Array(text.length);

        for (let i = 0; i < text.length; i++) out[i] = text.charCodeAt(i);

        return out;
    }
}

class TextDecoder {
    get encoding() { return 'utf-8'; }

    decode(bytes) {
        if (!bytes) {
            return '';
        }

        const view = bytes instanceof ArrayBuffer ? new Uint8Array(bytes)
            : new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        let s = '';

        for (let i = 0; i < view.length; i++) s += String.fromCharCode(view[i]);

        try {
            return decodeURIComponent(escape(s));
        } catch (e) {
            return s;
        }
    }
}

function describe(v, depth) {
    if (typeof v === 'string') {
        return depth ? JSON.stringify(v) : v;
    }

    if (v instanceof Error) {
        return v.stack ? String(v) + '\n' + v.stack : String(v);
    }

    if (v instanceof Element) {
        const id = v.getAttribute('id');
        const cls = v.getAttribute('class');

        return '<' + v.localName + (id ? '#' + id : '') + (cls ? '.' + cls.trim().replace(/\s+/g, '.') : '') + '>';
    }

    if (v instanceof Node) {
        return v.nodeName;
    }

    if (typeof v === 'function') {
        return 'function ' + (v.name || '') + '()';
    }

    if (v && typeof v === 'object') {
        try {
            const seen = new Set();
            const text = JSON.stringify(v, (k, val) => {
                if (val && typeof val === 'object') {
                    if (seen.has(val)) return '[circular]';
                    seen.add(val);
                    if (val instanceof Node) return describe(val, 1);
                }

                return typeof val === 'bigint' ? String(val) + 'n' : val;
            });

            if (text !== undefined) {
                return text.length > 600 ? text.slice(0, 600) + '...' : text;
            }
        } catch (e) {
            /* fall through */
        }
    }

    return String(v);
}

function logger(level) {
    return (...args) => {
        let text;

        if (typeof args[0] === 'string' && /%[sdifoOc]/.test(args[0])) {
            let k = 1;

            text = args[0].replace(/%([sdifoOc%])/g, (m, c) => {
                if (c === '%') return '%';
                if (c === 'c') { k++; return ''; }
                return k < args.length ? describe(args[k++], 0) : m;
            });

            for (; k < args.length; k++) text += ' ' + describe(args[k], 0);
        } else {
            text = args.map(a => describe(a, 0)).join(' ');
        }

        native.log(level, text);
    };
}

const noop = () => { };
const consoleObject = {
    log: logger('log'), info: logger('info'), debug: logger('log'),
    warn: logger('warn'), error: logger('error'), trace: logger('log'),
    dir: logger('log'), table: logger('log'),
    assert(ok, ...rest) { if (!ok) logger('error')('Assertion failed:', ...rest); },
    group: logger('log'), groupCollapsed: logger('log'), groupEnd: noop,
    time: noop, timeEnd: noop, timeLog: noop, count: noop, countReset: noop, clear: noop
};

function define(name, value) {
    Object.defineProperty(global, name, {
        value, writable: true, configurable: true, enumerable: false
    });
}

function installWindow() {
    for (const name of ['window', 'self', 'top', 'parent', 'frames']) {
        Object.defineProperty(global, name, { get() { return global; }, set() { }, configurable: true });
    }

    Object.defineProperty(global, 'document', { get() { return doc; }, configurable: true });
    Object.defineProperty(global, 'location', {
        get() { return location; },
        set(v) { location.href = v; },
        configurable: true
    });

    define('addEventListener', (t, f, o) => winTarget.addEventListener(t, f, o));
    define('removeEventListener', (t, f, o) => winTarget.removeEventListener(t, f, o));
    define('dispatchEvent', ev => dispatch(winTarget, ev));

    define('console', consoleObject);
    define('setTimeout', (fn, delay, ...args) => addTimer(fn, delay, args, false));
    define('setInterval', (fn, delay, ...args) => addTimer(fn, delay, args, true));
    define('clearTimeout', id => { timers.delete(id); });
    define('clearInterval', id => { timers.delete(id); });
    define('setImmediate', (fn, ...args) => addTimer(fn, 0, args, false));
    define('clearImmediate', id => { timers.delete(id); });
    define('requestAnimationFrame', fn => addTimer(() => fn(native.now()), 16, EMPTY, false));
    define('cancelAnimationFrame', id => { timers.delete(id); });
    define('requestIdleCallback', fn => addTimer(() => fn({ didTimeout: false, timeRemaining: () => 10 }), 1, EMPTY, false));
    define('cancelIdleCallback', id => { timers.delete(id); });
    define('queueMicrotask', fn => { Promise.resolve().then(fn).catch(report); });

    define('alert', text => { native.alert(text === undefined ? '' : String(text)); });
    define('confirm', text => native.confirm(text === undefined ? '' : String(text)));
    define('prompt', () => null);
    define('print', noop);
    define('focus', noop);
    define('blur', noop);
    define('close', noop);
    define('stop', noop);
    define('open', url => { if (url) location.assign(url); return null; });
    define('scrollTo', noop);
    define('scroll', noop);
    define('scrollBy', noop);
    define('postMessage', data => {
        addTimer(() => {
            const ev = new Event('message');

            ev.data = data;
            ev.origin = location.origin;
            ev.source = global;
            dispatch(winTarget, ev);
        }, 0, EMPTY, false);
    });

    define('getComputedStyle', el => new Proxy({}, {
        get(t, k) {
            if (k === 'getPropertyValue') {
                return name => (el && el.style ? el.style.getPropertyValue(name) : '');
            }

            if (typeof k !== 'string' || !el || !el.style) {
                return undefined;
            }

            return el.style.getPropertyValue(kebab(k));
        }
    }));

    define('matchMedia', query => ({
        matches: false, media: String(query), onchange: null,
        addListener: noop, removeListener: noop,
        addEventListener: noop, removeEventListener: noop, dispatchEvent() { return true; }
    }));

    define('getSelection', () => ({
        rangeCount: 0, isCollapsed: true, anchorNode: null, focusNode: null, type: 'None',
        toString() { return ''; }, removeAllRanges: noop, addRange: noop,
        getRangeAt() { return doc.createRange(); }, collapse: noop
    }));

    define('navigator', {
        userAgent: 'Mozilla/5.0 (NAQ OS; x86_64) NAQBrowser/0.2 QuickJS',
        appName: 'Netscape', appVersion: '5.0 (NAQ OS)', appCodeName: 'Mozilla',
        product: 'Gecko', platform: 'NAQ OS', vendor: '', language: 'en-US',
        languages: ['en-US', 'en'], onLine: true, cookieEnabled: false,
        doNotTrack: '1', hardwareConcurrency: 1, maxTouchPoints: 0,
        plugins: [], mimeTypes: [],
        javaEnabled() { return false; },
        sendBeacon() { return true; }
    });

    const size = native.size();

    define('screen', {
        width: size[0], height: size[1], availWidth: size[0], availHeight: size[1],
        colorDepth: 24, pixelDepth: 24, orientation: { type: 'landscape-primary', angle: 0 }
    });

    for (const [name, i] of [['innerWidth', 0], ['innerHeight', 1], ['outerWidth', 0], ['outerHeight', 1]]) {
        Object.defineProperty(global, name, { get() { return native.size()[i]; }, set() { }, configurable: true });
    }

    for (const name of ['scrollX', 'scrollY', 'pageXOffset', 'pageYOffset', 'screenX', 'screenY',
                        'screenLeft', 'screenTop']) {
        define(name, 0);
    }

    define('devicePixelRatio', 1);
    define('length', 0);
    define('name', '');
    define('closed', false);
    define('opener', null);
    define('frameElement', null);
    define('isSecureContext', /^https:/.test(native.url()));
    define('origin', location.origin);

    define('localStorage', new Storage());
    define('sessionStorage', new Storage());

    define('history', {
        length: 1, state: null, scrollRestoration: 'auto',
        pushState(state) { this.state = state; },
        replaceState(state) { this.state = state; },
        back() { native.back(); },
        forward: noop,
        go(n) { if (n < 0) native.back(); }
    });

    define('performance', {
        timeOrigin: Date.now() - native.now(),
        now() { return native.now(); },
        mark: noop, measure: noop, clearMarks: noop, clearMeasures: noop,
        getEntries() { return []; }, getEntriesByType() { return []; }, getEntriesByName() { return []; },
        timing: { navigationStart: Date.now() - native.now() },
        navigation: { type: 0, redirectCount: 0 }
    });

    define('fetch', (input, init) => new Promise((resolve, reject) => {
        try {
            const [full, text] = fetchText(typeof input === 'object' && input ? input.url : input, init);

            resolve(new Response(text, { status: 200, statusText: 'OK', url: full }));
        } catch (e) {
            reject(e);
        }
    }));

    define('btoa', s => {
        const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        let out = '';

        s = String(s);

        for (let i = 0; i < s.length; i += 3) {
            const a = s.charCodeAt(i);
            const b = s.charCodeAt(i + 1);
            const c = s.charCodeAt(i + 2);

            if (a > 255 || b > 255 || c > 255) {
                throw new DOMException('The string contains characters outside Latin-1', 'InvalidCharacterError');
            }

            out += chars[a >> 2] + chars[((a & 3) << 4) | (b >> 4)] +
                   (isNaN(b) ? '=' : chars[((b & 15) << 2) | (c >> 6)]) +
                   (isNaN(c) ? '=' : chars[c & 63]);
        }

        return out;
    });

    define('atob', s => {
        const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        let out = '';
        let bits = 0;
        let count = 0;

        for (const ch of String(s).replace(/[\s=]/g, '')) {
            const v = chars.indexOf(ch);

            if (v < 0) {
                throw new DOMException('The string is not correctly encoded', 'InvalidCharacterError');
            }

            bits = (bits << 6) | v;
            count += 6;

            if (count >= 8) {
                count -= 8;
                out += String.fromCharCode((bits >> count) & 255);
            }
        }

        return out;
    });

    define('structuredClone', v => JSON.parse(JSON.stringify(v)));

    define('crypto', {
        /* not a secure source: fine for ids, not for secrets */
        getRandomValues(a) {
            for (let i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296);
            return a;
        },
        randomUUID() {
            return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
                const r = Math.floor(Math.random() * 16);
                return (c === 'x' ? r : (r & 3) | 8).toString(16);
            });
        }
    });

    class Observer {
        constructor(fn) { this._fn = fn; }
        observe() { }
        unobserve() { }
        disconnect() { }
        takeRecords() { return []; }
    }

    class IntersectionObserver extends Observer {
        /* everything counts as visible, so "load when seen" content loads */
        observe(el) {
            addTimer(() => {
                this._fn([{ target: el, isIntersecting: true, intersectionRatio: 1,
                            boundingClientRect: ZERO_RECT, intersectionRect: ZERO_RECT,
                            rootBounds: null, time: native.now() }], this);
            }, 0, EMPTY, false);
        }
    }

    class AbortController {
        constructor() {
            this.signal = new EventTarget();
            this.signal.aborted = false;
        }

        abort() {
            this.signal.aborted = true;
            dispatch(this.signal, new Event('abort'));
        }
    }

    class DOMParser {
        parseFromString(text) { return makeDocument(String(text)); }
    }

    class XMLSerializer {
        serializeToString(node) { const out = []; serialize(node, out); return out.join(''); }
    }

    function Image(width, height) {
        const img = new Element('img');

        if (width !== undefined) img.setAttribute('width', width);
        if (height !== undefined) img.setAttribute('height', height);

        return img;
    }

    Image.prototype = Element.prototype;

    const classes = {
        Event, CustomEvent, UIEvent, MouseEvent, KeyboardEvent, EventTarget,
        PointerEvent: MouseEvent, FocusEvent: UIEvent, InputEvent: UIEvent,
        Node, Element, HTMLElement: Element, SVGElement: Element, Text, Comment,
        CharacterData, Document, HTMLDocument: Document, DocumentFragment,
        ShadowRoot: DocumentFragment, NodeList, HTMLCollection: NodeList,
        DOMException, URL, URLSearchParams, Storage, Headers, Response,
        XMLHttpRequest, TextEncoder, TextDecoder, DOMParser, XMLSerializer,
        MutationObserver: Observer, ResizeObserver: Observer, PerformanceObserver: Observer,
        IntersectionObserver, AbortController, Image
    };

    for (const tag of ['Anchor', 'Body', 'Button', 'Canvas', 'Div', 'Form', 'Head', 'Html',
                       'IFrame', 'Image', 'Input', 'LI', 'Label', 'Link', 'Meta', 'Option',
                       'Paragraph', 'Pre', 'Script', 'Select', 'Span', 'Style', 'Table',
                       'TableCell', 'TableRow', 'Template', 'TextArea', 'Title', 'UList',
                       'Video', 'Audio', 'Media', 'Unknown']) {
        classes['HTML' + tag + 'Element'] = Element;
    }

    for (const name of Object.keys(classes)) {
        define(name, classes[name]);
    }

    define('customElements', {
        define: noop, get() { return undefined; },
        whenDefined() { return new Promise(noop); }, upgrade: noop
    });

    define('CSS', {
        supports() { return false; },
        escape(s) { return String(s).replace(/[^a-zA-Z0-9_\-\u0080-￿]/g, c => '\\' + c); }
    });
}

/* ---------------------------------------------------------------------- */
/* What browser.c calls                                                    */
/* ---------------------------------------------------------------------- */

/* Builds the document from the page's HTML. */
define('__boot', html => {
    doc = makeDocument(html);
    treeVersion++;
    installWindow();
    dirty = false;
});

/* Runs the page's scripts in order, then announces that it has loaded. */
define('__runScripts', () => {
    for (const s of doc.getElementsByTagName('script')) {
        if (s.isConnected) {
            runScript(s);
        }
    }

    scriptsLive = true;

    doc.readyState = 'interactive';
    dispatch(doc, new Event('readystatechange'));
    dispatch(doc, new Event('DOMContentLoaded', { bubbles: true }));

    doc.readyState = 'complete';
    dispatch(doc, new Event('readystatechange'));

    const load = new Event('load');
    const body = doc.body;

    if (body && typeof global.onload !== 'function') {
        const h = handlerOf(body, 'load');

        if (h) {
            try {
                h.call(global, load);
            } catch (e) {
                report(e);
            }
        }
    }

    dispatch(winTarget, load);
    dispatch(winTarget, new Event('pageshow'));

    return scriptsRun;
});

/*
 * The page as HTML for the screen, or null if nothing changed. The first
 * time, a page with no controls and no listeners needs no second drawing.
 */
define('__flush', force => {
    if (!doc || (!force && !dirty && (rendered || !interactive))) {
        return null;
    }

    dirty = false;
    rendered = true;
    native.title(doc.title);

    return render();
});

/* A click on element number `id`. Bit 0: handled here; bit 1: a text box
   now has the keyboard. */
define('__click', id => {
    const el = nodeById.get(id);
    let flags = 0;

    if (el && el.isConnected) {
        flags = clickNode(el, true);
    } else if (focused) {
        setFocus(null);
    }

    return flags | (isTextInput(focused) ? 2 : 0);
});

/* A click on the page that hit no element. */
define('__clickAway', () => {
    if (focused) {
        setFocus(null);
    }

    if (doc && doc.body) {
        dispatch(doc.body, new MouseEvent('click', { bubbles: true, cancelable: true }));
    }

    return 0;
});

/* A key press. Bit 0: used here (do not scroll); bit 1: a text box has
   the keyboard. */
define('__key', (code, ch) => {
    const target = focused || (doc && doc.body);

    if (!target) {
        return 0;
    }

    const init = {
        bubbles: true, cancelable: true, key: ch || KEY_NAMES[code] || '',
        code: keyCodeName(code, ch), keyCode: code, charCode: ch ? ch.charCodeAt(0) : 0
    };

    let used = 0;
    const down = new KeyboardEvent('keydown', init);

    if (!dispatch(target, down)) {
        used = 1;
    } else if (ch && !dispatch(target, new KeyboardEvent('keypress', init))) {
        used = 1;
    }

    if (!used && isTextInput(focused)) {
        const box = focused;

        used = 1;

        if (ch) {
            const max = parseInt(box.getAttribute('maxlength'), 10);

            if (!(max >= 0) || box.value.length < max) {
                box.value = box.value + ch;
                dispatch(box, new Event('input', { bubbles: true }));
            }
        } else if (code === 8) {
            if (box.value.length) {
                box.value = box.value.slice(0, -1);
                dispatch(box, new Event('input', { bubbles: true }));
            }
        } else if (code === 13) {
            if (box.localName === 'textarea') {
                box.value = box.value + '\n';
                dispatch(box, new Event('input', { bubbles: true }));
            } else {
                if (box._before !== box.value) {
                    box._before = box.value;
                    dispatch(box, new Event('change', { bubbles: true }));
                }

                const form = box.form;

                if (form) {
                    const by = queryAll(form, 'button:not([type]),button[type=submit],input[type=submit]', true)[0];

                    submitForm(form, by || null, true);
                }
            }
        } else if (code === 27 || code === 9) {
            setFocus(null);
        } else {
            used = 0;
        }
    }

    dispatch(target, new KeyboardEvent('keyup', init));

    return used | (isTextInput(focused) ? 2 : 0);
});

/* Runs the timers that are due. Returns milliseconds until the next one,
   or -1 if there are none. */
define('__tick', () => {
    const now = native.now();
    const due = [];

    for (const [id, t] of timers) {
        if (t.at <= now) {
            due.push([id, t]);
        }
    }

    due.sort((a, b) => (a[1].at - b[1].at) || (a[0] - b[0]));

    let ran = 0;

    for (const [id, t] of due) {
        if (!timers.has(id)) {
            continue;
        }

        if (t.every) {
            t.at = now + t.every;
        } else {
            timers.delete(id);
        }

        try {
            t.fn.apply(global, t.args);
        } catch (e) {
            report(e);
        }

        if (++ran >= 100) {
            break;
        }
    }

    let next = -1;

    for (const t of timers.values()) {
        const wait = Math.max(0, t.at - now);

        if (next < 0 || wait < next) {
            next = wait;
        }
    }

    return next;
});

})(globalThis, __native);
