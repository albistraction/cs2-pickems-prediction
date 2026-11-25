#include "curation.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

#include "hltv.hpp"
#include "util.hpp"

using json = nlohmann::json;

// ============================================================================
// Normalização de nomes
// ============================================================================
std::string teamKey(const std::string& name) {
    std::string s = util::lower(util::htmlDecode(util::trim(name)));
    std::string k;
    for (unsigned char c : s)
        if (std::isalnum(c)) k += static_cast<char>(c);
    static const std::map<std::string, std::string> alias = {
        {"natusvincere", "navi"}, {"navi", "navi"},           {"ninjasinpyjamas", "nip"},
        {"nip", "nip"},           {"vp", "virtuspro"},        {"virtuspro", "virtuspro"},
        {"mousesports", "mouz"},  {"themongolz", "mongolz"},  {"mongolz", "mongolz"},
        {"teamspirit", "spirit"}, {"teamliquid", "liquid"},   {"teamvitality", "vitality"},
        {"faze", "faze"},         {"fazeclan", "faze"},       {"g2esports", "g2"},
        {"furiaesports", "furia"}, {"paingaming", "pain"},    {"imperialesports", "imperial"},
        {"teamfalcons", "falcons"}, {"falconsesports", "falcons"}};
    auto it = alias.find(k);
    if (it != alias.end()) return it->second;
    static const std::vector<std::string> suffixes = {"esports", "esport", "gaming", "clan"};
    for (const auto& suf : suffixes)
        if (k.size() > suf.size() + 1 && k.compare(k.size() - suf.size(), suf.size(), suf) == 0) {
            k.erase(k.size() - suf.size());
            break;
        }
    if (k.size() > 6 && k.rfind("team", 0) == 0) k = k.substr(4);
    it = alias.find(k);
    return it != alias.end() ? it->second : k;
}

std::string mapKey(const std::string& name) {
    std::string s = util::lower(util::trim(name));
    if (s.rfind("de_", 0) == 0) s = s.substr(3);
    std::string k;
    for (unsigned char c : s)
        if (std::isalnum(c)) k += static_cast<char>(c);
    if (k == "dust2" || k == "dustii" || k == "d2") return "dust2";
    if (k == "cobble") return "cobblestone";
    static const std::set<std::string> valid = {"ancient", "anubis",   "dust2",  "inferno",     "mirage",
                                                "nuke",    "overpass", "train",  "vertigo",     "cache",
                                                "cobblestone", "season", "tuscan"};
    return valid.count(k) ? k : "";
}

std::string playerKey(const std::string& name) { return util::lower(util::trim(util::htmlDecode(name))); }

// ============================================================================
// Cura
// ============================================================================
namespace {

struct Counter {
    std::map<std::string, int> c;
    void add(const std::string& k, int n = 1) { c[k] += n; }
    json toJson() const {
        json j = json::object();
        for (auto& kv : c) j[kv.first] = kv.second;
        return j;
    }
};

bool validScore(int a, int b) {
    int hi = std::max(a, b), lo = std::min(a, b);
    // MR12 (13) ou MR15 (16) + prorrogações: vencedor >= 13, diferença >= 2
    return lo >= 0 && hi >= 13 && hi <= 80 && hi - lo >= 2;
}

std::string tierFromStars(int stars) {
    if (stars >= 3) return "S";
    if (stars == 2) return "A";
    if (stars == 1) return "B";
    if (stars == 0) return "C";
    return "B";  // desconhecido
}

}  // namespace

Dataset curate(const std::string& rawDir, int majorsWanted, const std::function<void(const std::string&)>& log) {
    namespace fs = std::filesystem;
    Dataset ds;
    ds.generatedAt = util::nowString();
    json rep = json::object();
    json warnings = json::array();

    std::map<std::string, Match> raw;  // por id
    std::map<std::string, hltv::ResultEntry> resultIndex;
    std::vector<hltv::ArchiveEvent> archive;
    std::set<std::string> archiveIds;
    std::vector<RankedTeam> ranking;
    std::vector<PlayerCurrent> players;
    fs::file_time_type rankingTime{}, playersTime{};
    bool haveRanking = false, havePlayers = false;

    Counter byType, fileErrors;
    int htmlFiles = 0, jsonFiles = 0, duplicates = 0, parseFailed = 0;

    std::error_code ec;
    if (!fs::exists(rawDir, ec)) fs::create_directories(rawDir, ec);

    // ------------------------------------------------------------ 1. leitura
    for (auto& entry : fs::directory_iterator(rawDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = util::lower(entry.path().extension().string());
        std::string path = entry.path().string();
        std::string body;
        if (!util::readFile(path, body)) continue;
        auto mtime = fs::last_write_time(entry.path(), ec);

        if (ext == ".html" || ext == ".htm") {
            ++htmlFiles;
            auto t = hltv::detect(body);
            byType.add(hltv::typeName(t));
            switch (t) {
                case hltv::PageType::Match: {
                    Match m;
                    if (hltv::parseMatch(body, m)) {
                        m.source = "hltv";
                        if (!raw.emplace(m.id, m).second) ++duplicates;
                    } else ++parseFailed;
                    break;
                }
                case hltv::PageType::Results:
                    for (auto& e : hltv::parseResults(body)) resultIndex[e.matchId] = e;
                    break;
                case hltv::PageType::Archive:
                    for (auto& e : hltv::parseArchive(body))
                        if (archiveIds.insert(e.id).second) archive.push_back(e);
                    break;
                case hltv::PageType::Ranking:
                    if (!haveRanking || mtime > rankingTime) {
                        auto r = hltv::parseRanking(body);
                        if (!r.empty()) { ranking = r; rankingTime = mtime; haveRanking = true; }
                    }
                    break;
                case hltv::PageType::PlayerStats:
                    if (!havePlayers || mtime > playersTime) {
                        auto p = hltv::parsePlayerStats(body);
                        if (!p.empty()) { players = p; playersTime = mtime; havePlayers = true; }
                    }
                    break;
                default:
                    fileErrors.add(entry.path().filename().string());
            }
        } else if (ext == ".json") {
            ++jsonFiles;
            try {
                json j = json::parse(body);
                if (j.contains("matches") && j["matches"].is_array()) {
                    for (auto& item : j["matches"]) {
                        Match m = item.get<Match>();
                        if (m.id.empty()) {
                            std::string sig = m.date + "|" + teamKey(m.team1) + "|" + teamKey(m.team2);
                            for (auto& mp : m.maps) sig += "|" + mp.map + std::to_string(mp.r1) + "-" + std::to_string(mp.r2);
                            m.id = "imp_" + util::fnv1a(sig);
                        }
                        if (m.source.empty()) m.source = "import";
                        if (!raw.emplace(m.id, m).second) ++duplicates;
                    }
                }
                if (j.contains("ranking") && j["ranking"].is_array() && (!haveRanking || mtime > rankingTime)) {
                    ranking = j["ranking"].get<std::vector<RankedTeam>>();
                    rankingTime = mtime;
                    haveRanking = !ranking.empty();
                }
                if (j.contains("players") && j["players"].is_array() && (!havePlayers || mtime > playersTime)) {
                    players = j["players"].get<std::vector<PlayerCurrent>>();
                    playersTime = mtime;
                    havePlayers = !players.empty();
                }
            } catch (const std::exception& e) {
                fileErrors.add(entry.path().filename().string());
                log("JSON invalido em " + entry.path().filename().string() + ": " + e.what());
            }
        }
    }
    log("Arquivos lidos: " + std::to_string(htmlFiles) + " HTML, " + std::to_string(jsonFiles) +
        " JSON, " + std::to_string(raw.size()) + " partidas brutas");

    // ------------------------------------------------------------ 2. Majors
    ds.majors = hltv::groupMajors(archive, static_cast<size_t>(std::max(1, majorsWanted)));
    std::set<std::string> majorEventIds;
    for (auto& mj : ds.majors)
        for (auto& id : mj.eventIds) majorEventIds.insert(id);

    // ------------------------------------------------------------ 3. nomes canônicos
    std::map<std::string, std::map<std::string, int>> votes;  // chave -> grafia -> votos
    for (auto& kv : raw) {
        votes[teamKey(kv.second.team1)][util::trim(kv.second.team1)]++;
        votes[teamKey(kv.second.team2)][util::trim(kv.second.team2)]++;
    }
    for (auto& t : ranking) votes[teamKey(t.name)][util::trim(t.name)] += 1000;  // ranking tem prioridade
    std::map<std::string, std::string> display;
    json aliases = json::array();
    for (auto& kv : votes) {
        if (kv.first.empty()) continue;
        std::string best;
        int bv = -1;
        for (auto& g : kv.second)
            if (g.second > bv) { bv = g.second; best = g.first; }
        display[kv.first] = best;
        if (kv.second.size() > 1) {
            json v = json::array();
            for (auto& g : kv.second) v.push_back(g.first);
            aliases.push_back({{"nome", best}, {"variantes", v}});
        }
    }
    auto canon = [&](const std::string& n) {
        auto it = display.find(teamKey(n));
        return it == display.end() ? util::trim(n) : it->second;
    };

    // ------------------------------------------------------------ 4. limpeza das partidas
    Counter dropMatch, dropMap;
    int mapsKept = 0, playerLines = 0, playerFixed = 0, lineupsDropped = 0, lineupsTrimmed = 0;
    int today = util::todayDay();
    std::set<std::string> teamsSeen;

    for (auto& kv : raw) {
        Match m = kv.second;
        m.team1 = canon(m.team1);
        m.team2 = canon(m.team2);
        std::string k1 = teamKey(m.team1), k2 = teamKey(m.team2);
        if (k1.empty() || k2.empty()) { dropMatch.add("sem_nome_de_time"); continue; }
        if (k1 == k2) { dropMatch.add("time_contra_ele_mesmo"); continue; }

        if (m.day <= 0 && !m.date.empty()) {
            int d = util::dayNumber(m.date);
            if (d != INT_MIN) m.day = d;
        }
        if (m.day <= 0) {
            auto ri = resultIndex.find(m.id);
            if (ri != resultIndex.end() && ri->second.day > 0) m.day = ri->second.day;
        }
        if (m.day <= 0) { dropMatch.add("sem_data"); continue; }
        if (m.day > today + 1) { dropMatch.add("data_no_futuro"); continue; }
        m.date = util::dateString(m.day);

        std::string ev = util::lower(m.event);
        if (ev.find("showmatch") != std::string::npos || ev.find("all-star") != std::string::npos) {
            dropMatch.add("showmatch");
            continue;
        }

        // mapas
        std::vector<MapResult> maps;
        for (auto& mp : m.maps) {
            std::string key = mapKey(mp.map);
            if (util::lower(util::trim(mp.map)) == "default") { dropMap.add("wo_default"); continue; }
            if (key.empty()) { dropMap.add("mapa_desconhecido"); continue; }
            if (!validScore(mp.r1, mp.r2)) { dropMap.add("placar_invalido"); continue; }
            maps.push_back({key, mp.r1, mp.r2});
        }
        if (maps.empty()) { dropMatch.add("sem_mapas_validos(W.O./incompleta)"); continue; }
        m.maps = maps;
        mapsKept += static_cast<int>(maps.size());

        // jogadores
        auto cleanPlayers = [&](std::vector<PlayerLine>& ps) {
            std::vector<PlayerLine> out;
            std::set<std::string> seen;
            for (auto p : ps) {
                ++playerLines;
                p.name = util::trim(util::htmlDecode(p.name));
                std::string pk = playerKey(p.name);
                if (pk.empty() || seen.count(pk)) { ++playerFixed; continue; }
                if (p.kills < 0 || p.deaths < 0 || p.kills > 200 || p.deaths > 200) { ++playerFixed; continue; }
                bool fixed = false;
                if (p.rating < 0 || p.rating > 4) { p.rating = 0; fixed = true; }
                if (p.adr < 0 || p.adr > 300) { p.adr = 0; fixed = true; }
                if (p.kast < 0 || p.kast > 100) { p.kast = 0; fixed = true; }
                if (fixed) ++playerFixed;
                seen.insert(pk);
                out.push_back(p);
            }
            if (out.size() > 5) { out.resize(5); ++lineupsTrimmed; }
            if (out.size() < 4) {
                if (!out.empty() || !ps.empty()) ++lineupsDropped;
                out.clear();
            }
            ps = out;
        };
        cleanPlayers(m.p1);
        cleanPlayers(m.p2);

        // tier / Major
        if (!m.eventId.empty() && majorEventIds.count(m.eventId)) m.major = true;
        auto ri = resultIndex.find(m.id);
        if (m.stars < 0 && ri != resultIndex.end()) m.stars = ri->second.stars;
        if (m.event.empty() && ri != resultIndex.end()) m.event = ri->second.event;
        if (m.major) m.tier = "S";
        else if (m.tier != "S" && m.tier != "A" && m.tier != "B" && m.tier != "C") m.tier = tierFromStars(m.stars);

        teamsSeen.insert(k1);
        teamsSeen.insert(k2);
        ds.matches.push_back(std::move(m));
    }
    std::sort(ds.matches.begin(), ds.matches.end(), [](const Match& a, const Match& b) {
        return a.day != b.day ? a.day < b.day : a.id < b.id;
    });

    // ------------------------------------------------------------ 5. dados atuais
    for (auto& t : ranking) {
        t.name = canon(t.name);
        for (auto& p : t.lineup) p = util::trim(p);
    }
    std::vector<PlayerCurrent> cleanPlayers;
    for (auto p : players) {
        p.name = util::trim(p.name);
        if (p.name.empty() || p.maps <= 0) continue;
        p.team = canon(p.team);
        p.kd = std::clamp(p.kd, 0.0, 4.0);
        p.rating = std::clamp(p.rating, 0.0, 3.0);
        cleanPlayers.push_back(p);
    }
    ds.ranking = ranking;
    ds.players = cleanPlayers;

    if (ranking.empty()) warnings.push_back("Sem pagina de ranking: prior do ranking HLTV desativado.");
    if (cleanPlayers.empty()) warnings.push_back("Sem stats atuais de jogadores: usando so o historico das partidas.");
    if (ds.majors.empty()) warnings.push_back("Sem arquivo de Majors: partidas de Major nao serao marcadas.");
    if (ds.matches.size() < 200) warnings.push_back("Poucas partidas (<200): o treino sera pouco confiavel.");

    // ------------------------------------------------------------ 6. relatório
    int majorMatches = 0, withPlayers = 0;
    for (auto& m : ds.matches) {
        majorMatches += m.major;
        withPlayers += (!m.p1.empty() && !m.p2.empty());
    }
    json majorsJ = json::array();
    for (auto& mj : ds.majors) majorsJ.push_back({{"nome", mj.name}, {"data", mj.date}, {"eventos", mj.eventIds.size()}});

    rep["arquivos"] = {{"html", htmlFiles}, {"json", jsonFiles}, {"porTipo", byType.toJson()},
                       {"naoReconhecidos", fileErrors.toJson()}, {"falhaDeParse", parseFailed}};
    rep["partidas"] = {{"brutas", raw.size() + duplicates},
                       {"duplicadas", duplicates},
                       {"mantidas", ds.matches.size()},
                       {"deMajor", majorMatches},
                       {"comStatsDeJogador", withPlayers},
                       {"descartadas", dropMatch.toJson()}};
    rep["mapas"] = {{"mantidos", mapsKept}, {"descartados", dropMap.toJson()}};
    rep["jogadores"] = {{"linhas", playerLines},
                        {"corrigidasOuRemovidas", playerFixed},
                        {"lineupsDescartadas", lineupsDropped},
                        {"lineupsCortadasPara5", lineupsTrimmed},
                        {"statsAtuais", ds.players.size()}};
    rep["times"] = teamsSeen.size();
    rep["aliasesUnificados"] = aliases;
    rep["periodo"] = ds.matches.empty() ? json::object()
                                        : json{{"inicio", ds.matches.front().date}, {"fim", ds.matches.back().date}};
    rep["majors"] = majorsJ;
    rep["ranking"] = ds.ranking.size();
    rep["avisos"] = warnings;
    ds.report = rep;

    log("Cura: " + std::to_string(ds.matches.size()) + " partidas mantidas (" + std::to_string(majorMatches) +
        " de Major), " + std::to_string(mapsKept) + " mapas, " + std::to_string(teamsSeen.size()) + " times");
    return ds;
}
