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

void recenter(TeamState& s) {
    if (s.off.size() < 2) return;
    double mean = 0;
    for (auto& o : s.off) mean += o.second;
    mean /= static_cast<double>(s.off.size());
    for (auto& o : s.off) o.second -= mean;
}

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
        double f = std::exp(-(day - s.last) / P.tau);
        s.r = 1500 + (s.r - 1500) * f;
        for (auto& o : s.off) o.second *= f;
        s.n *= f;
    }
    if (day > s.last) s.last = day;
}

double Engine::offset(const TeamState& s, const std::string& map) {
    auto it = s.off.find(map);
    return it == s.off.end() ? 0.0 : it->second;
}

void Engine::playerShrunk(const std::string& nick, double& rating, double& kd, double& n) const {
    double r = 1.0, k = 1.0, nn = 0;
    auto it = PL.find(playerKey(nick));
    if (it != PL.end()) {
        r = it->second.rating;
        k = it->second.kd;
        nn = it->second.n;
    }
    const double n0 = 4.0;  // "jogos fantasma" na média
    rating = (nn * r + n0) / (nn + n0);
    kd = (nn * k + n0) / (nn + n0);
    n = nn;
}

double Engine::playerIndex(const std::vector<std::string>& lineup) const {
    if (lineup.empty()) return 0;
    double sum = 0;
    for (auto& nick : lineup) {
        double r, kd, n;
        playerShrunk(nick, r, kd, n);
        sum += composite(r, kd);
    }
    return sum / static_cast<double>(lineup.size());
}

void Engine::lineupChange(TeamState& s, const std::vector<std::string>& lineup) {
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
        }
    }
    s.lineup = lineup;
}

void Engine::updatePlayer(const PlayerLine& p) {
    auto& ps = PL[playerKey(p.name)];
    double kd = std::clamp(p.kills / static_cast<double>(std::max(1, p.deaths)), 0.1, 4.0);
    double w = std::max(P.playerAlpha, 1.0 / (ps.n + 1.0));
    ps.kd += w * (kd - ps.kd);
    if (p.rating > 0) ps.rating += w * (std::clamp(p.rating, 0.1, 3.0) - ps.rating);
    else ps.rating += 0.5 * w * (kd - ps.rating);  // sem rating: usa K/D como aproximação
    ps.n += 1;
}

Metrics Engine::run(const std::vector<Match>& ms, size_t scoreFrom, size_t scoreTo, int untilDay) {
    Metrics sc;
    for (size_t i = 0; i < ms.size(); ++i) {
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
        lineupChange(A, la);
        lineupChange(B, lb);
        double pia = playerIndex(la.empty() ? A.lineup : la);
        double pib = playerIndex(lb.empty() ? B.lineup : lb);

        bool scoring = i >= scoreFrom && i < scoreTo;
        double tw = P.tierWeight(m.tier);
        double target = m.day >= CS2_RELEASE_DAY ? 13.0 : 16.0;
        for (auto& mp : m.maps) {
            if (mp.r1 == mp.r2) continue;
            bool experienced = A.n >= 5 && B.n >= 5;
            double diff = A.r + offset(A, mp.map) - B.r - offset(B, mp.map) + P.beta * (pia - pib);
            double p = elo2p(diff);
            double y = mp.r1 > mp.r2 ? 1.0 : 0.0;
            if (scoring && experienced) {
                double pc = std::min(std::max(p, 1e-6), 1 - 1e-6);
                sc.logloss += -(y * std::log(pc) + (1 - y) * std::log(1 - pc));
                sc.brier += (p - y) * (p - y);
                sc.acc += ((p > 0.5) == (y > 0.5)) ? 1 : 0;
                sc.count++;
            }
            double mult = 1.0 + P.margin * std::min(1.0, std::abs(mp.r1 - mp.r2) / target);
            double d = tw * mult * (y - p);
            A.r += P.k * d;
            B.r -= P.k * d;
            A.off[mp.map] += P.kmap * d;
            B.off[mp.map] -= P.kmap * d;
            recenter(A);
            recenter(B);
            A.n += 1;
            B.n += 1;
        }
        for (auto& p : m.p1) updatePlayer(p);
        for (auto& p : m.p2) updatePlayer(p);
        lastDay = std::max(lastDay, m.day);
    }
    if (sc.count) {
        sc.logloss /= sc.count;
        sc.brier /= sc.count;
        sc.acc /= sc.count;
    }
    return sc;
}

// ============================================================================
// Treino: busca por coordenadas, log-loss walk-forward
// ============================================================================
json TrainReport::toJson() const {
    return json{{"params", params},
                {"antes", before},
                {"depois", after},
                {"semJogadores", eloOnly},
                {"chute5050", 0.693147},
                {"partidasTreino", trainMatches},
                {"partidasValidacao", validMatches},
                {"passos", steps}};
}

TrainReport train(const std::vector<Match>& ms, const Params& start,
                  const std::function<void(const std::string&)>& log, const std::atomic<bool>& stop) {
    TrainReport R;
    R.params = start;
    Params& P = R.params;
    size_t split = ms.size() * 8 / 10;
    R.trainMatches = split;
    R.validMatches = ms.size() - split;

    auto evalTrain = [&](const Params& p) {
        Engine e(p);
        Metrics m = e.run(ms, 0, split);
        return m.count >= 30 ? m.logloss : 1e9;
    };
    {
        Engine e(P);
        R.before = e.run(ms, split, ms.size());
    }
    struct Dim {
        double* v;
        double lo, hi;
        const char* name;
    };
    std::vector<Dim> dims = {{&P.k, 4, 150, "k"},           {&P.kmap, 0, 100, "kmap"},
                             {&P.margin, 0, 3, "margin"},   {&P.tau, 20, 1500, "tau"},
                             {&P.roster, 0, 0.35, "roster"}, {&P.beta, 0, 2500, "beta"},
                             {&P.kdMix, 0, 1, "kdMix"},     {&P.playerAlpha, 0.02, 0.6, "playerAlpha"}};
    double best = evalTrain(P);
    log("Treino: log-loss inicial " + std::to_string(best) + " (" + std::to_string(split) + " partidas)");
    double step = 0.25;
    int evals = 0;
    while (step > 0.01 && !stop) {
        bool improved = false;
        for (auto& d : dims) {
            for (int dir : {+1, -1}) {
                if (stop) break;
                double old = *d.v;
                double cand = std::clamp(old + dir * step * (d.hi - d.lo), d.lo, d.hi);
                if (cand == old) continue;
                *d.v = cand;
                double s = evalTrain(P);
                ++evals;
                if (s < best - 1e-7) {
                    best = s;
                    improved = true;
                    std::string msg = std::string(d.name) + " = " + std::to_string(cand) +
                                      "  (log-loss " + std::to_string(s) + ")";
                    R.steps.push_back(msg);
                    log("  " + msg);
                } else {
                    *d.v = old;
                }
            }
        }
        if (!improved) step /= 2;
    }
    {
        Engine e(P);
        R.after = e.run(ms, split, ms.size());
        Params p0 = P;
        p0.beta = 0;
        Engine e0(p0);
        R.eloOnly = e0.run(ms, split, ms.size());
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

// adv[m] = vantagem Elo do time A (que começa o veto) no mapa m
double seriesProb(const std::vector<double>& adv, int bo) {
    int nm = static_cast<int>(adv.size());
    std::vector<bool> avail(nm, true);
    int left = nm;
    std::vector<int> order;
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
        return best;
    };
    int picks = bo == 1 ? 0 : bo - 1;
    bool turnA = true;
    for (int i = 0; i < 2 && left - 1 > picks; ++i, turnA = !turnA) choose(turnA, true);
    for (int i = 0; i < picks && left > 1; ++i, turnA = !turnA) order.push_back(choose(turnA, false));
    while (left > 1) { choose(turnA, true); turnA = !turnA; }
    for (int m = 0; m < nm; ++m) if (avail[m]) order.push_back(m);
    while (static_cast<int>(order.size()) < bo) order.push_back(order.back());

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

struct SimModel {
    std::vector<double> r, sigma;
    std::vector<std::vector<double>> off;
    std::map<std::pair<int, int>, int> forced;  // (menor, maior) índice global -> vencedor
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
    bool slotsKnown = true;                 // todas as vagas já definidas (sem TBD)
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

    double prob(int i, int j, int bo) const {
        std::vector<double> adv(M.off[i].size());
        for (size_t m = 0; m < adv.size(); ++m) adv[m] = rr[i] + M.off[i][m] - rr[j] - M.off[j][m];
        return seriesProb(adv, bo);
    }
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
                // jogos reais (já jogados ou agendados) desta campanha
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
                    // 1ª rodada: seed i x seed i + n/2 (1x9, 2x10, ...)
                    std::sort(v.begin(), v.end());
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
        auto duel = [&](int a, int b, int bo) {
            return play(slots[a], slots[b], bo) == slots[a] ? a : b;
        };
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

    // Simula a cadeia de estágios: vagas TBD de um estágio recebem os classificados
    // do anterior (ordenados por campanha, Buchholz e seed). Para o último estágio
    // grava a categoria de cada time (swiss 0..5, playoffs 0..3) e a posição ocupada.
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

std::string strOr(const json& j, const char* key, const std::string& def = "") {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}
double numOr(const json& j, const char* key, double def = 0) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) return j[key].get<double>();
    return def;
}

// Força de cada time do universo (Elo + prior HLTV + jogadores + cenário) e
// informações exibidas no site.
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
        bool hasPrior = points[i] > 0;
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
        double sum = 0;
        for (auto& nick : lineup) {
            double r, kd, nn;
            eng.playerShrunk(nick, r, kd, nn);
            auto cp = curPlayers.find(playerKey(nick));
            int curMaps = 0;
            if (cp != curPlayers.end() && cp->second->maps > 0) {
                curMaps = cp->second->maps;
                double wc = curMaps / (curMaps + 15.0);
                if (cp->second->rating > 0) r = wc * cp->second->rating + (1 - wc) * r;
                if (cp->second->kd > 0) kd = wc * cp->second->kd + (1 - wc) * kd;
            }
            double c = eng.composite(r, kd);
            sum += c;
            pj.push_back({{"name", nick}, {"rating", r}, {"kd", kd}, {"partidasHistorico", nn},
                          {"mapasRecentes", curMaps}, {"indice", c}});
        }
        double pi = lineup.empty() ? 0 : sum / static_cast<double>(lineup.size());
        double adj = adjust.count(k) ? adjust[k] : 0;
        double rating = base + P.beta * pi + adj;

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

        teamsJ.push_back({{"name", display},
                          {"rating", rating},
                          {"elo", s.r},
                          {"prior", hasPrior ? json(prior) : json(nullptr)},
                          {"pesoPrior", w},
                          {"pontosHLTV", points[i]},
                          {"indiceJogadores", pi},
                          {"bonusJogadores", P.beta * pi},
                          {"ajusteCenario", adj},
                          {"mapasNoHistorico", s.n},
                          {"incerteza", M.sigma.back()},
                          {"lineupFonte", lineupSource},
                          {"jogadores", pj},
                          {"mapas", mapsJ}});
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

    // universo de times (todos os estágios da cadeia)
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
    std::vector<std::array<int, 3>> knownResults;  // playoffs: a, b, vencedor
    std::map<int, int> targetSeed;                 // time -> seed no estágio alvo
    std::vector<std::string> poolRaw;
    json out = json::object();

    std::string eventName = strOr(req, "event");
    if (!eventName.empty()) {
        // ---------------- estágio de um evento real
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
        if (req.contains("pool") && req["pool"].is_array() && !req["pool"].empty()) {
            for (auto& x : req["pool"])
                if (x.is_string()) poolRaw.push_back(x.get<std::string>());
        } else {
            poolRaw = ev->pool;
        }
        json chainJ = json::array();
        for (auto& c : chain) chainJ.push_back(c.name);
        out["modo"] = "evento";
        out["evento"] = ev->name;
        out["estagio"] = tg.name;
        out["cadeia"] = chainJ;
    } else {
        // ---------------- lista manual
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
        if (req.contains("pool") && req["pool"].is_array())
            for (auto& x : req["pool"])
                if (x.is_string()) poolRaw.push_back(x.get<std::string>());
        out["modo"] = "manual";
        out["cadeia"] = json::array({"Personalizado"});
    }

    // map pool
    std::vector<std::string> pool;
    for (auto& m : poolRaw) {
        std::string k = mapKey(m);
        if (!k.empty() && std::find(pool.begin(), pool.end(), k) == pool.end()) pool.push_back(k);
    }
    if (pool.empty()) pool = DEFAULT_POOL;
    if (pool.size() < 5) throw std::runtime_error("o map pool precisa de pelo menos 5 mapas");

    // força dos times
    SimModel M;
    json teamsJ, warnings = json::array();
    buildTeams(eng, data, names, sc, pool, M, teamsJ, warnings);
    for (auto& kr : knownResults) M.forced[{std::min(kr[0], kr[1]), std::max(kr[0], kr[1])}] = kr[2];
    if (sc.contains("force") && sc["force"].is_array())
        for (auto& f : sc["force"]) {
            auto look = [&](const std::string& n) {
                auto it = index.find(teamKey(n));
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
        // chave: times conhecidos ou, se ainda TBD, o ocupante mais provável de cada posição
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
        for (int pos = 0; pos < 8; ++pos) occJ.push_back({{"pos", pos + 1}, {"team", teamsJ[occ[pos]]["name"]}, {"prob", occP[pos]}});
        out["ocupantes"] = occJ;
        return out;
    }

    // ---------------- Pick'Em do Swiss
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
        double sum = 0;
        for (auto& nick : lineup) {
            double r, kd, nn;
            trained.playerShrunk(nick, r, kd, nn);
            auto cp = curPlayers.find(playerKey(nick));
            if (cp != curPlayers.end() && cp->second->maps > 0) {
                double wc = cp->second->maps / (cp->second->maps + 15.0);
                if (cp->second->rating > 0) r = wc * cp->second->rating + (1 - wc) * r;
                if (cp->second->kd > 0) kd = wc * cp->second->kd + (1 - wc) * kd;
            }
            sum += trained.composite(r, kd);
            pj.push_back({{"name", nick}, {"rating", r}, {"kd", kd}});
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
                       {"mapas", s.n},
                       {"indiceJogadores", lineup.empty() ? 0.0 : sum / lineup.size()},
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
        // trocas de roster ocasionais
        if (u(rng) < 0.02) {
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
