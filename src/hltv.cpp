#include "hltv.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <thread>
#include <tuple>

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
    if (h.find("data-multi-stage-simulator-json") != NPOS) return PageType::Event;
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
        case PageType::Event: return "evento";
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

int rankingDay(const std::string& h) {
    static const char* MONTHS[] = {"january", "february", "march", "april", "may", "june", "july",
                                   "august", "september", "october", "november", "december"};
    auto monthOf = [](const std::string& s) {
        for (int i = 0; i < 12; ++i)
            if (s.rfind(MONTHS[i], 0) == 0 || (s.size() >= 3 && std::string(MONTHS[i]).rfind(s.substr(0, 3), 0) == 0 && s.size() <= 4))
                return i + 1;
        return 0;
    };
    // 1) URL: meta pk-source (coletor) ou canonical
    std::vector<std::string> urls;
    size_t mp = h.find("name=\"pk-source\"");
    if (mp != NPOS) urls.push_back(util::attrValue(util::tagAt(h, mp), "content"));
    urls.push_back(canonicalUrl(h));
    for (const auto& u : urls) {
        size_t p = u.find("/ranking/teams/");
        if (p == NPOS) continue;
        std::string rest = util::lower(u.substr(p + 15));  // 2025/november/24
        int y = 0, d = 0;
        std::string mon;
        size_t s1 = rest.find('/'), s2 = s1 == NPOS ? NPOS : rest.find('/', s1 + 1);
        if (s2 == NPOS) continue;
        y = static_cast<int>(util::toNum(rest.substr(0, s1), 0));
        mon = rest.substr(s1 + 1, s2 - s1 - 1);
        d = static_cast<int>(util::toNum(util::digitsAt(rest, s2 + 1), 0));
        int m = monthOf(mon);
        if (y > 2000 && m && d >= 1 && d <= 31) return util::daysFromCivil(y, m, d);
    }
    // 2) cabeçalho: "... ranking on November 24th 2025" / "November 24th, 2025"
    std::string low = util::lower(h);
    for (size_t p = low.find("ranking on "); p != NPOS; p = low.find("ranking on ", p + 1)) {
        std::string txt = util::stripTags(low.substr(p + 11, 80));
        std::string mon;
        size_t i = 0;
        while (i < txt.size() && std::isspace(static_cast<unsigned char>(txt[i]))) ++i;
        while (i < txt.size() && std::isalpha(static_cast<unsigned char>(txt[i]))) mon += txt[i++];
        int m = monthOf(mon);
        if (!m) continue;
        std::string ds = util::digitsAt(txt, txt.find_first_of("0123456789", i));
        size_t yp = txt.find("20", i + ds.size() + 1);
        if (ds.empty() || yp == NPOS) continue;
        int d = static_cast<int>(util::toNum(ds, 0));
        int y = static_cast<int>(util::toNum(txt.substr(yp, 4), 0));
        if (y > 2000 && d >= 1 && d <= 31) return util::daysFromCivil(y, m, d);
    }
    return 0;
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
// Página de evento: JSON do simulador multi-estágio da HLTV
// ============================================================================
namespace {
using json = nlohmann::json;

// {"eventTeamSlotId":{...},"teamId":{"teamId":4411}} -> 4411
long long teamIdOf(const json& t) {
    if (t.is_object() && t.contains("teamId") && t["teamId"].is_object() && t["teamId"].contains("teamId") &&
        t["teamId"]["teamId"].is_number())
        return t["teamId"]["teamId"].get<long long>();
    return -1;
}

bool bucketWL(const std::string& b, int& w, int& l) {
    static const std::map<std::string, std::pair<int, int>> m = {
        {"ZeroZero", {0, 0}}, {"OneZero", {1, 0}}, {"ZeroOne", {0, 1}}, {"TwoZero", {2, 0}}, {"OneOne", {1, 1}},
        {"ZeroTwo", {0, 2}},  {"TwoOne", {2, 1}},  {"OneTwo", {1, 2}},  {"TwoTwo", {2, 2}}};
    auto it = m.find(b);
    if (it == m.end()) return false;
    w = it->second.first;
    l = it->second.second;
    return true;
}

// Procura recursivamente um "teamName" (slots da chave dos playoffs).
std::string findTeamName(const json& j) {
    if (j.is_object()) {
        if (j.contains("teamName") && j["teamName"].is_string()) {
            std::string n = j["teamName"].get<std::string>();
            if (n != "TBD") return n;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string s = findTeamName(it.value());
            if (!s.empty()) return s;
        }
    } else if (j.is_array()) {
        for (auto& x : j) {
            std::string s = findTeamName(x);
            if (!s.empty()) return s;
        }
    }
    return "";
}

int intField(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) return j[key].get<int>();
    return -1;
}
}  // namespace

bool parseEvent(const std::string& h, EventInfo& ev) {
    const std::string marker = "data-multi-stage-simulator-json=\"";
    size_t p = h.find(marker);
    if (p == NPOS) return false;
    p += marker.size();
    size_t q = h.find('"', p);
    if (q == NPOS) return false;
    json j;
    try {
        j = json::parse(util::htmlDecode(h.substr(p, q - p)));
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;
    ev.name = j.contains("name") && j["name"].is_string() ? j["name"].get<std::string>() : "";
    std::string url = canonicalUrl(h);
    size_t ep = url.find("/events/");
    if (ep != NPOS) ev.eventId = util::digitsAt(url, ep + 8);
    for (size_t mp : positions(h, "class=\"map-pool-map-name")) {
        std::string m = util::cellText(h, mp, "</");
        if (!m.empty()) ev.pool.push_back(m);
    }

    // ---------------- estágios Swiss
    if (j.contains("swissStages") && j["swissStages"].is_array()) {
        for (const auto& st : j["swissStages"]) {
            if (!st.is_object() || !st.contains("group") || !st["group"].is_object()) continue;
            const json& g = st["group"];
            StageInfo s;
            s.type = "swiss";
            s.size = 16;
            s.name = st.contains("name") && st["name"].is_string() ? st["name"].get<std::string>() : "Swiss";
            s.eventName = g.contains("eventName") && g["eventName"].is_string() ? g["eventName"].get<std::string>() : s.name;

            std::map<long long, std::string> names;
            if (g.contains("teams") && g["teams"].is_array())
                for (const auto& t : g["teams"]) {
                    if (!t.is_object() || !t.contains("teamId") || !t.contains("teamName") || !t["teamName"].is_string()) continue;
                    long long id = teamIdOf(t["teamId"]);
                    std::string n = t["teamName"].get<std::string>();
                    if (id > 0 && !n.empty() && n != "TBD") names[id] = n;
                }
            auto nameOf = [&](const json& tid) {
                auto it = names.find(teamIdOf(tid));
                return it == names.end() ? std::string() : it->second;
            };

            std::set<std::string> seeded;
            if (g.contains("initialSeedings") && g["initialSeedings"].is_object() &&
                g["initialSeedings"].contains("orderedSeedings") && g["initialSeedings"]["orderedSeedings"].is_array())
                for (const auto& sd : g["initialSeedings"]["orderedSeedings"]) {
                    if (!sd.is_object() || !sd.contains("teamId")) continue;
                    StageTeam t;
                    t.name = nameOf(sd["teamId"]);
                    t.seed = intField(sd, "seedingOrdinal");
                    if (t.name.empty() || t.seed <= 0) continue;
                    seeded.insert(t.name);
                    s.teams.push_back(t);
                }
            for (auto& kv : names)
                if (!seeded.count(kv.second)) s.teams.push_back({kv.second, 0});

            // resultados: lista achatada [chave, valor, chave, valor, ...]
            std::map<std::tuple<int, int, long long, long long>, std::pair<int, int>> results;
            if (g.contains("results") && g["results"].is_array()) {
                const json& r = g["results"];
                for (size_t i = 0; i + 1 < r.size(); i += 2) {
                    const json& k = r[i];
                    const json& v = r[i + 1];
                    int w, l;
                    if (!k.is_object() || !k.contains("swissScore") || !k["swissScore"].is_string() ||
                        !bucketWL(k["swissScore"].get<std::string>(), w, l) || !k.contains("matchupId"))
                        continue;
                    const json& mu = k["matchupId"];
                    if (!mu.is_object() || !mu.contains("team1") || !mu.contains("team2")) continue;
                    results[std::make_tuple(w, l, teamIdOf(mu["team1"]), teamIdOf(mu["team2"]))] =
                        {intField(v, "team1Score"), intField(v, "team2Score")};
                }
            }

            if (g.contains("matches") && g["matches"].is_object()) {
                for (auto it = g["matches"].begin(); it != g["matches"].end(); ++it) {
                    int w, l;
                    if (!bucketWL(it.key(), w, l) || !it.value().is_array()) continue;
                    for (const auto& m : it.value()) {
                        if (!m.is_object() || !m.contains("team1") || !m.contains("team2")) continue;
                        long long ia = teamIdOf(m["team1"]), ib = teamIdOf(m["team2"]);
                        StageMatch sm;
                        sm.a = names.count(ia) ? names[ia] : "";
                        sm.b = names.count(ib) ? names[ib] : "";
                        if (sm.a.empty() || sm.b.empty()) continue;
                        sm.w = w;
                        sm.l = l;
                        sm.round = w + l + 1;
                        if (m.contains("timestamp") && m["timestamp"].is_number()) sm.ts = m["timestamp"].get<long long>();
                        if (m.contains("matchId") && m["matchId"].is_object() && m["matchId"].contains("matchId") &&
                            m["matchId"]["matchId"].is_number())
                            sm.matchId = std::to_string(m["matchId"]["matchId"].get<long long>());
                        auto rit = results.find(std::make_tuple(w, l, ia, ib));
                        if (rit != results.end() && rit->second.first >= 0 && rit->second.second >= 0) {
                            sm.scoreA = rit->second.first;
                            sm.scoreB = rit->second.second;
                            if (sm.scoreA != sm.scoreB) sm.winner = sm.scoreA > sm.scoreB ? sm.a : sm.b;
                        }
                        s.matches.push_back(sm);
                    }
                }
            }
            ev.stages.push_back(s);
        }
    }

    // ---------------- playoffs
    if (j.contains("playoffs") && j["playoffs"].is_object()) {
        const json& po = j["playoffs"];
        StageInfo s;
        s.type = "playoffs";
        s.size = 8;
        s.name = po.contains("name") && po["name"].is_string() ? po["name"].get<std::string>() : "Playoffs";
        s.eventName = ev.name;
        if (po.contains("placement") && po["placement"].is_object() && po["placement"].contains("quarterfinals") &&
            po["placement"]["quarterfinals"].is_array())
            for (const auto& qf : po["placement"]["quarterfinals"]) {
                int a = intField(qf, "team1"), b = intField(qf, "team2");
                if (a > 0 && b > 0) s.qfSeeds.push_back({a, b});
            }
        if (po.contains("bracket") && po["bracket"].is_object() && po["bracket"].contains("rounds") &&
            po["bracket"]["rounds"].is_array()) {
            int ri = 0;
            for (const auto& rd : po["bracket"]["rounds"]) {
                ++ri;
                for (int k = 1; k <= 8; ++k) {
                    std::string key = "slot" + std::to_string(k);
                    if (!rd.is_object() || !rd.contains(key) || !rd[key].is_object()) break;
                    const json& sl = rd[key];
                    StageMatch sm;
                    sm.round = ri;
                    sm.slot = k;
                    if (sl.contains("matchup") && sl["matchup"].is_object()) {
                        const json& mu = sl["matchup"];
                        if (mu.contains("team1")) sm.a = findTeamName(mu["team1"]);
                        if (mu.contains("team2")) sm.b = findTeamName(mu["team2"]);
                        if (mu.contains("score") && mu["score"].is_object()) {
                            const json& sc = mu["score"];
                            sm.scoreA = std::max(intField(sc, "team1Score"), intField(sc, "team1"));
                            sm.scoreB = std::max(intField(sc, "team2Score"), intField(sc, "team2"));
                            if (sm.scoreA >= 0 && sm.scoreB >= 0 && sm.scoreA != sm.scoreB && !sm.a.empty() && !sm.b.empty())
                                sm.winner = sm.scoreA > sm.scoreB ? sm.a : sm.b;
                        }
                    }
                    s.matches.push_back(sm);
                }
            }
        }
        // times conhecidos dos playoffs: os das quartas, na ordem da chave
        int pos = 0;
        for (const auto& m : s.matches) {
            if (m.round != 1) continue;
            if (!m.a.empty()) s.teams.push_back({m.a, ++pos}); else ++pos;
            if (!m.b.empty()) s.teams.push_back({m.b, ++pos}); else ++pos;
        }
        ev.stages.push_back(s);
    }
    return !ev.name.empty() && !ev.stages.empty();
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
