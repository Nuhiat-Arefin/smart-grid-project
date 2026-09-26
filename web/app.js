'use strict';
/* Smart Grid Restoration Engine - dashboard. All decisions come from the C++17
   engine (POST /api/run); this file only draws the city and animates the trace. */

const $ = (s) => document.querySelector(s);
const NS = 'http://www.w3.org/2000/svg';
const S = { grid: null, nodes: {}, lines: {}, normal: new Set(), hour: 19, faults: new Set(), result: null, stage: 0, token: 0, request: 0, controller: null, dirty: true, busy: false, tourIndex: -1 };
const EL = { line: {}, hit: {}, flow: {}, mark: {}, node: {}, under: {} };

const KIND_COLOR = { plant: '#f97316', solar: '#facc15', wind: '#2dd4bf', battery: '#22c55e', substation: '#94a3b8' };
const PRIO_COLOR = { 1: '#ef4444', 2: '#f59e0b', 3: '#3b82f6', 4: '#a855f7' };
const PRIO_NAME = { 1: 'Critical', 2: 'Essential', 3: 'Residential', 4: 'Commercial' };
const ICONS = {
  plant: '⚡', solar: '☀️', wind: '🌬️', battery: '🔋', hospital: '🏥', water: '💧', emergency: '🚒', school: '🏫',
  telecom: '📡', university: '🎓', metro: '🚇', residential: '🏠', commercial: '🛍️', industrial: '🏭',
};
const ICON_BY_ID = { BIZ: '🏢' };
const BATTERY_COLORS = ['#22c55e', '#38bdf8', '#f472b6'];
const TOUR = [
  { preset: 'normal-peak', stage: 3, title: 'Begin with a healthy city', text: 'All demand is served at the evening peak. The four service bars show the priority classes. Line colour shows the fraction of each line rating in use.' },
  { preset: 'hospital-feeder', stage: 1, title: '1 · Detect the lost supply', text: 'The hospital feeder L10 fails at 10:00. BFS finds the disconnected area; the panel lists its loads and visit order. Click Detect to replay the search, or continue when ready.' },
  { preset: 'hospital-feeder', stage: 2, title: '2 · Rebuild without a loop', text: 'Kruskal tries healthy lines in resistance order. It closes tie T10 to reconnect the area, rejecting edges that would form a cycle. The panel records each choice.' },
  { preset: 'hospital-feeder', stage: 3, title: '3 · Check the bottleneck', text: 'Blind reconnection would exceed L09’s 15 MW rating. Max-flow uses the hospital battery to cover the gap. The final model serves every load within the line limits.' },
  { preset: 'east-substation-fire', stage: 3, title: 'Why branch exchange matters', text: 'Eight faults cut the East Substation. Two line swaps increase residential service. Press Compare policies to see each priority class: the policy values priority before total MW.' },
  { preset: 'hospital-island', stage: 4, title: '4 · Restore only what can be supplied', text: 'The hospital is now an island. Its battery supplies part of the demand. Compare policies shows why shedding unrelated neighbourhoods cannot solve an isolated bottleneck. Switching times are simulated.' },
  { preset: 'hospital-island', stage: 5, title: 'Keep energy ready for the fault', text: 'The DP chart shows charge, price and planned reserves. Compare daily costs in the table below. The reserve improves readiness but cannot guarantee full service for every fault.' },
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// Stage tabs replay the selected algorithm; running the engine does not wait for animation.
const PACE = { start: 120, node: 18, keep: 10, close: 220, reject: 60, root: 50, overload: 500, exchange: 500, iter: 350, op: 350 };
const fmt = (x, d = 1) => (x == null || Number.isNaN(x) ? '–' : Number(x).toFixed(d));
const hh = (h) => `${String(h).padStart(2, '0')}:00`;
const nm = (id) => (S.nodes[id] ? S.nodes[id].name : id);
const badge = (p) => `<span class="badge p${p}">P${p}</span>`;
const sum = (xs) => xs.reduce((a, b) => a + b, 0);

function svg(tag, attrs = {}, parent) {
  const e = document.createElementNS(NS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}

/* ------------------------------------------------------------ geometry & data */
function radius(n) {
  if (n.kind === 'plant') return 18;
  if (n.kind === 'solar' || n.kind === 'wind') return 16;
  if (n.kind === 'battery') return 14;
  if (n.kind === 'substation') return 9;
  return 11 + Math.sqrt(n.peak_mw) * 2;
}
const nodeColor = (n) => (n.kind === 'load' ? PRIO_COLOR[n.priority] : KIND_COLOR[n.kind]);
const icon = (n) => ICON_BY_ID[n.id] || ICONS[n.kind === 'load' ? n.category : n.kind] || '';
const shortName = (n) => n.name.replace(' Substation', ' Sub.');

function demandAt(n, h) { return n.kind === 'load' ? n.peak_mw * S.grid.demand_factor[n.category][h] : 0; }
function supplyAt(n, h) {
  if (n.kind === 'plant') return n.capacity_mw;
  if (n.kind === 'solar') return n.capacity_mw * S.grid.solar_factor[h];
  if (n.kind === 'wind') return n.capacity_mw * S.grid.wind_factor[h];
  return 0;
}
const plan = () => S.grid.battery_plans.dp;
const socAt = (id, h) => plan().batteries[id].soc_mwh[h];

function loadingColor(t) {
  const stops = [[0, [34, 197, 94]], [0.7, [234, 179, 8]], [1, [239, 68, 68]]];
  t = Math.max(0, Math.min(1, t));
  for (let i = 1; i < stops.length; i++) {
    if (t <= stops[i][0]) {
      const [t0, c0] = stops[i - 1];
      const [t1, c1] = stops[i];
      const u = (t - t0) / (t1 - t0);
      return `rgb(${c0.map((v, k) => Math.round(v + (c1[k] - v) * u)).join(',')})`;
    }
  }
  return 'rgb(239,68,68)';
}

/** Nodes reachable from `roots` over `closed` lines (BFS), to animate energisation. */
function energized(closed, roots) {
  const adj = {};
  for (const id in S.nodes) adj[id] = [];
  for (const lid of closed) { const l = S.lines[lid]; adj[l.a].push(l.b); adj[l.b].push(l.a); }
  const seen = new Set(roots);
  const queue = [...roots];
  while (queue.length) {
    const u = queue.shift();
    for (const v of adj[u]) if (!seen.has(v)) { seen.add(v); queue.push(v); }
  }
  return seen;
}

/* ----------------------------------------------------------------------- map */
function buildMap() {
  const root = $('#map');
  root.innerHTML = '';
  const gLines = svg('g', {}, root);
  const gMarks = svg('g', {}, root);
  const gNodes = svg('g', {}, root);
  for (const l of S.grid.lines) {
    const a = S.nodes[l.a];
    const b = S.nodes[l.b];
    const g = svg('g', {}, gLines);
    const pts = { x1: a.x, y1: a.y, x2: b.x, y2: b.y };
    const under = svg('line', { ...pts, class: 'under' }, g);
    under.style.display = 'none';
    const hit = svg('line', { ...pts, class: 'hit', tabindex: 0, role: 'button',
      'aria-label': `Toggle fault ${l.id}: ${a.name} to ${b.name}`, 'aria-pressed': 'false' }, g);
    const ln = svg('line', { ...pts, class: 'ln open' }, g);
    const fl = svg('line', { ...pts, class: 'flow' }, g);
    fl.style.display = 'none';
    const len = Math.hypot(b.x - a.x, b.y - a.y) || 1;
    const ox = (-(b.y - a.y) / len) * 9;
    const oy = ((b.x - a.x) / len) * 9;
    const lid = svg('text', { x: (a.x + b.x) / 2 + ox, y: (a.y + b.y) / 2 + oy + 3, class: 'lid', 'text-anchor': 'middle' }, gMarks);
    lid.textContent = l.id;
    const mark = svg('text', { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 + 5, class: 'xmark', 'text-anchor': 'middle' }, gMarks);
    mark.textContent = '✕';
    mark.style.display = 'none';
    hit.addEventListener('click', () => toggleFault(l.id));
    hit.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggleFault(l.id); }
    });
    hit.addEventListener('mouseenter', (e) => { ln.classList.add('hover'); showTip(e, lineTip(l.id)); });
    hit.addEventListener('mousemove', moveTip);
    hit.addEventListener('mouseleave', () => { ln.classList.remove('hover'); hideTip(); });
    EL.line[l.id] = ln; EL.hit[l.id] = hit; EL.flow[l.id] = fl; EL.mark[l.id] = mark; EL.under[l.id] = under;
  }
  for (const n of S.grid.nodes) {
    const r = radius(n);
    const g = svg('g', { class: 'node', transform: `translate(${n.x},${n.y})` }, gNodes);
    const shed = svg('circle', { r: r + 5, fill: 'none', stroke: '#ef4444', 'stroke-width': 3.5, opacity: 0 }, g);
    const served = svg('circle', { r: r + 5, class: 'served', transform: 'rotate(-90)', opacity: 0 }, g);
    let body;
    if (n.kind === 'plant') body = svg('rect', { x: -r, y: -r, width: 2 * r, height: 2 * r, rx: 6, class: 'body' }, g);
    else if (n.kind === 'substation') body = svg('rect', { x: -r, y: -r, width: 2 * r, height: 2 * r, class: 'body', transform: 'rotate(45)' }, g);
    else if (n.kind === 'battery') body = svg('rect', { x: -r, y: -r * 0.75, width: 2 * r, height: 1.5 * r, rx: 4, class: 'body' }, g);
    else body = svg('circle', { r, class: 'body' }, g);
    body.setAttribute('fill', nodeColor(n));
    svg('circle', { r: r + 5, class: 'ring' }, g);
    const ic = svg('text', { class: 'icon', y: 1 }, g);
    ic.textContent = icon(n);
    const name = svg('text', { class: 'name', y: r + 15 }, g);
    name.textContent = shortName(n);
    const sub = svg('text', { class: 'sub', y: r + 27 }, g);
    const rest = svg('text', { class: 'restored', y: -r - 9 }, g);
    g.addEventListener('mouseenter', (e) => showTip(e, nodeTip(n.id)));
    g.addEventListener('mousemove', moveTip);
    g.addEventListener('mouseleave', hideTip);
    EL.node[n.id] = { g, served, shed, sub, rest, r };
  }
}

/** Draw one "view" of the grid: which lines are closed, which nodes are dark, flows, labels. */
function applyView(v) {
  for (const l of S.grid.lines) {
    const ln = EL.line[l.id];
    let cls = 'ln';
    let stroke = '';
    let width = '';
    const dead = v.autoDead && (v.off.has(l.a) || v.off.has(l.b));
    if (v.faults.has(l.id)) cls += ' fault';
    else if (v.active.has(l.id)) {
      if (dead) cls += ' dead';
      else if (v.loading) {
        stroke = loadingColor(v.loading[l.id] || 0);
        width = `${2.5 + 3.5 * Math.min(1, Math.abs(v.flows[l.id] || 0) / 45)}px`;
        cls += ' active';
      } else if (v.fresh && v.fresh.has(l.id)) cls += ' new';
      else cls += ' active';
    } else if (v.opened && v.opened.has(l.id)) cls += ' opened';
    else cls += ' open';
    if (v.highlight && v.highlight.has(l.id)) cls += ` ${v.highlightClass || 'cut'}`;
    ln.setAttribute('class', cls);
    ln.style.stroke = stroke;
    ln.style.strokeWidth = width;
    EL.under[l.id].style.display = v.loading && v.fresh && v.fresh.has(l.id) && v.active.has(l.id) ? '' : 'none';
    EL.mark[l.id].style.display = v.faults.has(l.id) ? '' : 'none';
    const fl = EL.flow[l.id];
    const f = v.flows ? v.flows[l.id] || 0 : 0;
    if (v.flows && v.active.has(l.id) && !dead && Math.abs(f) > 0.05) {
      const [p, q] = f >= 0 ? [S.nodes[l.a], S.nodes[l.b]] : [S.nodes[l.b], S.nodes[l.a]];
      fl.setAttribute('x1', p.x); fl.setAttribute('y1', p.y); fl.setAttribute('x2', q.x); fl.setAttribute('y2', q.y);
      fl.style.display = '';
    } else fl.style.display = 'none';
  }
  for (const n of S.grid.nodes) {
    const e = EL.node[n.id];
    const off = v.off.has(n.id);
    e.g.classList.toggle('off', off);
    e.g.classList.toggle('isolated', off && !!v.isolated && v.isolated.has(n.id));
    e.g.classList.toggle('cutnode', !!v.nodeHighlight && v.nodeHighlight.has(n.id));
    let frac = 1;
    if (v.served && n.kind === 'load') {
      const info = v.served[n.id];
      frac = info.demand_mw > 0 ? info.served_mw / info.demand_mw : 1;
    }
    const C = 2 * Math.PI * (e.r + 5);
    const partial = !!v.served && n.kind === 'load' && frac < 0.999;
    e.shed.setAttribute('opacity', partial ? 1 : 0);
    e.served.setAttribute('opacity', partial ? 1 : 0);
    e.served.setAttribute('stroke-dasharray', `${frac * C} ${C}`);
    e.sub.textContent = v.sub ? v.sub(n) : '';
    e.rest.textContent = (v.restored && v.restored[n.id]) || '';
  }
}

/* --------------------------------------------------------------- views */
const subPlanned = (n) => {
  const h = S.hour;
  if (n.kind === 'load') return `${fmt(demandAt(n, h))} MW`;
  if (n.kind === 'plant' || n.kind === 'solar' || n.kind === 'wind') return `${fmt(supplyAt(n, h))} MW`;
  if (n.kind === 'battery') return `${fmt(socAt(n.id, h))}/${n.battery.energy_mwh} MWh`;
  return '';
};
const subServed = (r) => (n) => {
  const info = r.nodes[n.id];
  if (n.kind === 'load') return `${fmt(info.served_mw)}/${fmt(info.demand_mw)} MW`;
  if (n.kind === 'battery') return `${fmt(info.supplied_mw)} MW · ${fmt(info.soc_mwh)} MWh`;
  if (info.available_mw !== undefined) return `${fmt(info.supplied_mw)}/${fmt(info.available_mw)} MW`;
  return '';
};

function liveLines(faults) { return new Set([...S.normal].filter((x) => !faults.has(x))); }

function viewBefore(faults) {
  return { active: new Set(S.normal), faults, off: new Set(), sub: subPlanned };
}

function viewDetect(r) {
  const faults = new Set(r.summary.faults);
  return {
    active: liveLines(faults), faults, off: new Set(r.detect.deenergized), isolated: new Set(r.detect.isolated),
    autoDead: true, sub: subPlanned,
  };
}

function viewRebuild(r) {
  const faults = new Set(r.summary.faults);
  const mst = new Set(r.rebuild.mst_lines);
  return {
    active: mst, faults, off: new Set(r.detect.deenergized), isolated: new Set(r.detect.isolated),
    fresh: new Set(r.rebuild.closed_by_mst), sub: subPlanned,
  };
}

function viewFinal(r, withRestored = false) {
  const faults = new Set(r.summary.faults);
  const flows = {};
  const loading = {};
  for (const [id, x] of Object.entries(r.lines)) { flows[id] = x.flow_mw; loading[id] = x.loading; }
  const off = new Set(Object.keys(S.nodes).filter((id) => r.nodes[id].root == null));
  const restored = {};
  if (withRestored) {
    for (const id of Object.keys(S.nodes)) {
      const info = r.nodes[id];
      if (info.interrupted && info.restored_s) restored[id] = `+${fmt(info.restored_s, 0)}s`;
    }
  }
  return {
    active: new Set(r.verify.final_lines), faults, off, fresh: new Set(r.route.closed),
    opened: new Set(r.route.opened), flows, loading, served: r.nodes, sub: subServed(r), restored,
  };
}

const FINAL_VIEW = { 1: viewDetect, 2: viewRebuild, 3: (r) => viewFinal(r), 4: (r) => viewFinal(r, true), 5: (r) => viewFinal(r, true) };

/* ---------------------------------------------------------------- panels */
function stats(items) {
  return `<div class="stats">${items.map(([v, l, c]) => `<div class="stat"><b class="${c || ''}">${v}</b><span>${l}</span></div>`).join('')}</div>`;
}

function panelDetect(r) {
  const d = r.detect;
  const lost = d.deenergized.filter((id) => S.nodes[id].kind === 'load');
  const lostMW = sum(Object.values(d.lost_mw));
  const rows = lost.map((id) => `<tr><td>${nm(id)}</td><td>${badge(S.nodes[id].priority)}</td><td>${fmt(r.nodes[id].demand_mw)} MW</td></tr>`).join('');
  return `
    <h2>1 · Detect</h2><div class="algo">Breadth-first search from both plants · O(V + E)</div>
    <p class="explain">BFS starts from every power plant at once and follows only the lines that are still closed.
    A node the search never reaches has lost power. A second BFS over every surviving line (open ties included)
    finds nodes that no rerouting can reach.</p>
    ${stats([[d.deenergized.length, 'nodes lost power', d.deenergized.length ? 'bad' : 'good'], [lost.length, 'loads in the dark'], [fmt(lostMW), 'MW interrupted']])}
    ${r.summary.faults.length ? '' : '<div class="note">No faults: the search reaches every node from the plants.</div>'}
    ${d.isolated.length ? `<div class="note warn"><b>Physically isolated</b> (no surviving line to a plant): ${d.isolated.map(nm).join(', ')}</div>` : ''}
    ${lost.length ? `<h3>Loads that lost power</h3><table><tr><th>Load</th><th>Class</th><th>Demand</th></tr>${rows}</table>` : ''}
    <h3>BFS visit order (${d.bfs_order.length} nodes reached)</h3>
    <div>${d.bfs_order.map((id) => `<span class="pill">${id}</span>`).join('')}</div>`;
}

function panelRebuild(r) {
  const rb = r.rebuild;
  if (!rb.rebuilt) {
    return `<h2>2 · Rebuild</h2><div class="algo">Kruskal minimum spanning tree + union–find · O(E log E)</div>
      <div class="note">Nothing lost power, so the operating tree is kept as it is (no switching needed).</div>
      <p class="explain">The normal operating network is itself the minimum spanning tree of the intact grid:
      the ${S.normal.size} lines it keeps closed are the cheapest (lowest-resistance) loop-free way to reach every node.</p>`;
  }
  const live = liveLines(new Set(r.summary.faults));
  const lines = rb.kruskal.filter((k) => k.kind === 'line');
  const rejected = lines.filter((k) => !k.accepted).length;
  const rows = rb.kruskal.map((k, i) => {
    let what;
    if (k.kind === 'source') what = `${k.item} → virtual root`;
    else if (k.kind === 'backup') what = `${k.item} (battery) → virtual root`;
    else what = `${k.item} <span class="k">${k.a}–${k.b}</span>`;
    let res;
    if (k.accepted && k.kind === 'line') res = live.has(k.item) ? '<span class="ok">✓ keep</span>' : '<span class="ok"><b>✓ close</b></span>';
    else if (k.accepted && k.kind === 'backup') res = '<span class="warn">✓ island root</span>';
    else if (k.accepted) res = '<span class="ok">✓ root</span>';
    else res = k.kind === 'line' ? '<span class="no">✗ loop</span>' : '<span class="k">– already fed</span>';
    const w = k.weight == null ? '∞' : fmt(k.weight, 3);
    return `<tr id="k-${i}"><td>${i + 1}</td><td>${what}</td><td>${w}</td><td>${res}</td></tr>`;
  }).join('');
  return `
    <h2>2 · Rebuild</h2><div class="algo">Kruskal minimum spanning tree + union–find · O(E log E)</div>
    <p class="explain">Plants join a virtual root at zero cost, then surviving lines are tried from lowest resistance up.
    Union–find rejects any line whose two ends are already connected: it would close a loop. Batteries join last,
    so a battery only becomes a root for an island that no plant can reach.</p>
    ${stats([[rb.mst_lines.length, 'lines in the new tree'], [rejected, 'rejected (would loop)', 'warn'], [rb.closed_by_mst.length, 'tie lines to close', 'good']])}
    ${rb.closed_by_mst.length ? `<div class="note">Close ${rb.closed_by_mst.map((x) => `<b>${x}</b> (${S.lines[x].a}–${S.lines[x].b})`).join(', ')} to re-feed the dark area.</div>` : ''}
    ${rb.islands.length ? `<div class="note warn">Battery island: ${rb.islands.map(nm).join(', ')} will feed what no plant can reach.</div>` : ''}
    <h3>Kruskal log</h3>
    <div class="scroll" id="klog"><table><tr><th>#</th><th>Candidate</th><th>R (Ω)</th><th>Result</th></tr>${rows}</table></div>`;
}

function panelVerify(r) {
  const v = r.verify;
  const s = r.summary;
  const it0 = v.iterations[0];
  const shedMW = sum(Object.values(s.shed_mw));
  const naive = v.naive_overloads.map((id) => `<b>${id}</b> (${fmt(Math.abs(r.lines[id].naive_flow_mw))} MW on a ${S.lines[id].capacity_mw} MW line)`);
  const src = v.source_overloads.map((id) => `<b>${nm(id)}</b>`);
  const iterRows = v.iterations.map((it, i) => {
    let action = '<span class="ok">feasible ✓</span>';
    if (it.action === 'shed') action = `shed ${it.blocks} block${it.blocks > 1 ? 's' : ''} of <b>${nm(it.load)}</b> (${fmt(it.shed_mw, 2)} MW)`;
    if (it.action === 'added back') action = `<span class="ok">added back ${it.blocks} block(s)</span>`;
    const cut = it.bottleneck ? `<br><span class="k">cut: ${it.bottleneck.map((b) => b.id).join(', ')}</span>` : '';
    return `<tr data-iter="${i}" class="${it.action === 'shed' ? 'bad' : ''}"><td>${i + 1}</td><td>${fmt(it.flow_mw, 2)} / ${fmt(it.demand_mw, 2)}</td><td>${fmt(it.deficit_mw, 2)}</td><td>${action}${cut}</td></tr>`;
  }).join('');
  const sourceOrder = ['solar', 'wind', 'plant', 'battery'];
  const sourceIds = sourceOrder.flatMap((kind) => S.grid.nodes.filter((n) => n.kind === kind).map((n) => n.id));
  const supply = sourceIds.map((id) => {
    const mw = v.supply_mw[id];
    const avail = r.nodes[id].available_mw;
    const pct = avail > 0 ? (100 * mw) / avail : 0;
    return `<tr><td>${nm(id)}</td><td style="width:45%"><div class="bar"><i style="width:${pct}%"></i></div></td><td>${fmt(mw)} / ${fmt(avail)} MW</td></tr>`;
  }).join('');
  const exch = v.exchanges.map((x) => {
    const b = x.served_before_mw;
    const a = x.served_after_mw;
    const gains = [1, 2, 3, 4].filter((p) => Math.abs(a[p - 1] - b[p - 1]) > 1e-6)
      .map((p) => `${PRIO_NAME[p]} ${fmt(b[p - 1])} → ${fmt(a[p - 1])} MW`).join(', ');
    return `<div class="note">Branch exchange: close <b>${x.close}</b>, open <b>${x.open}</b> · ${gains}</div>`;
  }).join('');
  return `
    <h2>3 · Verify</h2><div class="algo">Edmonds–Karp max-flow + min-cut · O(V·E²)</div>
    <p class="explain">Super-source → every source (capacity = power it can give now; renewables first, then plants,
    then batteries), each tree line both ways (capacity = rating), each load → super-sink (capacity = demand).
    If the max flow equals the demand, every load is served with no line over its rating. If not, the minimum cut
    names the bottleneck: first the tree is re-shaped across it (branch exchange), then the lowest-priority blocks
    behind it are shed.</p>
    ${stats([[`${fmt(it0.flow_mw)}/${fmt(it0.demand_mw)}`, 'first max-flow / demand (MW)'], [v.naive_overloads.length + v.source_overloads.length, 'overloads caught', v.naive_overloads.length ? 'warn' : ''], [fmt(shedMW), 'MW shed', shedMW > 0 ? 'bad' : 'good']])}
    ${naive.length || src.length ? `<div class="note warn">Reconnecting everything blindly would overload ${[...naive, ...src].join(', ')}.</div>` : ''}
    ${exch}
    <h3>Capacity check iterations</h3>
    <table id="iters"><tr><th>#</th><th>Flow / demand (MW)</th><th>Deficit</th><th>Decision</th></tr>${iterRows}</table>
    <h3>Supply used</h3><table>${supply}</table>`;
}

function panelRoute(r) {
  const rt = r.route;
  const s = r.summary;
  const ops = rt.sequence.map((op, i) => {
    const what = op.line ? `${op.line} <span class="k">${S.lines[op.line].a}–${S.lines[op.line].b}</span>` : nm(op.target);
    const why = op.for ? ` → ${nm(op.for)}` : op.blocks ? ` (${op.blocks} block${op.blocks > 1 ? 's' : ''})` : '';
    return `<tr id="op-${i}"><td>${fmt(op.t_s, 0)} s</td><td>${op.op}</td><td>${what}${why}</td></tr>`;
  }).join('');
  const held = rt.held_off.map((h) => `${nm(h.target)} (${h.blocks} blocks)`).join(', ');
  const loads = S.grid.nodes.filter((n) => n.kind === 'load' && (r.nodes[n.id].interrupted || r.nodes[n.id].status !== 'served'));
  loads.sort((a, b) => a.priority - b.priority || (r.nodes[a.id].restored_s ?? 1e9) - (r.nodes[b.id].restored_s ?? 1e9));
  const rows = loads.map((n) => {
    const x = r.nodes[n.id];
    const t = x.restored_s == null ? (x.status === 'shed' ? '<span class="no">shed</span>' : '<span class="no">no path</span>') : `${fmt(x.restored_s, 0)} s`;
    return `<tr><td>${n.name}</td><td>${badge(n.priority)}</td><td>${t}</td><td>${fmt(x.served_mw)}/${fmt(x.demand_mw)}</td><td>${x.path_resistance == null ? '–' : fmt(x.path_resistance, 3)}</td></tr>`;
  }).join('');
  const crit = s.restoration_s[1];
  return `
    <h2>4 · Route</h2><div class="algo">${rt.algorithm === 'dijkstra' ? 'Dijkstra with a binary heap · O((V + E) log V)' : 'Floyd–Warshall · O(V³)'}</div>
    <p class="explain">From each tree's root the minimum-resistance path to every load is found. Switching is
    break-before-make: live loads that must be shed are opened first, then lines are closed along each load's path in
    priority order (critical first, then electrically nearest), so the network is radial at every single step.</p>
    ${stats([[rt.sequence.length, 'switching operations'], [crit.count ? `${fmt(crit.max, 0)} s` : '–', 'restored critical loads by']])}
    ${rt.sequence.length ? `<h3>Switching sequence (30 s per operation)</h3><table id="ops"><tr><th>t</th><th>Op</th><th>Target</th></tr>${ops}</table>` : '<div class="note">No switching needed.</div>'}
    ${held ? `<div class="note warn">Kept disconnected: ${held}</div>` : ''}
    ${rows ? `<h3>Affected loads</h3><table><tr><th>Load</th><th></th><th>Back at</th><th>MW</th><th>Path Ω</th></tr>${rows}</table>` : ''}`;
}

function batteryChart() {
  const W = 388; const H = 200;
  const pad = { l: 34, r: 38, t: 12, b: 24 };
  const iw = W - pad.l - pad.r; const ih = H - pad.t - pad.b;
  const x = (h) => pad.l + (h / 24) * iw;
  const price = S.grid.price;
  const pmax = Math.ceil(Math.max(...price) / 50) * 50;
  const yP = (p) => pad.t + ih - (p / pmax) * ih;
  const yS = (f) => pad.t + ih - f * ih;
  const risk = S.grid.risk_hours;
  let out = `<svg class="chart" viewBox="0 0 ${W} ${H}" width="100%">`;
  out += `<rect x="${x(Math.min(...risk))}" y="${pad.t}" width="${x(Math.max(...risk) + 1) - x(Math.min(...risk))}" height="${ih}" fill="rgba(245,158,11,.10)"/>`;
  out += `<text x="${x(Math.min(...risk)) + 3}" y="${pad.t + 11}" fill="#f59e0b">risk hours</text>`;
  for (const f of [0, 0.25, 0.5, 0.75, 1]) {
    out += `<line class="axis" x1="${pad.l}" x2="${pad.l + iw}" y1="${yS(f)}" y2="${yS(f)}"/>`;
    out += `<text x="${pad.l - 4}" y="${yS(f) + 3}" text-anchor="end">${f * 100}%</text>`;
    out += `<text x="${pad.l + iw + 4}" y="${yS(f) + 3}">$${Math.round(f * pmax)}</text>`;
  }
  for (const h of [0, 6, 12, 18, 24]) out += `<text x="${x(h)}" y="${H - 8}" text-anchor="middle">${hh(h % 24 === 0 && h ? 24 : h).replace('24:00', '24h')}</text>`;
  let pp = '';
  price.forEach((p, h) => { pp += `${h === 0 ? 'M' : 'L'}${x(h)},${yP(p)} L${x(h + 1)},${yP(p)} `; });
  out += `<path d="${pp}" fill="none" stroke="#64748b" stroke-width="1.5"/>`;
  S.grid.nodes.filter((n) => n.kind === 'battery').forEach((n, i) => {
    const soc = plan().batteries[n.id].soc_mwh;
    const pts = soc.map((e, h) => `${x(h)},${yS(e / n.battery.energy_mwh)}`).join(' ');
    out += `<polyline points="${pts}" fill="none" stroke="${BATTERY_COLORS[i]}" stroke-width="2.2"/>`;
    const res = S.grid.reserve_mwh[n.id].map((e, h) => `${x(h)},${yS(e / n.battery.energy_mwh)}`).join(' ');
    out += `<polyline points="${res}" fill="none" stroke="${BATTERY_COLORS[i]}" stroke-width="1" stroke-dasharray="3 3" opacity=".7"/>`;
  });
  out += `<line x1="${x(S.hour)}" x2="${x(S.hour)}" y1="${pad.t}" y2="${pad.t + ih}" stroke="#fff" stroke-width="1.5"/>`;
  out += '</svg>';
  const legend = S.grid.nodes.filter((n) => n.kind === 'battery')
    .map((n, i) => `<span class="pill" style="border-color:${BATTERY_COLORS[i]}">${n.name}</span>`).join('');
  return `${out}<div>${legend}<span class="pill">— price ($/MWh)</span><span class="pill">- - reserve</span></div>`;
}

function panelBattery() {
  const plans = S.grid.battery_plans;
  const idle = plans.idle.cost;
  const risk = S.grid.risk_hours;
  const batteries = S.grid.nodes.filter((n) => n.kind === 'battery');
  const planRows = ['idle', 'threshold', 'dp_no_reserve', 'dp'].map((k) => {
    const p = plans[k];
    const minRisk = Math.min(...risk.map((h) => sum(batteries.map((n) => p.batteries[n.id].soc_mwh[h]))));
    const cls = k === 'dp' ? 'hl' : '';
    return `<tr class="${cls}"><td>${p.label}</td><td>$${Math.round(p.cost).toLocaleString()}</td><td>${fmt(100 * (idle - p.cost) / idle, 2)}%</td><td>${fmt(minRisk)} MWh</td></tr>`;
  }).join('');
  return `
    <h2>5 · Batteries</h2><div class="algo">Dynamic programming over 24 hours · O(T · L · K)</div>
    <p class="explain">State = (hour, stored energy in 0.25 MWh steps). For every state the DP picks the next hour's
    charge level within the rate limits, paying for energy bought from the plants (price × import) plus wear;
    backtracking the cheapest choices gives the day plan. The plan keeps reserve energy during risk hours, which
    improves readiness without guaranteeing full service. A battery can give what it can hold for 2 hours.</p>
    ${batteryChart()}
    <h3>Day plans compared</h3>
    <table><tr><th>Plan</th><th>Daily cost</th><th>Saving vs idle</th><th>Min stored, risk hours</th></tr>${planRows}</table>`;
}

const PANELS = { 1: panelDetect, 2: panelRebuild, 3: panelVerify, 4: panelRoute, 5: panelBattery };

/* ------------------------------------------------------------- animations */
async function animDetect(r, token) {
  const faults = new Set(r.summary.faults);
  const view = { active: liveLines(faults), faults, off: new Set(Object.keys(S.nodes)), autoDead: true, sub: subPlanned };
  applyView(view);
  await sleep(PACE.start);
  for (const id of r.detect.bfs_order) {
    if (token !== S.token) return;
    view.off.delete(id);
    EL.node[id].g.classList.add('visit');
    applyView(view);
    await sleep(PACE.node);
    EL.node[id].g.classList.remove('visit');
  }
  applyView(viewDetect(r));
}

async function animRebuild(r, token) {
  if (!r.rebuild.rebuilt) { applyView(viewRebuild(r)); return; }
  const faults = new Set(r.summary.faults);
  const live = liveLines(faults);
  const base = viewDetect(r);
  const view = { ...base, autoDead: false, active: new Set(), fresh: new Set() };
  applyView(view);
  await sleep(PACE.start);
  const rows = r.rebuild.kruskal;
  for (let i = 0; i < rows.length; i++) {
    if (token !== S.token) return;
    const k = rows[i];
    const row = document.getElementById(`k-${i}`);
    if (row) { row.classList.add('hl'); row.scrollIntoView({ block: 'nearest' }); }
    if (k.kind === 'line') {
      if (k.accepted) {
        view.active.add(k.item);
        if (!live.has(k.item)) view.fresh.add(k.item);
        applyView(view);
        await sleep(live.has(k.item) ? PACE.keep : PACE.close);
      } else {
        view.highlight = new Set([k.item]);
        view.highlightClass = 'reject';
        applyView(view);
        await sleep(PACE.reject);
        view.highlight = null;
        applyView(view);
      }
    } else {
      EL.node[k.item].g.classList.add('visit');
      await sleep(PACE.root);
      EL.node[k.item].g.classList.remove('visit');
    }
    if (row) row.classList.remove('hl');
  }
  applyView(viewRebuild(r));
}

async function animVerify(r, token) {
  const view = viewRebuild(r);
  applyView(view);
  await sleep(PACE.start);
  if (r.verify.naive_overloads.length) {
    view.highlight = new Set(r.verify.naive_overloads);
    view.highlightClass = 'cut';
    view.nodeHighlight = new Set(r.verify.source_overloads);
    applyView(view);
    await sleep(PACE.overload);
    if (token !== S.token) return;
    view.highlight = null;
    view.nodeHighlight = null;
  }
  for (const x of r.verify.exchanges) {
    if (token !== S.token) return;
    view.active.delete(x.open);
    view.active.add(x.close);
    view.fresh.add(x.close);
    view.opened = new Set([...(view.opened || []), x.open]);
    view.highlight = new Set([x.close]);
    view.highlightClass = 'consider';
    applyView(view);
    await sleep(PACE.exchange);
    view.highlight = null;
  }
  const rows = document.querySelectorAll('#iters tr[data-iter]');
  for (const [i, it] of r.verify.iterations.entries()) {
    if (token !== S.token) return;
    if (rows[i]) { rows[i].classList.add('hl'); }
    if (it.bottleneck) {
      view.highlight = new Set(it.bottleneck.filter((b) => b.kind === 'line').map((b) => b.id));
      view.highlightClass = 'cut';
      view.nodeHighlight = new Set([...it.bottleneck.filter((b) => b.kind === 'supply').map((b) => b.id), it.load]);
      applyView(view);
      await sleep(PACE.iter);
    }
    if (rows[i]) rows[i].classList.remove('hl');
  }
  view.highlight = null;
  view.nodeHighlight = null;
  applyView(viewFinal(r));
}

async function animRoute(r, token) {
  const faults = new Set(r.summary.faults);
  const closed = liveLines(faults);
  const roots = new Set(S.grid.nodes.filter((n) => n.kind === 'plant').map((n) => n.id));
  const final = viewFinal(r);
  const view = { ...final, active: closed, opened: new Set(), autoDead: true, restored: {}, flows: null, loading: null };
  const lit = () => {
    const on = energized(closed, roots);
    view.off = new Set(Object.keys(S.nodes).filter((id) => !on.has(id)));
  };
  lit();
  applyView(view);
  await sleep(PACE.start);
  for (const [i, op] of r.route.sequence.entries()) {
    if (token !== S.token) return;
    const row = document.getElementById(`op-${i}`);
    if (row) { row.classList.add('hl'); row.scrollIntoView({ block: 'nearest' }); }
    const before = new Set(view.off);
    if (op.op === 'close') closed.add(op.line);
    else if (op.op === 'open') { closed.delete(op.line); view.opened.add(op.line); }
    else if (op.op === 'start island') roots.add(op.target);
    view.highlight = op.line ? new Set([op.line]) : null;
    view.highlightClass = 'consider';
    lit();
    for (const id of before) {
      if (!view.off.has(id) && S.nodes[id].kind === 'load') {
        view.restored[id] = `+${fmt(op.t_s, 0)}s`;
        EL.node[id].g.classList.add('flash');
        setTimeout(() => EL.node[id].g.classList.remove('flash'), 950);
      }
    }
    applyView(view);
    await sleep(PACE.op);
    if (row) row.classList.remove('hl');
  }
  view.highlight = null;
  applyView({ ...viewFinal(r, true) });
}

async function animBattery(r) { applyView(viewFinal(r, true)); }

const ANIM = { 1: animDetect, 2: animRebuild, 3: animVerify, 4: animRoute, 5: animBattery };

/* --------------------------------------------------------------- stages */
async function showStage(n, animate) {
  if (!S.result || S.dirty || S.busy || !PANELS[n]) return;
  if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) animate = false;
  const token = ++S.token;
  S.stage = n;
  document.querySelectorAll('#stages button').forEach((b) => {
    const k = Number(b.dataset.stage);
    b.classList.toggle('active', k === n);
    b.classList.toggle('done', k < n);
  });
  $('#stageBody').innerHTML = PANELS[n](S.result);
  wirePanel(n);
  if (animate) await ANIM[n](S.result, token);
  else applyView(FINAL_VIEW[n](S.result));
  return token;
}

function wirePanel(n) {
  if (n !== 3) return;
  document.querySelectorAll('#iters tr[data-iter]').forEach((tr) => {
    const it = S.result.verify.iterations[Number(tr.dataset.iter)];
    if (!it.bottleneck) return;
    tr.addEventListener('mouseenter', () => {
      const v = viewRebuild(S.result);
      v.active = new Set(S.result.verify.final_lines);
      v.highlight = new Set(it.bottleneck.filter((b) => b.kind === 'line').map((b) => b.id));
      v.highlightClass = 'cut';
      v.nodeHighlight = new Set(it.bottleneck.filter((b) => b.kind === 'supply').map((b) => b.id));
      applyView(v);
    });
    tr.addEventListener('mouseleave', () => applyView(viewFinal(S.result)));
  });
}

/* -------------------------------------------------------------- KPIs */
function renderKPIs(r) {
  const s = r.summary;
  const unserved = s.total_demand_mw - s.total_served_mw;
  const cards = [
    ['Served / demand', `${fmt(s.total_served_mw)}/${fmt(s.total_demand_mw)} MW`, s.total_served_mw + 1e-6 >= s.total_demand_mw ? 'good' : 'warn'],
    ['Unserved demand', `${fmt(unserved)} MW`, unserved > 0.001 ? 'warn' : 'good'],
    ['Switching operations', `${s.switch_ops}`, ''],
    ['Max line loading', `${fmt(100 * s.max_loading)}%`, s.max_loading > 1 + 1e-9 ? 'bad' : s.max_loading >= 0.9 ? 'warn' : 'good'],
    ['Loops in network', `${s.loop_violations}`, s.loop_violations ? 'bad' : 'good'],
  ];
  $('#kpis').innerHTML = cards.map(([k, v, c]) => `<div class="kpi"><span>${k}</span><b class="${c}">${v}</b></div>`).join('');
}

/* -------------------------------------------------------------- tooltips */
function lineTip(id) {
  const l = S.lines[id];
  const r = S.result;
  let html = `<b>${id}</b> · ${nm(l.a)} — ${nm(l.b)}<br><span class="k">Resistance</span> ${l.resistance} Ω · <span class="k">Rating</span> ${l.capacity_mw} MW`;
  if (S.faults.has(id)) html += '<br><span class="no">Faulted (click to repair)</span>';
  else html += '<br><span class="k">Click to break this line</span>';
  if (r && r.lines[id]) {
    const x = r.lines[id];
    html += `<br><span class="k">Status</span> ${x.status}${x.change ? ` (${x.change})` : ''}`;
    if (x.status === 'active') html += `<br><span class="k">Flow</span> ${fmt(Math.abs(x.flow_mw), 2)} MW · ${fmt(100 * x.loading)}% of rating`;
  }
  return html;
}

function nodeTip(id) {
  const n = S.nodes[id];
  const r = S.result;
  const x = r && r.nodes[id];
  let html = `<b>${n.name}</b> <span class="k">(${id})</span><br>`;
  if (n.kind === 'load') {
    html += `${badge(n.priority)} ${PRIO_NAME[n.priority]} · ${n.category} · ${n.blocks} blocks<br>`;
    if (x) {
      html += `<span class="k">Served</span> ${fmt(x.served_mw, 2)} / ${fmt(x.demand_mw, 2)} MW (${x.blocks_served}/${x.blocks} blocks) · ${x.status}`;
      if (x.interrupted) html += `<br><span class="k">Restored</span> ${x.restored_s == null ? '–' : `${fmt(x.restored_s, 0)} s`}`;
      if (x.path.length) html += `<br><span class="k">Fed from</span> ${nm(x.root)} via ${x.path.length - 1} lines, ${fmt(x.path_resistance, 3)} Ω`;
    } else html += `<span class="k">Demand now</span> ${fmt(demandAt(n, S.hour), 2)} MW`;
  } else if (n.kind === 'battery') {
    html += `Battery ${n.battery.energy_mwh} MWh · ±${n.battery.discharge_mw} MW`;
    if (x) html += `<br><span class="k">Stored</span> ${fmt(x.soc_mwh, 2)} MWh · <span class="k">can give</span> ${fmt(x.available_mw, 2)} MW · <span class="k">giving</span> ${fmt(x.supplied_mw, 2)} MW`;
  } else if (n.kind === 'substation') {
    html += 'Substation (junction)';
  } else {
    html += `${n.kind === 'plant' ? 'Power plant' : `${n.kind} farm`} · ${n.capacity_mw} MW nameplate`;
    if (x) html += `<br><span class="k">Available now</span> ${fmt(x.available_mw, 2)} MW · <span class="k">supplying</span> ${fmt(x.supplied_mw, 2)} MW`;
  }
  return html;
}

function showTip(e, html) { const t = $('#tip'); t.innerHTML = html; t.hidden = false; moveTip(e); }
function moveTip(e) {
  const t = $('#tip');
  const x = Math.min(e.clientX + 14, window.innerWidth - t.offsetWidth - 8);
  const y = Math.min(e.clientY + 14, window.innerHeight - t.offsetHeight - 8);
  t.style.left = `${x}px`;
  t.style.top = `${y}px`;
}
function hideTip() { $('#tip').hidden = true; }

/* -------------------------------------------------------------- controls */
function setStatus(text) { $('#status').textContent = text; }

function renderChips() {
  $('#faultChips').innerHTML = [...S.faults].sort().map((id) =>
    `<button class="chip" data-id="${id}" aria-label="Repair ${id}" title="${nm(S.lines[id].a)} – ${nm(S.lines[id].b)} (click to repair)">⚡ ${id} ✕</button>`).join('');
  document.querySelectorAll('.chip').forEach((c) => c.addEventListener('click', () => toggleFault(c.dataset.id)));
  for (const [id, hit] of Object.entries(EL.hit)) hit.setAttribute('aria-pressed', String(S.faults.has(id)));
}

function resultControls(ready) {
  $('#compare').disabled = !ready;
  document.querySelectorAll('#stages button').forEach((b) => { b.disabled = !ready; });
  $('#run').disabled = S.busy;
  $('#run').textContent = S.busy ? 'Running…' : '▶ Run engine';
  $('#tour').disabled = S.busy;
  $('#tourPrevious').disabled = S.busy || S.tourIndex <= 0;
  $('#tourNext').disabled = S.busy;
}

function endTour() {
  S.tourIndex = -1;
  $('#demoTour').hidden = true;
  $('#tour').textContent = 'Guided demo';
}

async function tourStep(index) {
  if (S.busy) return;
  if (index >= TOUR.length) { endTour(); return; }
  const step = TOUR[index];
  if (!step) return;
  const preset = S.grid.presets.find((p) => p.id === step.preset);
  S.tourIndex = index;
  $('#demoTour').hidden = false;
  $('#tour').textContent = 'Restart demo';
  $('#tourProgress').textContent = `GUIDED DEMO · ${index + 1} / ${TOUR.length}`;
  $('#tourTitle').textContent = step.title;
  $('#tourText').textContent = step.text;
  $('#tourNext').textContent = index === TOUR.length - 1 ? 'Finish demo' : 'Next step';
  $('#preset').value = preset.id;
  S.faults = new Set(preset.faults);
  setHour(preset.hour);
  renderChips();
  if (await run(true) && S.tourIndex === index) await showStage(step.stage, false);
}

function markDirty(message, preserveTour = false) {
  if (!preserveTour) endTour();
  S.token++;
  S.request++;
  if (S.controller) S.controller.abort();
  S.busy = false;
  S.dirty = true;
  S.result = null;
  resultControls(false);
  $('#comparison').hidden = true;
  $('#kpis').innerHTML = '';
  $('#serviceBars').innerHTML = '';
  $('#eventTitle').textContent = 'Inputs changed';
  $('#eventSummary').textContent = `Run the engine for ${hh(S.hour)} with ${S.faults.size} faulted line(s).`;
  $('#stageBody').innerHTML = '<h2>Ready for a new decision</h2><p class="explain">Press Run engine to calculate this fault set. The previous result has been cleared.</p>';
  setStatus(message || 'Changed: press ▶ Run engine');
  applyView(viewBefore(new Set(S.faults)));
  document.querySelectorAll('#stages button').forEach((b) => b.classList.remove('active', 'done'));
}

function toggleFault(id) {
  if (S.faults.has(id)) S.faults.delete(id); else S.faults.add(id);
  $('#preset').value = 'custom';
  renderChips();
  markDirty(S.faults.size ? `${S.faults.size} line(s) broken: press ▶ Run engine` : 'No faults: press ▶ Run engine');
}

function setHour(h) {
  S.hour = Number.isFinite(h) ? Math.max(0, Math.min(23, Math.trunc(h))) : 19;
  $('#hour').value = S.hour;
  $('#hourLabel').textContent = hh(S.hour);
}

function renderOverview(r) {
  const s = r.summary;
  const preset = S.grid.presets.find((p) => p.id === $('#preset').value);
  $('#eventTitle').textContent = `${preset ? preset.name : 'Custom fault scenario'} · ${hh(s.hour)}`;
  $('#eventSummary').textContent = preset ? preset.description : `${s.faults.length} faulted line(s). Click a stage to inspect how the city is restored.`;
  $('#serviceBars').innerHTML = [1, 2, 3, 4].map((p) => {
    const pct = s.demand_mw[p] ? 100 * s.served_mw[p] / s.demand_mw[p] : 100;
    return `<div class="service-row"><span>P${p} ${PRIO_NAME[p]}</span><meter min="0" max="100" value="${pct}" aria-label="${PRIO_NAME[p]} demand served" style="--bar:var(--p${p})">${fmt(pct)}%</meter><b>${fmt(pct)}%</b></div>`;
  }).join('');
}

async function requestResult(body, signal) {
  const res = await fetch('/api/run', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body), signal });
  const result = await res.json();
  if (!res.ok || result.error) throw new Error(result.error || `Request failed (${res.status})`);
  return result;
}

async function run(preserveTour = false) {
  markDirty(undefined, preserveTour);
  const request = S.request;
  S.controller = new AbortController();
  S.busy = true;
  resultControls(false);
  setStatus('Running the engine…');
  const body = {
    hour: S.hour, faults: [...S.faults], shedding: 'mincut', exchange: true, battery: 'dp',
  };
  try {
    const r = await requestResult(body, S.controller.signal);
    if (request !== S.request) return false;
    S.result = r;
    S.dirty = false;
    S.busy = false;
    renderKPIs(r);
    renderOverview(r);
    resultControls(true);
    const s = r.summary;
    setStatus('');
    await showStage(s.faults.length ? 4 : 3, false);
    return true;
  } catch (err) {
    if (request !== S.request) return false;
    setStatus(`Could not run the engine: ${err.message}. Check the local server and retry.`);
    $('#eventTitle').textContent = 'Decision unavailable';
    return false;
  } finally {
    if (request === S.request) { S.busy = false; resultControls(Boolean(S.result) && !S.dirty); }
  }
}

async function comparePolicies() {
  if (!S.result || S.dirty || S.busy) return;
  const current = S.result;
  const request = S.request;
  $('#compare').disabled = true;
  const panel = $('#comparison');
  panel.hidden = false;
  panel.textContent = 'Comparing the same fault and battery plan across three policies…';
  const policies = [['Literal proposal', 'global', false], ['Min-cut shedding', 'mincut', false], ['Full engine', 'mincut', true]];
  try {
    const results = await Promise.all(policies.map(([, shedding, exchange]) => requestResult({
      hour: current.summary.hour, faults: current.summary.faults, battery: current.summary.policy.battery, shedding, exchange,
    }, S.controller.signal)));
    if (request !== S.request) return;
    const gain = results[2].summary.total_served_mw - results[0].summary.total_served_mw;
    panel.innerHTML = '<div class="compare-heading"><h2>Same fault. Three restoration policies.</h2><button id="closeComparison" class="ghost" aria-label="Close policy comparison">Close</button></div>'
      + `<p>${hh(current.summary.hour)} · ${current.summary.faults.length} faulted line(s) · Same battery plan. Full engine serves ${fmt(Math.abs(gain))} MW ${gain >= 0 ? 'more' : 'less'} than the literal proposal.</p>`
      + '<div class="table-scroll"><table><thead><tr><th>Policy</th><th>Served / demand (MW)</th><th>P1 served</th><th>P2 served</th><th>P3 served</th><th>P4 served</th><th>Switches</th><th>Final checks</th></tr></thead><tbody>'
      + results.map((r, i) => { const s = r.summary; const classes = [1, 2, 3, 4].map((p) => `<td>${fmt(s.demand_mw[p] ? 100 * s.served_mw[p] / s.demand_mw[p] : 100)}%</td>`).join(''); return `<tr${i === 2 ? ' class="hl"' : ''}><th>${policies[i][0]}</th><td>${fmt(s.total_served_mw)} / ${fmt(s.total_demand_mw)}</td>${classes}<td>${s.switch_ops}</td><td>${s.capacity_violations + s.loop_violations + s.multi_source_trees === 0 ? 'Pass' : 'Violation'}</td></tr>`; }).join('')
      + '</tbody></table></div><p class="comparison-note">Priority order: P1 critical, P2 essential, P3 residential, P4 commercial. Total MW may decrease when higher-priority service improves; discrete blocks also affect totals. Final checks cover line ratings, no loops and one grid-forming root per tree.</p>';
    $('#closeComparison').addEventListener('click', () => { panel.hidden = true; $('#compare').focus(); });
  } catch (err) {
    if (request === S.request) panel.textContent = `Comparison unavailable: ${err.message}. Retry after checking the local server.`;
  } finally {
    if (request === S.request) $('#compare').disabled = false;
  }
}

async function init() {
  S.grid = await (await fetch('/api/grid')).json();
  for (const n of S.grid.nodes) S.nodes[n.id] = n;
  for (const l of S.grid.lines) S.lines[l.id] = l;
  S.normal = new Set(S.grid.normal_lines);
  const sel = $('#preset');
  sel.innerHTML = '<option value="custom">Custom (click lines)</option>'
    + S.grid.presets.map((p) => `<option value="${p.id}">${p.name} · ${hh(p.hour)}</option>`).join('');
  buildMap();

  sel.addEventListener('change', () => {
    const p = S.grid.presets.find((x) => x.id === sel.value);
    if (!p) return;
    S.faults = new Set(p.faults);
    setHour(p.hour);
    renderChips();
    setStatus(p.description);
    run(false);
  });
  $('#hour').addEventListener('input', (e) => {
    setHour(Number(e.target.value));
    $('#preset').value = 'custom';
    if (S.faults.size) markDirty(`Hour ${hh(S.hour)}: press ▶ Run engine`);
    else run(false);
  });
  $('#run').addEventListener('click', () => run(false));
  $('#tour').addEventListener('click', () => tourStep(0));
  $('#tourPrevious').addEventListener('click', () => tourStep(S.tourIndex - 1));
  $('#tourNext').addEventListener('click', () => tourStep(S.tourIndex + 1));
  $('#tourClose').addEventListener('click', endTour);
  $('#compare').addEventListener('click', comparePolicies);
  document.addEventListener('keydown', (e) => {   // Esc skips any running animation to its end state
    if (e.key === 'Escape' && S.result) showStage(S.stage || 4, false);
  });
  document.querySelectorAll('#stages button').forEach((b) => b.addEventListener('click', () => showStage(Number(b.dataset.stage), true)));

  // deep links, e.g. ?preset=west-plant-trip&stage=4  or  ?hour=10&faults=L10&stage=3
  const q = new URLSearchParams(window.location.search);
  if (q.get('report') === '1') document.body.classList.add('report-capture');
  const preset = S.grid.presets.find((p) => p.id === (q.get('preset') || 'normal-peak'));
  S.faults = new Set(q.has('faults') ? q.get('faults').split(',').filter((x) => S.lines[x]) : preset ? preset.faults : []);
  setHour(q.has('hour') ? Number(q.get('hour')) : preset ? preset.hour : 19);
  sel.value = q.has('faults') || q.has('hour') ? 'custom' : preset ? preset.id : 'custom';
  renderChips();
  if (q.has('delay')) await sleep(Number(q.get('delay')));   // lets a headless capture settle before timing
  if (q.has('stage')) {
    await run(false);
    await showStage(Number(q.get('stage')), false);
  } else {
    await run(false);
  }
}

init().catch((err) => setStatus(`Could not reach the engine: ${err}`));
