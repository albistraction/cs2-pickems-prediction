// pickems-prediction — servidor HTTP
//
// Variáveis de ambiente:
//   PORT              porta HTTP (Railway define automaticamente)       [8080]
//   DATA_DIR          onde ficam cache bruto, dataset e parâmetros        [./data]
//   PUBLIC_DIR        arquivos do site                                    [./public]
//   ADMIN_TOKEN       se definido, exigido (header X-Admin-Token) nas rotas que alteram dados
//   AUTO_COLLECT      1 = servidor tenta coletar da HLTV na 1ª execução  [0]
//                     (a HLTV bloqueia servidores; o normal é usar o coletor-extensao)
//   REFRESH_HOURS     coleta periódica pelo servidor (0 = off)            [0]
//   MAJORS            quantas Majors usar no treino                       [10]
//   RECENT_DAYS       janela de resultados recentes                       [120]
//   HLTV_DELAY_MS     intervalo entre requisições à HLTV                  [3500]
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include "httplib.h"

#include "curation.hpp"
#include "data.hpp"
#include "hltv.hpp"
#include "model.hpp"
#include "util.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct App {
    std::string dataDir, rawDir, publicDir, adminToken;

    std::mutex mu;  // protege tudo abaixo
    std::string phase = "iniciando";
    double progress = 0;
    std::string message;
    bool blocked = false;
    std::string lastCollect;
    std::deque<std::string> logs;
    std::shared_ptr<const Dataset> data;
    std::shared_ptr<const Engine> engine;
    Params params;
    json trainInfo = json::object();

    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
    std::thread worker;
    std::mutex workerMu;

    void log(const std::string& s) {
        std::string line = util::nowString() + "  " + s;
        std::cout << line << std::endl;
        std::lock_guard<std::mutex> g(mu);
        logs.push_back(line);
        while (logs.size() > 400) logs.pop_front();
    }
    void setPhase(const std::string& p, double prog, const std::string& msg) {
        {
            std::lock_guard<std::mutex> g(mu);
            phase = p;
            progress = prog;
            message = msg;
        }
        log("[" + p + "] " + msg);
    }
    void setProgress(double prog, const std::string& msg) {
        std::lock_guard<std::mutex> g(mu);
        progress = prog;
        message = msg;
    }
};

App app;

std::string datasetPath() { return app.dataDir + "/dataset.json"; }
std::string paramsPath() { return app.dataDir + "/params.json"; }
std::string trainPath() { return app.dataDir + "/train.json"; }

void installModel(Dataset ds, const Params& p, const json& tinfo) {
    auto eng = std::make_shared<Engine>(p);
    eng->run(ds.matches, 0, 0);
    auto dsp = std::make_shared<Dataset>(std::move(ds));
    std::lock_guard<std::mutex> g(app.mu);
    app.data = dsp;
    app.engine = eng;
    app.params = p;
    if (!tinfo.is_null()) app.trainInfo = tinfo;
}

struct JobOpts {
    bool collect = true;
    bool forceRefresh = false;
    bool retrain = true;
};

void runPipeline(JobOpts o) {
    auto logfn = [](const std::string& s) { app.log(s); };
    try {
        bool blocked = false;
        if (o.collect) {
            app.setPhase("coletando", 0, "Coletando dados da HLTV");
            hltv::CollectOptions co;
            co.majors = util::envInt("MAJORS", 10);
            co.recentDays = util::envInt("RECENT_DAYS", 120);
            co.delayMs = util::envInt("HLTV_DELAY_MS", 3500);
            co.forceRefresh = o.forceRefresh;
            hltv::collect(app.rawDir, co, [](double p, const std::string& m) { app.setProgress(p, m); }, logfn,
                          app.stop, blocked);
            std::lock_guard<std::mutex> g(app.mu);
            app.blocked = blocked;
            app.lastCollect = util::nowString();
        }
        if (app.stop) {
            app.setPhase("parado", 0, "Interrompido pelo usuario");
            app.running = false;
            return;
        }

        app.setPhase("curando", 0.0, "Curando os dados brutos");
        Dataset ds = curate(app.rawDir, util::envInt("MAJORS", 10), logfn);
        json dsJson = ds;
        util::writeFile(datasetPath(), dsJson.dump());

        Params p;
        {
            std::lock_guard<std::mutex> g(app.mu);
            p = app.params;
        }
        json tinfo;
        if (o.retrain && ds.matches.size() >= 30) {
            app.setPhase("treinando", 0.0, "Treinando parametros com " + std::to_string(ds.matches.size()) + " partidas");
            TrainReport tr = train(ds.matches, p, logfn, app.stop);
            tr.params.sims = p.sims;
            p = tr.params;
            tinfo = tr.toJson();
            tinfo["quando"] = util::nowString();
            util::writeFile(paramsPath(), json(p).dump(2));
            util::writeFile(trainPath(), tinfo.dump(2));
        } else if (ds.matches.size() < 30) {
            app.log("Menos de 30 partidas: usando parametros padrao, sem treino.");
        }
        bool hasData = !ds.matches.empty();
        installModel(std::move(ds), p, tinfo);

        if (!hasData)
            app.setPhase("sem_dados", 1, blocked ? "A HLTV bloqueou a coleta e nao ha dados. Importe paginas/JSON na aba Dados."
                                                 : "Nenhuma partida disponivel. Rode a coleta ou importe dados.");
        else if (blocked)
            app.setPhase("pronto", 1, "Modelo pronto com os dados disponiveis. A HLTV bloqueou parte da coleta; importe o que faltar na aba Dados.");
        else
            app.setPhase("pronto", 1, "Modelo pronto");
    } catch (const std::exception& e) {
        app.setPhase("erro", 0, std::string("Falha no pipeline: ") + e.what());
    }
    app.running = false;
}

bool startJob(JobOpts o) {
    std::lock_guard<std::mutex> g(app.workerMu);
    bool expected = false;
    if (!app.running.compare_exchange_strong(expected, true)) return false;
    app.stop = false;
    if (app.worker.joinable()) app.worker.join();
    app.worker = std::thread(runPipeline, o);
    return true;
}

void loadState() {
    std::string s;
    Params p;
    if (util::readFile(paramsPath(), s)) {
        try { p = json::parse(s).get<Params>(); } catch (...) { app.log("params.json invalido, usando padrao"); }
    }
    json tinfo = json::object();
    if (util::readFile(trainPath(), s)) {
        try { tinfo = json::parse(s); } catch (...) {}
    }
    {
        std::lock_guard<std::mutex> g(app.mu);
        app.params = p;
        app.trainInfo = tinfo;
    }
    if (util::readFile(datasetPath(), s)) {
        try {
            Dataset ds = json::parse(s).get<Dataset>();
            size_t n = ds.matches.size();
            installModel(std::move(ds), p, tinfo);
            app.setPhase(n ? "pronto" : "sem_dados", 1, "Dataset carregado (" + std::to_string(n) + " partidas)");
            return;
        } catch (const std::exception& e) {
            app.log(std::string("dataset.json invalido: ") + e.what());
        }
    }
    if (util::env("AUTO_COLLECT", "0") == "1") {
        app.log("Primeira execucao: iniciando coleta das ultimas Majors para treinar o modelo");
        startJob({true, false, true});
    } else {
        app.setPhase("sem_dados", 0,
                     "Aguardando dados: use a extensao do navegador (pasta coletor-extensao, 'Coleta completa'). "
                     "Ela envia as paginas da HLTV para ca e o treino comeca sozinho.");
    }
}

// Atualiza ranking/stats/resultados recentes periodicamente (dados atuais).
void refreshLoop() {
    int hours = util::envInt("REFRESH_HOURS", 0);
    if (hours <= 0) return;
    auto next = std::chrono::steady_clock::now() + std::chrono::hours(hours);
    while (true) {
        std::this_thread::sleep_for(std::chrono::minutes(5));
        if (std::chrono::steady_clock::now() < next) continue;
        next = std::chrono::steady_clock::now() + std::chrono::hours(hours);
        if (!app.running) {
            app.log("Atualizacao periodica dos dados atuais");
            startJob({true, false, true});
        }
    }
}

void sendJson(httplib::Response& res, const json& j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(), "application/json; charset=utf-8");
}
bool authorized(const httplib::Request& req, httplib::Response& res) {
    if (app.adminToken.empty() || req.get_header_value("X-Admin-Token") == app.adminToken) return true;
    sendJson(res, {{"error", "token de administrador invalido"}}, 401);
    return false;
}
json parseBody(const httplib::Request& req) {
    if (req.body.empty()) return json::object();
    return json::parse(req.body);
}

}  // namespace

int main() {
    app.dataDir = util::env("DATA_DIR", "./data");
    app.rawDir = app.dataDir + "/raw";
    app.publicDir = util::env("PUBLIC_DIR", "./public");
    app.adminToken = util::env("ADMIN_TOKEN");
    std::error_code ec;
    fs::create_directories(app.rawDir, ec);
    if (ec) std::cerr << "Nao consegui criar " << app.rawDir << ": " << ec.message() << std::endl;

    httplib::Server svr;
    svr.set_payload_max_length(64 * 1024 * 1024);
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "erro interno";
        try { if (ep) std::rethrow_exception(ep); }
        catch (const std::exception& e) { msg = e.what(); }
        catch (...) {}
        sendJson(res, {{"error", msg}}, 500);
    });

    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) { sendJson(res, {{"ok", true}}); });

    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> g(app.mu);
        json j = {{"phase", app.phase},
                  {"progress", app.progress},
                  {"message", app.message},
                  {"running", app.running.load()},
                  {"blocked", app.blocked},
                  {"lastCollect", app.lastCollect},
                  {"ssl", hltv::sslAvailable()},
                  {"adminRequired", !app.adminToken.empty()},
                  {"logs", json(std::vector<std::string>(app.logs.begin(), app.logs.end()))},
                  {"params", app.params},
                  {"train", app.trainInfo}};
        if (app.data) {
            j["dataset"] = {{"generatedAt", app.data->generatedAt},
                            {"partidas", app.data->matches.size()},
                            {"ranking", app.data->ranking.size()},
                            {"jogadores", app.data->players.size()},
                            {"majors", app.data->majors}};
            j["report"] = app.data->report;
        }
        sendJson(res, j);
    });

    svr.Get("/api/teams", [](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<const Engine> eng;
        std::shared_ptr<const Dataset> data;
        {
            std::lock_guard<std::mutex> g(app.mu);
            eng = app.engine;
            data = app.data;
        }
        if (!eng || !data) return sendJson(res, json::array());
        int limit = req.has_param("limit") ? std::atoi(req.get_param_value("limit").c_str()) : 40;
        sendJson(res, teamsOverview(*eng, *data, std::clamp(limit, 1, 300)));
    });

    svr.Get("/api/events", [](const httplib::Request&, httplib::Response& res) {
        std::shared_ptr<const Dataset> data;
        {
            std::lock_guard<std::mutex> g(app.mu);
            data = app.data;
        }
        if (!data) return sendJson(res, json::array());
        sendJson(res, eventsSummary(*data));
    });

    svr.Post("/api/predict", [](const httplib::Request& req, httplib::Response& res) {
        std::shared_ptr<const Engine> eng;
        std::shared_ptr<const Dataset> data;
        {
            std::lock_guard<std::mutex> g(app.mu);
            eng = app.engine;
            data = app.data;
        }
        if (!eng || !data) return sendJson(res, {{"error", "modelo ainda nao esta pronto"}}, 503);
        try {
            sendJson(res, predictEvent(*eng, *data, parseBody(req)));
        } catch (const json::exception& e) {
            sendJson(res, {{"error", std::string("JSON invalido: ") + e.what()}}, 400);
        } catch (const std::runtime_error& e) {
            sendJson(res, {{"error", e.what()}}, 400);
        }
    });

    svr.Post("/api/pipeline", [](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) return;
        json b = parseBody(req);
        JobOpts o;
        o.collect = b.value("collect", true);
        o.forceRefresh = b.value("forceRefresh", false);
        o.retrain = b.value("retrain", true);
        if (!startJob(o)) return sendJson(res, {{"error", "ja existe um processo rodando"}}, 409);
        sendJson(res, {{"ok", true}});
    });

    svr.Post("/api/pipeline/stop", [](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) return;
        app.stop = true;
        sendJson(res, {{"ok", true}});
    });

    // Upload de página HTML salva da HLTV (corpo = HTML)
    svr.Post("/api/import/html", [](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) return;
        if (req.body.size() < 200) return sendJson(res, {{"error", "arquivo vazio ou muito pequeno"}}, 400);
        auto type = hltv::detect(req.body);
        if (type == hltv::PageType::Unknown)
            return sendJson(res, {{"error", "nao reconheci como pagina da HLTV (partida, resultados, ranking, stats ou arquivo de eventos)"}}, 400);
        std::string file;
        if (type == hltv::PageType::Match) {
            Match m;
            if (!hltv::parseMatch(req.body, m)) return sendJson(res, {{"error", "pagina de partida sem times/ID legiveis"}}, 400);
            file = "match_" + m.id + ".html";
        } else if (type == hltv::PageType::Event) {
            EventInfo ev;
            if (!hltv::parseEvent(req.body, ev)) return sendJson(res, {{"error", "pagina de evento sem dados da estrutura (simulador da HLTV)"}}, 400);
            file = "event_" + util::fnv1a(ev.name) + ".html";  // uma versão por evento (a mais recente)
        } else {
            file = std::string("import_") + hltv::typeName(type) + "_" + util::fnv1a(req.body) + ".html";
        }
        if (!util::writeFile(app.rawDir + "/" + file, req.body)) return sendJson(res, {{"error", "falha ao gravar"}}, 500);
        bool curating = false;
        if (type == hltv::PageType::Event) curating = startJob({false, false, false});  // atualiza a estrutura do evento já
        sendJson(res, {{"ok", true}, {"tipo", hltv::typeName(type)}, {"arquivo", file}, {"atualizando", curating}});
    });

    // Upload de dataset JSON: {matches:[...], ranking:[...], players:[...]}
    svr.Post("/api/import/json", [](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) return;
        json j;
        try { j = json::parse(req.body); } catch (const std::exception& e) {
            return sendJson(res, {{"error", std::string("JSON invalido: ") + e.what()}}, 400);
        }
        if (!j.is_object() || (!j.contains("matches") && !j.contains("ranking") && !j.contains("players")))
            return sendJson(res, {{"error", "esperado objeto com 'matches', 'ranking' e/ou 'players'"}}, 400);
        std::string file = "import_" + util::fnv1a(req.body) + ".json";
        util::writeFile(app.rawDir + "/" + file, req.body);
        size_t nm = j.contains("matches") && j["matches"].is_array() ? j["matches"].size() : 0;
        sendJson(res, {{"ok", true}, {"arquivo", file}, {"partidas", nm}});
    });

    // Dados de demonstração (sintéticos)
    svr.Post("/api/demo", [](const httplib::Request& req, httplib::Response& res) {
        if (!authorized(req, res)) return;
        json b = parseBody(req);
        std::string path = app.rawDir + "/import_demo.json";
        if (b.value("remove", false)) std::remove(path.c_str());
        else util::writeFile(path, makeDemoImport().dump());
        if (!startJob({false, false, true})) return sendJson(res, {{"error", "ja existe um processo rodando"}}, 409);
        sendJson(res, {{"ok", true}});
    });

    if (!svr.set_mount_point("/", app.publicDir))
        std::cerr << "Aviso: pasta do site nao encontrada: " << app.publicDir << std::endl;

    loadState();
    std::thread(refreshLoop).detach();

    int port = util::envInt("PORT", 8080);
    app.log("pickems-prediction ouvindo na porta " + std::to_string(port) +
            (hltv::sslAvailable() ? "" : " (sem SSL: coleta HTTP desativada)"));
    if (!svr.listen("0.0.0.0", port)) {
        std::cerr << "Falha ao abrir a porta " << port << std::endl;
        return 1;
    }
    return 0;
}
