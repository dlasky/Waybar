#include "client.hpp"
#include <fstream>
#include <iostream>
#include "util/clara.hpp"
#include "util/json.hpp"

waybar::Client *waybar::Client::inst() {
  static auto c = new Client();
  return c;
}

const std::string waybar::Client::getValidPath(const std::vector<std::string> &paths) {
  wordexp_t p;

  for (const std::string &path : paths) {
    if (wordexp(path.c_str(), &p, 0) == 0) {
      if (access(*p.we_wordv, F_OK) == 0) {
        std::string result = *p.we_wordv;
        wordfree(&p);
        return result;
      }
      wordfree(&p);
    }
  }

  return std::string();
}

void waybar::Client::handleGlobal(void *data, struct wl_registry *registry, uint32_t name,
                                  const char *interface, uint32_t version) {
  auto client = static_cast<Client *>(data);
  if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    client->layer_shell = static_cast<struct zwlr_layer_shell_v1 *>(
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, version));
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    auto wl_output = static_cast<struct wl_output *>(
        wl_registry_bind(registry, name, &wl_output_interface, version));
    client->outputs_.emplace_back(new struct waybar_output({wl_output, "", name, nullptr}));
    client->handleOutput(client->outputs_.back());
  } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0 &&
             version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION) {
    client->xdg_output_manager = static_cast<struct zxdg_output_manager_v1 *>(wl_registry_bind(
        registry, name, &zxdg_output_manager_v1_interface, ZXDG_OUTPUT_V1_NAME_SINCE_VERSION));
  } else if (strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
    client->idle_inhibit_manager = static_cast<struct zwp_idle_inhibit_manager_v1 *>(
        wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1));
  }
}

void waybar::Client::handleGlobalRemove(void *   data, struct wl_registry * /*registry*/,
                                        uint32_t name) {
  auto client = static_cast<Client *>(data);
  for (auto it = client->bars.begin(); it != client->bars.end();) {
    if ((*it)->output->wl_name == name) {
      auto output_name = (*it)->output->name;
      (*it)->window.close();
      it = client->bars.erase(it);
      std::cout << "Bar removed from output: " + output_name << std::endl;
    } else {
      ++it;
    }
  }
  auto it = std::find_if(client->outputs_.begin(),
                         client->outputs_.end(),
                         [&name](const auto &output) { return output->wl_name == name; });
  if (it != client->outputs_.end()) {
    zxdg_output_v1_destroy((*it)->xdg_output);
    wl_output_destroy((*it)->output);
    client->outputs_.erase(it);
  }
}

void waybar::Client::handleOutput(std::unique_ptr<struct waybar_output> &output) {
  static const struct zxdg_output_v1_listener xdgOutputListener = {
      .logical_position = handleLogicalPosition,
      .logical_size = handleLogicalSize,
      .done = handleDone,
      .name = handleName,
      .description = handleDescription,
  };
  output->xdg_output = zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, output->output);
  zxdg_output_v1_add_listener(output->xdg_output, &xdgOutputListener, &output->wl_name);
}

void waybar::Client::handleLogicalPosition(void * /*data*/,
                                           struct zxdg_output_v1 * /*zxdg_output_v1*/,
                                           int32_t /*x*/, int32_t /*y*/) {
  // Nothing here
}

void waybar::Client::handleLogicalSize(void * /*data*/, struct zxdg_output_v1 * /*zxdg_output_v1*/,
                                       int32_t /*width*/, int32_t /*height*/) {
  // Nothing here
}

void waybar::Client::handleDone(void * /*data*/, struct zxdg_output_v1 * /*zxdg_output_v1*/) {
  // Nothing here
}

bool waybar::Client::isValidOutput(const Json::Value &                    config,
                                   std::unique_ptr<struct waybar_output> &output) {
  bool found = true;
  if (config["output"].isArray()) {
    bool in_array = false;
    for (auto const &output_conf : config["output"]) {
      if (output_conf.isString() && output_conf.asString() == output->name) {
        in_array = true;
        break;
      }
    }
    found = in_array;
  }
  if (config["output"].isString() && config["output"].asString() != output->name) {
    found = false;
  }
  return found;
}

std::unique_ptr<struct waybar::waybar_output> &waybar::Client::getOutput(uint32_t wl_name) {
  auto it = std::find_if(outputs_.begin(), outputs_.end(), [&wl_name](const auto &output) {
    return output->wl_name == wl_name;
  });
  if (it == outputs_.end()) {
    throw std::runtime_error("Unable to find valid output");
  }
  return *it;
}

std::vector<Json::Value> waybar::Client::getOutputConfigs(
    std::unique_ptr<struct waybar_output> &output) {
  std::vector<Json::Value> configs;
  if (config_.isArray()) {
    for (auto const &config : config_) {
      if (config.isObject() && isValidOutput(config, output)) {
        configs.push_back(config);
      }
    }
  } else if (isValidOutput(config_, output)) {
    configs.push_back(config_);
  }
  return configs;
}

void waybar::Client::handleName(void *      data, struct zxdg_output_v1 * /*xdg_output*/,
                                const char *name) {
  auto wl_name = *static_cast<uint32_t *>(data);
  auto client = waybar::Client::inst();
  try {
    auto &output = client->getOutput(wl_name);
    output->name = name;
    auto configs = client->getOutputConfigs(output);
    if (configs.empty()) {
      wl_output_destroy(output->output);
      zxdg_output_v1_destroy(output->xdg_output);
    } else {
      for (const auto &config : configs) {
        client->bars.emplace_back(std::make_unique<Bar>(output.get(), config));
        Glib::RefPtr<Gdk::Screen> screen = client->bars.back()->window.get_screen();
        client->style_context_->add_provider_for_screen(
            screen, client->css_provider_, GTK_STYLE_PROVIDER_PRIORITY_USER);
      }
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
  }
}

void waybar::Client::handleDescription(void * /*data*/, struct zxdg_output_v1 * /*zxdg_output_v1*/,
                                       const char * /*description*/) {
  // Nothing here
}

void waybar::Client::setupConfigs(const std::string &config, const std::string &style) {
  config_file_ = config.empty() ? getValidPath({
                                      "$XDG_CONFIG_HOME/waybar/config",
                                      "$HOME/.config/waybar/config",
                                      "$HOME/waybar/config",
                                      "/etc/xdg/waybar/config",
                                      "./resources/config",
                                  })
                                : config;
  css_file_ = style.empty() ? getValidPath({
                                  "$XDG_CONFIG_HOME/waybar/style.css",
                                  "$HOME/.config/waybar/style.css",
                                  "$HOME/waybar/style.css",
                                  "/etc/xdg/waybar/style.css",
                                  "./resources/style.css",
                              })
                            : style;
  if (css_file_.empty() || config_file_.empty()) {
    throw std::runtime_error("Missing required resources files");
  }
  std::cout << "Resources files: " + config_file_ + ", " + css_file_ << std::endl;
}

auto waybar::Client::setupConfig() -> void {
  std::ifstream file(config_file_);
  if (!file.is_open()) {
    throw std::runtime_error("Can't open config file");
  }
  std::string      str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  util::JsonParser parser;
  config_ = parser.parse(str);
}

auto waybar::Client::setupCss() -> void {
  css_provider_ = Gtk::CssProvider::create();
  style_context_ = Gtk::StyleContext::create();

  // Load our css file, wherever that may be hiding
  if (!css_provider_->load_from_path(css_file_)) {
    throw std::runtime_error("Can't open style file");
  }
}

void waybar::Client::bindInterfaces() {
  registry = wl_display_get_registry(wl_display);
  static const struct wl_registry_listener registry_listener = {
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };
  wl_registry_add_listener(registry, &registry_listener, this);
  wl_display_roundtrip(wl_display);
  if (layer_shell == nullptr || xdg_output_manager == nullptr) {
    throw std::runtime_error("Failed to acquire required resources.");
  }
}

int waybar::Client::main(int argc, char *argv[]) {
  gtk_app = Gtk::Application::create(argc, argv, "fr.arouillard.waybar");
  gdk_display = Gdk::Display::get_default();
  if (!gdk_display) {
    throw std::runtime_error("Can't find display");
  }
  if (!GDK_IS_WAYLAND_DISPLAY(gdk_display->gobj())) {
    throw std::runtime_error("Bar need to run under Wayland");
  }
  wl_display = gdk_wayland_display_get_wl_display(gdk_display->gobj());
  bool        show_help = false;
  bool        show_version = false;
  std::string config;
  std::string style;
  std::string bar_id;
  auto        cli = clara::detail::Help(show_help) |
             clara::detail::Opt(show_version)["-v"]["--version"]("Show version") |
             clara::detail::Opt(config, "config")["-c"]["--config"]("Config path") |
             clara::detail::Opt(style, "style")["-s"]["--style"]("Style path") |
             clara::detail::Opt(bar_id, "id")["-b"]["--bar"]("Bar id");
  auto res = cli.parse(clara::detail::Args(argc, argv));
  if (!res) {
    std::cerr << "Error in command line: " << res.errorMessage() << std::endl;
    return 1;
  }
  if (show_help) {
    std::cout << cli << std::endl;
    return 0;
  }
  if (show_version) {
    std::cout << "Waybar v" << VERSION << std::endl;
    return 0;
  }
  setupConfigs(config, style);
  setupConfig();
  setupCss();
  bindInterfaces();
  gtk_app->hold();
  gtk_app->run();
  bars.clear();
  zxdg_output_manager_v1_destroy(xdg_output_manager);
  zwlr_layer_shell_v1_destroy(layer_shell);
  zwp_idle_inhibit_manager_v1_destroy(idle_inhibit_manager);
  wl_registry_destroy(registry);
  wl_display_disconnect(wl_display);
  return 0;
}
