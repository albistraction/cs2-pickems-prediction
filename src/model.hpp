// Modelo estatístico (sem IA): Elo por mapa + índice individual dos jogadores
// + prior do ranking HLTV + veto + Monte Carlo do Swiss/Playoffs.
#pragma once
#include <atomic>
#include <climits>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "data.hpp"

struct Params {
    // ---- treináveis
    double k = 32;            // K do rating geral
    double kmap = 20;         // K do offset por mapa
    double margin = 0.5;      // peso da margem de rounds
    double tau = 200;         // dias para "esquecer" (decay exponencial)
    double roster = 0.12;     // regressão à média por jogador trocado
    double beta = 500;        // pontos Elo por unidade do índice de jogadores
    double kdMix = 0.4;       // 0 = só rating HLTV, 1 = só K/D
    double playerAlpha = 0.15;  // velocidade da média móvel dos jogadores
    // ---- fixos
    double tierS = 1.0, tierA = 0.85, tierB = 0.65, tierC = 0.45;
    double priorMaps = 20;    // nº de mapas em que Elo e ranking HLTV pesam igual
    double hltvScale = 90;    // Elo por unidade de ln(pontos HLTV)
    double sigmaFloor = 25;   // incerteza mínima por simulação
    double sigmaNew = 90;     // incerteza extra para time com poucos mapas
    int sims = 50000;

    double tierWeight(const std::string& t) const;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Params, k, kmap, margin, tau, roster, beta, kdMix, playerAlpha,
                                                tierS, tierA, tierB, tierC, priorMaps, hltvScale, sigmaFloor,
                                                sigmaNew, sims)

struct Metrics {
    double logloss = 0, brier = 0, acc = 0;
    int count = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Metrics, logloss, brier, acc, count)

struct PlayerState {
    double rating = 1.0, kd = 1.0, n = 0;
};

struct TeamState {
    double r = 1500, n = 0;
    int last = -1;
    std::map<std::string, double> off;   // offset por mapa (centrado em 0)
    std::vector<std::string> lineup;     // nicks da última partida
    std::string display;
};

class Engine {
public:
    Params P;
    std::unordered_map<std::string, TeamState> T;     // chave: teamKey
    std::unordered_map<std::string, PlayerState> PL;  // chave: playerKey
    int lastDay = 0;

    explicit Engine(const Params& p = Params()) : P(p) {}

    void decayTo(TeamState& s, int day) const;
    static double offset(const TeamState& s, const std::string& map);
    // rating e K/D do jogador encolhidos para a média (1.0) quando há poucos jogos
    void playerShrunk(const std::string& nick, double& rating, double& kd, double& n) const;
    double composite(double rating, double kd) const { return (1 - P.kdMix) * (rating - 1) + P.kdMix * (kd - 1); }
    double playerIndex(const std::vector<std::string>& lineup) const;

    // Processa partidas em ordem cronológica. Pontua (log-loss) apenas as
    // partidas de índice [scoreFrom, scoreTo) — cada mapa é previsto ANTES da atualização.
    Metrics run(const std::vector<Match>& ms, size_t scoreFrom, size_t scoreTo, int untilDay = INT_MAX);

private:
    void lineupChange(TeamState& s, const std::vector<std::string>& lineup);
    void updatePlayer(const PlayerLine& p);
};

struct TrainReport {
    Params params;
    Metrics before, after, eloOnly;
    size_t trainMatches = 0, validMatches = 0;
    std::vector<std::string> steps;
    nlohmann::json toJson() const;
};

TrainReport train(const std::vector<Match>& ms, const Params& start,
                  const std::function<void(const std::string&)>& log, const std::atomic<bool>& stop);

// Previsão de um estágio. Com {"event", "stage"} usa a estrutura real da Major
// (seeds, confrontos, resultados já jogados) e encadeia os estágios anteriores para
// preencher vagas TBD. Sem "event", usa a lista manual {"format", "teams"}.
// Lança std::runtime_error em entrada inválida.
nlohmann::json predictEvent(const Engine& trained, const Dataset& data, const nlohmann::json& req);

// Majors conhecidas com estágios, status, times e jogos.
nlohmann::json eventsSummary(const Dataset& data);

// Visão geral dos times (ranking + Elo + jogadores)
nlohmann::json teamsOverview(const Engine& trained, const Dataset& data, int limit);

// Dataset sintético para testar o deploy
nlohmann::json makeDemoImport();
