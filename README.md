# pickems-prediction

Site que prevê o **Pick'Em das Majors de CS2** com um modelo estatístico em C++, **sem IA**:

```
 SEU PC                                      RAILWAY
 extensão no seu Chrome (+ você)            backend C++
 HLTV ──► páginas HTML ──── upload ────►  cura dos dados ──► treino ──► modelo
          (cache local)                                                   │
                                              site ◄── /api/predict (Monte Carlo)
```

## Como o modelo funciona

| Componente | O que faz |
|---|---|
| **Elo por mapa** | Cada mapa atualiza a força geral do time e um *offset* por mapa. Considera tier do evento, margem de rounds (MR12/MR15), esquecimento temporal e trocas de roster (detectadas pela lineup). |
| **Índice de jogadores** | Média móvel do **rating HLTV** e do **K/D** de cada jogador, partida a partida, encolhida para a média quando há poucos jogos. Para a previsão, mistura com as stats dos últimos 90 dias. A soma do time vira um bônus em Elo (peso β treinado). |
| **Prior do ranking HLTV** | Os pontos do ranking entram como prior, com mais peso para times com poucos mapas no histórico. |
| **Veto** | Cada time pica o mapa onde tem mais vantagem e bane onde tem menos → probabilidade exata da série Bo1/Bo3/Bo5. |
| **Monte Carlo** | Swiss com as regras da Valve (1×9 na rodada 1, Buchholz, sem revanche, Bo3 em jogos de classificação/eliminação) e Playoffs. Cada simulação sorteia ruído na força de cada time, proporcional à incerteza. |
| **Otimizador do Pick'Em** | Escolhe 2× 3-0, 2× 0-3 e 6× avança maximizando **P(≥ 5 acertos)**, considerando a correlação entre os resultados. |
| **Treino** | Busca por coordenadas em 8 parâmetros, minimizando o log-loss *walk-forward*: cada mapa é previsto antes de ser usado. A validação usa os 20% de partidas mais recentes. O site mostra o ganho em relação ao Elo puro, para você ver quanto as stats individuais ajudam. |

### Cura dos dados
- Unifica nomes de times ("Natus Vincere" = "NAVI", "Team Vitality" = "Vitality"…) e de mapas ("de_dust2" = "Dust II").
- Remove duplicatas, W.O., showmatches, partidas sem data ou com data no futuro, mapas não jogados e placares impossíveis.
- Valida as linhas dos jogadores (K/D, ADR, KAST e rating fora da faixa) e as lineups incompletas.
- Marca as partidas das 10 últimas Majors, juntando os estágios separados de cada uma.
- Define o tier pelas estrelas da HLTV.

Tudo isso sai num relatório na aba **Dados**.

## Coleta dos dados: extensão do navegador (`coletor-extensao/`)

A HLTV usa proteção anti-bot (Cloudflare). Nos testes, servidores, curl e navegadores automatizados (Playwright) foram bloqueados: em navegador automatizado, a verificação "sou humano" fica em loop. Por isso a coleta roda **no seu próprio Chrome/Edge, com você presente**, por uma extensão:

1. Abra `chrome://extensions` (ou `edge://extensions`), ligue o **Modo do desenvolvedor** e clique em **Carregar sem compactação**. Escolha a pasta `coletor-extensao`.
2. Clique no ícone da extensão para abrir o painel. Informe a URL do site e o `ADMIN_TOKEN` e clique em **Salvar**; o navegador pede permissão para acessar o site.
3. Escolha:
   - **Coleta completa**: as 10 últimas Majors (juntando os estágios), resultados recentes, ranking e stats. Use na primeira vez e depois uma vez por ano. Leva horas.
   - **Só dados atuais**: ranking, stats dos últimos 90 dias e resultados recentes. Use antes de cada Major.

Uma janela separada navega sozinha, com uma página a cada ~8 s e pausas aleatórias. **Quando aparecer a verificação da Cloudflare, o painel apita e você resolve na janela.** A extensão não tenta resolver o CAPTCHA. Cada página vai direto para `/api/import/html`, e no fim a extensão pede ao servidor para curar e treinar. **Parar** interrompe; a próxima coleta continua de onde parou.

> Isso vai contra os termos de uso da HLTV. Se aparecer "you have been blocked", o IP foi bloqueado temporariamente: espere e aumente o intervalo.

### Importação manual (alternativa)
Páginas salvas no navegador com **Ctrl+S → "Página da Web, somente HTML"** podem ser enviadas na aba **Dados**. O tipo de página é detectado automaticamente: partida, resultados, ranking, stats ou arquivo de Majors.

Também dá para importar JSON (`POST /api/import/json`):
```json
{
  "matches": [{
    "id": "2380001", "date": "2026-06-20", "event": "IEM Cologne Major 2026", "stars": 5,
    "team1": "Vitality", "team2": "MOUZ",
    "maps": [{"map": "Nuke", "r1": 13, "r2": 9}, {"map": "Mirage", "r1": 13, "r2": 11}],
    "p1": [{"name": "ZywOo", "kills": 41, "deaths": 27, "adr": 98.1, "kast": 78.0, "rating": 1.45}],
    "p2": [{"name": "torzsi", "kills": 30, "deaths": 33, "adr": 75.2, "kast": 69.0, "rating": 1.02}]
  }],
  "ranking": [{"name": "Vitality", "rank": 1, "points": 1000, "lineup": ["apEX", "ZywOo", "ropz", "flameZ", "mezii"]}],
  "players": [{"name": "ZywOo", "team": "Vitality", "maps": 60, "kd": 1.38, "rating": 1.32}]
}
```

> Os parsers seguem a estrutura conhecida das páginas da HLTV. Se o site mudar o HTML, o relatório da cura mostra páginas com `falhaDeParse`, e basta ajustar os marcadores em `src/hltv.cpp`.

## Deploy no Railway

1. Suba esta pasta para um repositório no GitHub.
2. No Railway: **New Project → Deploy from GitHub repo**. O `railway.json` já aponta para o `Dockerfile`.
3. **Crie um Volume** montado em **`/data`** (botão direito no serviço → *Attach Volume*). Sem volume, o cache e o modelo treinado se perdem a cada deploy.
4. Variáveis (aba *Variables*):

| Variável | Padrão | Para quê |
|---|---|---|
| `ADMIN_TOKEN` | — | **Recomendado.** Protege coleta, importação e treino. O site pede o token na aba Dados. |
| `MAJORS` | `10` | Quantas Majors usar no treino |
| `RECENT_DAYS` | `120` | Janela de resultados recentes (forma atual) |
| `AUTO_COLLECT` | `0` | `1` faz o *servidor* tentar coletar direto da HLTV na 1ª execução (normalmente é bloqueado) |
| `REFRESH_HOURS` | `0` | coleta periódica pelo servidor (idem) |
| `HLTV_DELAY_MS` | `3500` | intervalo das requisições do servidor |

`PORT` é definido pelo próprio Railway.

5. **Settings → Networking → Generate Domain** para ter a URL pública.

Na primeira execução o site mostra **"Aguardando dados"**. Rode a **Coleta completa** na extensão: as páginas chegam ao servidor e o treino começa sozinho no final. O progresso aparece na aba **Dados**. Para testar o deploy antes disso, use **Carregar dados demo** (times fictícios "Demo 01…") e depois **Remover dados demo**.

## Rodar localmente

Com Docker:
```bash
docker build -t pickems-prediction .
docker run -p 8080:8080 -v ./data:/data pickems-prediction
```
Sem Docker (Linux/macOS/MSYS2, precisa de g++ ≥ 9, CMake, OpenSSL e zlib):
```bash
cmake -S . -B build && cmake --build build -j
DATA_DIR=./data PUBLIC_DIR=./public ./build/pickems-prediction
```
Acesse http://localhost:8080.

## API

| Rota | Descrição |
|---|---|
| `GET /api/health` | healthcheck |
| `GET /api/status` | fase do pipeline, progresso, logs, métricas do treino, relatório da cura |
| `GET /api/teams?limit=40` | ranking + Elo + jogadores (rating/K/D) + força por mapa |
| `POST /api/predict` | previsão (corpo abaixo) |
| `POST /api/pipeline` 🔒 | `{"collect":true,"forceRefresh":false}`. Com `collect:false`, só cura e treina |
| `POST /api/pipeline/stop` 🔒 | interrompe |
| `POST /api/import/html` 🔒 | corpo = HTML de uma página da HLTV |
| `POST /api/import/json` 🔒 | corpo = JSON no formato acima |
| `POST /api/demo` 🔒 | `{}` carrega a demo, `{"remove":true}` remove |

🔒 = exige o header `X-Admin-Token` se `ADMIN_TOKEN` estiver definido.

Exemplo de `/api/predict`:
```json
{
  "format": "swiss",
  "boRule": "valve",
  "sims": 50000,
  "pool": ["ancient", "anubis", "dust2", "inferno", "mirage", "nuke", "train"],
  "teams": [{"name": "Vitality", "seed": 1}, {"name": "MOUZ", "seed": 2}],
  "scenario": {
    "adjust":  [{"team": "FaZe", "delta": -50}],
    "lineups": {"FaZe": ["karrigan", "rain", "frozen", "broky", "standin"]},
    "mapadj":  [{"team": "Spirit", "map": "nuke", "delta": -150}],
    "force":   [{"a": "Vitality", "b": "MOUZ", "winner": "MOUZ"}]
  }
}
```
O exemplo mostra só 2 times para ficar curto; o Swiss exige 16 e os Playoffs, 8.

## Estrutura

```
coletor-extensao/  extensão Chrome/Edge: navega na HLTV no seu navegador e envia ao site
src/main.cpp       servidor HTTP, pipeline em background
src/hltv.cpp       parsers das páginas + coletor (cache, limite, detecção de bloqueio)
src/curation.cpp   cura dos dados + relatório
src/model.cpp      Elo por mapa, índice de jogadores, treino, veto, Monte Carlo, otimizador
public/            site (HTML/CSS/JS puro)
third_party/       cpp-httplib 0.18.3, nlohmann/json 3.11.3 (header-only)
```
