#include <api/dillinger_api.hpp>
#include <helpers/logger.hpp>
#include <helpers/utils.hpp>
#include <immer/map_transient.hpp>
#include <rfl/json.hpp>
#include <state/config.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>

namespace wolf::api {

namespace {

namespace core_events = wolf::core::events;

struct LaunchRequest {
  std::string cmd;
  std::optional<std::map<std::string, std::string>> env;
};

struct PairAcceptRequest {
  std::string pair_secret;
  std::string pin;
};

struct PendingPairStatus {
  std::string pair_secret;
  std::string client_ip;
};

struct PairedClientStatus {
  std::string client_id;
  std::string app_state_folder;
};

struct PairStatusResponse {
  bool success = true;
  std::vector<PendingPairStatus> pending;
  std::vector<PairedClientStatus> paired;
};

struct StatusResponse {
  bool success = true;
  bool stream_active = false;
  std::size_t running_sessions = 0;
  std::size_t connected_clients = 0;
  std::optional<std::string> session_id;
  std::string encoder = "auto";
  std::string running_cmd;
  std::map<std::string, std::string> running_env;
  std::uint64_t uptime_seconds = 0;
  std::size_t paired_clients = 0;
  std::size_t pending_pair_requests = 0;
};

struct GenericResponse {
  bool success = true;
  std::string message;
};

struct HealthResponse {
  std::string status = "ok";
};

std::string read_body(const std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTP>::Request> &request) {
  std::stringstream buffer;
  buffer << request->content.rdbuf();
  return buffer.str();
}

template <class T>
void respond_json(const std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTP>::Response> &response,
                  SimpleWeb::StatusCode code,
                  const T &payload) {
  SimpleWeb::CaseInsensitiveMultimap headers;
  headers.emplace("Content-Type", "application/json");
  response->write(code, rfl::json::write(payload), headers);
}

std::optional<core_events::StreamSession> get_active_session(const immer::box<state::AppState> &app_state) {
  auto sessions = app_state->running_sessions->load();
  const auto &sessions_value = sessions.get();
  if (sessions_value.empty()) {
    return std::nullopt;
  }
  return sessions_value.back();
}

} // namespace

class DillingerAPIState {
public:
  std::mutex mutex;
  std::string last_cmd;
  std::map<std::string, std::string> last_env;
  std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
};

DillingerAPI::DillingerAPI(immer::box<state::AppState> app_state) : app_state_(std::move(app_state)) {
  server_ = std::make_shared<HttpServer>();
}

void DillingerAPI::run(unsigned short port) {
  server_->config.port = port;
  server_->config.address = "0.0.0.0";
  register_routes();
  try {
    server_->start([port](unsigned short) { logs::log(logs::info, "Dillinger API listening on port: {}", port); });
  } catch (const std::exception &ex) {
    logs::log(logs::error, "Dillinger API failed to start: {}", ex.what());
    throw;
  }
}

void DillingerAPI::register_routes() {
  auto state = std::make_shared<DillingerAPIState>();

  server_->default_resource["GET"] = [](auto response, auto request) {
    respond_json(response, SimpleWeb::StatusCode::client_error_not_found, GenericResponse{false, "Not found"});
  };
  server_->default_resource["POST"] = server_->default_resource["GET"];

  server_->resource["^/health$"]["GET"] = [](auto response, auto request) {
    respond_json(response, SimpleWeb::StatusCode::success_ok, HealthResponse{});
  };

  server_->resource["^/status$"]["GET"] = [this, state](auto response, auto request) {
    auto sessions = app_state_->running_sessions->load();
    auto pending = app_state_->pairing_atom->load();
    auto paired = app_state_->config->paired_clients->load();
    const auto &sessions_value = sessions.get();

    StatusResponse status;
    status.running_sessions = sessions_value.size();
    status.stream_active = !sessions_value.empty();
    status.connected_clients = sessions_value.size();
    status.paired_clients = paired.get().size();
    status.pending_pair_requests = pending.get().size();

    if (!sessions_value.empty()) {
      status.session_id = std::to_string(sessions_value.back().session_id);
    }

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      status.running_cmd = state->last_cmd;
      status.running_env = state->last_env;
      status.uptime_seconds = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state->start_time)
              .count());
    }

    respond_json(response, SimpleWeb::StatusCode::success_ok, status);
  };

  server_->resource["^/launch$"]["POST"] = [this, state](auto response, auto request) {
    auto payload = rfl::json::read<LaunchRequest>(read_body(request));
    if (!payload) {
      respond_json(response,
                  SimpleWeb::StatusCode::client_error_bad_request,
                  GenericResponse{false, payload.error().what()});
      return;
    }

    auto session = get_active_session(app_state_);
    if (!session) {
      respond_json(response, SimpleWeb::StatusCode::client_error_conflict, GenericResponse{false, "No active session"});
      return;
    }

    auto runner = state::get_runner(core_events::RunnerTypes{wolf::config::AppCMD{.run_cmd = payload->cmd}},
                                    app_state_->event_bus);

    immer::map_transient<std::string, std::string> env_vars;
    if (payload->env) {
      for (const auto &pair : payload->env.value()) {
        env_vars.set(pair.first, pair.second);
      }
    }

    app_state_->event_bus->fire_event(
      immer::box<core_events::StopRunnerEvent>(core_events::StopRunnerEvent{.session_id = session->session_id}));
    app_state_->event_bus->fire_event(immer::box<core_events::StartRunner>(
      core_events::StartRunner{.stop_stream_when_over = false,
                   .runner = runner,
                   .stream_session = std::make_shared<core_events::StreamSession>(*session),
                   .extra_env = env_vars.persistent()}));

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->last_cmd = payload->cmd;
      state->last_env = payload->env.value_or(std::map<std::string, std::string>{});
    }

    respond_json(response, SimpleWeb::StatusCode::success_ok, GenericResponse{true, "launched"});
  };

  server_->resource["^/stop$"]["POST"] = [this, state](auto response, auto request) {
    auto session = get_active_session(app_state_);
    if (!session) {
      respond_json(response, SimpleWeb::StatusCode::client_error_conflict, GenericResponse{false, "No active session"});
      return;
    }

    app_state_->event_bus->fire_event(
      immer::box<core_events::StopRunnerEvent>(core_events::StopRunnerEvent{.session_id = session->session_id}));

    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->last_cmd.clear();
      state->last_env.clear();
    }

    respond_json(response, SimpleWeb::StatusCode::success_ok, GenericResponse{true, "stopped"});
  };

  server_->resource["^/pair/status$"]["GET"] = [this](auto response, auto request) {
    PairStatusResponse res;

    auto pending = app_state_->pairing_atom->load();
    for (const auto &[secret, pair_request] : pending.get()) {
      res.pending.push_back(PendingPairStatus{secret, pair_request->client_ip});
    }

    auto paired = app_state_->config->paired_clients->load();
    for (const auto &client : paired.get()) {
      res.paired.push_back(
          PairedClientStatus{std::to_string(state::get_client_id(client.get())), client->app_state_folder});
    }

    respond_json(response, SimpleWeb::StatusCode::success_ok, res);
  };

  server_->resource["^/pair/accept$"]["POST"] = [this](auto response, auto request) {
    auto payload = rfl::json::read<PairAcceptRequest>(read_body(request));
    if (!payload) {
      respond_json(response,
                  SimpleWeb::StatusCode::client_error_bad_request,
                  GenericResponse{false, payload.error().what()});
      return;
    }

    auto pair_request = app_state_->pairing_atom->load()->find(payload->pair_secret);
    if (!pair_request) {
      respond_json(response, SimpleWeb::StatusCode::client_error_not_found, GenericResponse{false, "Pair request not found"});
      return;
    }

    pair_request->get().user_pin->set_value(payload->pin);
    app_state_->pairing_atom->update([secret = payload->pair_secret](auto m) { return m.erase(secret); });

    respond_json(response, SimpleWeb::StatusCode::success_ok, GenericResponse{true, "paired"});
  };
}

} // namespace wolf::api
