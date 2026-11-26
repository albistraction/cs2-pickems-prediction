// Modelo estatístico (sem IA): Elo por mapa + fatores de time e de jogadores
// + prior do ranking HLTV + veto + Monte Carlo do Swiss/Playoffs.
#pragma once
#include <atomic>
#include <climits>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data.hpp"

struct Params {
    // ---- treináveis (busca por coordenadas, validação walk-forward)
    double k = 32;              // K do rating geral
    double kmap = 20;           // K do offset por mapa
    double margin = 0.5;        // peso da margem de rounds
    double tau = 200;           // dias para "esquecer" (decay exponencial)
    double roster = 0.12;       // regressão à média por jogador trocado
    double wRating = 300;       // Elo por +1.00 de rating médio do time
    double wKD = 200;           // Elo por +1.00 de K/D médio
    double wADR = 150;          // Elo por +100% de ADR (relativo a 75)
    double wKAST = 100;         // Elo por +100% de KAST (relativo a 72%)
    double gamma = 0.3;         // ajuste das stats individuais pela força do adversário
    double playerAlpha = 0.15;  // velocidade da média móvel dos jogadores
    double kForm = 10;          // K da forma recente (Elo de curto prazo)
    double tauForm = 30;        // dias de memória da forma
    double kH2H = 5;            // K do confronto direto
    double wRust = -10;         // Elo por ln(1 + dias sem jogar)
    double wSyn = 40;           // Elo de entrosamento (lineup estável vs. recém-trocada)
    double wRank = 60;          // Elo por unidade de ln(posição) no ranking HLTV da época
    // ---- fixos
    double tierS = 1.0, tierA = 0.85, tierB = 0.65, tierC = 0.45;
    double priorMaps = 20;      // nº de mapas em que Elo e ranking HLTV pesam igual
    double hltvScale = 90;      // Elo por unidade de ln(pontos HLTV) (só sem ranking histórico)
    double rankMaxAge = 70;     // ranking mais velho que isso (dias) não é usado
    double sigmaFloor = 25;     // incerteza mínima por simulação
    double sigmaNew = 90;       // incerteza extra para time com poucos mapas
    int sims = 50000;

    double tierWeight(const std::string& t) const;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Params, k, kmap, margin, tau, roster, wRating, wKD, wADR, wKAST, gamma,
                                                playerAlpha, kForm, tauForm, kH2H, wRust, wSyn, wRank, tierS, tierA,
                                                tierB, tierC, priorMaps, hltvScale, rankMaxAge, sigmaFloor, sigmaNew,
                                                sims)

struct Metrics {
    double logloss = 0, brier = 0, acc = 0;
    int count = 0;
    std::vector<std::vector<double>> calib;  // [prob prevista média, frequência real, nº de mapas] por faixa
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Metrics, logloss, brier, acc, count, calib)

struct PlayerState {
    double rating = 1.0, kd = 1.0, adr = 75, kast = 72, n = 0;
};

// Médias (encolhidas) das stats da lineup
struct PlayerAgg {
    double rating = 1.0, kd = 1.0, adr = 75, kast = 72;
};

struct TeamState {
    double r = 1500, n = 0;
    double form = 0;                     // Elo de curto prazo
    int last = -1;                       // até quando o decay foi aplicado
    int lastPlayed = -1;                 // último dia com partida
    int lastChange = -1000000;           // último dia com troca de lineup
    std::map<std::string, double> off;   // offset por mapa (centrado em 0)
    std::vector<std::string> lineup;     // nicks da última partida
    std::string display;
};

// Rankings HLTV por data, indexados por teamKey (compartilhado entre cópias do Engine)
struct RankIndex {
    std::vector<int> days;                                  // ordenado
    std::vector<std::unordered_map<std::string, int>> pos;  // chave -> posição
    std::vector<int> size;                                  // nº de times no ranking
    static std::shared_ptr<const RankIndex> build(const std::vector<RankSnapshot>& h);
};

class Engine {
public:
    std::shared_ptr<const RankIndex> RK;  // vazio = sem ranking histórico
    Params P;
    std::unordered_map<std::string, TeamState> T;     // chave: teamKey
    std::unordered_map<std::string, PlayerState> PL;  // chave: playerKey
    std::unordered_map<std::string, std::pair<double, int>> H2;  // "a|b" -> (valor p/ a, último dia)
    int lastDay = 0;

    explicit Engine(const Params& p = Params()) : P(p) {}

    void decayTo(TeamState& s, int day) const;
    static double offset(const TeamState& s, const std::string& map);
    // stats do jogador encolhidas para a média quando há poucos jogos
    void playerShrunk(const std::string& nick, PlayerState& out) const;
    PlayerAgg playerAgg(const std::vector<std::string>& lineup) const;
    double playerBonus(const PlayerAgg& a) const;
    double restScore(const TeamState& s, int day) const;   // ln(1 + dias sem jogar)
    double stability(const TeamState& s, int day) const;   // 0 = lineup nova, 1 = estável
    double h2h(const std::string& ka, const std::string& kb, int day) const;  // Elo extra para ka contra kb
    // -ln(posição) no último ranking publicado ANTES de `day`; false se não houver ranking recente
    bool rankScore(const std::string& key, int day, double& out, int* position = nullptr) const;
    bool hasRankHistory() const { return RK && RK->days.size() >= 4; }

    // Processa partidas em ordem cronológica. Pontua (log-loss) apenas as
    // partidas de índice [scoreFrom, scoreTo) — cada mapa é previsto ANTES da atualização.
    // `cursor` (opcional): começa no índice *cursor e devolve onde parou (para rodar em etapas).
    Metrics run(const std::vector<Match>& ms, size_t scoreFrom, size_t scoreTo, int untilDay = INT_MAX,
                size_t* cursor = nullptr);

private:
    void lineupChange(TeamState& s, const std::vector<std::string>& lineup, int day);
    void updatePlayer(const PlayerLine& p, double oppRating);
    void h2hAdd(const std::string& ka, const std::string& kb, double v, int day);
};

// Estágio Swiss de uma Major passada, reconstruído das partidas (para o backtest de Pick'Em)
struct BacktestStage {
    std::string name;                        // "IEM Cologne Major 2026 · Stage 1"
    int day = 0;                             // dia do primeiro jogo
    std::vector<std::string> teams;          // 16 times
    std::vector<std::pair<int, int>> round1; // confrontos reais da 1ª rodada (índices em teams)
    std::vector<int> cat;                    // resultado real: 0=3-0 1=3-1 2=3-2 3=2-3 4=1-3 5=0-3
    std::vector<std::string> pool;           // mapas jogados no estágio
};
std::vector<BacktestStage> buildBacktestStages(const Dataset& ds);

// Para cada estágio: estado do modelo só com partidas ANTERIORES, simulação, picks e acertos reais.
struct BacktestResult {
    double meanHits = 0;   // acertos médios (de 10)
    double rate5 = 0;      // fração de estágios com >= 5 acertos
    double meanP5 = 0;     // P(>=5) média prevista
    nlohmann::json detail = nlohmann::json::array();
};
BacktestResult backtestPickems(const Params& p, const std::shared_ptr<const RankIndex>& rk, const std::vector<Match>& ms,
                               const std::vector<BacktestStage>& stages, int sims, uint64_t seed);

struct TrainReport {
    Params params;
    Metrics before, after, eloOnly;
    size_t trainMatches = 0, validMatches = 0;
    std::vector<std::string> steps;
    nlohmann::json ablation = nlohmann::json::array();  // importância de cada fator
    nlohmann::json backtest;                             // Pick'Em nas Majors passadas
    nlohmann::json restarts = nlohmann::json::array();   // cada rodada do treino
    nlohmann::json toJson() const;
};

// Treina várias vezes (pontos de partida diferentes) e escolhe a rodada que mais acerta o
// Pick'Em das Majors passadas, sem piorar a previsão dos jogos.
TrainReport train(const Dataset& ds, const Params& start, int restarts,
                  const std::function<void(const std::string&)>& log, const std::atomic<bool>& stop);

// Previsão de um estágio. Com {"event", "stage"} usa a estrutura real da Major
// (seeds, confrontos, resultados já jogados) e encadeia os estágios anteriores para
// preencher vagas TBD. Sem "event", usa a lista manual {"format", "teams"}.
// Lança std::runtime_error em entrada inválida.
nlohmann::json predictEvent(const Engine& trained, const Dataset& data, const nlohmann::json& req);

// Majors conhecidas com estágios, status, times e jogos.
nlohmann::json eventsSummary(const Dataset& data);

// Comparação detalhada de dois times: fatores, jogadores, mapas, veto, H2H, forma.
nlohmann::json compareTeams(const Engine& trained, const Dataset& data, const nlohmann::json& req);

// Visão geral dos times (ranking + Elo + jogadores)
nlohmann::json teamsOverview(const Engine& trained, const Dataset& data, int limit);

// Dataset sintético para testar o deploy
nlohmann::json makeDemoImport();
