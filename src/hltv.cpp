#include "hltv.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <thread>

#include "util.hpp"

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#endif

namespace hltv {

static constexpr size_t NPOS = std::string::npos;

// ============================================================================
// Helpers de HTML
// ============================================================================
static std::string canonicalUrl(const std::string& h) {
    size_t p = h.find("rel=\"canonical\"");
    if (p != NPOS) {
        std::string tag = util::tagAt(h, p);
        std::string href = util::attrValue(tag, "href");
        if (!href.empty()) return href;
    }
    p = h.find("property=\"og:url\"");
    if (p != NPOS) {
        std::string tag = util::tagAt(h, p);
        std::string c = util::attrValue(tag, "content");
        if (!c.empty()) return c;
    }
    return "";
}

// Posições de todas as ocorrências de `marker`.
static std::vector<size_t> positions(const std::string& s, const std::string& marker, size_t from = 0,
                                     size_t to = NPOS) {
    std::vector<size_t> out;
    size_t p = from;
    while ((p = s.find(marker, p)) != NPOS && p < to) {
        out.push_back(p);
        p += marker.size();
    }
    return out;
}

// Divide `s` em blocos que começam em cada ocorrência de `marker`.
static std::vector<std::string> blocks(const std::string& s, const std::string& marker,
                                       size_t maxLen = 20000) {
    std::vector<std::string> out;
    auto pos = positions(s, marker);
    for (size_t i = 0; i < pos.size(); ++i) {
        size_t start = pos[i];
        size_t tagStart = s.rfind('<', start);
        if (tagStart != NPOS && start - tagStart < 300) start = tagStart;
        size_t end = i + 1 < pos.size() ? pos[i + 1] : s.size();
        end = std::min(end, start + maxLen);
        out.push_back(s.substr(start, end - start));
    }
    return out;
}

// ============================================================================
// Detecção do tipo de página
// ============================================================================
PageType detect(const std::string& h) {
    std::string url = canonicalUrl(h);
    if (url.find("/matches/") != NPOS && h.find("mapholder") != NPOS) return PageType::Match;
    if (h.find("player-ratings-table") != NPOS) return PageType::PlayerStats;
    if (h.find("ranked-team") != NPOS) return PageType::Ranking;
    if (h.find("result-con") != NPOS) return PageType::Results;
    if (h.find("small-event") != NPOS || h.find("big-event") != NPOS) return PageType::Archive;
    if (url.find("/matches/") != NPOS) return PageType::Match;
    return PageType::Unknown;
}

const char* typeName(PageType t) {
    switch (t) {
        case PageType::Match: return "partida";
        case PageType::Results: return "resultados";
        case PageType::Archive: return "arquivo_eventos";
        case PageType::Ranking: return "ranking";
        case PageType::PlayerStats: return "stats_jogadores";
        default: return "desconhecida";
    }
}

// ============================================================================
// Página de partida: /matches/{id}/{slug}
// ============================================================================
static std::string teamNameNear(const std::string& h, const std::string& gradient) {
    size_t p = h.find(gradient);
    if (p == NPOS) return "";
    size_t q = h.find("class=\"teamName", p);
    if (q == NPOS || q - p > 4000) return "";
    return util::cellText(h, q, "</");
}

static bool parseKd(const std::string& txt, int& k, int& d) {
    size_t dash = txt.find('-');
    if (dash == NPOS) return false;
    std::string a = util::trim(txt.substr(0, dash)), b = util::trim(txt.substr(dash + 1));
    if (!util::isInt(a) || !util::isInt(b)) return false;
    k = std::atoi(a.c_str());
    d = std::atoi(b.c_str());
    return true;
}

// Primeira célula cujo class começa com `cls` e cujo texto passa no filtro.
static std::string cellByClass(const std::string& row, const std::string& cls,
                               const std::function<bool(const std::string&)>& ok) {
    for (size_t p : positions(row, "class=\"" + cls)) {
        std::string t = util::cellText(row, p, "</td>");
        if (ok(t)) return t;
    }
    return "";
}

static std::vector<PlayerLine> parseStatsTable(const std::string& table) {
    std::vector<PlayerLine> out;
    auto rows = blocks(table, "<tr", 8000);
    for (auto& row : rows) {
        PlayerLine pl;
        size_t np = row.find("class=\"player-nick");
        if (np != NPOS) pl.name = util::cellText(row, np, "</");
        if (pl.name.empty()) {
            size_t sp = row.find("class=\"statsPlayerName");
            if (sp != NPOS) pl.name = util::cellText(row, sp, "</");
        }
        if (pl.name.empty()) continue;
        std::string kd = cellByClass(row, "kd", [](const std::string& t) {
            int a, b;
            return parseKd(t, a, b);
        });
        if (kd.empty() || !parseKd(kd, pl.kills, pl.deaths)) continue;
        auto numeric = [](const std::string& t) { return !std::isnan(util::toNum(t)); };
        pl.adr = util::toNum(cellByClass(row, "adr", numeric), 0);
        pl.kast = util::toNum(cellByClass(row, "kast", numeric), 0);
        pl.rating = util::toNum(cellByClass(row, "rating", numeric), 0);
        out.push_back(pl);
    }
    return out;
}

bool parseMatch(const std::string& h, Match& m) {
    std::string url = canonicalUrl(h);
    size_t mp = url.find("/matches/");
    if (mp == NPOS) return false;
    m.id = util::digitsAt(url, mp + 9);
    if (m.id.empty()) return false;

    m.team1 = teamNameNear(h, "team1-gradient");
    m.team2 = teamNameNear(h, "team2-gradient");

    // data
    size_t dp = h.find("class=\"date\"");
    std::string unixMs;  // obs.: "unix" é macro pré-definida no g++ (modo gnu++)
    if (dp != NPOS) unixMs = util::attrValue(util::tagAt(h, dp), "data-unix");
    if (unixMs.empty()) {
        size_t te = h.find("timeAndEvent");
        if (te != NPOS) unixMs = util::attrValue(h, "data-unix", te);
    }
    m.day = util::dayFromUnixMs(unixMs);
    m.date = m.day > 0 ? util::dateString(m.day) : "";

    // evento
    size_t ep = h.find("class=\"event text-ellipsis\"");
    if (ep != NPOS) {
        size_t end = h.find("</div>", ep);
        std::string seg = h.substr(ep, end == NPOS ? 600 : end - ep);
        size_t ev = seg.find("/events/");
        if (ev != NPOS) m.eventId = util::digitsAt(seg, ev + 8);
        size_t ap = seg.find("<a");
        m.event = ap != NPOS ? util::cellText(seg, ap, "</a>") : util::cellText(seg, 0, "</div>");
    }

    // mapas
    auto holders = positions(h, "class=\"mapholder\"");
    for (size_t i = 0; i < holders.size(); ++i) {
        size_t end = i + 1 < holders.size() ? holders[i + 1] : std::min(h.size(), holders[i] + 6000);
        std::string blk = h.substr(holders[i], end - holders[i]);
        size_t mn = blk.find("class=\"mapname\"");
        if (mn == NPOS) continue;
        MapResult r;
        r.map = util::cellText(blk, mn, "</");
        std::vector<std::string> sc;
        for (size_t p : positions(blk, "class=\"results-team-score\"")) sc.push_back(util::cellText(blk, p, "</"));
        if (sc.size() < 2 || !util::isInt(sc[0]) || !util::isInt(sc[1])) continue;  // não jogado
        r.r1 = std::atoi(sc[0].c_str());
        r.r2 = std::atoi(sc[1].c_str());
        m.maps.push_back(r);
    }

    // estatísticas individuais (aba "All maps")
    size_t all = h.find("id=\"all-content\"");
    if (all != NPOS) {
        size_t end = h.find("class=\"stats-content\"", all + 20);
        std::string sec = h.substr(all, end == NPOS ? NPOS : end - all);
        std::vector<std::string> tables;
        for (size_t p : positions(sec, "<table")) {
            std::string tag = util::tagAt(sec, p + 1);
            if (tag.find("totalstats") == NPOS) continue;
            size_t te = sec.find("</table>", p);
            tables.push_back(sec.substr(p, te == NPOS ? NPOS : te - p));
        }
        if (tables.size() >= 2) {
            m.p1 = parseStatsTable(tables[0]);
            m.p2 = parseStatsTable(tables[1]);
        }
    }
    return !m.team1.empty() && !m.team2.empty();
}

// ============================================================================
// Lista de resultados: /results?...
// ============================================================================
std::vector<ResultEntry> parseResults(const std::string& h) {
    std::vector<ResultEntry> out;
    std::set<std::string> seen;
    for (auto& blk : blocks(h, "class=\"result-con", 12000)) {
        ResultEntry e;
        e.day = util::dayFromUnixMs(util::attrValue(blk, "data-zonedgrouping-entry-unix"));
        size_t hp = blk.find("href=\"/matches/");
        if (hp == NPOS) continue;
        hp += 6;
        size_t q = blk.find('"', hp);
        e.path = blk.substr(hp, q - hp);
        e.matchId = util::digitsAt(e.path, 9);
        if (e.matchId.empty() || seen.count(e.matchId)) continue;
        // estrelas: apenas dentro da div "stars"
        size_t st = blk.find("class=\"stars\"");
        if (st != NPOS) {
            size_t se = blk.find("</div>", st);
            e.stars = static_cast<int>(util::countOf(blk.substr(st, se == NPOS ? 400 : se - st), "fa-star"));
        }
        size_t en = blk.find("class=\"event-name\"");
        if (en != NPOS) e.event = util::cellText(blk, en, "</");
        seen.insert(e.matchId);
        out.push_back(e);
    }
    return out;
}

// ============================================================================
// Arquivo de eventos: /events/archive?eventType=MAJOR
// ============================================================================
std::vector<ArchiveEvent> parseArchive(const std::string& h) {
    std::vector<ArchiveEvent> out;
    std::set<std::string> seen;
    auto pos = positions(h, "href=\"/events/");
    for (size_t i = 0; i < pos.size(); ++i) {
        std::string tag = util::tagAt(h, pos[i]);
        if (tag.find("small-event") == NPOS && tag.find("big-event") == NPOS) continue;
        ArchiveEvent ev;
        ev.path = util::attrValue(tag, "href");
        ev.id = util::digitsAt(ev.path, 8);
        if (ev.id.empty() || seen.count(ev.id)) continue;
        size_t end = i + 1 < pos.size() ? pos[i + 1] : std::min(h.size(), pos[i] + 5000);
        std::string blk = h.substr(pos[i], end - pos[i]);
        size_t np = blk.find("class=\"big-event-name");
        if (np == NPOS) np = blk.find("class=\"text-ellipsis");
        if (np == NPOS) np = blk.find("class=\"event-name");
        if (np != NPOS) ev.name = util::cellText(blk, np, "</");
        if (ev.name.empty()) {
            // fallback: slug -> nome
            size_t sl = ev.path.find('/', 8);
            ev.name = sl == NPOS ? ev.path : ev.path.substr(sl + 1);
            std::replace(ev.name.begin(), ev.name.end(), '-', ' ');
        }
        ev.day = util::dayFromUnixMs(util::attrValue(blk, "data-unix"));
        seen.insert(ev.id);
        out.push_back(ev);
    }
    return out;
}

std::string majorGroupKey(const std::string& name) {
    static const std::set<std::string> drop = {"stage",   "opening",   "elimination", "challengers",
                                               "legends", "champions", "playoffs",    "playoff",
                                               "main",    "new",       "1",           "2",
                                               "3",       "-",         "phase",       "swiss"};
    std::string s = util::lower(name), out, tok;
    std::istringstream in(s);
    while (in >> tok) {
        if (drop.count(tok)) continue;
        if (!out.empty()) out += ' ';
        out += tok;
    }
    return out;
}

std::vector<MajorInfo> groupMajors(const std::vector<ArchiveEvent>& events, size_t wanted) {
    std::map<std::string, MajorInfo> groups;
    std::map<std::string, int> groupDay;
    for (auto& e : events) {
        std::string key = majorGroupKey(e.name);
        auto& g = groups[key];
        if (g.name.empty() || e.name.size() < g.name.size()) g.name = e.name;
        g.eventIds.push_back(e.id);
        int& d = groupDay[key];
        if (d == 0 || (e.day > 0 && e.day < d)) d = e.day;
    }
    std::vector<std::pair<int, MajorInfo>> v;
    for (auto& kv : groups) {
        MajorInfo mi = kv.second;
        int d = groupDay[kv.first];
        mi.date = d > 0 ? util::dateString(d) : "";
        v.push_back({d, mi});
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<MajorInfo> out;
    int today = util::todayDay();
    for (auto& p : v) {
        if (p.first > today) continue;  // Major que ainda não começou
        if (out.size() >= wanted) break;
        out.push_back(p.second);
    }
    return out;
}

// ============================================================================
// Ranking: /ranking/teams
// ============================================================================
std::vector<RankedTeam> parseRanking(const std::string& h) {
    std::vector<RankedTeam> out;
    for (auto& blk : blocks(h, "class=\"ranked-team", 30000)) {
        RankedTeam t;
        size_t pp = blk.find("class=\"position");
        if (pp != NPOS) t.rank = static_cast<int>(util::toNum(util::cellText(blk, pp, "</"), 0));
        for (size_t np : positions(blk, "class=\"name")) {
            std::string n = util::cellText(blk, np, "</");
            if (!n.empty()) { t.name = n; break; }
        }
        size_t pt = blk.find("class=\"points");
        if (pt != NPOS) t.points = util::toNum(util::cellText(blk, pt, "</"), 0);
        std::set<std::string> seen;
        for (const char* marker : {"class=\"rankingNicknames\"", "class=\"nick\""}) {
            if (t.lineup.size() >= 5) break;
            for (size_t p : positions(blk, marker)) {
                std::string nick = util::cellText(blk, p, "</div>");
                if (nick.empty() || seen.count(util::lower(nick))) continue;
                seen.insert(util::lower(nick));
                t.lineup.push_back(nick);
                if (t.lineup.size() >= 5) break;
            }
        }
        if (!t.name.empty()) out.push_back(t);
    }
    return out;
}

// ============================================================================
// Estatísticas de jogadores: /stats/players?...
// ============================================================================
std::vector<PlayerCurrent> parsePlayerStats(const std::string& h) {
    std::vector<PlayerCurrent> out;
    size_t tp = h.find("player-ratings-table");
    if (tp == NPOS) return out;
    size_t te = h.find("</table>", tp);
    std::string table = h.substr(tp, te == NPOS ? NPOS : te - tp);
    for (auto& row : blocks(table, "<tr", 6000)) {
        size_t pc = row.find("class=\"playerCol");
        if (pc == NPOS) continue;
        PlayerCurrent p;
        p.name = util::cellText(row, pc, "</td>");
        if (p.name.empty()) continue;
        size_t tc = row.find("class=\"teamCol");
        if (tc != NPOS) {
            p.team = util::attrValue(util::tagAt(row, tc), "data-sort");
            if (p.team.empty()) p.team = util::attrValue(row, "title", tc);
        }
        std::vector<double> nums;
        for (size_t sp : positions(row, "class=\"statsDetail")) nums.push_back(util::toNum(util::cellText(row, sp, "</td>"), 0));
        if (nums.size() >= 1) p.maps = static_cast<int>(nums[0]);
        if (nums.size() >= 3) p.kd = nums[2];
        size_t rc = row.find("class=\"ratingCol");
        if (rc != NPOS) p.rating = util::toNum(util::cellText(row, rc, "</td>"), 0);
        out.push_back(p);
    }
    return out;
}

// ============================================================================
// Cliente HTTP
// ============================================================================
bool sslAvailable() {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    return true;
#else
    return false;
#endif
}

struct FetchResult {
    int status = 0;
    std::string body, error;
    bool blocked = false;
};

static bool looksBlocked(int status, const std::string& body) {
    if (status == 403 || status == 503 || status == 429) {
        if (body.find("Just a moment") != NPOS || body.find("challenge-platform") != NPOS ||
            body.find("cf-browser-verification") != NPOS || body.find("Attention Required") != NPOS ||
            body.find("cf_chl") != NPOS || status == 403)
            return true;
    }
    return false;
}

class Fetcher {
public:
    explicit Fetcher(int delayMs) : delayMs_(delayMs), rng_(std::random_device{}()) {}

    FetchResult get(const std::string& path, const std::atomic<bool>& stop) {
        FetchResult r;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::Client cli("https://www.hltv.org");
        cli.set_follow_location(true);
        cli.set_connection_timeout(20);
        cli.set_read_timeout(40);
        httplib::Headers hd = {
            {"User-Agent",
             "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36"},
            {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
            {"Accept-Language", "en-US,en;q=0.9"}};
        for (int attempt = 0; attempt < 3 && !stop; ++attempt) {
            waitTurn(attempt);
            auto res = cli.Get(path, hd);
            last_ = std::chrono::steady_clock::now();
            if (!res) {
                r.error = httplib::to_string(res.error());
                continue;
            }
            r.status = res->status;
            r.body = res->body;
            if (looksBlocked(res->status, res->body)) {
                r.blocked = true;
                return r;
            }
            if (res->status >= 500) continue;
            return r;
        }
#else
        (void)path;
        (void)stop;
        r.error = "binario compilado sem OpenSSL (HTTPS indisponivel)";
#endif
        return r;
    }

private:
    void waitTurn(int attempt) {
        std::uniform_int_distribution<int> jitter(0, delayMs_ / 2 + 1);
        auto wait = std::chrono::milliseconds(delayMs_ * (1 + attempt * 2) + jitter(rng_));
        auto next = last_ + wait;
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_for(next - now);
    }
    int delayMs_;
    std::chrono::steady_clock::time_point last_{};
    std::mt19937 rng_;
};

// ============================================================================
// Coletor
// ============================================================================
bool collect(const std::string& rawDir, const CollectOptions& opt,
             const std::function<void(double, const std::string&)>& progress,
             const std::function<void(const std::string&)>& log, const std::atomic<bool>& stop,
             bool& blocked) {
    namespace fs = std::filesystem;
    blocked = false;
    if (!sslAvailable()) {
        log("Coleta HTTP indisponivel: binario sem OpenSSL.");
        return false;
    }
    Fetcher fetcher(opt.delayMs);
    int requests = 0;

    // maxAgeHours < 0 => cache permanente
    auto fetch = [&](const std::string& path, const std::string& file, double maxAgeHours,
                     std::string& body) -> bool {
        std::string full = rawDir + "/" + file;
        std::error_code ec;
        if (fs::exists(full, ec) && !(opt.forceRefresh && maxAgeHours >= 0)) {
            bool fresh = maxAgeHours < 0;
            if (!fresh) {
                auto age = fs::file_time_type::clock::now() - fs::last_write_time(full, ec);
                fresh = !ec && std::chrono::duration_cast<std::chrono::hours>(age).count() < maxAgeHours;
            }
            if (fresh && util::readFile(full, body)) return true;
        }
        if (blocked || stop) return false;
        auto r = fetcher.get(path, stop);
        ++requests;
        if (r.blocked) {
            blocked = true;
            log("HLTV bloqueou a requisicao (HTTP " + std::to_string(r.status) + ", protecao Cloudflare) em " + path);
            return false;
        }
        if (r.status != 200) {
            log("Falha em " + path + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
            return false;
        }
        util::writeFile(full, r.body);
        body = std::move(r.body);
        return true;
    };

    std::string body;
    progress(0.01, "Lendo arquivo de Majors");
    if (!fetch("/events/archive?eventType=MAJOR", "archive_major.html", 24, body)) return false;
    auto events = parseArchive(body);
    auto majors = groupMajors(events, static_cast<size_t>(opt.majors));
    log("Arquivo: " + std::to_string(events.size()) + " eventos, " + std::to_string(majors.size()) + " Majors selecionadas");

    // dados atuais primeiro (mesmo se a coleta for interrompida depois)
    progress(0.03, "Ranking atual");
    if (fetch("/ranking/teams", "ranking.html", 12, body))
        log("Ranking: " + std::to_string(parseRanking(body).size()) + " times");
    int today = util::todayDay();
    std::string statsPath = "/stats/players?startDate=" + util::dateString(today - 90) +
                            "&endDate=" + util::dateString(today) + "&rankingFilter=Top50";
    progress(0.05, "Estatisticas atuais dos jogadores");
    if (fetch(statsPath, "playerstats.html", 12, body))
        log("Stats de jogadores (90 dias): " + std::to_string(parsePlayerStats(body).size()) + " jogadores");

    // listas de partidas das Majors
    std::map<std::string, ResultEntry> toFetch;
    for (size_t i = 0; i < majors.size() && !stop && !blocked; ++i) {
        const auto& mj = majors[i];
        progress(0.06 + 0.1 * i / std::max<size_t>(1, majors.size()), "Partidas: " + mj.name);
        bool old = !mj.date.empty() && util::dayNumber(mj.date) < today - 60;
        for (const auto& eid : mj.eventIds) {
            for (int off = 0; off <= 400 && !stop; off += 100) {
                std::string file = "results_event_" + eid + "_" + std::to_string(off) + ".html";
                std::string path = "/results?event=" + eid + (off ? "&offset=" + std::to_string(off) : "");
                if (!fetch(path, file, old ? -1 : 12, body)) break;
                auto entries = parseResults(body);
                for (auto& e : entries) toFetch.emplace(e.matchId, e);
                if (entries.size() < 100) break;
            }
        }
    }

    // resultados recentes (forma atual)
    for (int off = 0; off <= 2000 && !stop && !blocked; off += 100) {
        progress(0.17, "Resultados recentes (offset " + std::to_string(off) + ")");
        std::string file = "results_recent_" + std::to_string(off) + ".html";
        std::string path = "/results?stars=1" + (off ? "&offset=" + std::to_string(off) : std::string());
        if (!fetch(path, file, 12, body)) break;
        auto entries = parseResults(body);
        int oldest = INT_MAX;
        for (auto& e : entries) {
            if (e.day > 0) oldest = std::min(oldest, e.day);
            if (e.day == 0 || e.day >= today - opt.recentDays) toFetch.emplace(e.matchId, e);
        }
        if (entries.empty() || oldest < today - opt.recentDays) break;
    }
    log("Partidas a obter: " + std::to_string(toFetch.size()));

    // páginas das partidas (cache permanente: partida encerrada não muda)
    size_t done = 0, ok = 0;
    for (auto& kv : toFetch) {
        if (stop || blocked) break;
        ++done;
        progress(0.2 + 0.8 * done / toFetch.size(),
                 "Partida " + std::to_string(done) + "/" + std::to_string(toFetch.size()));
        if (fetch(kv.second.path, "match_" + kv.first + ".html", -1, body)) ++ok;
    }
    log("Coleta finalizada: " + std::to_string(ok) + " partidas disponiveis, " + std::to_string(requests) +
        " requisicoes feitas" + (blocked ? " (interrompida por bloqueio)" : ""));
    return !blocked && !stop;
}

}  // namespace hltv
