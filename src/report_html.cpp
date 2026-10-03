// report_html.cpp -- self-contained zh-CN HTML report (canvas charts, no external requests).
// Rendering is fully synchronous on load; the only listeners are mousemove/mouseleave (tooltips)
// and click (legend toggles), so it also works in embedded browsers where rAF/timers are starved.
#include "report.h"

#include <cstdio>
#include <string>

namespace {

const char* kTemplate = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>vk-bench 报告 — __TITLE__</title>
<style>
:root { --bg:#0d1117; --card:#161b22; --card2:#1b2129; --line:#2a3340; --fg:#e6edf3; --dim:#8b98a6;
        --acc:#4493f8; --g:#3fb950; --o:#d29922; --r:#f85149; --p:#a371f7; --y:#d6c86a; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
       font:14px/1.6 "Segoe UI", system-ui, "Microsoft YaHei", sans-serif; }
.wrap { max-width:1200px; margin:0 auto; padding:0 18px 64px; }
h1 { font-size:25px; margin:0 0 4px; letter-spacing:.2px; }
h1 small { color:var(--dim); font-weight:400; font-size:13px; }
h2 { font-size:16px; margin:0 0 4px; color:#fff; }
.h2sub { color:var(--dim); font-size:12.5px; margin:0 0 14px; }
.sub { color:var(--dim); font-size:13px; margin:0 0 18px; }
.card { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:18px 20px; margin:0 0 16px; }
.top { padding:22px 18px 6px; }
.nav { position:sticky; top:0; z-index:9; margin:0 0 16px; padding:8px 0;
       background:rgba(13,17,23,.88); backdrop-filter:blur(8px); border-bottom:1px solid var(--line); }
.nav a { color:var(--dim); text-decoration:none; margin-right:16px; font-size:13px; }
.nav a:hover { color:var(--fg); }
.hero { display:grid; grid-template-columns:repeat(auto-fit, minmax(178px, 1fr)); gap:12px; margin:0 0 16px; }
.tile { background:var(--card); border:1px solid var(--line); border-radius:12px; padding:14px 16px; }
.tile .k { color:var(--dim); font-size:12px; letter-spacing:.3px; }
.tile .v { font-size:26px; font-weight:600; line-height:1.25; font-variant-numeric:tabular-nums; }
.tile .v span { font-size:13px; font-weight:400; color:var(--dim); margin-left:4px; }
.tile .s { color:var(--dim); font-size:12px; }
table { border-collapse:collapse; width:100%; font-size:13px; }
th,td { text-align:left; padding:6px 9px; border-bottom:1px solid var(--line); }
th { color:var(--dim); font-weight:600; white-space:nowrap; }
tbody tr:nth-child(even) { background:var(--card2); }
td.num { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap; }
td.k { color:var(--dim); }
b.hi { color:#fff; }
.bar { position:relative; height:14px; border-radius:3px; background:var(--acc); min-width:2px; display:inline-block; vertical-align:middle; }
.barwrap { width:150px; display:inline-block; background:#222a34; border-radius:3px; }
.note { margin:5px 0; }
.note:before { content:"▸ "; color:var(--acc); }
.dim { color:var(--dim); }
.ok { color:var(--g); } .fail { color:var(--r); } .warn { color:var(--o); }
canvas { display:block; width:100%; cursor:crosshair; }
.grid2 { display:grid; grid-template-columns:1fr 1fr; gap:16px; }
@media (max-width:860px){ .grid2 { grid-template-columns:1fr; } }
.k2 { display:inline-block; width:10px; height:10px; border-radius:2px; margin-right:6px; }
.legend { margin:0 0 10px; font-size:12px; color:var(--dim); }
.legend span { margin-right:16px; cursor:pointer; user-select:none; }
.legend span.off { opacity:.35; }
code { background:#1e252d; padding:1px 5px; border-radius:4px; font-size:12px; }
details summary { cursor:pointer; color:var(--dim); font-size:13px; margin:0 0 10px; }
details[open] summary { margin-bottom:14px; }
.foot { color:var(--dim); font-size:12px; margin-top:22px; }
@media print {
  :root { --bg:#fff; --card:#fff; --card2:#fafafa; --line:#d0d7de; --fg:#111; --dim:#57606a; }
  body { background:#fff; color:#111; }
  .nav { display:none; }
  .card { break-inside:avoid; }
}
</style>
</head>
<body>
<div class="top wrap">
  <h1>vk-bench <small>Vulkan 1.1-1.4 GPU 架构能力基准</small></h1>
  <p class="sub" id="subtitle"></p>
  <div class="hero" id="hero"></div>
</div>
<div class="nav wrap">
  <a href="#devcard">设备</a><a href="#profile">架构画像</a><a href="#latcard">缓存延迟</a>
  <a href="#bwcard">带宽</a><a href="#stridecard">步长</a><a href="#occcard">占用</a>
  <a href="#matrixcard">矩阵</a><a href="#allcard">全部测点</a>
</div>
<div class="wrap">
<div class="card" id="devcard"><h2>设备能力</h2><p class="h2sub">枚举、特性探测与本次实测使用的限制</p><div id="devtbl"></div></div>
<div class="card" id="profile"><h2>架构画像</h2><p class="h2sub">归一化比值与自动生成的结论（每条都能在下面各图里找到对应数据）</p>
  <div id="notes"></div><div id="ratios"></div></div>
<div class="grid2">
  <div class="card"><h2>算力峰值</h2><p class="h2sub">每个测点取多次重复的中位数</p><div id="peaks"></div></div>
  <div class="card"><h2>带宽与缓存层级</h2><p class="h2sub">容量为延迟台阶 / 带宽台阶的推断值</p><div id="bwpeaks"></div><div id="levels"></div></div>
</div>
<div class="card" id="latcard"><h2>缓存延迟曲线（指针追逐）</h2>
  <p class="h2sub">横轴为工作集大小（2 的幂，1 KiB → 1 GiB），纵轴为一次依赖加载的耗时。
     色带是按实测台阶推断的缓存层级，虚线标注每级边界。</p>
  <p class="legend" id="leg-lat"></p><canvas id="c-lat" height="340"></canvas>
  <div id="lattbl"></div></div>
<div class="card" id="bwcard"><h2>带宽 vs 容量（流式访问）</h2>
  <p class="h2sub">同一套容量点：平台区的宽度就是该级缓存的容量，最右侧平台即显存带宽。</p>
  <p class="legend" id="leg-bw"></p><canvas id="c-bw" height="330"></canvas><div id="bwtbl"></div></div>
<div class="card" id="stridecard"><h2>访问步长扫描</h2>
  <p class="h2sub">每次只取 4 B，看有效带宽随步长的衰减：台阶出现的位置就是 sector / cacheline 尺寸。</p>
  <p class="legend" id="leg-stride"></p><canvas id="c-stride" height="260"></canvas></div>
<div class="card" id="occcard"><h2>占用扫描</h2>
  <p class="h2sub">同一个内核，改变 workgroup 数量（每点单独标定到相同 GPU 时长），看多少个 workgroup 才能跑满。</p>
  <p class="legend" id="leg-occ"></p><canvas id="c-occ" height="260"></canvas></div>
<div class="card" id="matrixcard"><h2>矩阵单元（cooperative matrix）</h2>
  <p class="h2sub">与向量单元的比值是判断“有没有独立矩阵硬件”的核心指标</p><div id="matrix"></div></div>
<div class="card" id="allcard"><h2>全部测点</h2>
  <p class="h2sub" id="allsub"></p>
  <details><summary>展开/收起原始测点表</summary><div id="all"></div></details></div>
<p class="foot">vk-bench · 数值为 GPU 时间戳实测中位数（个别低占用点因驱动时间戳不可靠改用主机墙钟，表内标注 host）·
缓存容量是台阶的推断值，不是 API 报告值 · 曲线由本次运行的扫频点绘制</p>
</div>
<script>
const DATA = __JSON__;
const COLORS = ["#4493f8","#3fb950","#d29922","#a371f7","#f85149","#d6c86a"];
const BANDC = ["rgba(63,185,80,.10)","rgba(68,147,248,.10)","rgba(163,113,247,.10)","rgba(248,81,73,.08)"];
const $ = id => document.getElementById(id);
const fmt = (v,d) => (typeof v!=="number"||!isFinite(v)) ? "—" : v.toLocaleString("zh-CN",{maximumFractionDigits:d===undefined?2:d, minimumFractionDigits:0});
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]));
function sizeStr(b){ if(!(b>0)) return "—"; const u=["B","KiB","MiB","GiB"]; let i=0,v=b;
  while(v>=1024&&i<3){v/=1024;i++;} return (i?v.toFixed(v<10?2:1):v)+" "+u[i]; }
function capOf(l){ return (l.bytes!==undefined? l.bytes : l.capacityBytes) || 0; }
function byTest(t){ return DATA.measures.filter(m=>m.test===t).sort((a,b)=>a.x-b.x); }
function row(cells){ return "<tr>"+cells.map(c=>"<td>"+c+"</td>").join("")+"</tr>"; }
function rowN(name,cells){ return "<tr><td class='k'>"+name+"</td>"+cells.map(c=>"<td class='num'>"+c+"</td>").join("")+"</tr>"; }
function bar(v,max,color){ const w=Math.max(2,Math.round(v/max*150)); return "<span class='barwrap'><span class='bar' style='width:"+w+"px;background:"+(color||"var(--acc)")+"'></span></span>"; }
function table(container, head, rows){ $(container).innerHTML = "<table>"+(head&&head.length?("<thead><tr>"+head.map(h=>"<th>"+h+"</th>").join("")+"</tr></thead>"):"")+"<tbody>"+rows.join("")+"</tbody></table>"; }
function tile(k,v,unit,sub){ return "<div class='tile'><div class='k'>"+k+"</div><div class='v'>"+v+"<span>"+(unit||"")+"</span></div><div class='s'>"+(sub||"")+"</div></div>"; }
function pow2Label(v){ const e=Math.log2(v); if(Math.abs(e-Math.round(e))>1e-9) return sizeStr(v);
  return Math.round(v)<1024 ? (Math.round(v)+" B") : sizeStr(v); }

// ---------------------------------------------------------------- charts
CHARTS = globalThis.__CHARTS = {};   // exposed for tools/check_report.js
function chart(cvId, series, opt) {
  const st = { cv:$(cvId), series:series, opt:opt||{}, hidden:{}, hover:null, px:null, py:null };
  CHARTS[cvId] = st;
  drawChart(st);
  const cv = st.cv;
  if (cv && !cv.__hooked) {
    cv.__hooked = true;
    if (cv.addEventListener) {
      cv.addEventListener("mousemove", function(ev){ hoverMove(cvId, ev); });
      cv.addEventListener("mouseleave", function(){ CHARTS[cvId].hover=null; drawChart(CHARTS[cvId]); });
    }
  }
}
function hoverMove(cvId, ev) {
  const st = CHARTS[cvId];
  if (!st || !st.px) return;
  const r = (st.cv.getBoundingClientRect ? st.cv.getBoundingClientRect() : {left:0, top:0});
  const mx = ev.clientX - r.left, my = ev.clientY - r.top;
  let best = null;
  st.series.forEach((s,i)=>{ if (st.hidden[i]) return;
    s.pts.forEach(p=>{ const px=st.px(p.x), py=st.py(p.y);
      const d=(px-mx)*(px-mx)+(py-my)*(py-my);
      if (!best || d<best.d) best={d:d, px:px, py:py, p:p, name:s.name||("曲线"+(i+1)), color:s.color||COLORS[i%COLORS.length]};
    });
  });
  st.hover = (best && best.d < 40*40) ? best : null;
  drawChart(st);
}
function drawChart(st) {
  const g = st.cv.getContext("2d");
  const opt = st.opt;
  const parent = st.cv.parentElement || {clientWidth:900};
  const cssW = Math.max((parent.clientWidth || 900) - 4, 320);
  const cssH = opt.height || 300;
  const dpr = (typeof window!=="undefined" && window.devicePixelRatio) || 1;
  st.cv.width = Math.round(cssW*dpr); st.cv.height = Math.round(cssH*dpr);
  st.cv.style.width = cssW+"px"; st.cv.style.height = cssH+"px";
  g.setTransform(dpr,0,0,dpr,0,0);
  g.clearRect(0,0,cssW,cssH);
  const L=62, R=16, T=14, B=34;
  const W = cssW-L-R, H = cssH-T-B;
  const vis = st.series.map((s,i)=>({s:s,i:i})).filter(o=>o.s.pts.length>0 && !st.hidden[o.i]);
  const all = st.series.filter(s=>s.pts.length>0);
  if (!all.length) { g.fillStyle="#8b98a6"; g.font="12px sans-serif"; g.textAlign="left"; g.fillText("无数据", L, T+20); return; }
  const xs = all.flatMap(s=>s.pts.map(p=>p.x));
  const ys = all.flatMap(s=>s.pts.map(p=>p.y));
  const tx = opt.logX ? (v=>Math.log2(Math.max(v,1))) : (v=>v);
  let x0 = Math.min.apply(null, xs.map(tx)), x1 = Math.max.apply(null, xs.map(tx));
  if (x0===x1) { x0-=0.5; x1+=0.5; }
  let y0 = 0, y1 = Math.max.apply(null, ys);
  if (y1<=y0) y1=y0+1;
  y1 += (y1-y0)*0.10;
  const X = v => L + (tx(v)-x0)/(x1-x0)*W;
  const Y = v => T + H - (v-y0)/(y1-y0)*H;
  st.px = X; st.py = Y;
  // level bands
  (opt.bands||[]).forEach((b,i)=>{
    const bx0 = Math.max(X(Math.pow(2, b.x0)), L), bx1 = Math.min(X(Math.pow(2, b.x1)), L+W);
    if (bx1 <= bx0) return;
    g.fillStyle = b.color || BANDC[i%BANDC.length];
    g.fillRect(bx0, T, bx1-bx0, H);
    if (bx1-bx0 > 46) { g.fillStyle="#8b98a6"; g.font="11px sans-serif"; g.textAlign="center";
      g.fillText(b.label, (bx0+bx1)/2, T+12); }
  });
  // grid + y labels
  g.strokeStyle="#232b36"; g.lineWidth=1; g.font="11px Segoe UI, sans-serif"; g.fillStyle="#8b98a6";
  g.textAlign="right";
  for (let i=0;i<=4;i++){
    const yv = y0 + (y1-y0)*i/4, py = Y(yv);
    g.beginPath(); g.moveTo(L,py); g.lineTo(L+W,py); g.stroke();
    g.fillText(fmt(yv, yv<10?2:0), L-8, py+4);
  }
  // x ticks at powers of two
  g.textAlign="center";
  const ticks = opt.logX ? xs.filter((v,i)=> i===0 || i===xs.length-1 ||
        (Math.abs(Math.log2(v)-Math.round(Math.log2(v)))<1e-9 && Math.round(Math.log2(v))%4===0))
      : xs.filter((v,i)=>i%Math.max(1,Math.ceil(xs.length/8))===0);
  ticks.forEach(v=>{ const px=X(v);
    g.strokeStyle="#1c232c"; g.beginPath(); g.moveTo(px,T); g.lineTo(px,T+H); g.stroke();
    g.fillStyle="#8b98a6"; g.fillText(opt.xFmt?opt.xFmt(v):String(v), px, T+H+17); });
  // knee vlines
  (opt.vlines||[]).forEach(vl=>{ const px=X(Math.pow(2,vl.x)); if (px<L||px>L+W) return;
    g.strokeStyle=vl.color||"#4a5666"; g.setLineDash([5,4]); g.beginPath(); g.moveTo(px,T); g.lineTo(px,T+H); g.stroke(); g.setLineDash([]);
    if (vl.label) { g.fillStyle=vl.color||"#8b98a6"; g.textAlign="left"; g.font="11px Segoe UI, sans-serif"; g.fillText(vl.label, px+5, T+26); } });
  // series
  vis.forEach(o=>{
    const s=o.s, col=s.color||COLORS[o.i%COLORS.length];
    g.strokeStyle=col; g.lineWidth = (opt.primary===o.i) ? 2.8 : 1.6;
    g.beginPath();
    s.pts.forEach((p,j)=>{ const px=X(p.x), py=Y(p.y); j?g.lineTo(px,py):g.moveTo(px,py); });
    g.stroke();
    g.fillStyle=col;
    s.pts.forEach(p=>{ g.beginPath(); g.arc(X(p.x),Y(p.y),(opt.primary===o.i?3:2.4),0,6.2832); g.fill(); });
  });
  // hover marker + tooltip
  if (st.hover) {
    const h = st.hover;
    g.strokeStyle=h.color; g.lineWidth=1.4;
    g.beginPath(); g.arc(h.px,h.py,6,0,6.2832); g.stroke();
    const lines = [h.name, pow2Label(h.p.x)+" → "+fmt(h.p.y, h.p.y<10?2:0)+(opt.yUnit||"")];
    g.font="12px Segoe UI, sans-serif";
    let wmax=0;
    lines.forEach(t=>{ const w=(g.measureText?g.measureText(t).width:t.length*6.4); if(w>wmax) wmax=w; });
    const bw=wmax+18, bh=16*lines.length+10;
    let bx=h.px+12, by=h.py-bh-8;
    if (bx+bw > L+W) bx=h.px-bw-12;
    if (by < T) by=h.py+12;
    g.fillStyle="rgba(13,17,23,.95)"; g.strokeStyle="#3b4655"; g.lineWidth=1;
    g.fillRect(bx,by,bw,bh); g.strokeRect(bx,by,bw,bh);
    g.fillStyle="#e6edf3"; g.textAlign="left";
    lines.forEach((t,i)=>g.fillText(t, bx+9, by+17+i*16));
  }
  // axis labels
  g.fillStyle="#8b98a6"; g.textAlign="left"; g.font="12px Segoe UI, sans-serif";
  g.fillText(opt.xLabel||"", L, T+H+31);
  g.save(); g.translate(13, T+H/2); g.rotate(-Math.PI/2); g.textAlign="center"; g.fillText(opt.yLabel||"", 0, 0); g.restore();
}

function legend(elId, items, chartId) {
  $(elId).innerHTML = items.map((it,i)=>
    "<span id='"+elId+"-"+i+"'><span class='k2' style='background:"+COLORS[i%COLORS.length]+"'></span>"+esc(it)+"</span>").join("");
  if (!chartId) return;
  items.forEach((it,i)=>{
    const el = $(elId+"-"+i);
    if (!el || !el.addEventListener) return;
    el.addEventListener("click", function(){
      const st = CHARTS[chartId]; if (!st) return;
      st.hidden[i] = !st.hidden[i];
      el.className = st.hidden[i] ? "off" : "";
      drawChart(st);
    });
  });
}

function render(){
  const d = DATA.device, p = DATA.profile;
  const cu = d.computeUnits|0;
  const clock = (cu>0 && p.fp32>0) ? (p.fp32/(cu*128)) : 0;   // GHz, classic AMD 128 flops/cycle/CU
  const matrixRatio = (p.fp32>0 && p.matrixBest>0) ? (p.matrixBest/p.fp32) : 0;
  const lv = p.levels||[];
  $("subtitle").innerHTML = "<b>"+esc(d.name)+"</b> · Vulkan "+esc(d.apiVersion)+" · "+esc(d.driverName||"")+
      (cu? (" · "+cu+" CU") : "") + " · 工作缓冲 "+sizeStr(d.maxWorkingBuffer)+
      " · 子组 "+d.subgroupSize+" · "+(d.hasTimestamps? "GPU 时间戳计时" : "<span class='warn'>主机计时</span>");
  document.title = "vk-bench — " + d.name;

  // ---- hero tiles
  $("hero").innerHTML =
    tile("FP32 FMA", fmt(p.fp32/1000,2), "TFLOPS", cu? ("每 CU "+fmt(p.fp32/cu,0)+" GFLOPS") : "") +
    tile("FP16 打包", fmt(p.fp16/1000,2), "TFLOPS", p.fp32>0? (fmt(p.fp16/p.fp32,2)+" × fp32") : "") +
    tile("显存读带宽", fmt(p.bwReadDram||p.bwRead,0), "GB/s", "写 "+fmt(p.bwWriteDram,0)+" · 拷贝 "+fmt(p.bwCopyDram,0)) +
    tile("L1 延迟", fmt(p.l1Latency,1), "ns", "最大容量处 "+fmt(p.dramLatency,0)+" ns") +
    (lv.length>2? tile("末级缓存（推断）", sizeStr(capOf(lv[lv.length-2])), "", "L1 "+sizeStr(capOf(lv[0]))) : "") +
    (matrixRatio? tile("矩阵 : 向量", fmt(matrixRatio,2), "×", esc(p.matrixBestName||"")) : "") +
    (clock? tile("推算运行时钟", fmt(clock,2), "GHz", "按每 CU 128 flops/cycle 推算") : "");

  // ---- device table
  const feats = Object.entries(d.features).map(([k,v])=> (v?"<span class='ok'>"+k+"</span>":"<span class='dim'>"+k+"</span>")).join(" ");
  let devRows = [
    row(["设备", "<b class='hi'>"+esc(d.name)+"</b> <span class='dim'>("+(["Other","Integrated GPU","Discrete GPU","Virtual GPU","CPU"][d.type]||"Other")+")</span>"]),
    row(["apiVersion / SPIR-V 上限", d.apiVersion + " / " + (d.spvMax||"?")]),
    row(["驱动", esc(d.driverName||"")+" "+(d.driverVersion?("0x"+d.driverVersion.toString(16)):"")+" <span class='dim'>"+esc(d.driverInfo||"")+"</span>"]),
    row(["子组", "size "+d.subgroupSize+" · ops 0x"+d.subgroupOps.toString(16)+" · stages 0x"+d.subgroupStages.toString(16)]),
    row(["显存/堆", "device-local "+sizeStr(d.heapDeviceLocal)+" · host-visible "+sizeStr(d.heapHostVisible)]),
    row(["限制", "maxWGInvocations "+d.maxComputeWorkGroupInvocations+" · sharedMem "+sizeStr(d.maxComputeSharedMemorySize)+
        " · timestamp "+(d.hasTimestamps?("有 ("+d.timestampPeriod+" ns)"):"<span class='warn'>无（主机侧计时）</span>")]),
    row(["启用特性", feats]),
    row(["启用扩展", d.enabledExtensions.length? d.enabledExtensions.map(e=>"<code>"+esc(e)+"</code>").join(" ") : "<span class='dim'>仅核心</span>"]),
  ];
  if (d.unsupported.length) devRows.push(row(["<span class='warn'>不支持/降级</span>", d.unsupported.map(esc).join("<br>")]));
  table("devtbl", ["项","值"], devRows);

  // ---- notes + ratios
  $("notes").innerHTML = p.notes.map(n=>"<div class='note'>"+esc(n)+"</div>").join("");
  const R=(a,b)=>(b>0? (a/b).toFixed(2):"—");
  table("ratios", [], [
    rowN("fp16 : fp32", [R(p.fp16,p.fp32)]),
    rowN("fp64 : fp32", [R(p.fp64,p.fp32)]),
    rowN("int32 : fp32", [R(p.int32,p.fp32)]),
    rowN("SFU : fp32", [R(p.sfu,p.fp32)]),
    rowN("<b class='hi'>矩阵 : 向量</b>", ["<b class='hi'>"+R(p.matrixBest,p.fp32)+"</b>"]),
    rowN("vec4 : vec1 (fp32)", [R(p.vec4,p.vec1)]),
    rowN("FMA : ADD", [R(p.fp32,p.add)]),
    rowN("LDS : 显存", [R(p.lds,p.dramBw)]),
  ]);

  // ---- peaks
  const peaks = [
    ["FP32 FMA (vec4)", p.fp32, "GFLOPS", "var(--acc)"],
    ["FP32 FMA (scalar)", p.vec1, "GFLOPS", "var(--acc)"],
    ["FP32 dep chain", p.fmaDep, "GFLOPS", "var(--dim)"],
    ["FP16 (best)", p.fp16, "GFLOPS", "var(--g)"],
    ["FP64", p.fp64, "GFLOPS", "var(--o)"],
    ["INT32 IMAD", p.int32, "GOPS", "var(--p)"],
    ["INT8 DP4A", p.dp4a, "GOPS", "var(--y)"],
    ["SFU", p.sfu, "GOPS", "var(--r)"],
  ].filter(x=>x[1]>0);
  const pmax = Math.max.apply(null, peaks.map(x=>x[1]).concat([1]));
  table("peaks", [], peaks.map(x=>rowN(esc(x[0]), [fmt(x[1],1)+" "+x[2]+" "+bar(x[1],pmax,x[3])])));

  // ---- bandwidth peaks + level table
  const bws = [["读（缓存内平台）", p.bwRead],["写（平台）", p.bwWrite],["拷贝（平台）", p.bwCopy],["共享内存 LDS", p.lds]].filter(x=>x[1]>0);
  const bmax = Math.max.apply(null, bws.map(x=>x[1]).concat([1]));
  let bwRows = bws.map(x=>rowN(esc(x[0]), [fmt(x[1],1)+" GB/s "+bar(x[1],bmax,"var(--g)")]));
  bwRows.push(rowN("<b class='hi'>显存 读 / 写 / 拷</b>", ["<b class='hi'>"+fmt(p.bwReadDram,0)+" / "+fmt(p.bwWriteDram,0)+" / "+fmt(p.bwCopyDram,0)+"</b> GB/s"]));
  table("bwpeaks", [], bwRows);
  if (lv.length) {
    table("levels", ["层级","容量（推断）","延迟","读带宽"], lv.map(l=>
      row([ "<b class='hi'>"+esc(l.name)+"</b>",
            (l.name==="显存"||l.name==="\u663e\u5b58") ? fmt(capOf(l),0)+" B 起" : "≤ "+sizeStr(capOf(l)),
            l.latency? fmt(l.latency,1)+" ns" : "—", l.bw? fmt(l.bw,1)+" GB/s" : "—" ])));
  }

  // ---- charts
  const bands = [];
  for (let i=0;i<lv.length;i++){
    const prev = i>0 ? Math.log2(capOf(lv[i-1])) : 10;
    bands.push({x0:prev, x1:Math.log2(capOf(lv[i])), label:lv[i].name});
  }
  const vlines = lv.slice(0,-1).map((l,i)=>({x:Math.log2(capOf(l)), color:COLORS[i%COLORS.length],
      label:"≈"+sizeStr(capOf(l))+(l.latency? " · "+fmt(l.latency,1)+" ns":"")}));

  const lat  = byTest("chase_latency").map(m=>({x:m.x,y:m.value}));
  const lat1 = byTest("chase_latency_1cu").map(m=>({x:m.x,y:m.value}));
  const lat0 = byTest("chase_latency_low").map(m=>({x:m.x,y:m.value}));
  const latSeries = [
    {name:"多 CU 聚合（8 链）", pts:lat},
    {name:"单 workgroup（8 链）", pts:lat1},
    {name:"单 workgroup 单链（低排队）", pts:lat0}
  ];
  legend("leg-lat", latSeries.map(s=>s.name), "c-lat");
  chart("c-lat", latSeries, {logX:true, yLabel:"ns / 次加载", yUnit:" ns", xLabel:"工作集", xFmt:pow2Label,
      height:340, bands:bands, vlines:vlines, primary:2});

  const r=[],w=[],cp=[];
  byTest("bw_read_footprint").forEach(m=>r.push({x:m.x,y:m.value}));
  byTest("bw_write_footprint").forEach(m=>w.push({x:m.x,y:m.value}));
  byTest("bw_copy_footprint").forEach(m=>cp.push({x:m.x,y:m.value}));
  const bwSeries = [{name:"读",pts:r},{name:"写",pts:w},{name:"拷贝",pts:cp}];
  legend("leg-bw", bwSeries.map(s=>s.name), "c-bw");
  chart("c-bw", bwSeries, {logX:true, yLabel:"GB/s", yUnit:" GB/s", xLabel:"工作集", xFmt:pow2Label, height:330,
      bands:bands});

  const st = byTest("bw_stride").map(m=>({x:m.x,y:m.value}));
  const stSeries = [{name:"有效带宽（每次 4 B）", pts:st}];
  legend("leg-stride", stSeries.map(s=>s.name), "c-stride");
  chart("c-stride", stSeries, {logX:true, yLabel:"GB/s 有效", yUnit:" GB/s", xLabel:"stride", xFmt:v=>v+" B", height:260,
      vlines:[{x:Math.log2(32),label:"32 B"},{x:Math.log2(128),label:"128 B"}]});

  const occ = byTest("fma_f32_occupancy").map(m=>({x:m.x,y:m.value}));
  const occSeries = [{name:"FP32 FMA (vec4)", pts:occ}];
  legend("leg-occ", occSeries.map(s=>s.name), "c-occ");
  chart("c-occ", occSeries, {logX:true, yLabel:"GFLOPS", xLabel:"workgroups", xFmt:v=>String(Math.round(v)), height:260,
      vlines: p.occSaturation? [{x:Math.log2(p.occSaturation), label:"95% 峰值"}] : []});

  // ---- latency level table
  if (lv.length) {
    table("lattbl", ["层级","推断容量","该级延迟","该级读带宽"], lv.map(l=>
      row([ "<b class='hi'>"+esc(l.name)+"</b>",
            (l.name==="\u663e\u5b58"||l.name==="显存") ? "≥ "+sizeStr(lv.length>1?capOf(lv[lv.length-2]):0) : sizeStr(capOf(l)),
            l.latency? fmt(l.latency,1)+" ns" : "—", l.bw? fmt(l.bw,1)+" GB/s" : "—" ])));
  }

  // ---- matrix table
  const mx = DATA.measures.filter(m=>m.group==="matrix");
  if (!mx.length) { $("matrix").innerHTML = "<p class='dim'>无矩阵测点</p>"; }
  else {
    table("matrix", ["config","形态","结果","单位","校验","备注"], mx.map(m=>{
      const v = m.value>0? fmt(m.value,2) : "<span class='dim'>—</span>";
      const vc = m.verify==="ok"? "<span class='ok'>ok</span>" : (m.verify? "<span class='warn'>"+esc(m.verify)+"</span>":"<span class='dim'>—</span>");
      return row([ "<code>"+esc(m.test)+"</code>", esc(m.label||""), v, esc(m.metric||""), vc, m.x? ("m*n*k="+(m.x|0)):"" ]);
    }));
  }

  // ---- all measures
  const groups = ["alu","matrix","cache","memory"];
  const gname = {alu:"向量 / ALU", matrix:"矩阵", cache:"缓存 / 延迟", memory:"带宽 / 其他"};
  let allRows = [];
  groups.forEach(gr=>{
    const list = DATA.measures.filter(m=>m.group===gr);
    if (!list.length) return;
    allRows.push("<tr><th colspan='7'>"+gname[gr]+"</th></tr>");
    list.forEach(m=>{
      const vv = m.value>0? fmt(m.value, m.value<10?3:1) : "";
      const vf = m.verify==="ok"? "<span class='ok'>ok</span>" : (m.verify? "<span class='warn'>"+esc(m.verify)+"</span>":"");
      allRows.push(row([ esc(m.test), esc(m.label||""), vv, esc(m.metric||""),
                         m.x? (esc(m.xLabel)+"="+(m.xLabel==="footprint"? sizeStr(m.x) : fmt(m.x,0))) : "",
                         vf, m.hostTimed? "<span class='warn'>host</span>" : "" ]));
    });
  });
  table("all", ["测点","说明","数值","单位","参数","校验","计时"], allRows);
  $("allsub").textContent = "共 " + DATA.measures.length + " 个测点（点开看原始数据；host = 该点用主机墙钟计时）";
}
render();
</script>
</body></html>
)HTML";

} // namespace

void writeHtmlFile(const BenchCtx& c, const std::string& path) {
    std::string json = buildJson(c);
    // keep the JSON safe to embed inside a <script> element
    std::string safe;
    safe.reserve(json.size() + 16);
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '<' && i + 1 < json.size() && json[i + 1] == '/') {
            safe += "<\\/";
            ++i;
        } else {
            safe += json[i];
        }
    }
    std::string html = kTemplate;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) {
            s.replace(p, from.size(), to);
            p += to.size();
        }
    };
    replaceAll(html, "__TITLE__", c.gpu->cap().props.deviceName);
    replaceAll(html, "__JSON__", safe);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(html.data(), 1, html.size(), f);
    std::fclose(f);
}
