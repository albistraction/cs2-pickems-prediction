// Cura dos dados: transforma páginas brutas (HTML da HLTV / JSON importado)
// em um dataset limpo, padronizado e sem duplicatas.
#pragma once
#include <functional>
#include <string>

#include "data.hpp"

// Chave canônica de time: "Team Vitality" / "Vitality" -> "vitality"
std::string teamKey(const std::string& name);
// "de_dust2" / "Dust II" -> "dust2" ("" se não for mapa competitivo conhecido)
std::string mapKey(const std::string& name);
std::string playerKey(const std::string& name);

Dataset curate(const std::string& rawDir, int majorsWanted,
               const std::function<void(const std::string&)>& log);
