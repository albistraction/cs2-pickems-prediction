'use strict';
// =============================================================================
//  Painel do coletor (página da extensão).
//  Controla uma janela separada que navega na HLTV, extrai o HTML e envia ao
//  site (POST /api/import/html). Verificações da Cloudflare: o usuário resolve.
// =============================================================================

const BASE = 'https://www.hltv.org';
const DAY = 86400000;
const DEFAULTS = { site: '', token: '', majors: 10, diasRecentes: 120, intervaloS: 3, eventoUrl: '', rapido: true, v: 2 };
// config salva antes da v2 tinha intervalo padrão de 8 s: migra para o novo padrão
function migrate(cfg) {
  if (!cfg.v) { if (cfg.intervaloS === 8) cfg.intervaloS = DEFAULTS.intervaloS; cfg.rapido = true; cfg.v = 2; }
  return cfg;
}
const $ = (s) => document.querySelector(s);

let running = false;
let stopFlag = false;
let worker = null;   // { windowId, tabId }
let lastNav = 0;
let counters = { paginas: 0, enviadas: 0, verificacoes: 0 };

// ------------------------------------------------------------------ util
// sleep via Web Worker: continua preciso mesmo com o painel em segundo plano
const timerWorker = new Worker('timer-worker.js');
const timerWaiters = new Map();
let timerSeq = 0;
timerWorker.onmessage = (e) => { const r = timerWaiters.get(e.data); timerWaiters.delete(e.data); if (r) r(); };
const sleep = (ms) => new Promise((r) => { const id = ++timerSeq; timerWaiters.set(id, r); timerWorker.postMessage({ id, ms: Math.max(0, ms) }); });
const ymd = (d) => d.toISOString().slice(0, 10);
const fmtDur = (ms) => { const m = Math.round(ms / 60000); return m < 60 ? `${m} min` : `${Math.floor(m / 60)} h ${m % 60} min`; };

async function getStore(key, def) { const r = await chrome.storage.local.get(key); return r[key] === undefined ? def : r[key]; }
async function setStore(key, val) { await chrome.storage.local.set({ [key]: val }); }

function log(msg, cls = '') {
  const line = document.createElement('div');
  line.className = cls;
  line.textContent = `[${new Date().toLocaleTimeString('pt-BR')}] ${msg}`;
  const box = $('#log');
  box.appendChild(line);
  while (box.childElementCount > 800) box.firstChild.remove();
  box.scrollTop = box.scrollHeight;
}
function status(text, frac) {
  $('#statusText').textContent = text;
  if (frac !== undefined) $('#bar').style.width = `${Math.max(0, Math.min(1, frac)) * 100}%`;
  $('#counters').textContent = `${counters.paginas} páginas · ${counters.enviadas} enviadas · ${counters.verificacoes} verificações`;
}
function checkStop() { if (stopFlag) throw new Error('Interrompido pelo usuário'); }
async function sleepStop(ms) { const end = Date.now() + ms; while (Date.now() < end) { checkStop(); await sleep(Math.min(500, end - Date.now())); } }

function beep() {
  try {
    const ctx = new AudioContext();
    [0, 0.35].forEach((t) => {
      const o = ctx.createOscillator(); const g = ctx.createGain();
      o.frequency.value = 880; g.gain.value = 0.15;
      o.connect(g); g.connect(ctx.destination);
      o.start(ctx.currentTime + t); o.stop(ctx.currentTime + t + 0.2);
    });
  } catch { /* sem áudio */ }
}

// ------------------------------------------------------------------ Majors
const DROP = new Set(['stage', 'opening', 'elimination', 'challengers', 'legends', 'champions', 'playoffs',
  'playoff', 'main', 'new', '1', '2', '3', '-', 'phase', 'swiss']);
const majorGroupKey = (name) => name.toLowerCase().split(/\s+/).filter((t) => t && !DROP.has(t)).join(' ');

function groupMajors(events, wanted) {
  const groups = new Map();
  for (const e of events) {
    const k = majorGroupKey(e.name);
    const g = groups.get(k) || { name: e.name, ids: [], start: 0 };
    if (e.name.length < g.name.length) g.name = e.name;
    g.ids.push(e.id);
    if (e.unix && (!g.start || e.unix < g.start)) g.start = e.unix;
    groups.set(k, g);
  }
  return [...groups.values()].filter((g) => !g.start || g.start <= Date.now())
    .sort((a, b) => b.start - a.start).slice(0, wanted);
}

// ------------------------------------------------------------------ funções injetadas na página da HLTV
// (precisam ser autocontidas: rodam no contexto da aba)
function pageState() {
  const t = (document.title || '').toLowerCase();
  const body = document.body ? document.body.innerText.slice(0, 4000).toLowerCase() : '';
  return {
    challenge: t.includes('just a moment') || t.includes('um momento') || t.includes('attention required')
      || body.includes('verifying you are human') || body.includes('verify you are human')
      || body.includes('verificando se você é humano'),
    blocked: body.includes('you have been blocked'),
  };
}
function pageHtml(canonical) {
  const root = document.documentElement.cloneNode(true);
  root.querySelectorAll('script, style, noscript').forEach((e) => e.remove());
  let html = `<!DOCTYPE html>${root.outerHTML}`;
  if (!/rel="canonical"/i.test(html)) html = html.replace(/<head[^>]*>/i, (m) => `${m}<link rel="canonical" href="${canonical}">`);
  return html;
}
function pageArchive() {
  return [...document.querySelectorAll('a[href^="/events/"]')]
    .filter((a) => /small-event|big-event/.test(a.className))
    .map((a) => {
      const m = a.getAttribute('href').match(/^\/events\/(\d+)/);
      const nameEl = a.querySelector('.big-event-name, .text-ellipsis, .event-name');
      const unixEl = a.querySelector('[data-unix]');
      return m ? { id: m[1], name: (nameEl ? nameEl.textContent : a.textContent).trim().replace(/\s+/g, ' '),
        unix: unixEl ? Number(unixEl.getAttribute('data-unix')) : 0 } : null;
    })
    .filter(Boolean);
}
function pageResults() {
  return [...document.querySelectorAll('.result-con')]
    .map((el) => {
      const a = el.querySelector('a[href^="/matches/"]') || el.closest('a[href^="/matches/"]');
      if (!a) return null;
      const href = a.getAttribute('href');
      const m = href.match(/^\/matches\/(\d+)/);
      return m ? { id: m[1], path: href, unix: Number(el.getAttribute('data-zonedgrouping-entry-unix') || 0) } : null;
    })
    .filter(Boolean);
}

// Links de Majors em andamento/próximas na página /events
function pageMajorLinks() {
  const seen = new Set();
  return [...document.querySelectorAll('a[href^="/events/"]')]
    .map((a) => ({ href: a.getAttribute('href').split('#')[0], text: a.textContent.replace(/\s+/g, ' ').trim() }))
    .filter((l) => /^\/events\/\d+\//.test(l.href) && /major/i.test(l.text) && !/qualifier|rmr|closed|open qual/i.test(l.text))
    .filter((l) => (seen.has(l.href) ? false : seen.add(l.href)));
}
function pageHasEventJson() { return !!document.querySelector('[data-multi-stage-simulator-json]'); }

// ------------------------------------------------------------------ controle da aba
async function ensureWorker() {
  if (worker) {
    try { await chrome.tabs.get(worker.tabId); return worker; } catch { worker = null; }
  }
  const w = await chrome.windows.create({ url: 'about:blank', focused: false, width: 1280, height: 860 });
  worker = { windowId: w.id, tabId: w.tabs[0].id };
  await setBlocking(worker.tabId, blockingOn);
  return worker;
}

// Modo rápido: só na aba de coleta, não baixa imagens/fontes/mídia nem scripts, iframes e
// requisições de terceiros (anúncios, rastreadores). A Cloudflare continua liberada para a
// verificação funcionar normalmente.
let blockingOn = true;
async function setBlocking(tabId, on) {
  const addRules = on ? [
    { id: 1, priority: 1, action: { type: 'block' },
      condition: { tabIds: [tabId], resourceTypes: ['image', 'media', 'font'], excludedRequestDomains: ['cloudflare.com'] } },
    { id: 2, priority: 1, action: { type: 'block' },
      condition: { tabIds: [tabId], domainType: 'thirdParty', excludedRequestDomains: ['cloudflare.com', 'hltv.org'],
        resourceTypes: ['script', 'sub_frame', 'xmlhttprequest', 'stylesheet', 'ping', 'websocket', 'other'] } },
  ] : [];
  try {
    await chrome.declarativeNetRequest.updateSessionRules({ removeRuleIds: [1, 2], addRules });
  } catch (e) { log(`Modo rápido indisponível: ${e.message}`, 'warn'); }
}

// Navega e volta assim que o HTML da página nova foi lido (readyState != loading), sem
// esperar imagens e anúncios. Uma marca no documento antigo distingue a página nova da velha.
async function navigate(tabId, url, timeoutMs) {
  const mark = Math.random().toString(36).slice(2);
  try { await inTab(tabId, (m) => { window.__pkMark = m; }, [mark]); } catch { /* about:blank */ }
  await chrome.tabs.update(tabId, { url });
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    checkStop();
    await sleep(300);
    try {
      const s = await inTab(tabId, (m) => ({ fresh: window.__pkMark !== m, host: location.hostname, ready: document.readyState }), [mark]);
      if (s && s.fresh && /(^|\.)hltv\.org$/.test(s.host) && s.ready !== 'loading') return;
    } catch { /* ainda trocando de documento */ }
  }
  throw new Error('tempo esgotado carregando a página');
}

async function inTab(tabId, func, args = []) {
  const [r] = await chrome.scripting.executeScript({ target: { tabId }, func, args });
  return r ? r.result : undefined;
}
async function stateOf(tabId) {
  try { return (await inTab(tabId, pageState)) || { challenge: true }; } catch { return { challenge: true }; }
}

async function open(cfg, p) {
  const wait = lastNav + cfg.intervaloS * 1000 * (1 + Math.random() * 0.5) - Date.now();
  if (wait > 0) await sleepStop(wait);
  const { tabId, windowId } = await ensureWorker();
  for (let attempt = 0; attempt < 3; attempt++) {
    checkStop();
    try { await navigate(tabId, BASE + p, 90000); } catch (e) {
      if (stopFlag) throw e;
      log(`  ${e.message}: ${p}`, 'warn'); await sleepStop(5000); continue;
    }
    lastNav = Date.now();
    await sleep(150);
    let st = await stateOf(tabId);
    if (st.challenge) {
      await sleepStop(5000); // o desafio às vezes passa sozinho
      st = await stateOf(tabId);
      if (st.challenge) {
        counters.verificacoes++;
        $('#alert').hidden = false;
        beep();
        log('A HLTV pediu verificação: resolva na janela da HLTV.', 'warn');
        await chrome.windows.update(windowId, { focused: true, drawAttention: true });
        const deadline = Date.now() + 15 * 60000;
        let lastBeep = Date.now();
        while ((st = await stateOf(tabId)).challenge) {
          checkStop();
          if (Date.now() > deadline) throw new Error('verificação não resolvida em 15 minutos');
          if (Date.now() - lastBeep > 60000) { beep(); lastBeep = Date.now(); }
          await sleep(2000);
        }
        $('#alert').hidden = true;
        log('Verificação concluída, continuando.', 'ok');
        await sleep(1500);
      }
    }
    if (st.blocked) throw new Error('A HLTV bloqueou este IP ("you have been blocked"). Espere algumas horas e aumente o intervalo.');
    counters.paginas++;
    return tabId;
  }
  throw new Error(`não consegui abrir ${p}`);
}

// ------------------------------------------------------------------ site
async function sitePost(cfg, route, body, contentType) {
  const headers = { 'Content-Type': contentType };
  if (cfg.token) headers['X-Admin-Token'] = cfg.token;
  let lastErr = null;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const r = await fetch(`${cfg.site}${route}`, { method: 'POST', headers, body });
      const t = await r.text();
      let j;
      try { j = JSON.parse(t); } catch { j = { error: t.slice(0, 200) }; }
      if (r.status === 401) throw Object.assign(new Error('token de administrador inválido'), { fatal: true });
      if (!r.ok) throw Object.assign(new Error(j.error || `HTTP ${r.status}`), { fatal: r.status < 500 });
      return j;
    } catch (e) {
      lastErr = e;
      if (e.fatal) throw e;
      await sleep(3000 * (attempt + 1));
    }
  }
  throw lastErr;
}

// ------------------------------------------------------------------ coleta
// Página da Major (qualquer estágio): traz a estrutura completa (estágios, seeds, jogos, resultados).
async function collectEvent(cfg, grab) {
  let path = '';
  if (cfg.eventoUrl) {
    try {
      const u = new URL(cfg.eventoUrl);
      if (/(^|\.)hltv\.org$/.test(u.hostname) && u.pathname.startsWith('/events/')) path = u.pathname;
    } catch { /* URL inválida: detecta automaticamente */ }
  }
  if (!path) {
    status('Procurando a Major atual', 0.005);
    log('Procurando a Major em hltv.org/events');
    const tab = await open(cfg, '/events');
    const links = await inTab(tab, pageMajorLinks);
    if (!links || !links.length) {
      log('Nenhuma Major encontrada em /events. Informe a URL da Major na configuração.', 'err');
      return;
    }
    path = links[0].href;
    log(`  encontrada: ${links[0].text}`);
  }
  status('Estrutura da Major', 0.01);
  log(`Estrutura da Major (estágios, seeds, confrontos e resultados): ${path}`);
  const tab = await grab(path, `event:${path}:${Date.now()}`);
  if (tab && !(await inTab(tab, pageHasEventJson))) log('Essa página não tem a estrutura de estágios. Informe a URL de um estágio da Major.', 'err');
}

async function collect(cfg, mode) {
  const sent = new Set(await getStore('sent', []));
  const persistSent = () => setStore('sent', [...sent]);
  const t0 = Date.now();

  // abre, extrai HTML, envia; devolve o id da aba para extrações adicionais
  // envios rodam em paralelo com a navegação (até 3 ao mesmo tempo)
  const pending = new Set();
  let fatalErr = null;
  const flush = () => Promise.allSettled([...pending]);
  const upload = (name, html) => {
    const pr = sitePost(cfg, '/api/import/html', html, 'text/html; charset=utf-8')
      .then(async (r) => {
        sent.add(name);
        counters.enviadas++;
        if (counters.enviadas % 10 === 0) await persistSent();
        if (r && r.tipo === 'desconhecida') log(`  ${name}: o site não reconheceu a página`, 'warn');
      })
      .catch((e) => { log(`  falha ao enviar ${name}: ${e.message}`, 'err'); if (e.fatal) fatalErr = e; })
      .finally(() => pending.delete(pr));
    pending.add(pr);
  };
  const grab = async (p, name, { skipIfSent = false } = {}) => {
    if (fatalErr) throw fatalErr;
    if (skipIfSent && sent.has(name)) return null;
    const tabId = await open(cfg, p);
    const html = await inTab(tabId, pageHtml, [BASE + p]);
    while (pending.size >= 3) await Promise.race(pending);
    if (fatalErr) throw fatalErr;
    upload(name, html);
    return tabId;
  };

  try {
    await collectEvent(cfg, grab);
    if (mode === 'evento') {
      await flush();
      await persistSent();
      status('Concluído', 1);
      return;
    }

    status('Ranking atual', 0.01);
    log('Ranking atual de times');
    await grab('/ranking/teams', 'ranking');

    status('Stats dos jogadores', 0.02);
    log('Stats dos jogadores (últimos 90 dias)');
    const today = new Date();
    await grab(`/stats/players?startDate=${ymd(new Date(today.getTime() - 90 * DAY))}&endDate=${ymd(today)}&rankingFilter=Top50`, 'playerstats');

    const matches = new Map();
    if (mode === 'completa') {
      status('Arquivo de Majors', 0.03);
      log(`Arquivo de Majors (${cfg.majors} mais recentes)`);
      const tab = await grab('/events/archive?eventType=MAJOR', 'archive');
      const majors = groupMajors(await inTab(tab, pageArchive), cfg.majors);
      if (!majors.length) log('Nenhuma Major encontrada no arquivo (a HLTV pode ter mudado o layout).', 'err');
      for (const [mi, mj] of majors.entries()) {
        status(`Listas de partidas: ${mj.name}`, 0.04 + 0.1 * (mi / majors.length));
        log(`  ${mj.name} (${mj.start ? ymd(new Date(mj.start)) : '?'})`);
        const old = mj.start && mj.start < Date.now() - 60 * DAY;
        for (const id of mj.ids) {
          for (let off = 0; off <= 400; off += 100) {
            const name = `results_event_${id}_${off}`;
            let entries = old ? await getStore(`list:${name}`, null) : null;
            if (!entries) {
              const t = await grab(`/results?event=${id}${off ? `&offset=${off}` : ''}`, name);
              entries = await inTab(t, pageResults);
              await setStore(`list:${name}`, entries);
            }
            for (const e of entries) matches.set(e.id, e);
            if (entries.length < 100) break;
          }
        }
      }
    }

    log(`Resultados recentes (${cfg.diasRecentes} dias)`);
    const cutoff = Date.now() - cfg.diasRecentes * DAY;
    for (let off = 0; off <= 3000; off += 100) {
      status(`Resultados recentes (página ${off / 100 + 1})`, 0.15);
      const t = await grab(`/results?stars=1${off ? `&offset=${off}` : ''}`, `results_recent_${off}`);
      const entries = await inTab(t, pageResults);
      let oldest = Infinity;
      for (const e of entries) {
        if (e.unix) oldest = Math.min(oldest, e.unix);
        if (!e.unix || e.unix >= cutoff) matches.set(e.id, e);
      }
      if (!entries.length || oldest < cutoff) break;
    }

    const todo = [...matches.values()].filter((e) => !sent.has(`match_${e.id}`));
    log(`Partidas: ${matches.size} no total, ${todo.length} para baixar (~${fmtDur(todo.length * (cfg.intervaloS * 1250 + 1500))})`);
    const tm = Date.now();
    for (const [i, e] of todo.entries()) {
      const eta = i >= 3 ? ` · faltam ~${fmtDur(((Date.now() - tm) / i) * (todo.length - i))}` : '';
      status(`Partida ${i + 1}/${todo.length}${eta}`, 0.2 + 0.8 * (i / Math.max(1, todo.length)));
      await grab(e.path, `match_${e.id}`);
    }

    await flush();
    if (fatalErr) throw fatalErr;
    await persistSent();
    status('Concluído', 1);
    log(`Coleta concluída em ${fmtDur(Date.now() - t0)}.`, 'ok');
    try {
      await sitePost(cfg, '/api/pipeline', JSON.stringify({ collect: false }), 'application/json');
      log('Servidor avisado: curando e treinando. Acompanhe na aba Dados do site.', 'ok');
    } catch (e) {
      log(`Não consegui iniciar o treino (${e.message}). Clique em "Curar + treinar" no site.`, 'err');
    }
  } finally {
    await flush();
    await persistSent();
  }
}

// ------------------------------------------------------------------ interface
async function loadConfig() {
  const cfg = migrate({ ...DEFAULTS, ...(await getStore('config', {})) });
  await setStore('config', cfg);
  $('#site').value = cfg.site;
  $('#token').value = cfg.token;
  $('#majors').value = cfg.majors;
  $('#dias').value = cfg.diasRecentes;
  $('#intervalo').value = cfg.intervaloS;
  $('#eventoUrl').value = cfg.eventoUrl || '';
  $('#rapido').checked = cfg.rapido !== false;
  return cfg;
}

async function testSite(cfg) {
  const pill = $('#siteState');
  if (!cfg.site) { pill.textContent = 'site: não configurado'; pill.className = 'pill err'; return false; }
  try {
    const r = await fetch(`${cfg.site}/api/status`);
    const s = await r.json();
    pill.textContent = `site: ${s.phase}${s.dataset ? ` · ${s.dataset.partidas} partidas` : ''}`;
    pill.className = 'pill ok';
    if (s.adminRequired && !cfg.token) $('#cfgMsg').textContent = 'O site exige token e nenhum foi informado.';
    return true;
  } catch (e) {
    pill.textContent = 'site: sem conexão';
    pill.className = 'pill err';
    return false;
  }
}

$('#save').addEventListener('click', async () => {
  const msg = $('#cfgMsg');
  let site = $('#site').value.trim().replace(/\/+$/, '');
  let origin;
  try { origin = new URL(site).origin; } catch { msg.textContent = 'URL inválida.'; return; }
  // permissão para enviar ao site (precisa vir direto do clique)
  const granted = await chrome.permissions.request({ origins: [`${origin}/*`] });
  if (!granted) { msg.textContent = 'Sem permissão para acessar o site: a extensão não conseguirá enviar os dados.'; return; }
  const cfg = {
    site,
    token: $('#token').value.trim(),
    majors: Math.max(1, Number($('#majors').value) || DEFAULTS.majors),
    diasRecentes: Math.max(7, Number($('#dias').value) || DEFAULTS.diasRecentes),
    intervaloS: Math.max(2, Number($('#intervalo').value) || DEFAULTS.intervaloS),
    eventoUrl: $('#eventoUrl').value.trim(),
    rapido: $('#rapido').checked,
    v: 2,
  };
  await setStore('config', cfg);
  msg.textContent = (await testSite(cfg)) ? 'Salvo. Conexão com o site ok.' : 'Salvo, mas não consegui acessar o site.';
});

function setRunning(on) {
  running = on;
  $('#full').disabled = on;
  $('#event').disabled = on;
  $('#current').disabled = on;
  $('#reset').disabled = on;
  $('#save').disabled = on;
  $('#stop').disabled = !on;
}

async function start(mode) {
  if (running) return;
  const cfg = migrate({ ...DEFAULTS, ...(await getStore('config', {})) });
  if (!cfg.site) { log('Configure a URL do site primeiro.', 'err'); return; }
  stopFlag = false;
  counters = { paginas: 0, enviadas: 0, verificacoes: 0 };
  setRunning(true);
  blockingOn = cfg.rapido !== false;
  if (worker) await setBlocking(worker.tabId, blockingOn);
  log({ completa: '=== Coleta completa ===', atuais: '=== Só dados atuais ===', evento: '=== Atualizar Major ===' }[mode]);
  try {
    await collect(cfg, mode);
  } catch (e) {
    log(`Parado: ${e.message}`, 'err');
    status('Parado');
  } finally {
    $('#alert').hidden = true;
    setRunning(false);
  }
}

$('#event').addEventListener('click', () => start('evento'));
$('#full').addEventListener('click', () => start('completa'));
$('#current').addEventListener('click', () => start('atuais'));
$('#stop').addEventListener('click', () => { stopFlag = true; log('Parando...'); });
$('#reset').addEventListener('click', async () => {
  const keys = Object.keys(await chrome.storage.local.get(null)).filter((k) => k === 'sent' || k.startsWith('list:'));
  await chrome.storage.local.remove(keys);
  log('Progresso esquecido: a próxima coleta baixa tudo de novo.');
});

window.addEventListener('beforeunload', (e) => { if (running) { e.preventDefault(); e.returnValue = ''; } });

loadConfig().then(testSite);
status('Parado', 0);
