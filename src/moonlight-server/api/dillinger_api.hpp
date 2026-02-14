#pragma once

#include <events/events.hpp>
#include <server_http.hpp>
#include <state/data-structures.hpp>

namespace wolf::api {

class DillingerAPI {
public:
  explicit DillingerAPI(immer::box<state::AppState> app_state);

  void run(unsigned short port);

private:
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

  void register_routes();

  std::shared_ptr<HttpServer> server_;
  immer::box<state::AppState> app_state_;
};

} // namespace wolf::api
