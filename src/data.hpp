// Estruturas de dados do dataset curado (serializadas em JSON).
#pragma once
#include <string>
#include <vector>

#include "json.hpp"

struct PlayerLine {
    std::string name;
    int kills = 0, deaths = 0;
    double adr = 0, kast = 0, rating = 0;  // rating 0 = indisponível
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlayerLine, name, kills, deaths, adr, kast, rating)

struct MapResult {
    std::string map;
    int r1 = 0, r2 = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MapResult, map, r1, r2)

struct Match {
    std::string id, date, event, eventId, tier, source;
    int day = 0;
    int stars = -1;  // -1 = desconhecido
    bool major = false;
    std::string team1, team2;
    std::vector<MapResult> maps;
    std::vector<PlayerLine> p1, p2;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Match, id, date, event, eventId, tier, source, day, stars,
                                                major, team1, team2, maps, p1, p2)

struct RankedTeam {
    std::string name;
    int rank = 0;
    double points = 0;
    std::vector<std::string> lineup;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RankedTeam, name, rank, points, lineup)

struct PlayerCurrent {
    std::string name, team;
    int maps = 0;
    double kd = 0, rating = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlayerCurrent, name, team, maps, kd, rating)

struct MajorInfo {
    std::string name, date;
    std::vector<std::string> eventIds;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MajorInfo, name, date, eventIds)

// ---------------------------------------------------------------- estrutura de um evento (Major)
// Partida de um estágio. Swiss: w/l = campanha das equipes antes do jogo
// (0-0, 1-0, ...). Playoffs: round 1 = quartas, 2 = semi, 3 = final; slot = posição na rodada.
struct StageMatch {
    std::string a, b, winner;  // winner vazio = ainda não jogado
    int w = 0, l = 0;
    int round = 0, slot = 0;
    int scoreA = 0, scoreB = 0;
    long long ts = 0;
    std::string matchId;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StageMatch, a, b, winner, w, l, round, slot, scoreA, scoreB, ts, matchId)

struct StageTeam {
    std::string name;  // vazio = vaga TBD
    int seed = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StageTeam, name, seed)

struct StageInfo {
    std::string name, eventName, type;  // type: "swiss" | "playoffs"
    int size = 16;                      // nº de vagas
    std::vector<StageTeam> teams;       // times conhecidos com seed
    std::vector<StageMatch> matches;
    std::vector<std::vector<int>> qfSeeds;  // playoffs: pares de seeds das quartas (ex.: [1,8])
    long long start = 0, end = 0;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StageInfo, name, eventName, type, size, teams, matches, qfSeeds, start, end)

struct EventInfo {
    std::string name, eventId, updatedAt;
    std::vector<std::string> pool;  // map pool do evento
    std::vector<StageInfo> stages;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(EventInfo, name, eventId, updatedAt, pool, stages)

struct Dataset {
    std::string generatedAt;
    std::vector<Match> matches;
    std::vector<RankedTeam> ranking;
    std::vector<PlayerCurrent> players;
    std::vector<MajorInfo> majors;
    std::vector<EventInfo> events;
    nlohmann::json report = nlohmann::json::object();
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Dataset, generatedAt, matches, ranking, players, majors, events, report)
