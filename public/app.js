'use strict';

// ============================================================ helpers
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const pct = (x, d = 1) => `${((x || 0) * 100).toFixed(d)}%`;
const num = (x, d = 0) => (x == null || Number.isNaN(+x) ? '–' : (+x).toFixed(d));
const signed = (x, d = 0) => (x > 0 ? '+' : '') + num(x, d);

const ALL_MAPS = ['ancient', 'anubis', 'cache', 'dust2', 'inferno', 'mirage', 'nuke', 'overpass', 'train', 'vertigo'];
const mapKey = (m) => { const k = String(m).toLowerCase().replace(/^de_/, '').replace(/[^a-z0-9]/g, ''); return k === 'dustii' ? 'dust2' : k; };
const DEFAULT_POOL = ['ancient', 'anubis', 'dust2', 'inferno', 'mirage', 'nuke', 'train'];
const BUSY = ['iniciando', 'coletando', 'curando', 'treinando'];

const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v ? JSON.parse(v) : d; } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* sem storage */ } },
};

async function api(path, opts = {}) {
  const headers = { ...(opts.headers || {}) };
  const token = store.get('pp-token', '');
  if (token) headers['X-Admin-Token'] = token;
  let body = opts.body;
  if (opts.json !== undefined) { headers['Content-Type'] = 'application/json'; body = JSON.stringify(opts.json); }
  const res = await fetch(path, { method: opts.method || (body !== undefined ? 'POST' : 'GET'), headers, body });
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch { data = { error: text.slice(0, 200) }; }
  if (!res.ok) throw new Error((data && data.error) || `HTTP ${res.status}`);
  return data;
}

function showErr(sel, msg) { const el = $(sel); el.textContent = msg; el.hidden = !msg; }

// ============================================================ estado do formulário
const state = {
  mode: 'evento',        // 'evento' (Major real) | 'manual'
  eventName: '',
  stageIndex: null,
  poolEvent: '',         // evento cujo map pool já foi aplicado
  teamAdj: {},           // nome -> { adjust, lineup } (modo evento)
  format: 'swiss',
  rows: [],
  pool: [...DEFAULT_POOL],
  force: [],
  mapadj: [],
  boRule: 'valve',
  finalBo: '3',
  sims: '50000',
  adv30: false,
};

function loadForm() {
  const f = store.get('pp-form', null);
  if (f && typeof f === 'object') Object.assign(state, f);
  while (state.rows.length < 16) state.rows.push({ name: '', adjust: '', lineup: '' });
  state.rows = state.rows.slice(0, 16);
  if (!Array.isArray(state.pool) || state.pool.length === 0) state.pool = [...DEFAULT_POOL];
  $('#boRule').value = state.boRule;
  $('#finalBo').value = state.finalBo;
  $('#sims').value = state.sims;
  $('#adv30').checked = !!state.adv30;
  $$('#formatSeg button').forEach((b) => b.classList.toggle('on', b.dataset.format === state.format));
}

function saveForm() {
  state.boRule = $('#boRule').value;
  state.finalBo = $('#finalBo').value;
  state.sims = $('#sims').value;
  state.adv30 = $('#adv30').checked;
  store.set('pp-form', state);
}

const nTeams = () => (state.format === 'swiss' ? 16 : 8);
function activeNames() {
  if (state.mode === 'evento') {
    const ev = currentEvent();
    if (!ev) return [];
    const set = new Set();
    ev.stages.forEach((s) => s.teams.forEach((t) => t.name && set.add(t.name)));
    return [...set].sort((a, b) => a.localeCompare(b));
  }
  return state.rows.slice(0, nTeams()).map((r) => r.name.trim()).filter(Boolean);
}

// ============================================================ eventos (Majors)
let events = [];
const currentEvent = () => events.find((e) => e.name === state.eventName) || events[0] || null;
const ROUND_BUCKETS = [[[0, 0]], [[1, 0], [0, 1]], [[2, 0], [1, 1], [0, 2]], [[2, 1], [1, 2]], [[2, 2]]];
const statusCls = (s) => (s === 'terminado' ? 'done' : s === 'em andamento' ? 'live' : '');

function defaultStage(ev) {
  let i = ev.stages.findIndex((s) => s.status === 'em andamento');
  if (i < 0) i = ev.stages.findIndex((s) => s.status !== 'terminado');
  return i < 0 ? ev.stages.length - 1 : i;
}

async function loadEvents() {
  try { events = await api('/api/events'); } catch { events = []; }
  renderEvents();
}

function applyMode() {
  const ev = state.mode === 'evento';
  $$('#modeSeg button').forEach((b) => b.classList.toggle('on', b.dataset.mode === state.mode));
  $('#eventPane').hidden = !ev;
  $('#manualCard').hidden = ev;
  $('#eventTeamsCard').hidden = !ev;
  renderScenario();
}
$$('#modeSeg button').forEach((b) => b.addEventListener('click', () => {
  state.mode = b.dataset.mode;
  saveForm();
  applyMode();
  if (state.mode === 'evento') renderEvents();
}));
$('#eventSelect').addEventListener('change', (e) => {
  state.eventName = e.target.value;
  state.stageIndex = null;
  saveForm();
  renderEvents();
});

function renderEvents() {
  const sel = $('#eventSelect');
  const ev = currentEvent();
  if (!ev) {
    sel.innerHTML = '<option>Nenhuma Major carregada</option>';
    sel.disabled = true;
    $('#eventHint').innerHTML = 'Envie a página de qualquer estágio da Major (<code>hltv.org/events/…</code>) pela extensão ou em <b>Dados → Importar manualmente</b>. Uma página traz todos os estágios.';
    $('#stageCards').innerHTML = '';
    $('#stageBoard').innerHTML = '';
    $('#eventTeamRows').innerHTML = '';
    return;
  }
  sel.disabled = false;
  state.eventName = ev.name;
  sel.innerHTML = events.map((e) => `<option value="${esc(e.name)}"${e.name === ev.name ? ' selected' : ''}>${esc(e.name)}</option>`).join('');
  $('#eventHint').textContent = `${ev.stages.length} estágios · map pool: ${ev.pool.join(', ') || '–'} · atualizado ${ev.updatedAt || '–'}`;
  if (state.poolEvent !== ev.name && ev.pool.length) {
    state.pool = ev.pool.map(mapKey);
    state.poolEvent = ev.name;
    renderPool();
  }
  if (state.stageIndex == null || state.stageIndex >= ev.stages.length) state.stageIndex = defaultStage(ev);
  $('#stageCards').innerHTML = ev.stages.map((s, i) => `
    <button class="stage-card${i === state.stageIndex ? ' on' : ''}" data-i="${i}">
      <span class="n">${esc(s.name)}</span>
      <span class="s">${s.type === 'playoffs' ? 'Playoffs · 8 times' : 'Swiss · 16 times'} · ${s.conhecidos}/${s.size} definidos</span>
      <span class="s"><span class="badge ${statusCls(s.status)}">${esc(s.status)}</span>${s.jogados ? ` ${s.jogados} jogos` : ''}${s.pendentes ? ` · ${s.pendentes} marcados` : ''}</span>
    </button>`).join('');
  $$('.stage-card').forEach((b) => b.addEventListener('click', () => {
    state.stageIndex = Number(b.dataset.i);
    saveForm();
    renderEvents();
  }));
  renderStageBoard(ev, state.stageIndex);
  saveForm();
}

function stageRecords(st) {
  const rec = {};
  st.teams.forEach((t) => { if (t.name) rec[t.name] = [0, 0]; });
  st.matches.forEach((m) => {
    if (!m.winner) return;
    const los = m.winner === m.a ? m.b : m.a;
    (rec[m.winner] = rec[m.winner] || [0, 0])[0]++;
    (rec[los] = rec[los] || [0, 0])[1]++;
  });
  return rec;
}

function matchHtml(m) {
  const cls = (n) => (!m.winner ? '' : m.winner === n ? 'w' : 'l');
  const score = m.winner ? `${m.scoreA}-${m.scoreB}` : (m.ts ? new Date(m.ts).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' }) : 'a definir');
  return `<div class="bm${m.winner ? '' : ' pending'}"><span><span class="${cls(m.a)}">${esc(m.a || 'TBD')}</span> × <span class="${cls(m.b)}">${esc(m.b || 'TBD')}</span></span><span class="sc">${esc(score)}</span></div>`;
}

function renderStageBoard(ev, idx) {
  const st = ev.stages[idx];
  const prev = idx > 0 ? ev.stages[idx - 1] : null;
  const missing = st.size - st.conhecidos;
  const note = missing > 0
    ? `<p class="board-note">${missing} vaga(s) ainda sem time: a previsão simula ${prev ? `o <b>${esc(prev.name)}</b>${idx > 1 && ev.stages[idx - 1].conhecidos < ev.stages[idx - 1].size ? ' (e os anteriores)' : ''}` : 'os estágios anteriores'} para preencher com os classificados mais prováveis.</p>`
    : '';
  let board = '';
  if (st.type === 'playoffs') {
    const qf = st.matches.filter((m) => m.round === 1);
    const known = qf.length === 4 && qf.every((m) => m.a && m.b);
    board = known
      ? `<h3>Chave</h3><div class="qf-list">${st.matches.filter((m) => m.a || m.b).map(matchHtml).join('')}</div>`
      : `<h3>Chave por seed</h3><div class="qf-list">${(st.qfSeeds.length ? st.qfSeeds : [[1, 8], [4, 5], [2, 7], [3, 6]]).map((q) => `<div class="bm pending"><span>Seed ${q[0]} × Seed ${q[1]}</span><span class="sc">quartas</span></div>`).join('')}</div>`;
  } else {
    board = `<div class="swiss-board">${ROUND_BUCKETS.map((buckets, r) => `
      <div class="sround"><h4>Rodada ${r + 1}</h4>${buckets.map(([w, l]) => {
        const ms = st.matches.filter((m) => m.w === w && m.l === l);
        return ms.length ? `<div class="bucket"><span class="bucket-t">${w}-${l}</span>${ms.map(matchHtml).join('')}</div>` : '';
      }).join('') || '<span class="hint">—</span>'}</div>`).join('')}</div>`;
  }
  $('#stageBoard').innerHTML = board + note;
  renderEventTeams(st);
}

function renderEventTeams(st) {
  const rec = stageRecords(st);
  const teams = [...st.teams].sort((a, b) => (a.seed || 99) - (b.seed || 99));
  const box = $('#eventTeamRows');
  box.innerHTML = '';
  teams.forEach((t) => {
    if (!t.name) return;
    const adj = state.teamAdj[t.name] || (state.teamAdj[t.name] = { adjust: '', lineup: '' });
    const r = rec[t.name] || [0, 0];
    const div = document.createElement('div');
    div.className = 'ev-row';
    div.innerHTML = `
      <span class="seed">${t.seed || '–'}</span>
      <span class="nm" title="${esc(t.name)}">${esc(t.name)}</span>
      <span class="rec">${st.type === 'playoffs' ? '' : `${r[0]}-${r[1]}`}</span>
      <input class="adj" type="number" step="10" placeholder="±Elo" value="${esc(adj.adjust)}" aria-label="Ajuste de Elo ${esc(t.name)}">
      <input class="lineup" placeholder="Lineup (opcional): nick1, nick2, …" value="${esc(adj.lineup)}" aria-label="Lineup ${esc(t.name)}">`;
    $('.adj', div).addEventListener('input', (e) => { adj.adjust = e.target.value; saveForm(); });
    $('.lineup', div).addEventListener('input', (e) => { adj.lineup = e.target.value; saveForm(); });
    box.appendChild(div);
  });
  for (let k = st.conhecidos; k < st.size; k++) {
    const div = document.createElement('div');
    div.className = 'ev-row tbd';
    div.innerHTML = '<span class="seed">–</span><span class="nm">TBD (vem do estágio anterior)</span>';
    box.appendChild(div);
  }
}

// ============================================================ abas
function switchTab(t) {
  $$('.tab').forEach((b) => b.classList.toggle('active', b.dataset.tab === t));
  $$('.panel').forEach((p) => p.classList.toggle('active', p.id === `tab-${t}`));
  store.set('pp-tab', t);
  if (t === 'teams') loadTeams();
  if (t === 'data') refreshStatus();
}
$$('.tab').forEach((b) => b.addEventListener('click', () => switchTab(b.dataset.tab)));

// ============================================================ times / seeds
function renderRows() {
  const n = nTeams();
  const box = $('#teamRows');
  box.innerHTML = '';
  for (let i = 0; i < n; i++) {
    const r = state.rows[i];
    const div = document.createElement('div');
    div.className = `team-row${state.format === 'swiss' && i === 8 ? ' divider' : ''}`;
    div.innerHTML = `
      <span class="seed">${i + 1}</span>
      <input class="name" list="teamList" placeholder="Time" value="${esc(r.name)}" aria-label="Time seed ${i + 1}">
      <input class="adj" type="number" step="10" placeholder="±Elo" value="${esc(r.adjust)}" title="Ajuste de força (cenário). Ex.: -50 para stand-in" aria-label="Ajuste de Elo">
      <input class="lineup" placeholder="Lineup (opcional): nick1, nick2, …" value="${esc(r.lineup)}" title="Substitui a lineup do ranking HLTV (stand-ins, trocas recentes)" aria-label="Lineup">
      <span class="moves"><button data-mv="-1" title="Subir seed">▲</button><button data-mv="1" title="Descer seed">▼</button></span>`;
    $('.name', div).addEventListener('input', (e) => { r.name = e.target.value; saveForm(); });
    $('.name', div).addEventListener('change', () => renderScenario());
    $('.adj', div).addEventListener('input', (e) => { r.adjust = e.target.value; saveForm(); });
    $('.lineup', div).addEventListener('input', (e) => { r.lineup = e.target.value; saveForm(); });
    $$('.moves button', div).forEach((b) => b.addEventListener('click', () => {
      const j = i + Number(b.dataset.mv);
      if (j < 0 || j >= n) return;
      [state.rows[i], state.rows[j]] = [state.rows[j], state.rows[i]];
      saveForm();
      renderRows();
    }));
    box.appendChild(div);
  }
  $('#seedHint').textContent = state.format === 'swiss'
    ? 'Ordem = seed (▲▼ para reordenar). Rodada 1: 1×9, 2×10, … A Valve usa o próprio ranking; o botão abaixo usa o da HLTV como aproximação.'
    : 'Ordem = seed da chave: 1×8 e 4×5 de um lado, 2×7 e 3×6 do outro.';
  renderScenario();
}

$$('#formatSeg button').forEach((b) => b.addEventListener('click', () => {
  state.format = b.dataset.format;
  $$('#formatSeg button').forEach((x) => x.classList.toggle('on', x === b));
  saveForm();
  renderRows();
}));

$('#fillRanking').addEventListener('click', async () => {
  showErr('#predictError', '');
  try {
    const teams = await api(`/api/teams?limit=${nTeams()}`);
    if (!teams.length) throw new Error('Ainda não há ranking. Veja a aba Dados.');
    teams.slice(0, nTeams()).forEach((t, i) => { state.rows[i] = { name: t.name, adjust: '', lineup: '' }; });
    saveForm();
    renderRows();
  } catch (e) { showErr('#predictError', e.message); }
});

$('#clearTeams').addEventListener('click', () => {
  state.rows = Array.from({ length: 16 }, () => ({ name: '', adjust: '', lineup: '' }));
  state.force = [];
  state.mapadj = [];
  saveForm();
  renderRows();
});

['#boRule', '#finalBo', '#sims', '#adv30'].forEach((s) => $(s).addEventListener('change', saveForm));

// ============================================================ map pool
function renderPool() {
  const box = $('#poolChips');
  const maps = [...new Set([...ALL_MAPS, ...state.pool])];
  box.innerHTML = maps.map((m) => {
    const on = state.pool.includes(m);
    return `<label class="${on ? 'on' : ''}"><input type="checkbox" value="${m}" ${on ? 'checked' : ''}>${m}</label>`;
  }).join('');
  $$('input', box).forEach((inp) => inp.addEventListener('change', () => {
    state.pool = $$('input:checked', box).map((x) => x.value);
    saveForm();
    renderPool();
    renderScenario();
  }));
}

// ============================================================ cenários
const options = (list, sel, ph) => `<option value="">${esc(ph)}</option>${list.map((v) => `<option value="${esc(v)}"${v === sel ? ' selected' : ''}>${esc(v)}</option>`).join('')}`;

function renderScenario() {
  const names = activeNames();
  const fbox = $('#forceRows');
  fbox.innerHTML = '';
  state.force.forEach((f, i) => {
    const d = document.createElement('div');
    d.className = 'mini-row';
    d.innerHTML = `
      <select data-k="a" aria-label="Time A">${options(names, f.a, 'Time A')}</select>
      <select data-k="b" aria-label="Time B">${options(names, f.b, 'Time B')}</select>
      <select data-k="winner" aria-label="Vencedor">${options([f.a, f.b].filter(Boolean), f.winner, 'Vencedor')}</select>
      <button class="x" title="Remover">×</button>`;
    $$('select', d).forEach((s) => s.addEventListener('change', () => {
      f[s.dataset.k] = s.value;
      if (f.winner && f.winner !== f.a && f.winner !== f.b) f.winner = '';
      saveForm();
      renderScenario();
    }));
    $('.x', d).addEventListener('click', () => { state.force.splice(i, 1); saveForm(); renderScenario(); });
    fbox.appendChild(d);
  });

  const mbox = $('#mapadjRows');
  mbox.innerHTML = '';
  state.mapadj.forEach((m, i) => {
    const d = document.createElement('div');
    d.className = 'mini-row';
    d.innerHTML = `
      <select data-k="team" aria-label="Time">${options(names, m.team, 'Time')}</select>
      <select data-k="map" aria-label="Mapa">${options(state.pool, m.map, 'Mapa')}</select>
      <input data-k="delta" type="number" step="10" placeholder="±Elo" value="${esc(m.delta)}" aria-label="Ajuste">
      <button class="x" title="Remover">×</button>`;
    $$('select', d).forEach((s) => s.addEventListener('change', () => { m[s.dataset.k] = s.value; saveForm(); }));
    $('input', d).addEventListener('input', (e) => { m.delta = e.target.value; saveForm(); });
    $('.x', d).addEventListener('click', () => { state.mapadj.splice(i, 1); saveForm(); renderScenario(); });
    mbox.appendChild(d);
  });
}
$('#addForce').addEventListener('click', () => { state.force.push({ a: '', b: '', winner: '' }); renderScenario(); });
$('#addMapadj').addEventListener('click', () => { state.mapadj.push({ team: '', map: '', delta: '' }); renderScenario(); });

// ============================================================ previsão
$('#runPredict').addEventListener('click', runPredict);

async function runPredict() {
  showErr('#predictError', '');
  // linhas de ajuste/lineup: do estágio do evento ou da lista manual
  let adjRows;
  const body = {
    boRule: $('#boRule').value,
    finalBo: Number($('#finalBo').value),
    advIncludes30: $('#adv30').checked,
    sims: Number($('#sims').value),
    pool: state.pool,
  };
  if (state.mode === 'evento') {
    const ev = currentEvent();
    if (!ev) { showErr('#predictError', 'Nenhuma Major carregada. Use o modo Manual ou importe a página do evento.'); return; }
    body.event = ev.name;
    body.stage = state.stageIndex;
    adjRows = Object.entries(state.teamAdj).map(([name, v]) => ({ name, adjust: v.adjust, lineup: v.lineup }));
  } else {
    const rows = state.rows.slice(0, nTeams());
    if (rows.some((r) => !r.name.trim())) { showErr('#predictError', `Preencha os ${nTeams()} times.`); return; }
    body.format = state.format;
    body.teams = rows.map((r, i) => ({ name: r.name.trim(), seed: i + 1 }));
    adjRows = rows.map((r) => ({ name: r.name.trim(), adjust: r.adjust, lineup: r.lineup }));
  }
  const lineups = {};
  adjRows.forEach((r) => {
    const l = String(r.lineup || '').split(',').map((s) => s.trim()).filter(Boolean);
    if (l.length) lineups[r.name] = l;
  });
  body.scenario = {
    adjust: adjRows.filter((r) => Number(r.adjust)).map((r) => ({ team: r.name, delta: Number(r.adjust) })),
    lineups,
    force: state.force.filter((f) => f.a && f.b && f.winner),
    mapadj: state.mapadj.filter((m) => m.team && m.map && Number(m.delta)).map((m) => ({ team: m.team, map: m.map, delta: Number(m.delta) })),
  };
  const btn = $('#runPredict');
  btn.disabled = true;
  btn.textContent = 'Simulando…';
  try {
    const r = await api('/api/predict', { json: body });
    renderResults(r);
    $('#results').scrollIntoView({ behavior: 'smooth', block: 'start' });
  } catch (e) {
    showErr('#predictError', e.message);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Calcular previsão';
  }
}

const heat = (p) => `style="background: rgba(var(--heat), ${(Math.min(1, p) * 0.75).toFixed(3)})"`;
const barCell = (p) => `<td class="bar"><i style="width:${(p * 100).toFixed(1)}%"></i><span>${pct(p)}</span></td>`;

function warningsHtml(list) {
  if (!list || !list.length) return '';
  return `<h3>Avisos</h3><ul class="warn-list">${list.map((w) => `<li>${esc(w)}</li>`).join('')}</ul>`;
}

function detailHtml(t) {
  const players = (t.jogadores || []).map((p) => `
    <tr><td>${esc(p.name)}</td><td>${num(p.rating, 2)}</td><td>${num(p.kd, 2)}</td>
    <td>${num(p.partidasHistorico, 0)}</td><td>${p.mapasRecentes || '–'}</td><td>${signed(p.indice * 100, 1)}</td></tr>`).join('');
  const maps = Object.entries(t.mapas || {}).sort((a, b) => b[1] - a[1]).map(([m, o]) => {
    const w = Math.min(50, (Math.abs(o) / 150) * 50);
    return `<div class="mapbar"><span>${esc(m)}</span><span class="track"><span class="fill ${o >= 0 ? 'pos' : 'neg'}" style="width:${w}%"></span></span><span class="n">${signed(o)}</span></div>`;
  }).join('');
  return `<div class="detail-grid">
    <div><h3>Jogadores · lineup: ${esc(t.lineupFonte)}</h3>
      ${players ? `<table class="players"><tr><th>Nick</th><th>Rating</th><th>K/D</th><th>Hist.</th><th>90d</th><th>Índice</th></tr>${players}</table>` : '<p class="hint">Sem lineup conhecida.</p>'}
    </div>
    <div><h3>Composição da força</h3><dl class="kv">
      <dt>Elo (histórico)</dt><dd>${num(t.elo)}</dd>
      <dt>Prior ranking HLTV</dt><dd>${t.prior == null ? '–' : `${num(t.prior)} (${num(t.pontosHLTV)} pts, peso ${pct(t.pesoPrior, 0)})`}</dd>
      <dt>Bônus jogadores</dt><dd>${signed(t.bonusJogadores)}</dd>
      <dt>Ajuste do cenário</dt><dd>${signed(t.ajusteCenario)}</dd>
      <dt>Força final</dt><dd><b>${num(t.rating)}</b></dd>
      <dt>Incerteza (σ)</dt><dd>±${num(t.incerteza)}</dd>
      <dt>Mapas no histórico</dt><dd>${num(t.mapasNoHistorico)}</dd>
    </dl></div>
    <div><h3>Força relativa por mapa</h3><div class="mapbars">${maps}</div></div>
  </div>`;
}

function attachDetails(table, teams, cols) {
  $$('tr.main-row', table).forEach((tr) => tr.addEventListener('click', () => {
    const next = tr.nextElementSibling;
    if (next && next.classList.contains('detail')) { next.remove(); return; }
    const t = teams[Number(tr.dataset.i)];
    const d = document.createElement('tr');
    d.className = 'detail';
    d.innerHTML = `<td colspan="${cols}">${detailHtml(t)}</td>`;
    tr.after(d);
  }));
}

function renderResults(r) {
  const box = $('#results');
  const teams = r.teams;
  const title = r.modo === 'evento' ? `${esc(r.evento)} · ${esc(r.estagio)}` : 'Personalizado';
  const sub = `${Number(r.sims).toLocaleString('pt-BR')} simulações · pool: ${r.pool.join(', ')}`;
  const chainNote = r.cadeia && r.cadeia.length > 1
    ? `<p class="hint">Vagas ainda sem time foram preenchidas simulando ${r.cadeia.slice(0, -1).map(esc).join(' → ')} a cada rodada de simulação (resultados já jogados são respeitados). A coluna <b>Chega</b> mostra a chance de o time estar neste estágio.</p>`
    : '';
  const chained = teams.some((t) => t.participa < 0.9995);
  const seedCell = (t) => `<td class="num">${t.seed ?? '–'}</td>`;

  if (r.format === 'playoffs') {
    const order = teams.map((t, i) => i).sort((a, b) => teams[b].campeao - teams[a].campeao);
    const stages = ['Quartas', 'Semi', 'Final'];
    const occ = !r.chaveDefinida && r.ocupantes
      ? `<p class="hint">Chave ainda não definida: usando o ocupante mais provável de cada seed — ${r.ocupantes.map((o) => `${o.pos}: ${esc(o.team)} (${pct(o.prob, 0)})`).join(' · ')}</p>`
      : '';
    box.innerHTML = `
      <div class="card"><div class="card-head"><h2>${title}</h2><span class="hint">${esc(sub)}</span></div>
        ${chainNote}
        <div class="table-wrap"><table class="tbl" id="resTable">
          <tr><th class="num">Seed</th><th>Time</th><th class="num">Força</th>${chained ? '<th>Chega</th>' : ''}<th>Semi</th><th>Final</th><th>Campeão</th></tr>
          ${order.map((i) => { const t = teams[i]; return `<tr class="main-row" data-i="${i}">${seedCell(t)}<td class="team-name">${esc(t.name)}</td><td class="num">${num(t.rating)}</td>${chained ? barCell(t.quartas) : ''}${barCell(t.semi)}${barCell(t.final)}${barCell(t.campeao)}</tr>`; }).join('')}
        </table></div>
        ${warningsHtml(r.avisos)}
      </div>
      <div class="card"><h2>Chave sugerida</h2>${occ}<div class="bracket">
        ${stages.map((s) => `<div><h3>${s}</h3>${r.chave.filter((m) => m.fase === s).map((m) => `
          <div class="match"><span class="p">${pct(m.prob, 0)}</span>
            <div class="${m.vencedor === m.a ? 'w' : ''}">${esc(m.a)}</div>
            <div class="${m.vencedor === m.b ? 'w' : ''}">${esc(m.b)}</div></div>`).join('')}</div>`).join('')}
      </div></div>`;
    attachDetails($('#resTable'), teams, chained ? 7 : 6);
    return;
  }

  const pickCol = (ttl, cls, list) => `<div class="pick-col ${cls}"><h3>${ttl}</h3><ul>${list.map((p) => `<li class="pick"><span>${esc(p.team)}</span><span class="p">${pct(p.prob)}</span></li>`).join('')}</ul></div>`;
  const order = teams.map((t, i) => i).sort((a, b) => teams[b].avanca - teams[a].avanca || teams[b].participa - teams[a].participa);
  const cats = ['3-0', '3-1', '3-2', '2-3', '1-3', '0-3'];
  box.innerHTML = `
    <div class="card"><div class="card-head"><h2>Pick'Em · ${title}</h2><span class="hint">${esc(sub)}</span></div>
      <div class="pickem">
        ${pickCol('3-0', '', r.picks['3-0'])}
        ${pickCol('0-3', 'zero', r.picks['0-3'])}
        ${pickCol('Avançam (3-1 / 3-2)', 'adv', r.picks.avanca)}
        <div class="big-stat">
          <div class="v">${pct(r.p5, 0)}</div><div class="l">chance de ≥ 5 acertos</div>
          <div class="v2">${num(r.acertosEsperados, 2)} / 10</div><div class="l">acertos esperados</div>
        </div>
      </div>
      <p class="hint">Os picks maximizam a chance de bater 5 acertos considerando a correlação entre resultados, e não apenas a probabilidade individual de cada time.</p>
      ${chainNote}
      ${warningsHtml(r.avisos)}
    </div>
    <div class="card"><h2>Probabilidades por time</h2>
      <p class="hint">Clique num time para ver lineup, K/D e rating dos jogadores, composição da força e força por mapa.</p>
      <div class="table-wrap"><table class="tbl" id="resTable">
        <tr><th class="num">Seed</th><th>Time</th><th class="num">Força</th>${chained ? '<th>Chega</th>' : ''}${cats.map((c) => `<th class="num">${c}</th>`).join('')}<th>Avança</th></tr>
        ${order.map((i) => { const t = teams[i]; return `<tr class="main-row" data-i="${i}">
          ${seedCell(t)}<td class="team-name">${esc(t.name)}</td><td class="num">${num(t.rating)}</td>${chained ? barCell(t.participa) : ''}
          ${cats.map((c) => `<td class="heat" ${heat(t.dist[c])}>${pct(t.dist[c])}</td>`).join('')}
          ${barCell(t.avanca)}</tr>`; }).join('')}
      </table></div>
    </div>`;
  attachDetails($('#resTable'), teams, chained ? 11 : 10);
}

// ============================================================ aba Times
async function loadTeams() {
  const table = $('#teamsTable');
  try {
    const teams = await api('/api/teams?limit=60');
    if (!teams.length) { table.innerHTML = '<tr><td class="hint">Sem dados ainda. Veja a aba Dados.</td></tr>'; return; }
    table.innerHTML = `<tr><th class="num">#</th><th>Time</th><th class="num">Pontos</th><th class="num">Elo</th><th class="num">Mapas</th><th class="num">Índ. jog.</th><th>Lineup (rating · K/D)</th><th>Melhor mapa</th><th>Pior mapa</th></tr>
      ${teams.map((t) => {
        const best = t.mapPool[0];
        const worst = t.mapPool[t.mapPool.length - 1];
        return `<tr><td class="num">${t.rank || '–'}</td><td class="team-name">${esc(t.name)}</td><td class="num">${t.pontos ? num(t.pontos) : '–'}</td>
          <td class="num">${num(t.elo)}</td><td class="num">${num(t.mapas)}</td><td class="num">${signed(t.indiceJogadores * 100, 1)}</td>
          <td class="lineup-chips">${t.jogadores.map((p) => `<span class="tag"><b>${esc(p.name)}</b> ${num(p.rating, 2)} · ${num(p.kd, 2)}</span>`).join('')}</td>
          <td>${best ? `${esc(best.map)} <span class="hint">${signed(best.offset)}</span>` : '–'}</td>
          <td>${worst && t.mapPool.length > 1 ? `${esc(worst.map)} <span class="hint">${signed(worst.offset)}</span>` : '–'}</td></tr>`;
      }).join('')}`;
  } catch (e) {
    table.innerHTML = `<tr><td class="error">${esc(e.message)}</td></tr>`;
  }
}
$('#reloadTeams').addEventListener('click', loadTeams);

async function loadKnownTeams() {
  try {
    const teams = await api('/api/teams?limit=200');
    $('#teamList').innerHTML = teams.map((t) => `<option value="${esc(t.name)}">`).join('');
  } catch { /* ignorado */ }
}

// ============================================================ aba Dados
let lastPhase = null;
let lastStatus = null;

function metric(label, value, sub = '') {
  return `<div class="metric"><div class="l">${esc(label)}</div><div class="v">${value}</div>${sub ? `<div class="s">${sub}</div>` : ''}</div>`;
}

function renderTrain(t) {
  const box = $('#trainBox');
  if (!t || !t.depois) { box.innerHTML = 'Ainda não treinado.'; return; }
  const p = t.params || {};
  box.innerHTML = `
    <p>Validação walk-forward nos ${t.partidasValidacao} jogos mais recentes (o treino usou os ${t.partidasTreino} anteriores). Treinado em ${esc(t.quando || '')}.</p>
    <div class="metric-grid">
      ${metric('Log-loss', num(t.depois.logloss, 4), `antes ${num(t.antes.logloss, 4)} · 50/50 = 0.6931`)}
      ${metric('Acerto por mapa', pct(t.depois.acc), `${t.depois.count} mapas avaliados`)}
      ${metric('Sem stats de jogador', num(t.semJogadores.logloss, 4), 'log-loss só com Elo')}
    </div>
    <dl class="kv">
      <dt>K geral / K mapa</dt><dd>${num(p.k, 1)} / ${num(p.kmap, 1)}</dd>
      <dt>Margem de rounds</dt><dd>${num(p.margin, 2)}</dd>
      <dt>Memória (tau)</dt><dd>${num(p.tau)} dias</dd>
      <dt>Troca de roster</dt><dd>${pct(p.roster)} por jogador</dd>
      <dt>Peso dos jogadores (β)</dt><dd>${num(p.beta)} Elo</dd>
      <dt>Mistura rating × K/D</dt><dd>${pct(1 - p.kdMix, 0)} rating · ${pct(p.kdMix, 0)} K/D</dd>
      <dt>Velocidade da forma</dt><dd>${num(p.playerAlpha, 2)}</dd>
    </dl>`;
}

function renderReport(r) {
  const box = $('#reportBox');
  if (!r || !r.partidas) { box.innerHTML = 'Sem relatório.'; return; }
  const list = (obj) => Object.entries(obj || {}).map(([k, v]) => `<span class="tag">${esc(k.replace(/_/g, ' '))}: <b>${v}</b></span>`).join('') || '<span class="hint">nenhum</span>';
  box.innerHTML = `
    <div class="metric-grid">
      ${metric('Partidas mantidas', r.partidas.mantidas, `${r.partidas.brutas} brutas · ${r.partidas.duplicadas} duplicadas`)}
      ${metric('De Major', r.partidas.deMajor, `${r.partidas.comStatsDeJogador} com stats de jogador`)}
      ${metric('Mapas', r.mapas.mantidos, `${r.times} times`)}
    </div>
    <h3>Partidas descartadas</h3><div>${list(r.partidas.descartadas)}</div>
    <h3>Mapas descartados</h3><div>${list(r.mapas.descartados)}</div>
    <h3>Jogadores</h3><div>${list(r.jogadores)}</div>
    <h3>Páginas lidas</h3><div>${list(r.arquivos.porTipo)}</div>
    <h3>Majors (${(r.majors || []).length})</h3><div>${(r.majors || []).map((m) => `<span class="tag">${esc(m.nome)} <b>${esc(m.data)}</b></span>`).join('') || '<span class="hint">nenhuma</span>'}</div>
    <h3>Nomes unificados</h3><div>${(r.aliasesUnificados || []).slice(0, 30).map((a) => `<span class="tag"><b>${esc(a.nome)}</b> ← ${a.variantes.map(esc).join(', ')}</span>`).join('') || '<span class="hint">nenhum</span>'}</div>
    ${r.periodo && r.periodo.inicio ? `<p class="hint">Período: ${esc(r.periodo.inicio)} → ${esc(r.periodo.fim)}</p>` : ''}
    ${warningsHtml(r.avisos)}`;
}

function renderStatus(s) {
  lastStatus = s;
  const chip = $('#statusChip');
  chip.textContent = s.running ? `${s.phase} ${Math.round(s.progress * 100)}%` : s.phase;
  chip.className = `chip ${s.phase === 'pronto' ? 'ok' : BUSY.includes(s.phase) ? 'busy' : 'err'}`;
  $('#phase').textContent = s.phase;
  $('#phaseMsg').textContent = s.message || '';
  $('#progressBar').style.width = `${(s.running ? s.progress : s.phase === 'pronto' ? 1 : 0) * 100}%`;

  const d = s.dataset || {};
  $('#dataKv').innerHTML = `
    <dt>Partidas</dt><dd>${d.partidas ?? 0}</dd>
    <dt>Majors</dt><dd>${(d.majors || []).length}</dd>
    <dt>Ranking</dt><dd>${d.ranking ?? 0} times</dd>
    <dt>Stats atuais</dt><dd>${d.jogadores ?? 0} jogadores</dd>
    <dt>Dataset gerado</dt><dd>${esc(d.generatedAt || '–')}</dd>
`;

  ['#btnCurate', '#btnDemo', '#btnDemoRemove'].forEach((id) => { $(id).disabled = s.running; });
  $('#btnStop').disabled = !s.running;
  $('#tokenField').style.display = s.adminRequired ? '' : 'none';

  renderTrain(s.train);
  renderReport(s.report);
  const logs = $('#logs');
  const atBottom = logs.scrollHeight - logs.scrollTop - logs.clientHeight < 40;
  logs.textContent = (s.logs || []).join('\n');
  if (atBottom) logs.scrollTop = logs.scrollHeight;

  if (lastPhase && lastPhase !== s.phase && s.phase === 'pronto') { loadKnownTeams(); loadEvents(); }
  lastPhase = s.phase;
}

async function refreshStatus() {
  try { renderStatus(await api('/api/status')); } catch (e) {
    const chip = $('#statusChip');
    chip.textContent = 'offline';
    chip.className = 'chip err';
  }
}

async function poll() {
  await refreshStatus();
  const dataTab = $('#tab-data').classList.contains('active');
  setTimeout(poll, (lastStatus && lastStatus.running) || dataTab ? 3000 : 15000);
}

async function action(path, json) {
  showErr('#dataError', '');
  try { await api(path, { json }); } catch (e) { showErr('#dataError', e.message); }
  refreshStatus();
}
$('#btnCurate').addEventListener('click', () => action('/api/pipeline', { collect: false }));
$('#btnStop').addEventListener('click', () => action('/api/pipeline/stop', {}));
$('#btnDemo').addEventListener('click', () => action('/api/demo', {}));
$('#btnDemoRemove').addEventListener('click', () => action('/api/demo', { remove: true }));

const tokenInput = $('#adminToken');
tokenInput.value = store.get('pp-token', '');
tokenInput.addEventListener('change', () => store.set('pp-token', tokenInput.value.trim()));

// upload
async function uploadFiles(files) {
  const log = $('#uploadLog');
  for (const f of files) {
    const li = document.createElement('li');
    li.textContent = `… ${f.name}`;
    log.prepend(li);
    try {
      const text = await f.text();
      const isJson = f.name.toLowerCase().endsWith('.json');
      const r = await api(isJson ? '/api/import/json' : '/api/import/html', {
        body: text,
        headers: { 'Content-Type': isJson ? 'application/json' : 'text/html; charset=utf-8' },
      });
      li.className = 'ok';
      li.textContent = `✓ ${f.name} → ${isJson ? `${r.partidas} partidas` : r.tipo}`;
    } catch (e) {
      li.className = 'bad';
      li.textContent = `✗ ${f.name}: ${e.message}`;
    }
  }
}
$('#fileInput').addEventListener('change', (e) => { uploadFiles([...e.target.files]); e.target.value = ''; });
const drop = $('#dropZone');
['dragenter', 'dragover'].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add('over'); }));
['dragleave', 'drop'].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove('over'); }));
drop.addEventListener('drop', (e) => uploadFiles([...e.dataTransfer.files]));

// ============================================================ init
loadForm();
renderPool();
renderRows();
applyMode();
loadEvents();
loadKnownTeams();
switchTab(store.get('pp-tab', 'predict'));
poll();
