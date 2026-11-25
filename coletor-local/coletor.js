#!/usr/bin/env node
'use strict';
// =============================================================================
//  Coletor local assistido — pickems-prediction
// -----------------------------------------------------------------------------
//  Abre a HLTV num Chromium com janela, percorre as páginas necessárias e envia
//  o HTML para o seu site (rota /api/import/html). Quando a Cloudflare pedir
//  verificação, o programa apita e espera VOCÊ resolver na janela.
//
//  Uso:  node coletor.js            (menu)
//        node coletor.js completa   (10 Majors + resultados recentes + dados atuais)
//        node coletor.js atuais     (ranking, stats e resultados recentes)
//        node coletor.js reenviar   (envia de novo o que já foi coletado)
//        node coletor.js config     (configura URL do site e token)
// =============================================================================

const fs = require('fs');
const path = require('path');
const readline = require('readline/promises');
const { chromium } = require('playwright');

const ROOT = __dirname;
const DATA_DIR = path.join(ROOT, 'dados');
const PAGES_DIR = path.join(DATA_DIR, 'paginas');
const LISTS_DIR = path.join(DATA_DIR, 'listas');
const PROFILE_DIR = path.join(DATA_DIR, 'perfil-navegador');
const CONFIG_FILE = path.join(ROOT, 'config.json');
const SENT_FILE = path.join(DATA_DIR, 'enviados.json');
const BASE = 'https://www.hltv.org';
const DAY_MS = 86400000;

const DEFAULTS = { site: '', token: '', majors: 10, diasRecentes: 120, intervaloMs: 5000, navegador: 'chromium' };

// ------------------------------------------------------------------ utilidades
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const ymd = (d) => d.toISOString().slice(0, 10);
const log = (...a) => console.log(`[${new Date().toLocaleTimeString('pt-BR')}]`, ...a);
const loadJson = (f, d) => { try { return JSON.parse(fs.readFileSync(f, 'utf8')); } catch { return d; } };
const saveJson = (f, v) => { fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, JSON.stringify(v, null, 2)); };
const fmtDur = (ms) => { const m = Math.round(ms / 60000); return m < 60 ? `${m} min` : `${Math.floor(m / 60)} h ${m % 60} min`; };

for (const d of [PAGES_DIR, LISTS_DIR, PROFILE_DIR]) fs.mkdirSync(d, { recursive: true });

// ------------------------------------------------------------------ Majors
// "BLAST.tv Austin Major 2025 Stage 1" -> "blast.tv austin major 2025" (mesma regra do backend)
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
  return [...groups.values()]
    .filter((g) => !g.start || g.start <= Date.now())
    .sort((a, b) => b.start - a.start)
    .slice(0, wanted);
}

// ------------------------------------------------------------------ site
async function siteStatus(cfg) {
  const r = await fetch(`${cfg.site}/api/status`);
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

async function sitePost(cfg, route, body, contentType) {
  const headers = { 'Content-Type': contentType };
  if (cfg.token) headers['X-Admin-Token'] = cfg.token;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const r = await fetch(`${cfg.site}${route}`, { method: 'POST', headers, body });
      const t = await r.text();
      let j;
      try { j = JSON.parse(t); } catch { j = { error: t.slice(0, 200) }; }
      if (r.status === 401) throw Object.assign(new Error('token de administrador invalido (rode a opcao Configurar)'), { fatal: true });
      if (!r.ok) throw Object.assign(new Error(j.error || `HTTP ${r.status}`), { fatal: r.status < 500 });
      return j;
    } catch (e) {
      if (e.fatal || attempt === 2) throw e;
      await sleep(3000 * (attempt + 1));
    }
  }
  return null;
}

// ------------------------------------------------------------------ navegador
class Hltv {
  constructor(cfg) { this.cfg = cfg; this.last = 0; this.pages = 0; this.challenges = 0; }

  async start() {
    const opts = { headless: false, viewport: null, locale: 'en-US' };
    if (this.cfg.navegador === 'chrome' || this.cfg.navegador === 'msedge') opts.channel = this.cfg.navegador;
    this.ctx = await chromium.launchPersistentContext(PROFILE_DIR, opts);
    await this.ctx.route('**/*', (route) => {
      const t = route.request().resourceType();
      return t === 'image' || t === 'media' || t === 'font' ? route.abort() : route.continue();
    });
    this.page = this.ctx.pages()[0] || (await this.ctx.newPage());
  }

  async stop() { try { if (this.ctx) await this.ctx.close(); } catch { /* já fechado */ } }

  async isChallenge() {
    try {
      const t = (await this.page.title()).toLowerCase();
      return t.includes('just a moment') || t.includes('um momento') || t.includes('attention required');
    } catch { return true; }
  }

  async isHardBlocked() {
    try {
      const txt = await this.page.textContent('body', { timeout: 2000 });
      return /you have been blocked/i.test(txt || '');
    } catch { return false; }
  }

  // Abre https://www.hltv.org + p respeitando o intervalo; espera o usuário em caso de verificação.
  async open(p) {
    const wait = this.last + this.cfg.intervaloMs + Math.random() * this.cfg.intervaloMs * 0.5 - Date.now();
    if (wait > 0) await sleep(wait);
    for (let attempt = 0; attempt < 3; attempt++) {
      try {
        await this.page.goto(BASE + p, { waitUntil: 'domcontentloaded', timeout: 90000 });
      } catch (e) {
        log(`  erro ao abrir ${p}: ${String(e.message).split('\n')[0]}`);
        await sleep(5000 * (attempt + 1));
        continue;
      }
      this.last = Date.now();
      if (await this.isChallenge()) {
        await sleep(6000); // às vezes o desafio passa sozinho
        if (await this.isChallenge()) {
          this.challenges++;
          process.stdout.write('\x07');
          log('>>> A HLTV pediu verificacao. Resolva na janela do navegador. Aguardando voce...');
          await this.page.bringToFront().catch(() => {});
          const deadline = Date.now() + 15 * 60 * 1000;
          while (await this.isChallenge()) {
            if (Date.now() > deadline) throw new Error('verificacao nao resolvida em 15 minutos');
            await sleep(2000);
          }
          log('    ok, continuando.');
          await this.page.waitForLoadState('domcontentloaded').catch(() => {});
        }
      }
      if (await this.isHardBlocked()) throw new Error('A HLTV bloqueou este IP ("you have been blocked"). Espere algumas horas e tente de novo.');
      this.pages++;
      return this.page;
    }
    throw new Error(`nao consegui abrir ${p}`);
  }

  // HTML sem <script>/<style> (menor para enviar), com link canônico garantido.
  async html(p) {
    let h = await this.page.content();
    h = h.replace(/<script\b[^>]*>[\s\S]*?<\/script>/gi, '').replace(/<style\b[^>]*>[\s\S]*?<\/style>/gi, '');
    if (!/rel="canonical"/i.test(h)) h = h.replace(/<head[^>]*>/i, (m) => `${m}<link rel="canonical" href="${BASE}${p}">`);
    return h;
  }
}

// Extração de links direto do DOM (o navegador já interpretou a página)
const extractArchive = (page) => page.$$eval('a[href^="/events/"]', (as) => as
  .filter((a) => /small-event|big-event/.test(a.className))
  .map((a) => {
    const m = a.getAttribute('href').match(/^\/events\/(\d+)/);
    const nameEl = a.querySelector('.big-event-name, .text-ellipsis, .event-name');
    const unixEl = a.querySelector('[data-unix]');
    return m ? { id: m[1], name: (nameEl ? nameEl.textContent : a.textContent).trim().replace(/\s+/g, ' '), unix: unixEl ? Number(unixEl.getAttribute('data-unix')) : 0 } : null;
  })
  .filter(Boolean));

const extractResults = (page) => page.$$eval('.result-con', (els) => els
  .map((el) => {
    const a = el.querySelector('a[href^="/matches/"]') || el.closest('a[href^="/matches/"]');
    if (!a) return null;
    const href = a.getAttribute('href');
    const m = href.match(/^\/matches\/(\d+)/);
    return m ? { id: m[1], path: href, unix: Number(el.getAttribute('data-zonedgrouping-entry-unix') || 0) } : null;
  })
  .filter(Boolean));

// ------------------------------------------------------------------ coleta
async function collect(cfg, mode) {
  const sent = new Set(loadJson(SENT_FILE, []));
  const stats = { coletadas: 0, enviadas: 0, falhasEnvio: 0, doCache: 0 };
  const pagePath = (name) => path.join(PAGES_DIR, name);

  const send = async (name, html) => {
    try {
      const r = await sitePost(cfg, '/api/import/html', html, 'text/html; charset=utf-8');
      sent.add(name);
      saveJson(SENT_FILE, [...sent]);
      stats.enviadas++;
      return r;
    } catch (e) {
      stats.falhasEnvio++;
      log(`  falha ao enviar ${name}: ${e.message}`);
      if (e.message.includes('token')) throw e;
      return null;
    }
  };

  const hltv = new Hltv(cfg);
  const onSigint = async () => { log('Interrompido. O que ja foi coletado esta salvo; rode de novo para continuar.'); await hltv.stop(); process.exit(130); };
  process.once('SIGINT', onSigint);
  await hltv.start();
  const t0 = Date.now();

  // abre, salva, envia; devolve a página aberta
  const grab = async (p, name) => {
    const page = await hltv.open(p);
    const html = await hltv.html(p);
    fs.writeFileSync(pagePath(name), html);
    stats.coletadas++;
    await send(name, html);
    return page;
  };

  try {
    const today = new Date();
    log('Ranking atual de times');
    await grab('/ranking/teams', 'ranking.html');

    log('Stats dos jogadores (ultimos 90 dias)');
    const start = ymd(new Date(today.getTime() - 90 * DAY_MS));
    await grab(`/stats/players?startDate=${start}&endDate=${ymd(today)}&rankingFilter=Top50`, 'playerstats.html');

    const matches = new Map();

    if (mode === 'completa') {
      log(`Arquivo de Majors (usando as ${cfg.majors} mais recentes)`);
      const page = await grab('/events/archive?eventType=MAJOR', 'archive_major.html');
      const majors = groupMajors(await extractArchive(page), cfg.majors);
      if (!majors.length) log('  AVISO: nenhuma Major encontrada no arquivo (a HLTV pode ter mudado o layout).');
      for (const mj of majors) {
        const old = mj.start && mj.start < Date.now() - 60 * DAY_MS;
        log(`  ${mj.name} (${mj.start ? ymd(new Date(mj.start)) : '?'})`);
        for (const id of mj.ids) {
          for (let off = 0; off <= 400; off += 100) {
            const name = `results_event_${id}_${off}`;
            const listFile = path.join(LISTS_DIR, `${name}.json`);
            let entries = old ? loadJson(listFile, null) : null;
            if (entries) {
              stats.doCache++;
              if (!sent.has(`${name}.html`) && fs.existsSync(pagePath(`${name}.html`))) await send(`${name}.html`, fs.readFileSync(pagePath(`${name}.html`), 'utf8'));
            } else {
              const pg = await grab(`/results?event=${id}${off ? `&offset=${off}` : ''}`, `${name}.html`);
              entries = await extractResults(pg);
              saveJson(listFile, entries);
            }
            for (const e of entries) matches.set(e.id, e);
            if (entries.length < 100) break;
          }
        }
      }
    }

    log(`Resultados recentes (${cfg.diasRecentes} dias)`);
    const cutoff = Date.now() - cfg.diasRecentes * DAY_MS;
    for (let off = 0; off <= 3000; off += 100) {
      const pg = await grab(`/results?stars=1${off ? `&offset=${off}` : ''}`, `results_recent_${off}.html`);
      const entries = await extractResults(pg);
      let oldest = Infinity;
      for (const e of entries) {
        if (e.unix) oldest = Math.min(oldest, e.unix);
        if (!e.unix || e.unix >= cutoff) matches.set(e.id, e);
      }
      if (!entries.length || oldest < cutoff) break;
    }

    // páginas das partidas (cache permanente)
    const todo = [...matches.values()];
    const missing = todo.filter((e) => !fs.existsSync(pagePath(`match_${e.id}.html`)));
    log(`Partidas: ${todo.length} no total, ${missing.length} para baixar (~${fmtDur(missing.length * cfg.intervaloMs * 1.35)})`);
    for (const e of todo) {
      const name = `match_${e.id}.html`;
      if (fs.existsSync(pagePath(name))) {
        if (!sent.has(name)) await send(name, fs.readFileSync(pagePath(name), 'utf8'));
      }
    }
    let i = 0;
    const tm = Date.now();
    for (const e of missing) {
      i++;
      const eta = i > 3 ? ` | faltam ~${fmtDur(((Date.now() - tm) / (i - 1)) * (missing.length - i + 1))}` : '';
      log(`  [${i}/${missing.length}] ${e.path}${eta}`);
      await grab(e.path, `match_${e.id}.html`);
    }
  } finally {
    process.removeListener('SIGINT', onSigint);
    await hltv.stop();
  }

  log(`Coleta concluida em ${fmtDur(Date.now() - t0)}: ${stats.coletadas} paginas baixadas, ${stats.enviadas} enviadas, ${hltv.challenges} verificacoes.`);
  if (stats.falhasEnvio) log(`  ${stats.falhasEnvio} envios falharam. Rode a opcao "Reenviar" depois.`);
  await triggerTraining(cfg);
}

async function resend(cfg, all) {
  const sent = new Set(loadJson(SENT_FILE, []));
  const files = fs.readdirSync(PAGES_DIR).filter((f) => f.endsWith('.html') && (all || !sent.has(f)));
  log(`Reenviando ${files.length} arquivos`);
  let ok = 0;
  for (const [i, f] of files.entries()) {
    try {
      await sitePost(cfg, '/api/import/html', fs.readFileSync(path.join(PAGES_DIR, f), 'utf8'), 'text/html; charset=utf-8');
      sent.add(f);
      ok++;
      if ((i + 1) % 25 === 0) { saveJson(SENT_FILE, [...sent]); log(`  ${i + 1}/${files.length}`); }
    } catch (e) {
      log(`  ${f}: ${e.message}`);
      if (e.message.includes('token')) break;
    }
  }
  saveJson(SENT_FILE, [...sent]);
  log(`${ok}/${files.length} enviados.`);
  if (ok) await triggerTraining(cfg);
}

async function triggerTraining(cfg) {
  try {
    await sitePost(cfg, '/api/pipeline', JSON.stringify({ collect: false }), 'application/json');
    log('Servidor avisado: curando os dados e treinando o modelo. Acompanhe na aba Dados do site.');
  } catch (e) {
    log(`Nao consegui iniciar o treino no servidor (${e.message}). Clique em "Curar + treinar" no site.`);
  }
}

// ------------------------------------------------------------------ interface
async function configure(rl, cfg) {
  console.log('\n--- Configuracao ---');
  const site = (await rl.question(`URL do site [${cfg.site || 'ex.: https://pickems-prediction.up.railway.app'}]: `)).trim();
  if (site) cfg.site = site.replace(/\/+$/, '');
  const token = (await rl.question(`Token de administrador (ADMIN_TOKEN) [${cfg.token ? 'Enter mantem o atual' : 'vazio se nao usa'}]: `)).trim();
  if (token) cfg.token = token;
  const mj = (await rl.question(`Quantas Majors coletar [${cfg.majors}]: `)).trim();
  if (mj && Number(mj) > 0) cfg.majors = Number(mj);
  const dias = (await rl.question(`Dias de resultados recentes [${cfg.diasRecentes}]: `)).trim();
  if (dias && Number(dias) > 0) cfg.diasRecentes = Number(dias);
  const intv = (await rl.question(`Intervalo entre paginas em segundos [${cfg.intervaloMs / 1000}]: `)).trim();
  if (intv && Number(intv) >= 2) cfg.intervaloMs = Number(intv) * 1000;
  const nav = (await rl.question(`Navegador (chromium / chrome / msedge) [${cfg.navegador}]: `)).trim().toLowerCase();
  if (['chromium', 'chrome', 'msedge'].includes(nav)) cfg.navegador = nav;
  saveJson(CONFIG_FILE, cfg);
  console.log('Configuracao salva em config.json\n');
}

async function main() {
  const cfg = { ...DEFAULTS, ...loadJson(CONFIG_FILE, {}) };
  const rlRaw = readline.createInterface({ input: process.stdin, output: process.stdout });
  let rlClosed = false;
  const rl = { question: (q) => rlRaw.question(q), close: () => { if (!rlClosed) { rlClosed = true; rlRaw.close(); } } };
  try {
    console.log('==============================================');
    console.log('  pickems-prediction - coletor local da HLTV');
    console.log('==============================================');
    let mode = (process.argv[2] || '').toLowerCase();
    if (!cfg.site || mode === 'config') await configure(rl, cfg);
    if (mode === 'config') return;

    try {
      const s = await siteStatus(cfg);
      log(`Site ok (${cfg.site}) - fase: ${s.phase}${s.dataset ? `, ${s.dataset.partidas} partidas no servidor` : ''}`);
      if (s.adminRequired && !cfg.token) log('AVISO: o site exige token e nenhum foi configurado (rode "config").');
    } catch (e) {
      log(`Nao consegui acessar ${cfg.site} (${e.message}). Confira a URL com "config". As paginas serao salvas localmente mesmo assim.`);
    }

    if (!['completa', 'atuais', 'reenviar', 'reenviar-tudo'].includes(mode)) {
      console.log(`
  1) Coleta completa  - ${cfg.majors} ultimas Majors + ${cfg.diasRecentes} dias de resultados + ranking e stats atuais
  2) So dados atuais  - ranking, stats dos jogadores e resultados recentes (rapido, ideal antes de cada Major)
  3) Reenviar         - envia ao site o que ja foi coletado e ainda nao subiu
  4) Configurar       - URL do site, token, intervalo, navegador
  0) Sair`);
      const op = (await rl.question('\nEscolha: ')).trim();
      mode = { 1: 'completa', 2: 'atuais', 3: 'reenviar', 4: 'config' }[op] || 'sair';
      if (mode === 'config') { await configure(rl, cfg); return; }
      if (mode === 'sair') return;
    }
    rl.close();

    if (mode === 'reenviar' || mode === 'reenviar-tudo') await resend(cfg, mode === 'reenviar-tudo');
    else {
      console.log('\nUma janela do navegador vai abrir. Nao feche. Se aparecer a verificacao da Cloudflare, resolva-a e o coletor continua sozinho.');
      console.log('Pode interromper com Ctrl+C a qualquer momento: o progresso fica salvo.\n');
      await collect(cfg, mode);
    }
  } catch (e) {
    log(`ERRO: ${e.message}`);
    process.exitCode = 1;
  } finally {
    rl.close();
  }
}

main();
