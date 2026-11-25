// Parsers de páginas da HLTV + coletor com cache e limite de requisições.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "data.hpp"

namespace hltv {

enum class PageType { Unknown, Match, Results, Archive, Ranking, PlayerStats, Event };
PageType detect(const std::string& html);
const char* typeName(PageType t);

struct ResultEntry {
    std::string matchId, path, event;
    int day = 0;
    int stars = 0;
};
struct ArchiveEvent {
    std::string id, path, name;
    int day = 0;
};

bool parseMatch(const std::string& html, Match& out);
std::vector<ResultEntry> parseResults(const std::string& html);
std::vector<ArchiveEvent> parseArchive(const std::string& html);
std::vector<RankedTeam> parseRanking(const std::string& html);
std::vector<PlayerCurrent> parsePlayerStats(const std::string& html);
// Página de evento (/events/{id}/...): lê o JSON do simulador multi-estágio da HLTV
// (data-multi-stage-simulator-json) com todos os estágios, seeds, jogos e resultados.
bool parseEvent(const std::string& html, EventInfo& out);

// "BLAST.tv Austin Major 2025 Stage 1" -> "blast.tv austin major 2025"
std::string majorGroupKey(const std::string& eventName);

// Agrupa eventos de Major (estágios separados) e devolve os N mais recentes.
std::vector<MajorInfo> groupMajors(const std::vector<ArchiveEvent>& events, size_t wanted);

bool sslAvailable();

struct CollectOptions {
    int majors = 10;         // quantas Majors baixar
    int recentDays = 120;    // janela de resultados recentes (forma atual)
    int delayMs = 3500;      // intervalo entre requisições
    bool forceRefresh = false;
};

// Coleta páginas para `rawDir`. Retorna false se não conseguiu concluir.
bool collect(const std::string& rawDir, const CollectOptions& opt,
             const std::function<void(double, const std::string&)>& progress,
             const std::function<void(const std::string&)>& log, const std::atomic<bool>& stop,
             bool& blocked);

}  // namespace hltv
