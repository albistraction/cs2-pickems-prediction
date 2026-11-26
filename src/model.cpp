#include "model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>

#include "curation.hpp"
#include "util.hpp"

using json = nlohmann::json;

namespace {

double elo2p(double d) { return 1.0 / (1.0 + std::pow(10.0, -d / 400.0)); }
const int CS2_RELEASE_DAY = util::daysFromCivil(2023, 9, 27);  // MR12 a partir daqui
const double ADR_BASE = 75.0, KAST_BASE = 72.0;

void recenter(TeamState& s) {
    if (s.off.size() < 2) return;
    double mean = 0;
    for (auto& o : s.off) mean += o.second;
    mean /= static_cast<double>(s.off.size());
    for (auto& o : s.off) o.second -= mean;
}

std::string h2hKey(const std::string& a, const std::string& b) { return a < b ? a + "|" + b : b + "|" + a; }

const std::vector<std::string> DEFAULT_POOL = {"ancient", "anubis", "dust2", "inferno", "mirage", "nuke", "train"};

}  // namespace

// ============================================================================
// Params / Engine
// ============================================================================
double Params::tierWeight(const std::string& t) const {
    char c = t.empty() ? 'B' : static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    switch (c) {
        case 'S': return tierS;
        case 'A': return tierA;
        case 'B': return tierB;
        default: return tierC;
    }
}

void Engine::decayTo(TeamState& s, int day) const {
    if (s.last >= 0 && day > s.last) {
        double dt = day - s.last;
        double f = std::exp(-dt / P.tau);
        s.r = 1500 + (s.r - 1500) * f;
        for (auto& o : s.off) o.second *= f;
        s.n *= f;
        s.form *= std::exp(-dt / std::max(1.0, P.tauForm));
    }
    if (day > s.last) s.last = day;
}

double Engine::offset(const TeamState& s, const std::string& map) {
    auto it = s.off.find(map);
    return it == s.off.end() ? 0.0 : it->second;
}

void Engine::playerShrunk(const std::string& nick, PlayerState& out) const {
    out = PlayerState();
    auto it = PL.find(playerKey(nick));
    if (it == PL.end()) return;
    const PlayerState& p = it->second;
    const double n0 = 4.0;  // "jogos fantasma" na média
    auto sh = [&](double x, double prior) { return (p.n * x + n0 * prior) / (p.n + n0); };
    out.rating = sh(p.rating, 1.0);
    out.kd = sh(p.kd, 1.0);
    out.adr = sh(p.adr, ADR_BASE);
    out.kast = sh(p.kast, KAST_BASE);
    out.n = p.n;
}

PlayerAgg Engine::playerAgg(const std::vector<std::string>& lineup) const {
    PlayerAgg a;
    if (lineup.empty()) return a;
    a.rating = a.kd = a.adr = a.kast = 0;
    for (auto& nick : lineup) {
        PlayerState ps;
        playerShrunk(nick, ps);
        a.rating += ps.rating;
        a.kd += ps.kd;
        a.adr += ps.adr;
        a.kast += ps.kast;
    }
    double n = static_cast<double>(lineup.size());
    a.rating /= n;
    a.kd /= n;
    a.adr /= n;
    a.kast /= n;
    return a;
}

double Engine::playerBonus(const PlayerAgg& a) const {
    return P.wRating * (a.rating - 1) + P.wKD * (a.kd - 1) + P.wADR * (a.adr / ADR_BASE - 1) +
           P.wKAST * (a.kast / KAST_BASE - 1);
}

double Engine::restScore(const TeamState& s, int day) const {
    int d = s.lastPlayed < 0 ? 30 : std::max(0, day - s.lastPlayed);
    return std::log1p(static_cast<double>(d));
}

double Engine::stability(const TeamState& s, int day) const {
    double d = std::max(0, day - s.lastChange);
    return 1.0 - std::exp(-d / 45.0);
}

double Engine::h2h(const std::string& ka, const std::string& kb, int day) const {
    auto it = H2.find(h2hKey(ka, kb));
    if (it == H2.end()) return 0;
    double v = it->second.first * std::exp(-std::max(0, day - it->second.second) / P.tau);
    return ka < kb ? v : -v;
}

std::shared_ptr<const RankIndex> RankIndex::build(const std::vector<RankSnapshot>& h) {
    auto idx = std::make_shared<RankIndex>();
    std::vector<const RankSnapshot*> v;
    for (auto& s : h)
        if (s.day > 0 && !s.teams.empty()) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](const RankSnapshot* a, const RankSnapshot* b) { return a->day < b->day; });
    for (auto* s : v) {
        std::unordered_map<std::string, int> m;
        int mx = 0;
        for (auto& t : s->teams) {
            std::string k = teamKey(t.name);
            if (k.empty() || t.rank <= 0) continue;
            auto it = m.find(k);
            if (it == m.end() || t.rank < it->second) m[k] = t.rank;
            mx = std::max(mx, t.rank);
        }
        if (m.empty()) continue;
        if (!idx->days.empty() && idx->days.back() == s->day) {
            idx->pos.back() = std::move(m);
            idx->size.back() = mx;
        } else {
            idx->days.push_back(s->day);
            idx->pos.push_back(std::move(m));
            idx->size.push_back(mx);
        }
    }
    return idx;
}

bool Engine::rankScore(const std::string& key, int day, double& out, int* position) const {
    if (!RK || RK->days.empty()) return false;
    auto it = std::lower_bound(RK->days.begin(), RK->days.end(), day);  // primeiro >= day
    if (it == RK->days.begin()) return false;
    size_t i = static_cast<size_t>(it - RK->days.begin()) - 1;          // último < day
    if (day - RK->days[i] > P.rankMaxAge) return false;
    auto f = RK->pos[i].find(key);
    // fora do ranking: um pouco abaixo do último colocado
    int r = f != RK->pos[i].end() ? f->second : static_cast<int>(std::max(30.0, RK->size[i] * 1.25));
    if (position) *position = f != RK->pos[i].end() ? f->second : 0;
    out = -std::log(static_cast<double>(r));
    return true;
}

void Engine::h2hAdd(const std::string& ka, const std::string& kb, double v, int day) {
    auto& e = H2[h2hKey(ka, kb)];
    double cur = e.first * std::exp(-std::max(0, day - e.second) / P.tau);
    e.first = cur + (ka < kb ? v : -v);
    e.second = day;
}

void Engine::lineupChange(TeamState& s, const std::vector<std::string>& lineup, int day) {
    if (lineup.size() < 4) return;
    if (s.lineup.size() >= 4) {
        std::set<std::string> prev;
        for (auto& p : s.lineup) prev.insert(playerKey(p));
        int overlap = 0;
        for (auto& p : lineup) overlap += static_cast<int>(prev.count(playerKey(p)));
        int changes = static_cast<int>(std::min<size_t>(5, lineup.size())) - overlap;
        if (changes > 0) {
            double f = 1.0 - std::min(1.0, P.roster * changes);
            s.r = 1500 + (s.r - 1500) * f;
            for (auto& o : s.off) o.second *= f;
            s.n *= f;
            s.form *= f;
            s.lastChange = day;
        }
    }
    s.lineup = lineup;
}

void Engine::updatePlayer(const PlayerLine& p, double oppRating) {
    auto& ps = PL[playerKey(p.name)];
    // stat contra adversário forte vale mais; contra fraco, menos
    double f = std::clamp(1.0 + P.gamma * (oppRating - 1500) / 400.0, 0.5, 1.5);
    double kd = std::clamp(p.kills / static_cast<double>(std::max(1, p.deaths)), 0.1, 4.0) * f;
    double w = std::max(P.playerAlpha, 1.0 / (ps.n + 1.0));
    ps.kd += w * (kd - ps.kd);
    if (p.rating > 0) ps.rating += w * (std::clamp(p.rating, 0.1, 3.0) * f - ps.rating);
    else ps.rating += 0.5 * w * (kd - ps.rating);  // sem rating: K/D como aproximação
    if (p.adr > 0) ps.adr += w * (std::clamp(p.adr, 10.0, 250.0) * f - ps.adr);
    if (p.kast > 0) ps.kast += w * (std::min(100.0, std::clamp(p.kast, 10.0, 100.0) * f) - ps.kast);
    ps.n += 1;
}

Metrics Engine::run(const std::vector<Match>& ms, size_t scoreFrom, size_t scoreTo, int untilDay, size_t* cursor) {
    Metrics sc;
    std::vector<std::array<double, 3>> bins(10, std::array<double, 3>{0, 0, 0});
    size_t i = cursor ? *cursor : 0;
    for (; i < ms.size(); ++i) {
        const Match& m = ms[i];
        if (m.day >= untilDay) break;
        std::string ka = teamKey(m.team1), kb = teamKey(m.team2);
        if (ka.empty() || kb.empty() || ka == kb) continue;
        TeamState& A = T[ka];
        TeamState& B = T[kb];
        A.display = m.team1;
        B.display = m.team2;
        decayTo(A, m.day);
        decayTo(B, m.day);

        std::vector<std::string> la, lb;
        for (auto& p : m.p1) la.push_back(p.name);
        for (auto& p : m.p2) lb.push_back(p.name);
        lineupChange(A, la, m.day);
        lineupChange(B, lb, m.day);
        PlayerAgg ga = playerAgg(la.empty() ? A.lineup : la);
        PlayerAgg gb = playerAgg(lb.empty() ? B.lineup : lb);
        double extra = playerBonus(ga) - playerBonus(gb) + P.wRust * (restScore(A, m.day) - restScore(B, m.day)) +
                       P.wSyn * (stability(A, m.day) - stability(B, m.day));
        double rsA = 0, rsB = 0;
        if (P.wRank != 0 && rankScore(ka, m.day, rsA) && rankScore(kb, m.day, rsB)) extra += P.wRank * (rsA - rsB);
        double preA = A.r, preB = B.r;

        bool scoring = i >= scoreFrom && i < scoreTo;
        double tw = P.tierWeight(m.tier);
        double target = m.day >= CS2_RELEASE_DAY ? 13.0 : 16.0;
        for (auto& mp : m.maps) {
            if (mp.r1 == mp.r2) continue;
            bool experienced = A.n >= 5 && B.n >= 5;
            double diff = A.r + offset(A, mp.map) + A.form - B.r - offset(B, mp.map) - B.form + h2h(ka, kb, m.day) + extra;
            double p = elo2p(diff);
            double y = mp.r1 > mp.r2 ? 1.0 : 0.0;
            if (scoring && experienced) {
                double pc = std::min(std::max(p, 1e-6), 1 - 1e-6);
                sc.logloss += -(y * std::log(pc) + (1 - y) * std::log(1 - pc));
                sc.brier += (p - y) * (p - y);
                sc.acc += ((p > 0.5) == (y > 0.5)) ? 1 : 0;
                sc.count++;
                auto& b = bins[std::min(9, static_cast<int>(p * 10))];
                b[0] += p;
                b[1] += y;
                b[2] += 1;
            }
            double mult = 1.0 + P.margin * std::min(1.0, std::abs(mp.r1 - mp.r2) / target);
            double d = tw * mult * (y - p);
            A.r += P.k * d;
            B.r -= P.k * d;
            A.off[mp.map] += P.kmap * d;
            B.off[mp.map] -= P.kmap * d;
            recenter(A);
            recenter(B);
            A.form += P.kForm * d;
            B.form -= P.kForm * d;
            h2hAdd(ka, kb, P.kH2H * d, m.day);
            A.n += 1;
            B.n += 1;
        }
        for (auto& p : m.p1) updatePlayer(p, preB);
        for (auto& p : m.p2) updatePlayer(p, preA);
        A.lastPlayed = m.day;
        B.lastPlayed = m.day;
        lastDay = std::max(lastDay, m.day);
    }
    if (cursor) *cursor = i;
    if (sc.count) {
        sc.logloss /= sc.count;
        sc.brier /= sc.count;
        sc.acc /= sc.count;
    }
    for (auto& b : bins)
        if (b[2] > 0) sc.calib.push_back({b[0] / b[2], b[1] / b[2], b[2]});
    return sc;
}

// ============================================================================
// Treino: busca por coordenadas, log-loss walk-forward + importância dos fatores
// ============================================================================
json TrainReport::toJson() const {
    return json{{"params", params},
                {"antes", before},
                {"depois", after},
                {"semJogadores", eloOnly},
                {"chute5050", 0.693147},
                {"partidasTreino", trainMatches},
                {"partidasValidacao", validMatches},
                {"importancia", ablation},
                {"backtest", backtest},
                {"rodadas", restarts},
                {"passos", steps}};
}

TrainReport train(const Dataset& ds, const Params& start, int restarts,
                  const std::function<void(const std::string&)>& log, const std::atomic<bool>& stop) {
    const std::vector<Match>& ms = ds.matches;
    TrainReport R;
    R.params = start;
    auto rk = RankIndex::build(ds.rankHistory);
    auto stages = buildBacktestStages(ds);
    log("Rankings historicos disponiveis para o treino: " + std::to_string(rk->days.size()));
    log("Estagios Swiss de Majors passadas para o backtest de Pick'Em: " + std::to_string(stages.size()));
    size_t split = ms.size() * 8 / 10;
    R.trainMatches = split;
    R.validMatches = ms.size() - split;

    auto evalTrain = [&](const Params& p) {
        Engine e(p);
        e.RK = rk;
        Metrics m = e.run(ms, 0, split);
        return m.count >= 30 ? m.logloss : 1e9;
    };
    auto evalValid = [&](const Params& p) {
        Engine e(p);
        e.RK = rk;
        return e.run(ms, split, ms.size());
    };
    R.before = evalValid(start);

    struct Dim {
        double Params::*v;
        double lo, hi;
        const char* name;
    };
    std::vector<Dim> dims = {
        {&Params::k, 4, 150, "k"},           {&Params::kmap, 0, 100, "kmap"},       {&Params::margin, 0, 3, "margin"},
        {&Params::tau, 20, 1500, "tau"},     {&Params::roster, 0, 0.35, "roster"},  {&Params::wRating, 0, 2000, "wRating"},
        {&Params::wKD, 0, 1500, "wKD"},      {&Params::wADR, 0, 1500, "wADR"},      {&Params::wKAST, 0, 1500, "wKAST"},
        {&Params::gamma, 0, 1, "gamma"},     {&Params::playerAlpha, 0.02, 0.6, "playerAlpha"},
        {&Params::kForm, 0, 60, "kForm"},    {&Params::tauForm, 5, 120, "tauForm"}, {&Params::kH2H, 0, 30, "kH2H"},
        {&Params::wRust, -80, 80, "wRust"},  {&Params::wSyn, 0, 200, "wSyn"}};
    bool useRank = rk->days.size() >= 4;
    if (useRank) dims.push_back({&Params::wRank, 0, 400, "wRank"});

    // busca por coordenadas a partir de `p`
    int evals = 0;
    auto descend = [&](Params p, bool verbose) {
        if (!useRank) p.wRank = 0;
        double best = evalTrain(p);
        double step = 0.25;
        while (step > 0.01 && !stop) {
            bool improved = false;
            for (auto& d : dims) {
                for (int dir : {+1, -1}) {
                    if (stop) break;
                    double old = p.*(d.v);
                    double cand = std::clamp(old + dir * step * (d.hi - d.lo), d.lo, d.hi);
                    if (cand == old) continue;
                    p.*(d.v) = cand;
                    double s = evalTrain(p);
                    ++evals;
                    if (s < best - 1e-7) {
                        best = s;
                        improved = true;
                        if (verbose) {
                            std::string msg = std::string(d.name) + " = " + std::to_string(cand) + "  (log-loss " + std::to_string(s) + ")";
                            R.steps.push_back(msg);
                        }
                    } else {
                        p.*(d.v) = old;
                    }
                }
            }
            if (!improved) step /= 2;
        }
        return p;
    };

    // várias rodadas: a 1ª parte dos parâmetros atuais, as outras de pontos sorteados
    restarts = std::max(1, restarts);
    std::mt19937_64 rng(20261126);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    struct Cand {
        Params p;
        Metrics valid;
        BacktestResult bt;
    };
    std::vector<Cand> cands;
    log("Treino: " + std::to_string(restarts) + " rodadas, " + std::to_string(dims.size()) + " parametros, " +
        std::to_string(split) + " partidas de treino");
    for (int r = 0; r < restarts && !stop; ++r) {
        Params p0 = start;
        if (r > 0)
            for (auto& d : dims) {
                double cur = std::clamp(start.*(d.v), d.lo, d.hi);
                double jump = (U(rng) - 0.5) * 0.6 * (d.hi - d.lo);  // ±30% do intervalo
                p0.*(d.v) = std::clamp(r % 2 ? cur + jump : d.lo + U(rng) * (d.hi - d.lo), d.lo, d.hi);
            }
        Cand c;
        c.p = descend(p0, r == 0);
        c.valid = evalValid(c.p);
        c.bt = stages.empty() ? BacktestResult{} : backtestPickems(c.p, rk, ms, stages, 8000, 1000);  // mesma semente: comparação justa entre rodadas
        log("  rodada " + std::to_string(r + 1) + "/" + std::to_string(restarts) + ": validacao log-loss " +
            std::to_string(c.valid.logloss) + ", acerto " + std::to_string(c.valid.acc * 100).substr(0, 5) + "%" +
            (stages.empty() ? std::string() : ", Pick'Em " + std::to_string(c.bt.meanHits).substr(0, 4) + "/10 em media, " +
                                                  std::to_string(static_cast<int>(std::lround(c.bt.rate5 * 100))) + "% dos estagios com 5+"));
        R.restarts.push_back({{"rodada", r + 1},
                              {"logloss", c.valid.logloss},
                              {"acerto", c.valid.acc},
                              {"pickemMedia", c.bt.meanHits},
                              {"pickem5", c.bt.rate5}});
        cands.push_back(std::move(c));
    }
    if (cands.empty()) {
        R.after = R.before;
        return R;
    }
    // escolha: mais acertos no Pick'Em entre as rodadas que não pioram a previsão dos jogos
    double bestLL = 1e9;
    for (auto& c : cands) bestLL = std::min(bestLL, c.valid.logloss);
    size_t pick = 0;
    double bestScore = -1e9;
    for (size_t i = 0; i < cands.size(); ++i) {
        const auto& c = cands[i];
        if (c.valid.logloss > bestLL + 0.004) continue;
        double sc = stages.empty() ? -c.valid.logloss : c.bt.meanHits + 2.0 * c.bt.rate5 - 50.0 * (c.valid.logloss - bestLL);
        if (sc > bestScore) { bestScore = sc; pick = i; }
    }
    R.params = cands[pick].p;
    R.after = cands[pick].valid;
    for (size_t i = 0; i < R.restarts.size(); ++i) R.restarts[i]["escolhida"] = i == pick;
    log("Escolhida a rodada " + std::to_string(pick + 1));
    Params& P = R.params;

    if (!stages.empty() && !stop) {
        // backtest final com mais simulações, para o relatório
        BacktestResult fin = backtestPickems(P, rk, ms, stages, 30000, 7);
        R.backtest = {{"mediaAcertos", fin.meanHits}, {"estagiosCom5", fin.rate5}, {"p5PrevistaMedia", fin.meanP5},
                      {"estagios", fin.detail}};
        log("Backtest Pick'Em: " + std::to_string(fin.meanHits).substr(0, 4) + " acertos em media, " +
            std::to_string(static_cast<int>(std::lround(fin.rate5 * 100))) + "% dos estagios com 5+ (" +
            std::to_string(stages.size()) + " estagios)");
    }
    {
        Params p0 = P;
        p0.wRating = p0.wKD = p0.wADR = p0.wKAST = 0;
        p0.kForm = p0.kH2H = p0.wRust = p0.wSyn = p0.wRank = 0;
        R.eloOnly = evalValid(p0);
    }

    // importância: tira um fator por vez e mede a piora na validação
    struct Abl {
        const char* name;
        std::function<void(Params&)> off;
    };
    std::vector<Abl> abl = {
        {"Estatisticas dos jogadores (todas)", [](Params& p) { p.wRating = p.wKD = p.wADR = p.wKAST = 0; }},
        {"Rating HLTV dos jogadores", [](Params& p) { p.wRating = 0; }},
        {"K/D dos jogadores", [](Params& p) { p.wKD = 0; }},
        {"ADR dos jogadores", [](Params& p) { p.wADR = 0; }},
        {"KAST dos jogadores", [](Params& p) { p.wKAST = 0; }},
        {"Ajuste pela forca do adversario", [](Params& p) { p.gamma = 0; }},
        {"Forma recente", [](Params& p) { p.kForm = 0; }},
        {"Confronto direto (H2H)", [](Params& p) { p.kH2H = 0; }},
        {"Descanso / inatividade", [](Params& p) { p.wRust = 0; }},
        {"Entrosamento da lineup", [](Params& p) { p.wSyn = 0; }},
        {"Forca por mapa", [](Params& p) { p.kmap = 0; }},
        {"Margem de rounds", [](Params& p) { p.margin = 0; }},
        {"Troca de roster", [](Params& p) { p.roster = 0; }},
        {"Ranking HLTV da epoca", [](Params& p) { p.wRank = 0; }}};
    for (auto& a : abl) {
        if (stop) break;
        Params p0 = P;
        a.off(p0);
        Metrics m = evalValid(p0);
        R.ablation.push_back({{"fator", a.name},
                              {"logloss", m.logloss},
                              {"piora", m.logloss - R.after.logloss},
                              {"acertoSem", m.acc}});
    }
    log("Treino concluido (" + std::to_string(evals) + " avaliacoes). Validacao: log-loss " +
        std::to_string(R.before.logloss) + " -> " + std::to_string(R.after.logloss) + ", acerto " +
        std::to_string(R.after.acc * 100) + "%");
    return R;
}

// ============================================================================
// Veto + série
// ============================================================================
namespace {

struct VetoStep {
    int map;
    bool byA, ban;
};

// Veto guloso: cada time pica onde tem mais vantagem e bane onde tem menos.
// adv[m] = vantagem Elo do time A (que começa o veto) no mapa m.
void vetoOrder(const std::vector<double>& adv, int bo, std::vector<int>& order, std::vector<VetoStep>* steps) {
    int nm = static_cast<int>(adv.size());
    std::vector<bool> avail(nm, true);
    int left = nm;
    order.clear();
    auto choose = [&](bool teamA, bool ban) {
        int best = -1;
        double bestVal = 0;
        for (int m = 0; m < nm; ++m) {
            if (!avail[m]) continue;
            double own = teamA ? adv[m] : -adv[m];
            double val = ban ? -own : own;
            if (best < 0 || val > bestVal) { best = m; bestVal = val; }
        }
        avail[best] = false;
        --left;
        if (steps) steps->push_back({best, teamA, ban});
        return best;
    };
    int picks = bo == 1 ? 0 : bo - 1;
    bool turnA = true;
    for (int i = 0; i < 2 && left - 1 > picks; ++i, turnA = !turnA) choose(turnA, true);
    for (int i = 0; i < picks && left > 1; ++i, turnA = !turnA) order.push_back(choose(turnA, false));
    while (left > 1) { choose(turnA, true); turnA = !turnA; }
    for (int m = 0; m < nm; ++m)
        if (avail[m]) order.push_back(m);
    while (static_cast<int>(order.size()) < bo) order.push_back(order.back());
}

double seriesFromOrder(const std::vector<double>& adv, const std::vector<int>& order, int bo) {
    int need = bo / 2 + 1;
    std::vector<std::vector<double>> dp(need + 1, std::vector<double>(need + 1, 0));
    dp[0][0] = 1;
    double win = 0;
    for (int w = 0; w < need; ++w)
        for (int l = 0; l < need; ++l) {
            double pr = dp[w][l];
            if (pr == 0) continue;
            double p = elo2p(adv[order[w + l]]);
            if (w + 1 == need) win += pr * p; else dp[w + 1][l] += pr * p;
            if (l + 1 < need) dp[w][l + 1] += pr * (1 - p);
        }
    return win;
}

double seriesProb(const std::vector<double>& adv, int bo) {
    std::vector<int> order;
    vetoOrder(adv, bo, order, nullptr);
    return seriesFromOrder(adv, order, bo);
}

struct SimModel {
    std::vector<double> r, sigma;
    std::vector<std::vector<double>> off;
    std::map<std::pair<int, int>, int> forced;     // (menor, maior) índice global -> vencedor
    std::map<std::pair<int, int>, double> h2h;     // (menor, maior) -> Elo extra para o menor
};

// Jogo conhecido/agendado de um estágio Swiss (índices globais). winner -1 = ainda não jogado.
struct FixedMatch {
    int a, b, winner;
};

// Um estágio da cadeia simulada.
struct ChainStage {
    bool swiss = true;
    std::string name;
    std::vector<int> slots;                 // swiss: posição = seed-1 | playoffs: posições da chave
    std::vector<std::pair<int, int>> qf;    // playoffs: pares de posições das quartas, na ordem da chave
    std::map<std::pair<int, int>, std::vector<FixedMatch>> fixed;  // swiss: campanha (w,l) -> jogos
    bool slotsKnown = true;
};

bool pairUp(const std::vector<int>& list, const std::vector<std::vector<int>>& opp, bool allowRematch,
            std::vector<std::pair<int, int>>& out) {
    if (list.empty()) return true;
    int a = list[0];
    for (int k = static_cast<int>(list.size()) - 1; k >= 1; --k) {
        int b = list[k];
        if (!allowRematch && std::find(opp[a].begin(), opp[a].end(), b) != opp[a].end()) continue;
        std::vector<int> rest;
        rest.reserve(list.size());
        for (int x : list) if (x != a && x != b) rest.push_back(x);
        out.emplace_back(a, b);
        if (pairUp(rest, opp, allowRematch, out)) return true;
        out.pop_back();
    }
    return false;
}

class Simulator {
public:
    Simulator(const SimModel& m, uint64_t seed) : M(m), rng(seed), rr(m.r) {}

    void drawRatings() {
        for (size_t i = 0; i < rr.size(); ++i) rr[i] = M.r[i] + M.sigma[i] * gauss(rng);
    }
    void meanRatings() { rr = M.r; }

    double h2hFor(int i, int j) const {
        auto it = M.h2h.find({std::min(i, j), std::max(i, j)});
        if (it == M.h2h.end()) return 0;
        return i < j ? it->second : -it->second;
    }
    std::vector<double> advantages(int i, int j) const {
        std::vector<double> adv(M.off[i].size());
        double h = h2hFor(i, j);
        for (size_t m = 0; m < adv.size(); ++m) adv[m] = rr[i] + M.off[i][m] - rr[j] - M.off[j][m] + h;
        return adv;
    }
    double prob(int i, int j, int bo) const { return seriesProb(advantages(i, j), bo); }
    // prob. de `a` vencer `b` (a começa o veto), considerando resultados forçados
    double probFor(int a, int b, int bo) const {
        auto it = M.forced.find({std::min(a, b), std::max(a, b)});
        if (it != M.forced.end()) return it->second == a ? 1.0 : 0.0;
        return prob(a, b, bo);
    }
    int play(int a, int b, int bo) { return uni(rng) < probFor(a, b, bo) ? a : b; }

    // Swiss em posições locais (posição = seed-1). Devolve vitórias, derrotas e Buchholz.
    void swiss(const std::vector<int>& slots, const std::map<std::pair<int, int>, std::vector<FixedMatch>>& fixed,
               const std::string& boRule, std::vector<int>& W, std::vector<int>& L, std::vector<int>& B) {
        const int N = static_cast<int>(slots.size());
        W.assign(N, 0);
        L.assign(N, 0);
        B.assign(N, 0);
        std::vector<std::vector<int>> opp(N);
        auto posOf = [&](int g) {
            for (int i = 0; i < N; ++i)
                if (slots[i] == g) return i;
            return -1;
        };
        auto bo = [&](int w, int l) {
            if (boRule == "bo1") return 1;
            if (boRule == "bo3") return 3;
            return (w == 2 || l == 2) ? 3 : 1;  // regra Valve
        };
        auto playPair = [&](int a, int b, int forcedWinner) {
            if (a > b) std::swap(a, b);  // melhor seed começa o veto
            int win;
            if (forcedWinner == a || forcedWinner == b) win = forcedWinner;
            else win = play(slots[a], slots[b], bo(W[a], L[a])) == slots[a] ? a : b;
            int los = win == a ? b : a;
            W[win]++;
            L[los]++;
            opp[a].push_back(b);
            opp[b].push_back(a);
        };
        for (int round = 1; round <= 5; ++round) {
            std::map<std::pair<int, int>, std::vector<int>> groups;
            for (int i = 0; i < N; ++i)
                if (W[i] < 3 && L[i] < 3) groups[{W[i], L[i]}].push_back(i);
            if (groups.empty()) break;
            std::vector<int> buch(N, 0);
            for (int i = 0; i < N; ++i)
                for (int o : opp[i]) buch[i] += W[o] - L[o];
            std::vector<std::array<int, 3>> games;
            for (auto& g : groups) {
                std::vector<int> v = g.second;
                auto fit = fixed.find(g.first);
                if (fit != fixed.end()) {
                    for (const auto& fm : fit->second) {
                        int pa = posOf(fm.a), pb = posOf(fm.b);
                        if (pa < 0 || pb < 0) continue;
                        if (std::find(v.begin(), v.end(), pa) == v.end() || std::find(v.begin(), v.end(), pb) == v.end()) continue;
                        games.push_back({pa, pb, fm.winner >= 0 ? posOf(fm.winner) : -1});
                        v.erase(std::find(v.begin(), v.end(), pa));
                        v.erase(std::find(v.begin(), v.end(), pb));
                    }
                }
                if (v.size() < 2) continue;
                std::vector<std::pair<int, int>> pairs;
                if (g.first.first == 0 && g.first.second == 0) {
                    std::sort(v.begin(), v.end());  // 1ª rodada: 1x9, 2x10, ...
                    size_t h = v.size() / 2;
                    for (size_t k = 0; k < h; ++k) pairs.push_back({v[k], v[k + h]});
                } else {
                    std::sort(v.begin(), v.end(), [&](int a, int b) {
                        return buch[a] != buch[b] ? buch[a] > buch[b] : a < b;
                    });
                    if (!pairUp(v, opp, false, pairs)) {
                        pairs.clear();
                        pairUp(v, opp, true, pairs);
                    }
                }
                for (auto& p : pairs) games.push_back({p.first, p.second, -1});
            }
            for (auto& gm : games) playPair(gm[0], gm[1], gm[2]);
        }
        for (int i = 0; i < N; ++i)
            for (int o : opp[i]) B[i] += W[o] - L[o];
    }

    // reach[pos]: 0 = caiu nas quartas, 1 = semi, 2 = final, 3 = campeão
    void playoffs(const std::vector<int>& slots, const std::vector<std::pair<int, int>>& qf, int finalBo, int* reach) {
        for (int i = 0; i < 8; ++i) reach[i] = 0;
        auto duel = [&](int a, int b, int bo) { return play(slots[a], slots[b], bo) == slots[a] ? a : b; };
        int w[4];
        for (int q = 0; q < 4; ++q) {
            w[q] = duel(qf[q].first, qf[q].second, 3);
            reach[w[q]] = 1;
        }
        int f1 = duel(std::min(w[0], w[1]), std::max(w[0], w[1]), 3);
        int f2 = duel(std::min(w[2], w[3]), std::max(w[2], w[3]), 3);
        reach[f1] = 2;
        reach[f2] = 2;
        reach[duel(std::min(f1, f2), std::max(f1, f2), finalBo)] = 3;
    }

    // Cadeia de estágios: vagas TBD recebem os classificados do estágio anterior
    // (ordenados por campanha, Buchholz e seed). Grava a categoria de cada time no
    // último estágio (swiss 0..5, playoffs 0..3) e a posição ocupada.
    void chain(const std::vector<ChainStage>& stages, const std::string& boRule, int finalBo, uint8_t* cat,
               int8_t* slotOf) {
        std::vector<int> carry, W, L, B;
        for (size_t si = 0; si < stages.size(); ++si) {
            const ChainStage& cs = stages[si];
            std::vector<int> slots = cs.slots;
            std::vector<int> incoming;
            for (int t : carry)
                if (std::find(slots.begin(), slots.end(), t) == slots.end()) incoming.push_back(t);
            size_t k = 0;
            for (auto& s : slots)
                if (s < 0 && k < incoming.size()) s = incoming[k++];
            for (int s : slots)
                if (s < 0)
                    throw std::runtime_error("o estagio '" + cs.name +
                                             "' tem vagas sem time e o estagio anterior nao preenche todas");
            bool last = si + 1 == stages.size();
            if (cs.swiss) {
                swiss(slots, cs.fixed, boRule, W, L, B);
                std::vector<int> adv;
                for (int i = 0; i < static_cast<int>(slots.size()); ++i)
                    if (W[i] == 3) adv.push_back(i);
                std::sort(adv.begin(), adv.end(), [&](int a, int b) {
                    if (L[a] != L[b]) return L[a] < L[b];
                    if (B[a] != B[b]) return B[a] > B[b];
                    return a < b;
                });
                carry.clear();
                for (int i : adv) carry.push_back(slots[i]);
                if (last)
                    for (int i = 0; i < static_cast<int>(slots.size()); ++i)
                        cat[slots[i]] = static_cast<uint8_t>(W[i] == 3 ? L[i] : 5 - W[i]);
            } else {
                int reach[8];
                playoffs(slots, cs.qf, finalBo, reach);
                if (last)
                    for (int i = 0; i < 8; ++i) cat[slots[i]] = static_cast<uint8_t>(reach[i]);
            }
            if (last)
                for (int i = 0; i < static_cast<int>(slots.size()); ++i) slotOf[slots[i]] = static_cast<int8_t>(i);
        }
    }

    const SimModel& M;
    std::mt19937_64 rng;
    std::normal_distribution<double> gauss{0.0, 1.0};
    std::uniform_real_distribution<double> uni{0.0, 1.0};
    std::vector<double> rr;
};

// ---------------------------------------------------------------- Pick'Em (Swiss)
using Picks = std::array<int, 10>;  // [0,1]=3-0 [2,3]=0-3 [4..9]=avança
int slotType(int s) { return s < 2 ? 0 : (s < 4 ? 1 : 2); }

bool hit(int ty, uint8_t c, bool adv30) {
    if (ty == 0) return c == 0;
    if (ty == 1) return c == 5;
    return c == 1 || c == 2 || (adv30 && c == 0);
}

void evalPicks(const Picks& pk, const std::vector<uint8_t>& cats, size_t nt, size_t n, bool adv30, double& p5,
               double& eh) {
    size_t ok = 0, hits = 0;
    for (size_t s = 0; s < n; ++s) {
        const uint8_t* c = &cats[s * nt];
        int h = 0;
        for (int k = 0; k < 10; ++k) h += hit(slotType(k), c[pk[k]], adv30);
        hits += h;
        ok += h >= 5;
    }
    p5 = static_cast<double>(ok) / n;
    eh = static_cast<double>(hits) / n;
}

double pickProb(const std::array<double, 6>& d, int ty, bool adv30) {
    return ty == 0 ? d[0] : ty == 1 ? d[5] : d[1] + d[2] + (adv30 ? d[0] : 0);
}

Picks optimizePicks(const std::vector<std::array<double, 6>>& dist, const std::vector<int>& cand,
                    const std::vector<uint8_t>& cats, size_t nt, size_t nSims, bool adv30) {
    Picks pk{};
    std::set<int> used;
    for (int k = 0; k < 10; ++k) {
        int b = -1;
        double bp = -1;
        for (int t : cand) {
            if (used.count(t)) continue;
            double p = pickProb(dist[t], slotType(k), adv30);
            if (p > bp) { bp = p; b = t; }
        }
        pk[k] = b;
        used.insert(b);
    }
    size_t n = std::min<size_t>(nSims, 40000);
    auto score = [&](const Picks& p) {
        double a, b;
        evalPicks(p, cats, nt, n, adv30, a, b);
        return a + 1e-4 * b;  // prioriza P(>=5), desempata por acertos esperados
    };
    double best = score(pk);
    bool improved = true;
    while (improved) {
        improved = false;
        for (int k = 0; k < 10; ++k)
            for (int t : cand) {
                if (std::find(pk.begin(), pk.end(), t) != pk.end()) continue;
                Picks c = pk;
                c[k] = t;
                double s = score(c);
                if (s > best + 1e-12) { best = s; pk = c; improved = true; }
            }
        for (int a = 0; a < 10; ++a)
            for (int b = a + 1; b < 10; ++b) {
                if (slotType(a) == slotType(b)) continue;
                Picks c = pk;
                std::swap(c[a], c[b]);
                double s = score(c);
                if (s > best + 1e-12) { best = s; pk = c; improved = true; }
            }
    }
    return pk;
}

// Força de um time num dia passado, só com o estado do Engine (sem dados "atuais")
double strengthAt(const Engine& eng, const std::string& key, int day, TeamState& out) {
    const Params& P = eng.P;
    auto it = eng.T.find(key);
    out = it != eng.T.end() ? it->second : TeamState();
    eng.decayTo(out, day);
    double r = out.r + out.form + eng.playerBonus(eng.playerAgg(out.lineup)) + P.wRust * eng.restScore(out, day) +
               P.wSyn * eng.stability(out, day);
    double rs = 0;
    if (P.wRank != 0 && eng.rankScore(key, day, rs)) r += P.wRank * rs;
    return r;
}

}  // namespace

std::vector<BacktestStage> buildBacktestStages(const Dataset& ds) {
    std::vector<BacktestStage> out;
    for (const auto& mj : ds.majors) {
        for (const auto& eid : mj.eventIds) {
            std::vector<const Match*> mm;
            for (const auto& m : ds.matches)
                if (m.eventId == eid) mm.push_back(&m);
            if (mm.size() < 30) continue;
            std::stable_sort(mm.begin(), mm.end(), [](const Match* a, const Match* b) {
                return a->day != b->day ? a->day < b->day : a->id < b->id;
            });
            std::map<std::string, int> idx;
            BacktestStage st;
            for (auto* m : mm)
                for (const std::string* n : {&m->team1, &m->team2}) {
                    std::string k = teamKey(*n);
                    if (!idx.count(k)) { idx[k] = static_cast<int>(st.teams.size()); st.teams.push_back(*n); }
                }
            if (st.teams.size() != 16) continue;  // só Swiss de 16
            std::vector<int> W(16, 0), L(16, 0);
            std::map<std::string, int> mapCount;
            bool ok = true;
            for (auto* m : mm) {
                int a = idx[teamKey(m->team1)], b = idx[teamKey(m->team2)];
                if (W[a] >= 3 && W[b] >= 3) continue;  // playoffs no mesmo evento (Stage 3 + Playoffs)
                if (W[a] >= 3 || L[a] >= 3 || W[b] >= 3 || L[b] >= 3) { ok = false; break; }
                int wa = 0, wb = 0;
                for (auto& mp : m->maps) {
                    if (mp.r1 > mp.r2) ++wa; else if (mp.r2 > mp.r1) ++wb;
                    if (!mp.map.empty()) mapCount[mp.map]++;
                }
                if (wa == wb) { ok = false; break; }
                if (W[a] == 0 && L[a] == 0 && W[b] == 0 && L[b] == 0) st.round1.push_back({a, b});
                if (wa > wb) { W[a]++; L[b]++; } else { W[b]++; L[a]++; }
            }
            int adv = 0;
            for (int i = 0; i < 16 && ok; ++i) {
                if (W[i] != 3 && L[i] != 3) ok = false;
                adv += W[i] == 3;
            }
            if (!ok || adv != 8 || st.round1.size() != 8) continue;
            for (int i = 0; i < 16; ++i) st.cat.push_back(W[i] == 3 ? L[i] : 5 - W[i]);
            std::vector<std::pair<int, std::string>> mc;
            for (auto& kv : mapCount) mc.push_back({-kv.second, kv.first});
            std::sort(mc.begin(), mc.end());
            for (size_t i = 0; i < mc.size() && i < 7; ++i) st.pool.push_back(mc[i].second);
            st.day = mm.front()->day;
            std::string ev = mm.front()->event.empty() ? ("evento " + eid) : mm.front()->event;
            st.name = ev;
            out.push_back(std::move(st));
        }
    }
    std::sort(out.begin(), out.end(), [](const BacktestStage& a, const BacktestStage& b) { return a.day < b.day; });
    return out;
}

BacktestResult backtestPickems(const Params& p, const std::shared_ptr<const RankIndex>& rk, const std::vector<Match>& ms,
                               const std::vector<BacktestStage>& stages, int sims, uint64_t seed) {
    BacktestResult R;
    Engine eng(p);
    eng.RK = rk;
    size_t cursor = 0;
    int counted = 0;
    for (const auto& st : stages) {
        eng.run(ms, 0, 0, st.day, &cursor);  // só partidas antes do estágio
        if (eng.lastDay == 0) continue;     // sem histórico antes deste estágio
        const size_t nt = st.teams.size();
        SimModel M;
        std::vector<std::pair<double, int>> order;
        for (size_t i = 0; i < nt; ++i) {
            TeamState s;
            double r = strengthAt(eng, teamKey(st.teams[i]), st.day, s);
            M.r.push_back(r);
            M.sigma.push_back(p.sigmaFloor + p.sigmaNew * 30.0 / (30.0 + s.n));
            std::vector<double> offs;
            for (auto& mp : st.pool) offs.push_back(Engine::offset(s, mp));
            M.off.push_back(offs);
            order.push_back({-r, static_cast<int>(i)});
        }
        for (size_t i = 0; i < nt; ++i)
            for (size_t j = i + 1; j < nt; ++j) {
                double h = eng.h2h(teamKey(st.teams[i]), teamKey(st.teams[j]), st.day);
                if (h != 0) M.h2h[{static_cast<int>(i), static_cast<int>(j)}] = h;
            }
        // seeds: força estimada (a ordem só pesa nos desempates); 1ª rodada = confrontos reais
        std::sort(order.begin(), order.end());
        std::vector<int> slots;
        for (auto& o : order) slots.push_back(o.second);
        std::map<std::pair<int, int>, std::vector<FixedMatch>> fixed;
        for (auto& r1 : st.round1) fixed[{0, 0}].push_back({r1.first, r1.second, -1});

        Simulator sim(M, seed + static_cast<uint64_t>(st.day));
        std::vector<uint8_t> cats(static_cast<size_t>(sims) * nt);
        std::vector<std::array<double, 6>> dist(nt, std::array<double, 6>{0, 0, 0, 0, 0, 0});
        std::vector<int> W, L, B;
        for (int s = 0; s < sims; ++s) {
            sim.drawRatings();
            sim.swiss(slots, fixed, "valve", W, L, B);
            for (size_t i = 0; i < nt; ++i) {
                uint8_t c = static_cast<uint8_t>(W[i] == 3 ? L[i] : 5 - W[i]);
                cats[static_cast<size_t>(s) * nt + slots[i]] = c;
                dist[slots[i]][c] += 1.0 / sims;
            }
        }
        std::vector<int> cand(nt);
        std::iota(cand.begin(), cand.end(), 0);
        Picks pk = optimizePicks(dist, cand, cats, nt, static_cast<size_t>(sims), false);
        double p5 = 0, eh = 0;
        evalPicks(pk, cats, nt, static_cast<size_t>(sims), false, p5, eh);
        int hits = 0;
        json picksJ = json::array();
        for (int k = 0; k < 10; ++k) {
            bool h = hit(slotType(k), static_cast<uint8_t>(st.cat[pk[k]]), false);
            hits += h;
            static const char* TY[] = {"3-0", "0-3", "avanca"};
            static const char* RES[] = {"3-0", "3-1", "3-2", "2-3", "1-3", "0-3"};
            picksJ.push_back({{"time", st.teams[pk[k]]}, {"pick", TY[slotType(k)]}, {"real", RES[st.cat[pk[k]]]}, {"acertou", h}});
        }
        R.meanHits += hits;
        R.rate5 += hits >= 5;
        R.meanP5 += p5;
        ++counted;
        R.detail.push_back({{"estagio", st.name}, {"data", util::dateString(st.day)}, {"acertos", hits},
                            {"p5Prevista", p5}, {"acertosEsperados", eh}, {"picks", picksJ}});
    }
    if (counted) {
        R.meanHits /= counted;
        R.rate5 /= counted;
        R.meanP5 /= counted;
    }
    return R;
}

namespace {

std::string strOr(const json& j, const char* key, const std::string& def = "") {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}
double numOr(const json& j, const char* key, double def = 0) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) return j[key].get<double>();
    return def;
}

// Força de cada time do universo e a decomposição por fator (exibida no site).
void buildTeams(Engine& eng, const Dataset& data, const std::vector<std::string>& names, const json& sc,
                const std::vector<std::string>& pool, SimModel& M, json& teamsJ, json& warnings) {
    const Params& P = eng.P;
    std::map<std::string, double> adjust;
    if (sc.contains("adjust") && sc["adjust"].is_array())
        for (auto& a : sc["adjust"]) adjust[teamKey(strOr(a, "team"))] += numOr(a, "delta");
    std::map<std::pair<std::string, std::string>, double> mapadj;
    if (sc.contains("mapadj") && sc["mapadj"].is_array())
        for (auto& a : sc["mapadj"]) mapadj[{teamKey(strOr(a, "team")), mapKey(strOr(a, "map"))}] += numOr(a, "delta");
    std::map<std::string, std::vector<std::string>> lineupOverride;
    if (sc.contains("lineups") && sc["lineups"].is_object())
        for (auto it = sc["lineups"].begin(); it != sc["lineups"].end(); ++it) {
            std::vector<std::string> v;
            if (it.value().is_array())
                for (auto& p : it.value())
                    if (p.is_string() && !util::trim(p.get<std::string>()).empty()) v.push_back(util::trim(p.get<std::string>()));
            if (!v.empty()) lineupOverride[teamKey(it.key())] = v;
        }

    std::map<std::string, const RankedTeam*> rankMap;
    for (auto& r : data.ranking) rankMap[teamKey(r.name)] = &r;
    std::map<std::string, const PlayerCurrent*> curPlayers;
    for (auto& p : data.players) curPlayers[playerKey(p.name)] = &p;

    int day = std::max(util::todayDay(), eng.lastDay);
    size_t n = names.size();
    std::vector<TeamState> st(n);
    std::vector<double> points(n, 0);
    double meanElo = 0, meanLn = 0;
    int withPts = 0;
    for (size_t i = 0; i < n; ++i) {
        std::string k = teamKey(names[i]);
        auto it = eng.T.find(k);
        if (it != eng.T.end()) st[i] = it->second;
        eng.decayTo(st[i], day);
        meanElo += st[i].r;
        auto rk = rankMap.find(k);
        if (rk != rankMap.end() && rk->second->points > 0) {
            points[i] = rk->second->points;
            meanLn += std::log(points[i]);
            ++withPts;
        }
    }
    meanElo /= std::max<double>(1, static_cast<double>(n));
    if (withPts) meanLn /= withPts;

    teamsJ = json::array();
    for (size_t i = 0; i < n; ++i) {
        std::string k = teamKey(names[i]);
        const TeamState& s = st[i];
        auto rk = rankMap.find(k);
        std::string display = rk != rankMap.end() ? rk->second->name : names[i];

        double base = s.r, prior = 0, w = 0;
        // com ranking histórico, o ranking entra pelo fator treinado (wRank), não pelo prior fixo
        bool hasPrior = points[i] > 0 && !eng.hasRankHistory();
        if (hasPrior) {
            prior = meanElo + P.hltvScale * (std::log(points[i]) - meanLn);
            w = P.priorMaps / (P.priorMaps + s.n);
            base = w * prior + (1 - w) * s.r;
        }

        std::vector<std::string> lineup;
        std::string lineupSource;
        if (lineupOverride.count(k)) { lineup = lineupOverride[k]; lineupSource = "cenario"; }
        else if (rk != rankMap.end() && rk->second->lineup.size() >= 4) { lineup = rk->second->lineup; lineupSource = "ranking HLTV"; }
        else if (!s.lineup.empty()) { lineup = s.lineup; lineupSource = "ultima partida"; }
        else lineupSource = "desconhecida";

        json pj = json::array();
        PlayerAgg agg;
        if (!lineup.empty()) agg.rating = agg.kd = agg.adr = agg.kast = 0;
        for (auto& nick : lineup) {
            PlayerState ps;
            eng.playerShrunk(nick, ps);
            auto cp = curPlayers.find(playerKey(nick));
            int curMaps = 0;
            if (cp != curPlayers.end() && cp->second->maps > 0) {
                curMaps = cp->second->maps;
                double wc = curMaps / (curMaps + 15.0);
                if (cp->second->rating > 0) ps.rating = wc * cp->second->rating + (1 - wc) * ps.rating;
                if (cp->second->kd > 0) ps.kd = wc * cp->second->kd + (1 - wc) * ps.kd;
            }
            agg.rating += ps.rating;
            agg.kd += ps.kd;
            agg.adr += ps.adr;
            agg.kast += ps.kast;
            pj.push_back({{"name", nick}, {"rating", ps.rating}, {"kd", ps.kd}, {"adr", ps.adr}, {"kast", ps.kast},
                          {"partidasHistorico", ps.n}, {"mapasRecentes", curMaps}});
        }
        if (!lineup.empty()) {
            double ln = static_cast<double>(lineup.size());
            agg.rating /= ln;
            agg.kd /= ln;
            agg.adr /= ln;
            agg.kast /= ln;
        }
        double fRating = P.wRating * (agg.rating - 1), fKD = P.wKD * (agg.kd - 1);
        double fADR = P.wADR * (agg.adr / ADR_BASE - 1), fKAST = P.wKAST * (agg.kast / KAST_BASE - 1);
        double bonus = fRating + fKD + fADR + fKAST;
        double fRest = P.wRust * eng.restScore(s, day);
        double fSyn = P.wSyn * eng.stability(s, day);
        double adj = adjust.count(k) ? adjust[k] : 0;
        double rs = 0, fRank = 0;
        int rankPos = 0;
        if (eng.hasRankHistory() && eng.rankScore(k, day + 1, rs, &rankPos)) fRank = P.wRank * rs;
        double rating = base + bonus + s.form + fRest + fSyn + fRank + adj;

        std::vector<double> offs;
        json mapsJ = json::object();
        for (auto& mp : pool) {
            double o = Engine::offset(s, mp);
            auto ma = mapadj.find({k, mp});
            if (ma != mapadj.end()) o += ma->second;
            offs.push_back(o);
            mapsJ[mp] = o;
        }
        M.r.push_back(rating);
        M.sigma.push_back(P.sigmaFloor + P.sigmaNew * 30.0 / (30.0 + s.n));
        M.off.push_back(offs);
        if (s.n < 10)
            warnings.push_back(display + ": poucos mapas no historico (" + std::to_string(static_cast<int>(s.n)) + ")");

        json fatores = {{"elo", s.r},
                        {"ajustePrior", base - s.r},
                        {"rating", fRating},
                        {"kd", fKD},
                        {"adr", fADR},
                        {"kast", fKAST},
                        {"forma", s.form},
                        {"descanso", fRest},
                        {"entrosamento", fSyn},
                        {"ranking", fRank},
                        {"cenario", adj}};
        json medias = {{"rating", agg.rating}, {"kd", agg.kd}, {"adr", agg.adr}, {"kast", agg.kast}};
        int daysOff = s.lastPlayed < 0 ? -1 : day - s.lastPlayed;
        int daysChange = s.lastChange < -100000 ? -1 : day - s.lastChange;
        teamsJ.push_back({{"name", display},
                          {"key", k},
                          {"rating", rating},
                          {"elo", s.r},
                          {"prior", hasPrior ? json(prior) : json(nullptr)},
                          {"pesoPrior", w},
                          {"pontosHLTV", points[i]},
                          {"posicaoRanking", rankPos},
                          {"bonusJogadores", bonus},
                          {"ajusteCenario", adj},
                          {"mapasNoHistorico", s.n},
                          {"incerteza", M.sigma.back()},
                          {"diasSemJogar", daysOff},
                          {"diasDesdeTroca", daysChange},
                          {"lineupFonte", lineupSource},
                          {"jogadores", pj},
                          {"medias", medias},
                          {"fatores", fatores},
                          {"mapas", mapsJ}});
    }
    // confronto direto entre todos os pares do universo
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            double h = eng.h2h(teamKey(names[i]), teamKey(names[j]), day);
            if (std::abs(h) > 0.01) M.h2h[{static_cast<int>(i), static_cast<int>(j)}] = h;
        }
}

const char* stageStatus(const StageInfo& s, int& known, int& played, int& pending) {
    known = 0;
    played = 0;
    pending = 0;
    for (auto& t : s.teams)
        if (!t.name.empty()) ++known;
    for (auto& m : s.matches) {
        if (m.a.empty() || m.b.empty()) continue;
        if (!m.winner.empty()) ++played; else ++pending;
    }
    int total = s.type == "playoffs" ? 7 : 33;
    if (played >= total) return "terminado";
    if (played > 0 || pending > 0) return "em andamento";
    if (known < s.size) return "aguardando times";
    return "nao comecou";
}

std::vector<std::string> resolvePool(const json& req, const std::vector<std::string>& fallback) {
    std::vector<std::string> raw;
    if (req.contains("pool") && req["pool"].is_array() && !req["pool"].empty()) {
        for (auto& x : req["pool"])
            if (x.is_string()) raw.push_back(x.get<std::string>());
    } else {
        raw = fallback;
    }
    std::vector<std::string> pool;
    for (auto& m : raw) {
        std::string k = mapKey(m);
        if (!k.empty() && std::find(pool.begin(), pool.end(), k) == pool.end()) pool.push_back(k);
    }
    if (pool.empty()) pool = DEFAULT_POOL;
    if (pool.size() < 5) throw std::runtime_error("o map pool precisa de pelo menos 5 mapas");
    return pool;
}

}  // namespace

// ============================================================================
// Eventos (Majors) disponíveis
// ============================================================================
json eventsSummary(const Dataset& data) {
    json out = json::array();
    for (auto& ev : data.events) {
        json stages = json::array();
        for (size_t i = 0; i < ev.stages.size(); ++i) {
            const StageInfo& s = ev.stages[i];
            int known, played, pending;
            const char* status = stageStatus(s, known, played, pending);
            stages.push_back({{"index", i},
                              {"name", s.name},
                              {"eventName", s.eventName},
                              {"type", s.type},
                              {"size", s.size},
                              {"conhecidos", known},
                              {"jogados", played},
                              {"pendentes", pending},
                              {"status", status},
                              {"teams", s.teams},
                              {"matches", s.matches},
                              {"qfSeeds", s.qfSeeds}});
        }
        out.push_back({{"name", ev.name}, {"eventId", ev.eventId}, {"updatedAt", ev.updatedAt}, {"pool", ev.pool},
                       {"stages", stages}});
    }
    return out;
}

// ============================================================================
// Previsão: estágio de um evento real ou lista manual de times
// ============================================================================
json predictEvent(const Engine& trained, const Dataset& data, const json& req) {
    Engine eng = trained;  // cópia: não altera o estado compartilhado
    const Params& P = eng.P;

    std::string boRule = util::lower(strOr(req, "boRule", "valve"));
    int finalBo = static_cast<int>(numOr(req, "finalBo", 3));
    if (finalBo != 1 && finalBo != 3 && finalBo != 5) finalBo = 3;
    bool adv30 = req.contains("advIncludes30") && req["advIncludes30"].is_boolean() && req["advIncludes30"].get<bool>();
    int sims = std::clamp(static_cast<int>(numOr(req, "sims", P.sims)), 1000, 200000);
    uint64_t seed = (req.contains("seed") && req["seed"].is_number_unsigned()) ? req["seed"].get<uint64_t>()
                                                                               : static_cast<uint64_t>(std::random_device{}());
    json sc = (req.contains("scenario") && req["scenario"].is_object()) ? req["scenario"] : json::object();

    std::vector<std::string> names;
    std::map<std::string, int> index;
    auto idOf = [&](const std::string& n) -> int {
        std::string k = teamKey(n);
        if (k.empty()) throw std::runtime_error("nome de time vazio");
        auto it = index.find(k);
        if (it != index.end()) return it->second;
        int id = static_cast<int>(names.size());
        index[k] = id;
        names.push_back(n);
        return id;
    };

    std::vector<ChainStage> chain;
    std::vector<std::array<int, 3>> knownResults;
    std::map<int, int> targetSeed;
    std::vector<std::string> poolFallback;
    json out = json::object();

    std::string eventName = strOr(req, "event");
    if (!eventName.empty()) {
        const EventInfo* ev = nullptr;
        for (auto& e : data.events)
            if (e.name == eventName) ev = &e;
        if (!ev) throw std::runtime_error("evento nao encontrado: " + eventName);
        int target = static_cast<int>(numOr(req, "stage", 0));
        if (target < 0 || target >= static_cast<int>(ev->stages.size())) throw std::runtime_error("estagio invalido");
        auto knownCount = [](const StageInfo& s) {
            int c = 0;
            for (auto& t : s.teams)
                if (!t.name.empty()) ++c;
            return c;
        };
        int start = target;
        while (start > 0 && knownCount(ev->stages[start]) < ev->stages[start].size) --start;
        if (knownCount(ev->stages[start]) < ev->stages[start].size)
            throw std::runtime_error("o estagio '" + ev->stages[start].name +
                                     "' ainda nao tem todos os times e nao ha estagio anterior para completar");

        for (int si = start; si <= target; ++si) {
            const StageInfo& st = ev->stages[si];
            ChainStage cs;
            cs.name = st.name;
            cs.swiss = st.type != "playoffs";
            if (cs.swiss) {
                cs.slots.assign(st.size, -1);
                std::vector<int> noSeed;
                for (auto& t : st.teams) {
                    if (t.name.empty()) continue;
                    int id = idOf(t.name);
                    if (t.seed >= 1 && t.seed <= st.size && cs.slots[t.seed - 1] < 0) cs.slots[t.seed - 1] = id;
                    else noSeed.push_back(id);
                }
                for (int id : noSeed)
                    for (auto& s : cs.slots)
                        if (s < 0) { s = id; break; }
                for (auto& m : st.matches) {
                    if (m.a.empty() || m.b.empty()) continue;
                    int a = idOf(m.a), b = idOf(m.b);
                    int w = m.winner.empty() ? -1 : idOf(m.winner);
                    cs.fixed[{m.w, m.l}].push_back({a, b, w});
                }
            } else {
                std::vector<const StageMatch*> qfm;
                for (auto& m : st.matches)
                    if (m.round == 1) qfm.push_back(&m);
                bool known = qfm.size() == 4;
                for (auto* m : qfm)
                    if (m->a.empty() || m->b.empty()) known = false;
                if (known) {
                    for (auto* m : qfm) {
                        cs.slots.push_back(idOf(m->a));
                        cs.slots.push_back(idOf(m->b));
                    }
                    cs.qf = {{0, 1}, {2, 3}, {4, 5}, {6, 7}};
                } else {
                    cs.slots.assign(8, -1);
                    for (auto& q : st.qfSeeds)
                        if (q.size() == 2 && q[0] >= 1 && q[0] <= 8 && q[1] >= 1 && q[1] <= 8)
                            cs.qf.push_back({q[0] - 1, q[1] - 1});
                    if (cs.qf.size() != 4) cs.qf = {{0, 7}, {3, 4}, {1, 6}, {2, 5}};
                }
                for (auto& m : st.matches)
                    if (!m.winner.empty() && !m.a.empty() && !m.b.empty())
                        knownResults.push_back({idOf(m.a), idOf(m.b), idOf(m.winner)});
            }
            cs.slotsKnown = std::find(cs.slots.begin(), cs.slots.end(), -1) == cs.slots.end();
            chain.push_back(cs);
        }
        const StageInfo& tg = ev->stages[target];
        for (auto& t : tg.teams)
            if (!t.name.empty()) targetSeed[idOf(t.name)] = t.seed;
        poolFallback = ev->pool;
        json chainJ = json::array();
        for (auto& c : chain) chainJ.push_back(c.name);
        out["modo"] = "evento";
        out["evento"] = ev->name;
        out["estagio"] = tg.name;
        out["cadeia"] = chainJ;
    } else {
        std::string format = util::lower(strOr(req, "format", "swiss"));
        if (format != "swiss" && format != "playoffs") throw std::runtime_error("format deve ser 'swiss' ou 'playoffs'");
        size_t need = format == "playoffs" ? 8 : 16;
        struct In { std::string name; int seed; };
        std::vector<In> in;
        if (!req.contains("teams") || !req["teams"].is_array()) throw std::runtime_error("campo 'teams' ausente");
        int idx = 0;
        for (auto& t : req["teams"]) {
            ++idx;
            if (t.is_string()) in.push_back({util::trim(t.get<std::string>()), idx});
            else if (t.is_object()) in.push_back({util::trim(strOr(t, "name")), static_cast<int>(numOr(t, "seed", idx))});
        }
        std::stable_sort(in.begin(), in.end(), [](const In& a, const In& b) { return a.seed < b.seed; });
        if (in.size() != need)
            throw std::runtime_error("o formato " + format + " precisa de " + std::to_string(need) + " times (recebi " +
                                     std::to_string(in.size()) + ")");
        ChainStage cs;
        cs.name = "Personalizado";
        cs.swiss = format == "swiss";
        for (size_t i = 0; i < in.size(); ++i) {
            int before = static_cast<int>(names.size());
            int id = idOf(in[i].name);
            if (id < before) throw std::runtime_error("time repetido: " + in[i].name);
            cs.slots.push_back(id);
            targetSeed[id] = static_cast<int>(i) + 1;
        }
        if (!cs.swiss) cs.qf = {{0, 7}, {3, 4}, {1, 6}, {2, 5}};
        chain.push_back(cs);
        out["modo"] = "manual";
        out["cadeia"] = json::array({"Personalizado"});
    }
    std::vector<std::string> pool = resolvePool(req, poolFallback);

    SimModel M;
    json teamsJ, warnings = json::array();
    buildTeams(eng, data, names, sc, pool, M, teamsJ, warnings);
    for (auto& kr : knownResults) M.forced[{std::min(kr[0], kr[1]), std::max(kr[0], kr[1])}] = kr[2];
    if (sc.contains("force") && sc["force"].is_array())
        for (auto& f : sc["force"]) {
            auto look = [&](const std::string& nm) {
                auto it = index.find(teamKey(nm));
                return it == index.end() ? -1 : it->second;
            };
            int ia = look(strOr(f, "a")), ib = look(strOr(f, "b")), iw = look(strOr(f, "winner"));
            if (ia < 0 || ib < 0 || iw < 0 || ia == ib || (iw != ia && iw != ib)) {
                warnings.push_back("Resultado forcado ignorado: " + strOr(f, "a") + " x " + strOr(f, "b"));
                continue;
            }
            M.forced[{std::min(ia, ib), std::max(ia, ib)}] = iw;
        }

    // ---------------- simulação
    const size_t NT = names.size();
    const bool playoffsTarget = !chain.back().swiss;
    Simulator sim(M, seed);
    std::vector<uint8_t> cats(static_cast<size_t>(sims) * NT, 255);
    std::vector<std::vector<int>> slotCount(8, std::vector<int>(NT, 0));
    std::vector<int8_t> slotOf(NT, -1);
    for (int s = 0; s < sims; ++s) {
        sim.drawRatings();
        std::fill(slotOf.begin(), slotOf.end(), static_cast<int8_t>(-1));
        sim.chain(chain, boRule, finalBo, &cats[static_cast<size_t>(s) * NT], slotOf.data());
        if (playoffsTarget)
            for (size_t t = 0; t < NT; ++t)
                if (slotOf[t] >= 0 && slotOf[t] < 8) slotCount[slotOf[t]][t]++;
    }

    std::vector<double> part(NT, 0);
    std::vector<std::array<double, 6>> dist(NT, std::array<double, 6>{0, 0, 0, 0, 0, 0});
    for (int s = 0; s < sims; ++s)
        for (size_t t = 0; t < NT; ++t) {
            uint8_t c = cats[static_cast<size_t>(s) * NT + t];
            if (c == 255) continue;
            part[t] += 1.0 / sims;
            if (c < 6) dist[t][c] += 1.0 / sims;
        }

    out["format"] = playoffsTarget ? "playoffs" : "swiss";
    out["sims"] = sims;
    out["seed"] = seed;
    out["pool"] = pool;
    out["avisos"] = warnings;

    json teamsOut = json::array();
    std::vector<int> cand;
    for (size_t t = 0; t < NT; ++t) {
        if (part[t] <= 0) continue;
        cand.push_back(static_cast<int>(t));
        json tj = teamsJ[t];
        tj["participa"] = part[t];
        tj["seed"] = targetSeed.count(static_cast<int>(t)) ? json(targetSeed[static_cast<int>(t)]) : json(nullptr);
        tj["id"] = t;
        if (playoffsTarget) {
            tj["quartas"] = part[t];
            tj["semi"] = dist[t][1] + dist[t][2] + dist[t][3];
            tj["final"] = dist[t][2] + dist[t][3];
            tj["campeao"] = dist[t][3];
        } else {
            static const char* labels[6] = {"3-0", "3-1", "3-2", "2-3", "1-3", "0-3"};
            json d = json::object();
            for (int c = 0; c < 6; ++c) d[labels[c]] = dist[t][c];
            tj["dist"] = d;
            tj["avanca"] = dist[t][0] + dist[t][1] + dist[t][2];
        }
        teamsOut.push_back(tj);
    }
    out["teams"] = teamsOut;

    if (playoffsTarget) {
        const ChainStage& cs = chain.back();
        std::vector<int> occ(8, -1);
        std::vector<double> occP(8, 1.0);
        if (cs.slotsKnown) {
            occ = cs.slots;
        } else {
            std::set<int> usedT;
            for (int pos = 0; pos < 8; ++pos) {
                int best = -1;
                for (size_t t = 0; t < NT; ++t)
                    if (!usedT.count(static_cast<int>(t)) && (best < 0 || slotCount[pos][t] > slotCount[pos][best]))
                        best = static_cast<int>(t);
                occ[pos] = best;
                occP[pos] = best >= 0 ? static_cast<double>(slotCount[pos][best]) / sims : 0;
                usedT.insert(best);
            }
        }
        for (int o : occ)
            if (o < 0) throw std::runtime_error("nao ha times suficientes para montar a chave");
        sim.meanRatings();
        json bracket = json::array();
        auto pick = [&](int a, int b, int bo, const char* stage) {
            double p = sim.probFor(a, b, bo);
            int w = p >= 0.5 ? a : b;
            bracket.push_back({{"fase", stage}, {"a", teamsJ[a]["name"]}, {"b", teamsJ[b]["name"]},
                               {"vencedor", teamsJ[w]["name"]}, {"prob", std::max(p, 1 - p)}});
            return w;
        };
        int w[4];
        for (int q = 0; q < 4; ++q) w[q] = pick(occ[cs.qf[q].first], occ[cs.qf[q].second], 3, "Quartas");
        int f1 = pick(w[0], w[1], 3, "Semi"), f2 = pick(w[2], w[3], 3, "Semi");
        pick(f1, f2, finalBo, "Final");
        out["chave"] = bracket;
        out["chaveDefinida"] = cs.slotsKnown;
        json occJ = json::array();
        for (int pos = 0; pos < 8; ++pos)
            occJ.push_back({{"pos", pos + 1}, {"team", teamsJ[occ[pos]]["name"]}, {"prob", occP[pos]}});
        out["ocupantes"] = occJ;
        return out;
    }

    if (cand.size() < 10) throw std::runtime_error("menos de 10 times possiveis no estagio");
    Picks pk = optimizePicks(dist, cand, cats, NT, static_cast<size_t>(sims), adv30);
    double p5, eh;
    evalPicks(pk, cats, NT, static_cast<size_t>(sims), adv30, p5, eh);
    json picks = {{"3-0", json::array()}, {"0-3", json::array()}, {"avanca", json::array()}};
    for (int k = 0; k < 10; ++k) {
        int t = pk[k], ty = slotType(k);
        const char* key = ty == 0 ? "3-0" : ty == 1 ? "0-3" : "avanca";
        picks[key].push_back({{"team", teamsJ[t]["name"]}, {"prob", pickProb(dist[t], ty, adv30)}});
    }
    out["picks"] = picks;
    out["p5"] = p5;
    out["acertosEsperados"] = eh;
    return out;
}

// ============================================================================
// Comparação de dois times
// ============================================================================
json compareTeams(const Engine& trained, const Dataset& data, const json& req) {
    Engine eng = trained;
    std::string a = util::trim(strOr(req, "a")), b = util::trim(strOr(req, "b"));
    if (a.empty() || b.empty()) throw std::runtime_error("informe os dois times");
    if (teamKey(a) == teamKey(b)) throw std::runtime_error("escolha dois times diferentes");
    int bo = static_cast<int>(numOr(req, "bo", 3));
    if (bo != 1 && bo != 3 && bo != 5) bo = 3;

    std::vector<std::string> fallback;
    std::string eventName = strOr(req, "event");
    for (auto& e : data.events)
        if (e.name == eventName) fallback = e.pool;
    std::vector<std::string> pool = resolvePool(req, fallback);

    SimModel M;
    json teamsJ, warnings = json::array();
    buildTeams(eng, data, {a, b}, json::object(), pool, M, teamsJ, warnings);
    Simulator sim(M, 1);
    sim.meanRatings();

    // séries: média de A começando o veto e B começando
    json series = json::object();
    for (int s : {1, 3, 5}) {
        double p = 0.5 * (sim.probFor(0, 1, s) + (1 - sim.probFor(1, 0, s)));
        series["bo" + std::to_string(s)] = p;
    }

    // mapas
    std::vector<double> adv = sim.advantages(0, 1);
    json mapsJ = json::array();
    for (size_t m = 0; m < pool.size(); ++m)
        mapsJ.push_back({{"map", pool[m]},
                         {"offA", M.off[0][m]},
                         {"offB", M.off[1][m]},
                         {"pA", elo2p(adv[m])}});

    // veto previsto (A começa)
    std::vector<int> order;
    std::vector<VetoStep> steps;
    vetoOrder(adv, bo, order, &steps);
    json passos = json::array();
    for (auto& st : steps)
        passos.push_back({{"time", st.byA ? teamsJ[0]["name"] : teamsJ[1]["name"]},
                          {"acao", st.ban ? "ban" : "pick"},
                          {"mapa", pool[st.map]}});
    json jogados = json::array();
    for (size_t i = 0; i < static_cast<size_t>(bo) && i < order.size(); ++i)
        jogados.push_back({{"map", pool[order[i]]}, {"pA", elo2p(adv[order[i]])},
                           {"decider", i + 1 == order.size() || static_cast<int>(i) == bo - 1}});

    // fatores lado a lado (pontos de Elo)
    static const std::vector<std::pair<const char*, const char*>> FACT = {
        {"elo", "Elo historico"},          {"ajustePrior", "Ranking HLTV (prior)"},
        {"rating", "Rating dos jogadores"}, {"kd", "K/D dos jogadores"},
        {"adr", "ADR dos jogadores"},       {"kast", "KAST dos jogadores"},
        {"forma", "Forma recente"},         {"descanso", "Descanso / inatividade"},
        {"entrosamento", "Entrosamento"},   {"ranking", "Ranking HLTV atual"}};
    json fatores = json::array();
    for (auto& f : FACT) {
        double va = teamsJ[0]["fatores"][f.first].get<double>();
        double vb = teamsJ[1]["fatores"][f.first].get<double>();
        fatores.push_back({{"chave", f.first}, {"nome", f.second}, {"a", va}, {"b", vb}, {"diff", va - vb}});
    }
    double h = sim.h2hFor(0, 1);
    fatores.push_back({{"chave", "h2h"}, {"nome", "Confronto direto"}, {"a", h}, {"b", -h}, {"diff", h}});

    // histórico: recentes e confronto direto
    std::string ka = teamKey(a), kb = teamKey(b);
    auto summarize = [&](const Match& m, const std::string& me) {
        bool first = teamKey(m.team1) == me;
        int won = 0, lost = 0;
        for (auto& mp : m.maps) {
            bool w1 = mp.r1 > mp.r2;
            if (w1 == first) ++won; else ++lost;
        }
        std::string placar = m.maps.size() == 1
                                 ? std::to_string(first ? m.maps[0].r1 : m.maps[0].r2) + "-" +
                                       std::to_string(first ? m.maps[0].r2 : m.maps[0].r1)
                                 : std::to_string(won) + "-" + std::to_string(lost);
        return json{{"data", m.date}, {"evento", m.event}, {"adversario", first ? m.team2 : m.team1},
                    {"placar", placar}, {"venceu", won > lost}};
    };
    json recA = json::array(), recB = json::array(), h2hHist = json::array();
    for (auto it = data.matches.rbegin(); it != data.matches.rend(); ++it) {
        std::string k1 = teamKey(it->team1), k2 = teamKey(it->team2);
        bool hasA = k1 == ka || k2 == ka, hasB = k1 == kb || k2 == kb;
        if (hasA && recA.size() < 10) recA.push_back(summarize(*it, ka));
        if (hasB && recB.size() < 10) recB.push_back(summarize(*it, kb));
        if (hasA && hasB && h2hHist.size() < 10) h2hHist.push_back(summarize(*it, ka));
        if (recA.size() >= 10 && recB.size() >= 10 && h2hHist.size() >= 10) break;
    }

    return json{{"a", teamsJ[0]},
                {"b", teamsJ[1]},
                {"pool", pool},
                {"bo", bo},
                {"series", series},
                {"fatores", fatores},
                {"mapas", mapsJ},
                {"veto", {{"passos", passos}, {"jogados", jogados}}},
                {"h2h", {{"elo", h}, {"historico", h2hHist}}},
                {"recentes", {{"a", recA}, {"b", recB}}},
                {"avisos", warnings}};
}

// ============================================================================
// Visão geral dos times
// ============================================================================
json teamsOverview(const Engine& trained, const Dataset& data, int limit) {
    int day = std::max(util::todayDay(), trained.lastDay);
    std::map<std::string, const PlayerCurrent*> curPlayers;
    for (auto& p : data.players) curPlayers[playerKey(p.name)] = &p;

    std::vector<std::string> order;
    std::set<std::string> seen;
    std::map<std::string, const RankedTeam*> rankMap;
    for (auto& r : data.ranking) {
        std::string k = teamKey(r.name);
        rankMap[k] = &r;
        if (seen.insert(k).second) order.push_back(k);
    }
    std::vector<std::pair<double, std::string>> byElo;
    for (auto& kv : trained.T)
        if (!seen.count(kv.first) && kv.second.n >= 10) {
            TeamState s = kv.second;
            trained.decayTo(s, day);
            byElo.push_back({s.r, kv.first});
        }
    std::sort(byElo.rbegin(), byElo.rend());
    for (auto& e : byElo) order.push_back(e.second);

    json out = json::array();
    for (auto& k : order) {
        if (static_cast<int>(out.size()) >= limit) break;
        TeamState s;
        auto it = trained.T.find(k);
        if (it != trained.T.end()) s = it->second;
        trained.decayTo(s, day);
        auto rk = rankMap.find(k);
        std::vector<std::string> lineup = rk != rankMap.end() && rk->second->lineup.size() >= 4 ? rk->second->lineup : s.lineup;
        json pj = json::array();
        PlayerAgg agg;
        if (!lineup.empty()) agg.rating = agg.kd = agg.adr = agg.kast = 0;
        for (auto& nick : lineup) {
            PlayerState ps;
            trained.playerShrunk(nick, ps);
            auto cp = curPlayers.find(playerKey(nick));
            if (cp != curPlayers.end() && cp->second->maps > 0) {
                double wc = cp->second->maps / (cp->second->maps + 15.0);
                if (cp->second->rating > 0) ps.rating = wc * cp->second->rating + (1 - wc) * ps.rating;
                if (cp->second->kd > 0) ps.kd = wc * cp->second->kd + (1 - wc) * ps.kd;
            }
            agg.rating += ps.rating;
            agg.kd += ps.kd;
            agg.adr += ps.adr;
            agg.kast += ps.kast;
            pj.push_back({{"name", nick}, {"rating", ps.rating}, {"kd", ps.kd}, {"adr", ps.adr}, {"kast", ps.kast}});
        }
        if (!lineup.empty()) {
            double ln = static_cast<double>(lineup.size());
            agg.rating /= ln;
            agg.kd /= ln;
            agg.adr /= ln;
            agg.kast /= ln;
        }
        std::vector<std::pair<double, std::string>> offs;
        for (auto& o : s.off) offs.push_back({o.second, o.first});
        std::sort(offs.rbegin(), offs.rend());
        json mapsJ = json::array();
        for (auto& o : offs) mapsJ.push_back({{"map", o.second}, {"offset", o.first}});
        out.push_back({{"name", rk != rankMap.end() ? rk->second->name : s.display},
                       {"rank", rk != rankMap.end() ? rk->second->rank : 0},
                       {"pontos", rk != rankMap.end() ? rk->second->points : 0.0},
                       {"elo", s.r},
                       {"forma", s.form},
                       {"mapas", s.n},
                       {"bonusJogadores", lineup.empty() ? 0.0 : trained.playerBonus(agg)},
                       {"jogadores", pj},
                       {"mapPool", mapsJ}});
    }
    return out;
}

// ============================================================================
// Dados sintéticos (para testar o deploy sem HLTV)
// ============================================================================
json makeDemoImport() {
    std::mt19937_64 rng(7);
    std::normal_distribution<double> g(0, 1);
    std::uniform_real_distribution<double> u(0, 1);
    const int NT = 20;
    std::vector<std::string> names(NT);
    std::vector<std::vector<std::string>> roster(NT);
    std::vector<std::vector<double>> skill(NT), mapOff(NT);
    for (int i = 0; i < NT; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "Demo %02d", i + 1);
        names[i] = buf;
        for (int p = 0; p < 5; ++p) {
            std::snprintf(buf, sizeof buf, "demo%02d_p%d", i + 1, p + 1);
            roster[i].push_back(buf);
            skill[i].push_back(0.08 * g(rng) - 0.004 * i);
        }
        for (size_t m = 0; m < DEFAULT_POOL.size(); ++m) mapOff[i].push_back(50 * g(rng));
    }
    auto strength = [&](int t) {
        double s = 0;
        for (double x : skill[t]) s += x;
        return 1500 + 2500 * s / 5;
    };
    json matches = json::array();
    int start = util::todayDay() - 400;
    int id = 0;
    for (int d = start; d < util::todayDay() - 1; d += 2) {
        for (int k = 0; k < 3; ++k) {
            int a = static_cast<int>(u(rng) * NT), b = static_cast<int>(u(rng) * NT);
            if (a == b) continue;
            json mapsJ = json::array();
            int wa = 0, wb = 0;
            int bo = u(rng) < 0.3 ? 1 : 3;
            while (wa < bo / 2 + 1 && wb < bo / 2 + 1) {
                int mi = static_cast<int>(u(rng) * DEFAULT_POOL.size());
                double p = elo2p(strength(a) + mapOff[a][mi] - strength(b) - mapOff[b][mi]);
                bool aw = u(rng) < p;
                int lr = 3 + static_cast<int>(u(rng) * 9);
                (aw ? wa : wb)++;
                mapsJ.push_back({{"map", DEFAULT_POOL[mi]}, {"r1", aw ? 13 : lr}, {"r2", aw ? lr : 13}});
            }
            bool aWon = wa > wb;
            auto lines = [&](int t, bool won) {
                json arr = json::array();
                for (int p = 0; p < 5; ++p) {
                    double sk = skill[t][p] + (won ? 0.08 : -0.08) + 0.12 * g(rng);
                    int kills = std::max(3, static_cast<int>(std::lround(19 * (1 + sk))));
                    int deaths = std::max(3, static_cast<int>(std::lround(19 * (1 - sk))));
                    arr.push_back({{"name", roster[t][p]}, {"kills", kills}, {"deaths", deaths},
                                   {"adr", 75 * (1 + sk)}, {"kast", 70 + 20 * sk}, {"rating", 1 + sk}});
                }
                return arr;
            };
            matches.push_back({{"id", "demo_" + std::to_string(++id)},
                               {"date", util::dateString(d)},
                               {"event", "Demo Cup"},
                               {"stars", 2},
                               {"team1", names[a]},
                               {"team2", names[b]},
                               {"maps", mapsJ},
                               {"p1", lines(a, aWon)},
                               {"p2", lines(b, !aWon)}});
        }
        if (u(rng) < 0.02) {  // trocas de roster ocasionais
            int t = static_cast<int>(u(rng) * NT);
            int p = static_cast<int>(u(rng) * 5);
            roster[t][p] = "demo_new" + std::to_string(d);
            skill[t][p] = 0.08 * g(rng);
        }
    }
    std::vector<int> idx(NT);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return strength(a) > strength(b); });
    json ranking = json::array();
    for (int r = 0; r < NT; ++r) {
        int t = idx[r];
        ranking.push_back({{"name", names[t]},
                           {"rank", r + 1},
                           {"points", std::round(1000 * std::exp((strength(t) - strength(idx[0])) / 150))},
                           {"lineup", roster[t]}});
    }
    return json{{"demo", true}, {"matches", matches}, {"ranking", ranking}, {"players", json::array()}};
}
