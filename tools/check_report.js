// check_report.js -- executes the <script> block of a generated vk-bench HTML report against a
// stubbed DOM/canvas, so a syntax error or a broken render path is caught without a browser.
// Also fires the hover and legend-click handlers once to exercise the interactive paths.
// usage: node tools/check_report.js <report.html>
'use strict';
const fs = require('fs');
const path = require('path');

const file = process.argv[2];
if (!file) {
    console.error('usage: node tools/check_report.js <report.html>');
    process.exit(2);
}
const html = fs.readFileSync(file, 'utf8');

const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) {
    console.error('[FAIL] no <script> block found');
    process.exit(1);
}
const script = m[1];

// ---- minimal DOM stub ------------------------------------------------------
const drawn = { lines: 0, arcs: 0, texts: 0, fills: 0, contexts: 0, rects: 0, measures: 0, bands: 0, nan: 0 };
// Any drawing call with a non-finite coordinate means the chart is invisible in a real browser
// (that is how a p[0]/p[1]-vs-{x,y} mix-up once slipped through this checker).
function nanChk(name, fn) {
    return function () {
        for (let i = 0; i < arguments.length; ++i) {
            if (typeof arguments[i] === 'number' && !isFinite(arguments[i])) {
                drawn.nan++;
                if (!drawn.nanAt) drawn.nanAt = name;
            }
        }
        if (fn) return fn.apply(this, arguments);
    };
}
function ctxStub() {
    return {
        setTransform() {}, clearRect() {}, beginPath() {}, closePath() {},
        moveTo: nanChk('moveTo', function () { drawn.lines++; }),
        lineTo: nanChk('lineTo', function () { drawn.lines++; }),
        rect: nanChk('rect', function () { drawn.rects++; }),
        fillRect: nanChk('fillRect', function () {
            drawn.rects++;
            if (String(this.fillStyle).indexOf('rgba') === 0) drawn.bands++;
        }),
        strokeRect: nanChk('strokeRect', function () { drawn.rects++; }),
        stroke() {}, arc: nanChk('arc', function () { drawn.arcs++; }), fill() { drawn.fills++; },
        fillText() { drawn.texts++; },
        measureText(t) { drawn.measures++; return { width: String(t).length * 6.4 }; },
        createLinearGradient() { return { addColorStop() {} }; },
        createRadialGradient() { return { addColorStop() {} }; },
        save() {}, restore() {}, translate() {}, rotate() {}, setLineDash() {}, scale() {}, clip() {},
        fillStyle: '', strokeStyle: '', lineWidth: 1, font: '', textAlign: '', textBaseline: '',
        globalAlpha: 1, lineCap: 'butt', lineJoin: 'miter', shadowBlur: 0,
    };
}
const elements = new Map();
function el(id) {
    if (!elements.has(id)) {
        const e = {
            id, innerHTML: '', textContent: '', className: '', style: {},
            clientWidth: 900, clientHeight: 300, width: 900, height: 300,
            parentElement: null, __listeners: {},
            getContext: () => { drawn.contexts++; return ctxStub(); },
            addEventListener(type, fn) { (this.__listeners[type] = this.__listeners[type] || []).push(fn); },
            getBoundingClientRect: () => ({ left: 0, top: 0, width: 900, height: 300 }),
        };
        e.parentElement = { clientWidth: 900 };
        elements.set(id, e);
    }
    return elements.get(id);
}
const document = { getElementById: el, title: '', body: el('body') };
const window = { devicePixelRatio: 1 };

// capture DATA while injecting the report script
let data = null;
const captureScript = script.replace(/^const DATA = /m, 'DATA = globalThis.__DATA = ');

try {
    const fn = new Function('document', 'window', 'globalThis', captureScript + '\nreturn {DATA: globalThis.__DATA, CHARTS: globalThis.__CHARTS};');
    const out = fn(document, window, globalThis);
    data = out.DATA;
    globalThis.__CHARTS = out.CHARTS;
} catch (e) {
    console.error('[FAIL] report script threw during render: ' + e.message);
    process.exit(1);
}

// ---- exercise the interactive paths ---------------------------------------
let hoverErr = null, clickErr = null, clicks = 0;
try {
    for (const [, e] of elements) {
        const h = e.__listeners['mousemove'] || [];
        const st = (globalThis.__CHARTS || {})[e.id];
        if (!st || !st.px || !st.series.length || !st.series[0].pts.length) continue;
        const p = st.series[0].pts[0];                        // aim exactly at the first data point
        for (const fn of h) fn({ clientX: st.px(p.x), clientY: st.py(p.y) });
    }
} catch (e) { hoverErr = e.message; }
try {
    for (const [, e] of elements) {
        const c = e.__listeners['click'] || [];
        for (const fn of c) { fn({}); clicks++; }
    }
} catch (e) { clickErr = e.message; }

// ---- assertions ------------------------------------------------------------
const problems = [];
if (!data || !data.device || !data.profile || !Array.isArray(data.measures)) problems.push('DATA payload incomplete');
else {
    if (!data.measures.length) problems.push('no measures in payload');
    if (!data.device.name) problems.push('device name missing');
    if (!data.profile.notes || !data.profile.notes.length) problems.push('no profile notes');
    if (!data.profile.levels || !data.profile.levels.length) problems.push('no inferred cache levels');
}
const checks = [
    ['hero', 'overview tiles'],
    ['subtitle', 'subtitle'],
    ['devtbl', 'device table'],
    ['notes', 'profile notes'],
    ['ratios', 'ratio table'],
    ['peaks', 'peak table'],
    ['bwpeaks', 'bandwidth table'],
    ['levels', 'cache level table'],
    ['lattbl', 'latency level table'],
    ['matrix', 'matrix table'],
    ['all', 'full measure table'],
    ['leg-lat', 'latency legend'],
    ['leg-bw', 'bandwidth legend'],
    ['leg-occ', 'occupancy legend'],
];
for (const [id, what] of checks) {
    const e = elements.get(id);
    if (!e || e.innerHTML.length < 20) problems.push('empty render: ' + what + ' (#' + id + ')');
}
if (drawn.contexts < 4) problems.push('expected 4 canvases with contexts, got ' + drawn.contexts);
if (drawn.lines < 60) problems.push('charts drew almost nothing (lines=' + drawn.lines + ')');
if (drawn.arcs < 40) problems.push('charts drew too few data points (arcs=' + drawn.arcs + ')');
if (drawn.texts < 20) problems.push('charts drew almost no labels (texts=' + drawn.texts + ')');
if (drawn.bands < 1) problems.push('no level bands drawn');
if (hoverErr) problems.push('mousemove handler threw: ' + hoverErr);
if (clickErr) problems.push('legend click handler threw: ' + clickErr);
if (clicks < 3) problems.push('expected legend click handlers, got ' + clicks);
if (drawn.measures < 1) problems.push('tooltip path never ran (measureText not called)');
if (drawn.nan > 0) problems.push('non-finite drawing coordinate in ' + (drawn.nanAt || '?') + ' (' + drawn.nan +
    ' calls) -> the chart would be invisible in a browser');

const stats = 'measures=' + (data && data.measures ? data.measures.length : 0) +
    ' ctx=' + drawn.contexts + ' lines=' + drawn.lines + ' arcs=' + drawn.arcs + ' texts=' + drawn.texts +
    ' bands=' + drawn.bands + ' legendClicks=' + clicks + ' tooltip=' + drawn.measures + ' nan=' + drawn.nan;
if (problems.length) {
    console.error('[FAIL] ' + path.basename(file) + ' -> ' + stats);
    for (const p of problems) console.error('  - ' + p);
    process.exit(1);
}
console.log('[OK] ' + path.basename(file) + ' rendered + interactive paths exercised: ' + stats);
