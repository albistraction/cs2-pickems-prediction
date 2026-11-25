'use strict';

// ============================================================ helpers
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const pct = (x, d = 1) => `${((x || 0) * 100).toFixed(d)}%`;
const num = (x, d = 0) => (x == null || Number.isNaN(+x) ? '–' : (+x).toFixed(d));
const signed = (x, d = 0) => (x > 0 ? '+' : '') + num(x, d);

const ALL_MAPS = ['ancient', 'anubis', 'dust2', 'inferno', 'mirage', 'nuke', 'train', 'overpass', 'vertigo'];
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
const activeNames = () => state.rows.slice(0, nTeams()).map((r) => r.name.trim()).filter(Boolean);

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
  box.innerHTML = ALL_MAPS.map((m) => {
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
  const rows = state.rows.slice(0, nTeams());
  if (rows.some((r) => !r.name.trim())) { showErr('#predictError', `Preencha os ${nTeams()} times.`); return; }
  const lineups = {};
  rows.forEach((r) => {
    const l = r.lineup.split(',').map((s) => s.trim()).filter(Boolean);
    if (l.length) lineups[r.name.trim()] = l;
  });
  const body = {
    format: state.format,
    boRule: $('#boRule').value,
    finalBo: Number($('#finalBo').value),
    advIncludes30: $('#adv30').checked,
    sims: Number($('#sims').value),
    pool: state.pool,
    teams: rows.map((r, i) => ({ name: r.name.trim(), seed: i + 1 })),
    scenario: {
      adjust: rows.filter((r) => Number(r.adjust)).map((r) => ({ team: r.name.trim(), delta: Number(r.adjust) })),
      lineups,
      force: state.force.filter((f) => f.a && f.b && f.winner),
      mapadj: state.mapadj.filter((m) => m.team && m.map && Number(m.delta)).map((m) => ({ team: m.team, map: m.map, delta: Number(m.delta) })),
    },
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
  const sub = `${Number(r.sims).toLocaleString('pt-BR')} simulações · pool: ${r.pool.join(', ')}`;

  if (r.format === 'playoffs') {
    const order = teams.map((t, i) => i).sort((a, b) => teams[b].campeao - teams[a].campeao);
    const stages = ['Quartas', 'Semi', 'Final'];
    box.innerHTML = `
      <div class="card"><div class="card-head"><h2>Playoffs</h2><span class="hint">${esc(sub)}</span></div>
        <div class="table-wrap"><table class="tbl" id="resTable">
          <tr><th class="num">Seed</th><th>Time</th><th class="num">Força</th><th>Semi</th><th>Final</th><th>Campeão</th></tr>
          ${order.map((i) => { const t = teams[i]; return `<tr class="main-row" data-i="${i}"><td class="num">${t.seed}</td><td class="team-name">${esc(t.name)}</td><td class="num">${num(t.rating)}</td>${barCell(t.semi)}${barCell(t.final)}${barCell(t.campeao)}</tr>`; }).join('')}
        </table></div>
        ${warningsHtml(r.avisos)}
      </div>
      <div class="card"><h2>Chave sugerida</h2><div class="bracket">
        ${stages.map((s) => `<div><h3>${s}</h3>${r.chave.filter((m) => m.fase === s).map((m) => `
          <div class="match"><span class="p">${pct(m.prob, 0)}</span>
            <div class="${m.vencedor === m.a ? 'w' : ''}">${esc(m.a)}</div>
            <div class="${m.vencedor === m.b ? 'w' : ''}">${esc(m.b)}</div></div>`).join('')}</div>`).join('')}
      </div></div>`;
    attachDetails($('#resTable'), teams, 6);
    return;
  }

  const pickCol = (title, cls, list) => `<div class="pick-col ${cls}"><h3>${title}</h3><ul>${list.map((p) => `<li class="pick"><span>${esc(p.team)}</span><span class="p">${pct(p.prob)}</span></li>`).join('')}</ul></div>`;
  const order = teams.map((t, i) => i).sort((a, b) => teams[b].avanca - teams[a].avanca);
  const cats = ['3-0', '3-1', '3-2', '2-3', '1-3', '0-3'];
  box.innerHTML = `
    <div class="card"><div class="card-head"><h2>Pick'Em sugerido</h2><span class="hint">${esc(sub)}</span></div>
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
      ${warningsHtml(r.avisos)}
    </div>
    <div class="card"><h2>Probabilidades por time</h2>
      <p class="hint">Clique num time para ver lineup, K/D e rating dos jogadores, composição da força e força por mapa.</p>
      <div class="table-wrap"><table class="tbl" id="resTable">
        <tr><th class="num">Seed</th><th>Time</th><th class="num">Força</th>${cats.map((c) => `<th class="num">${c}</th>`).join('')}<th>Avança</th></tr>
        ${order.map((i) => { const t = teams[i]; return `<tr class="main-row" data-i="${i}">
          <td class="num">${t.seed}</td><td class="team-name">${esc(t.name)}</td><td class="num">${num(t.rating)}</td>
          ${cats.map((c) => `<td class="heat" ${heat(t.dist[c])}>${pct(t.dist[c])}</td>`).join('')}
          ${barCell(t.avanca)}</tr>`; }).join('')}
      </table></div>
    </div>`;
  attachDetails($('#resTable'), teams, 10);
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

  if (lastPhase && lastPhase !== s.phase && s.phase === 'pronto') loadKnownTeams();
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
loadKnownTeams();
switchTab(store.get('pp-tab', 'predict'));
poll();
