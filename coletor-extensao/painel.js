'use strict';
// =============================================================================
//  Painel do coletor (página da extensão).
//  Controla uma janela separada que navega na HLTV, extrai o HTML e envia ao
//  site (POST /api/import/html). Verificações da Cloudflare: o usuário resolve.
// =============================================================================

const BASE = 'https://www.hltv.org';
const DAY = 86400000;
const DEFAULTS = { site: '', token: '', majors: 10, diasRecentes: 120, intervaloS: 3, eventoUrl: '', rapido: true,
  timesTop: 30, diasTimes: 365, rankings: true, v: 3 };
// config salva antes da v2 tinha intervalo padrão de 8 s: migra para o novo padrão
function migrate(cfg) {
  if (!cfg.v) { if (cfg.intervaloS === 8) cfg.intervaloS = DEFAULTS.intervaloS; cfg.rapido = true; cfg.v = 2; }
  if (cfg.v < 3) { cfg.timesTop = DEFAULTS.timesTop; cfg.diasTimes = DEFAULTS.diasTimes; cfg.rankings = true; cfg.v = 3; }
  return cfg;
}
const $ = (s) => document.querySelector(s);

let running = false;
let stopFlag = false;
let worker = null;   // { windowId, tabId }
let lastNav = 0;
let lastLoadMs = 0;
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
  html = html.replace(/<head[^>]*>/i, (m) => `${m}<meta name="pk-source" content="${canonical.replace(/"/g, '')}">`);
  return html;
}
// Times (id + slug) linkados na página, na ordem em que aparecem
function pageTeamLinks() {
  const seen = new Set();
  return [...document.querySelectorAll('a[href^="/team/"]')]
    .map((a) => a.getAttribute('href').match(/^\/team\/(\d+)\/([^/?#]+)/))
    .filter((m) => m && !seen.has(m[1]) && seen.add(m[1]))
    .map((m) => ({ id: m[1], slug: m[2] }));
}
// Data no cabeçalho do ranking ("... ranking on November 24th 2025"), se houver
function pageRankingDate() {
  const txt = document.body ? document.body.innerText : '';
  const m = txt.match(/ranking on\s+([A-Za-z]+)\s+(\d{1,2})(?:st|nd|rd|th)?,?\s+(\d{4})/i);
  if (!m) return null;
  const d = new Date(`${m[1]} ${m[2]}, ${m[3]} 12:00 UTC`);
  return Number.isNaN(d.getTime()) ? null : d.getTime();
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
  // Os dados (stats, resultados, estrutura da Major) já vêm no HTML do servidor: nenhum script
  // é necessário. Scripts no <head> seguram o carregamento, então todos são bloqueados, menos
  // os da Cloudflare (/cdn-cgi/ e challenges.cloudflare.com), para a verificação funcionar.
  const addRules = on ? [
    { id: 1, priority: 1, action: { type: 'block' },
      condition: { tabIds: [tabId], excludedRequestDomains: ['cloudflare.com'],
        resourceTypes: ['image', 'media', 'font', 'script', 'stylesheet', 'sub_frame', 'xmlhttprequest', 'ping', 'websocket', 'other'] } },
    { id: 2, priority: 2, action: { type: 'allow' },
      condition: { tabIds: [tabId], urlFilter: '/cdn-cgi/' } },
  ] : [];
  if (!chrome.declarativeNetRequest) {
    if (on) log('Modo rápido indisponível: a extensão está com o manifest antigo. Em chrome://extensions, clique em ↻ no coletor (ou remova e carregue a pasta coletor-extensao de novo).', 'warn');
    return;
  }
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
async function tabAlive(tabId) { try { await chrome.tabs.get(tabId); return true; } catch { return false; } }
async function stateOf(tabId) {
  try { return (await inTab(tabId, pageState)) || { challenge: true }; } catch {
    return (await tabAlive(tabId)) ? { challenge: true } : { gone: true };
  }
}
const TabGone = () => Object.assign(new Error('a janela da HLTV foi fechada; abrindo outra'), { gone: true });

async function open(cfg, p) {
  const wait = lastNav + cfg.intervaloS * 1000 * (1 + Math.random() * 0.5) - Date.now();
  if (wait > 0) await sleepStop(wait);
  let reopen = 0;
  for (let attempt = 0; attempt < 3; attempt++) {
    checkStop();
    // a janela da HLTV pode ter sido fechada (ou descartada pelo Chrome): recria se preciso
    const { tabId, windowId } = await ensureWorker();
    const n0 = Date.now();
    let st;
    try {
      try { await navigate(tabId, BASE + p, 90000); lastLoadMs = Date.now() - n0; } catch (e) {
        if (stopFlag) throw e;
        if (!(await tabAlive(tabId))) throw TabGone();
        log(`  ${e.message}: ${p}`, 'warn'); await sleepStop(5000); continue;
      }
      lastNav = Date.now();
      await sleep(150);
      st = await stateOf(tabId);
      if (st.gone) throw TabGone();
    } catch (e) {
      if (!e.gone) throw e;
      log(`  ${e.message}`, 'warn'); worker = null; attempt--; await sleepStop(1000);
      if (++reopen > 5) throw new Error('a janela da HLTV continua sendo fechada');
      continue;
    }
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
        if (st.gone) { log('  a janela da HLTV foi fechada durante a verificação; abrindo outra', 'warn'); worker = null; attempt--; if (++reopen > 5) throw new Error('a janela da HLTV continua sendo fechada'); continue; }
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
      // 400 = só esta página foi recusada (segue a coleta); 403/404 = site ou rota errada (para)
      if (!r.ok) throw Object.assign(new Error(j.error || `HTTP ${r.status}`), { fatal: r.status === 403 || r.status === 404, rejected: r.status === 400 });
      return j;
    } catch (e) {
      lastErr = e;
      if (e.fatal || e.rejected) throw e;
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
  return tab ? (await inTab(tab, pageTeamLinks)) || [] : [];
}

async function collect(cfg, mode) {
  const sent = new Set(await getStore('sent', []));
  const persistSent = () => setStore('sent', [...sent]);
  const t0 = Date.now();

  // partidas que o site já tem não são baixadas de novo (vale mesmo após "Esquecer progresso")
  try {
    const r = await fetch(`${cfg.site}/api/raw/matches`, { headers: cfg.token ? { 'X-Admin-Token': cfg.token } : {} });
    if (r.ok) {
      const { ids = [] } = await r.json();
      let novos = 0;
      for (const id of ids) if (!sent.has(`match_${id}`)) { sent.add(`match_${id}`); novos++; }
      log(`O site já tem ${ids.length} partidas${novos ? ` (${novos} marcadas como feitas)` : ''}; essas serão puladas.`);
    }
  } catch { /* site antigo sem a rota: segue só com o progresso local */ }

  // abre, extrai HTML, envia; devolve o id da aba para extrações adicionais
  // envios rodam em paralelo com a navegação (até 3 ao mesmo tempo)
  const pending = new Set();
  let fatalErr = null;
  const flush = () => Promise.allSettled([...pending]);
  // tempos médios, para saber onde está a demora
  const tm = { n: 0, wait: 0, load: 0, up: 0, upN: 0, kb: 0, blocked: 0 };
  const report = () => {
    if (!tm.n) return;
    log(`  média por página: espera ${(tm.wait / tm.n / 1000).toFixed(1)} s · carregar ${(tm.load / tm.n / 1000).toFixed(1)} s · `
      + `envio ${tm.upN ? (tm.up / tm.upN / 1000).toFixed(1) : '?'} s (${Math.round(tm.kb / tm.n)} KB) · fila de envio ${(tm.blocked / tm.n / 1000).toFixed(1)} s`);
    Object.keys(tm).forEach((k) => { tm[k] = 0; });
  };
  const upload = (name, html) => {
    const u0 = Date.now();
    const pr = sitePost(cfg, '/api/import/html', html, 'text/html; charset=utf-8')
      .then(async (r) => {
        tm.up += Date.now() - u0; tm.upN++;
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
    const g0 = Date.now();
    const tabId = await open(cfg, p);
    const html = await inTab(tabId, pageHtml, [BASE + p]);
    const g1 = Date.now();
    while (pending.size >= 4) await Promise.race(pending);
    if (fatalErr) throw fatalErr;
    tm.n++; tm.load += lastLoadMs; tm.wait += (g1 - g0) - lastLoadMs; tm.blocked += Date.now() - g1; tm.kb += html.length / 1024;
    if (tm.n >= 20) report();
    upload(name, html);
    return tabId;
  };

  try {
    const eventTeams = await collectEvent(cfg, grab);
    if (mode === 'evento') {
      await flush();
      await persistSent();
      status('Concluído', 1);
      return;
    }

    status('Ranking atual', 0.01);
    log('Ranking atual de times');
    const rankTab = await grab('/ranking/teams', 'ranking');
    const rankTeams = rankTab ? (await inTab(rankTab, pageTeamLinks)) || [] : [];

    status('Stats dos jogadores', 0.02);
    log('Stats dos jogadores (últimos 90 dias)');
    const today = new Date();
    await grab(`/stats/players?startDate=${ymd(new Date(today.getTime() - 90 * DAY))}&endDate=${ymd(today)}&rankingFilter=Top50`, 'playerstats');

    const matches = new Map();
    let oldestNeeded = Date.now() - Math.max(cfg.diasRecentes, cfg.diasTimes) * DAY;
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
        if (mj.start) oldestNeeded = Math.min(oldestNeeded, mj.start - 30 * DAY);
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

    // Partidas de cada time em todos os campeonatos (não só Majors e eventos com estrela)
    const teams = new Map();
    for (const t of eventTeams) teams.set(t.id, t);
    for (const t of rankTeams.slice(0, cfg.timesTop)) teams.set(t.id, t);
    if (teams.size && cfg.diasTimes > 0) {
      const cutT = Date.now() - cfg.diasTimes * DAY;
      log(`Partidas dos times em todos os campeonatos (${teams.size} times, ${cfg.diasTimes} dias)`);
      let ti = 0;
      for (const t of teams.values()) {
        ti++;
        status(`Partidas de ${t.slug} (${ti}/${teams.size})`, 0.15 + 0.03 * (ti / teams.size));
        let added = 0;
        for (let off = 0; off <= 1000; off += 100) {
          const name = `results_team_${t.id}_${off}`;
          const tab = await grab(`/results?team=${t.id}${off ? `&offset=${off}` : ''}`, name);
          const entries = (await inTab(tab, pageResults)) || [];
          let oldest = Infinity;
          for (const e of entries) {
            if (e.unix) oldest = Math.min(oldest, e.unix);
            if ((!e.unix || e.unix >= cutT) && !matches.has(e.id)) { matches.set(e.id, e); added++; }
          }
          if (entries.length < 100 || oldest < cutT) break;
        }
        log(`  ${t.slug}: +${added} partidas`);
      }
    }

    // Rankings HLTV semanais (segundas-feiras): o treino usa o ranking da época de cada jogo
    if (cfg.rankings) {
      const mondays = [];
      const d = new Date();
      d.setUTCHours(12, 0, 0, 0);
      while (d.getUTCDay() !== 1) d.setUTCDate(d.getUTCDate() - 1);
      const limit = Math.max(oldestNeeded, Date.now() - 4 * 365 * DAY);
      for (; d.getTime() >= limit; d.setUTCDate(d.getUTCDate() - 7)) mondays.push(new Date(d));
      const MONTHS = ['january', 'february', 'march', 'april', 'may', 'june', 'july', 'august', 'september', 'october', 'november', 'december'];
      const todoR = mondays.filter((m) => !sent.has(`ranking_${ymd(m)}`));
      log(`Rankings históricos: ${mondays.length} semanas desde ${ymd(mondays[mondays.length - 1] || new Date())}, ${todoR.length} para baixar`);
      for (const [i, m] of todoR.entries()) {
        status(`Ranking de ${ymd(m)} (${i + 1}/${todoR.length})`, 0.18 + 0.02 * (i / Math.max(1, todoR.length)));
        const p = `/ranking/teams/${m.getUTCFullYear()}/${MONTHS[m.getUTCMonth()]}/${m.getUTCDate()}`;
        const tabId = await open(cfg, p);
        // a HLTV pode mostrar outro ranking se não houver um nessa data: confere o cabeçalho
        if (!(await inTab(tabId, () => !!document.querySelector('.ranked-team')))) {
          log(`  ${ymd(m)}: sem ranking publicado nessa semana, pulando`, 'warn');
          sent.add(`ranking_${ymd(m)}`);
          continue;
        }
        const shown = await inTab(tabId, pageRankingDate);
        if (shown && Math.abs(shown - m.getTime()) > 3 * DAY) {
          log(`  ${ymd(m)}: a HLTV mostrou o ranking de ${ymd(new Date(shown))}, pulando`, 'warn');
          sent.add(`ranking_${ymd(m)}`);
          continue;
        }
        const html = await inTab(tabId, pageHtml, [BASE + p]);
        while (pending.size >= 4) await Promise.race(pending);
        if (fatalErr) throw fatalErr;
        upload(`ranking_${ymd(m)}`, html);
      }
    }

    const todo = [...matches.values()].filter((e) => !sent.has(`match_${e.id}`));
    log(`Partidas: ${matches.size} no total, ${todo.length} para baixar (~${fmtDur(todo.length * (cfg.intervaloS * 1250 + 1500))})`);
    const tm = Date.now();
    let fails = 0;
    for (const [i, e] of todo.entries()) {
      const eta = i >= 3 ? ` · faltam ~${fmtDur(((Date.now() - tm) / i) * (todo.length - i))}` : '';
      status(`Partida ${i + 1}/${todo.length}${eta}`, 0.2 + 0.8 * (i / Math.max(1, todo.length)));
      try {
        await grab(e.path, `match_${e.id}`);
        fails = 0;
      } catch (err) {
        if (stopFlag || err.fatal || /bloqueou|verificação não resolvida/.test(err.message)) throw err;
        log(`  pulando ${e.path}: ${err.message}`, 'warn');
        if (++fails >= 10) throw new Error('10 partidas seguidas falharam');
      }
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
  $('#timesTop').value = cfg.timesTop;
  $('#diasTimes').value = cfg.diasTimes;
  $('#rankings').checked = cfg.rankings !== false;
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
    timesTop: Math.max(0, Number($('#timesTop').value) || 0),
    diasTimes: Math.max(0, Number($('#diasTimes').value) || 0),
    rankings: $('#rankings').checked,
    v: 3,
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
  log('Progresso local esquecido. Partidas que o site já tem continuam sendo puladas.');
});

window.addEventListener('beforeunload', (e) => { if (running) { e.preventDefault(); e.returnValue = ''; } });

loadConfig().then(testSite);
status('Parado', 0);
