#include "dots/client/client_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace dots::client {
namespace {

using mycore::platform_sdl::Key;

[[noreturn]] void
fail(const std::filesystem::path& source, std::string_view field, std::string detail) {
    throw StartupError{source, field, std::move(detail)};
}

void validate_keys(const toml::table& table,
                   std::initializer_list<std::string_view> allowed,
                   const std::filesystem::path& source,
                   std::string_view section) {
    for (const auto& [key, value] : table) {
        static_cast<void>(value);
        const auto name = key.str();
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            const auto field = section.empty() ? std::string{name}
                                               : std::string{section} + "." + std::string{name};
            fail(source, field, "unknown field");
        }
    }
}

const toml::table* optional_table(const toml::table& root,
                                  std::string_view name,
                                  const std::filesystem::path& source) {
    const auto* node = root.get(name);
    if (node == nullptr) {
        return nullptr;
    }
    const auto* table = node->as_table();
    if (table == nullptr) {
        fail(source, name, "expected a table");
    }
    return table;
}

std::string read_string(const toml::table& table,
                        std::string_view name,
                        const std::filesystem::path& source,
                        std::string_view field) {
    const auto value = table.get(name)->value<std::string>();
    if (!value) {
        fail(source, field, "expected a string");
    }
    return *value;
}

bool read_bool(const toml::table& table,
               std::string_view name,
               const std::filesystem::path& source,
               std::string_view field) {
    const auto value = table.get(name)->value<bool>();
    if (!value) {
        fail(source, field, "expected a boolean");
    }
    return *value;
}

std::int64_t read_integer(const toml::table& table,
                          std::string_view name,
                          const std::filesystem::path& source,
                          std::string_view field) {
    const auto value = table.get(name)->value<std::int64_t>();
    if (!value) {
        fail(source, field, "expected an integer");
    }
    return *value;
}

double read_number(const toml::table& table,
                   std::string_view name,
                   const std::filesystem::path& source,
                   std::string_view field) {
    const auto* node = table.get(name);
    if (const auto floating = node->value<double>()) {
        return *floating;
    }
    if (const auto integer = node->value<std::int64_t>()) {
        return static_cast<double>(*integer);
    }
    fail(source, field, "expected a number");
}

std::string uppercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }
    return result;
}

std::optional<Key> key_from_name(std::string_view name) {
    const auto normalized = uppercase(name);
    if (normalized.size() == 1) {
        const auto character = normalized.front();
        if (character >= 'A' && character <= 'Z') {
            return static_cast<Key>(static_cast<int>(Key::A) + (character - 'A'));
        }
        if (character >= '0' && character <= '9') {
            return static_cast<Key>(static_cast<int>(Key::Digit0) + (character - '0'));
        }
    }
    if (normalized.size() == 6 && normalized.starts_with("DIGIT") && normalized.back() >= '0' &&
        normalized.back() <= '9') {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + (normalized.back() - '0'));
    }

    constexpr std::array named_keys{
        std::pair{"UP", Key::Up},
        std::pair{"DOWN", Key::Down},
        std::pair{"LEFT", Key::Left},
        std::pair{"RIGHT", Key::Right},
        std::pair{"ESCAPE", Key::Escape},
        std::pair{"SPACE", Key::Space},
        std::pair{"ENTER", Key::Enter},
        std::pair{"RETURN", Key::Enter},
        std::pair{"TAB", Key::Tab},
        std::pair{"BACKSPACE", Key::Backspace},
        std::pair{"LEFTSHIFT", Key::LeftShift},
        std::pair{"RIGHTSHIFT", Key::RightShift},
        std::pair{"LEFTCONTROL", Key::LeftControl},
        std::pair{"RIGHTCONTROL", Key::RightControl},
        std::pair{"LEFTCTRL", Key::LeftControl},
        std::pair{"RIGHTCTRL", Key::RightControl},
        std::pair{"LEFTALT", Key::LeftAlt},
        std::pair{"RIGHTALT", Key::RightAlt},
        std::pair{"LEFTGUI", Key::LeftGui},
        std::pair{"RIGHTGUI", Key::RightGui},
        std::pair{"LEFTSUPER", Key::LeftGui},
        std::pair{"RIGHTSUPER", Key::RightGui},
        std::pair{"INSERT", Key::Insert},
        std::pair{"DELETE", Key::Delete},
        std::pair{"HOME", Key::Home},
        std::pair{"END", Key::End},
        std::pair{"PAGEUP", Key::PageUp},
        std::pair{"PAGEDOWN", Key::PageDown},
        std::pair{"F1", Key::F1},
        std::pair{"F2", Key::F2},
        std::pair{"F3", Key::F3},
        std::pair{"F4", Key::F4},
        std::pair{"F5", Key::F5},
        std::pair{"F6", Key::F6},
        std::pair{"F7", Key::F7},
        std::pair{"F8", Key::F8},
        std::pair{"F9", Key::F9},
        std::pair{"F10", Key::F10},
        std::pair{"F11", Key::F11},
        std::pair{"F12", Key::F12},
    };
    for (const auto& [candidate, key] : named_keys) {
        if (normalized == candidate) {
            return key;
        }
    }
    return std::nullopt;
}

std::vector<Key> read_binding(const toml::table& table,
                              std::string_view name,
                              const std::filesystem::path& source,
                              std::string_view field) {
    const auto* array = table.get(name)->as_array();
    if (array == nullptr) {
        fail(source, field, "expected an array of key names");
    }
    if (array->empty()) {
        fail(source, field, "binding must not be empty");
    }

    std::vector<Key> result;
    result.reserve(array->size());
    for (const auto& node : *array) {
        const auto name_value = node.value<std::string>();
        if (!name_value) {
            fail(source, field, "expected an array of key names");
        }
        const auto key = key_from_name(*name_value);
        if (!key) {
            fail(source, field, "unknown key name '" + *name_value + "'");
        }
        result.push_back(*key);
    }
    return result;
}

int hex_digit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (upper >= 'A' && upper <= 'F') {
        return 10 + (upper - 'A');
    }
    return -1;
}

RgbColor
parse_color(std::string_view value, const std::filesystem::path& source, std::string_view field) {
    if (value.size() != 7 || value.front() != '#') {
        fail(source, field, "expected a #RRGGBB color");
    }

    std::array<int, 6> digits{};
    for (std::size_t index = 0; index < digits.size(); ++index) {
        digits[index] = hex_digit(value[index + 1]);
        if (digits[index] < 0) {
            fail(source, field, "expected a #RRGGBB color");
        }
    }
    return {
        .red = static_cast<std::uint8_t>((digits[0] * 16) + digits[1]),
        .green = static_cast<std::uint8_t>((digits[2] * 16) + digits[3]),
        .blue = static_cast<std::uint8_t>((digits[4] * 16) + digits[5]),
    };
}

InputMode parse_input_mode(std::string_view value,
                           const std::filesystem::path& source,
                           std::string_view field) {
    const auto normalized = uppercase(value);
    if (normalized == "MOUSE") {
        return InputMode::Mouse;
    }
    if (normalized == "KEYBOARD") {
        return InputMode::Keyboard;
    }
    if (normalized == "HYBRID") {
        return InputMode::Hybrid;
    }
    fail(source, field, "expected mouse, keyboard, or hybrid");
}

void validate_binding_conflicts(const ClientConfig& config, const std::filesystem::path& source) {
    struct BindingView {
        std::string_view name;
        const std::vector<Key>* keys;
    };
    const std::array bindings{
        BindingView{"up", &config.controls.bindings.up},
        BindingView{"down", &config.controls.bindings.down},
        BindingView{"left", &config.controls.bindings.left},
        BindingView{"right", &config.controls.bindings.right},
        BindingView{"quit", &config.controls.bindings.quit},
    };

    std::array<std::string_view, static_cast<std::size_t>(Key::Count)> owners{};
    for (const auto& binding : bindings) {
        if (binding.keys->empty()) {
            fail(source, "bindings." + std::string{binding.name}, "binding must not be empty");
        }
        for (const auto key : *binding.keys) {
            const auto index = static_cast<std::size_t>(key);
            if (!owners[index].empty() && owners[index] != binding.name) {
                fail(source,
                     "bindings." + std::string{binding.name},
                     "key conflicts with bindings." + std::string{owners[index]});
            }
            owners[index] = binding.name;
        }
    }
}

void parse_window(const toml::table& table,
                  ClientConfig& config,
                  const std::filesystem::path& source) {
    validate_keys(table,
                  {"title", "width", "height", "resizable", "fullscreen", "high_dpi", "vsync"},
                  source,
                  "window");
    if (table.contains("title")) {
        config.window.title = read_string(table, "title", source, "window.title");
    }
    if (table.contains("width")) {
        const auto value = read_integer(table, "width", source, "window.width");
        if (value < 1 || value > 16384) {
            fail(source, "window.width", "must be in the range 1..16384");
        }
        config.window.width = static_cast<int>(value);
    }
    if (table.contains("height")) {
        const auto value = read_integer(table, "height", source, "window.height");
        if (value < 1 || value > 16384) {
            fail(source, "window.height", "must be in the range 1..16384");
        }
        config.window.height = static_cast<int>(value);
    }
    if (table.contains("resizable")) {
        config.window.resizable = read_bool(table, "resizable", source, "window.resizable");
    }
    if (table.contains("fullscreen")) {
        config.window.fullscreen = read_bool(table, "fullscreen", source, "window.fullscreen");
    }
    if (table.contains("high_dpi")) {
        config.window.high_dpi = read_bool(table, "high_dpi", source, "window.high_dpi");
    }
    if (table.contains("vsync")) {
        config.window.vsync = read_bool(table, "vsync", source, "window.vsync");
    }
}

void parse_input(const toml::table& table,
                 ClientConfig& config,
                 const std::filesystem::path& source) {
    validate_keys(table, {"mode", "mouse_dead_zone_pixels"}, source, "input");
    if (table.contains("mode")) {
        config.controls.mode = parse_input_mode(
            read_string(table, "mode", source, "input.mode"), source, "input.mode");
    }
    if (table.contains("mouse_dead_zone_pixels")) {
        const auto value =
            read_number(table, "mouse_dead_zone_pixels", source, "input.mouse_dead_zone_pixels");
        if (!std::isfinite(value) || value < 0.0 ||
            value > static_cast<double>(std::numeric_limits<float>::max())) {
            fail(source, "input.mouse_dead_zone_pixels", "must be a finite non-negative number");
        }
        config.controls.mouse_dead_zone_pixels = static_cast<float>(value);
    }
}

void parse_bindings(const toml::table& table,
                    ClientConfig& config,
                    const std::filesystem::path& source) {
    validate_keys(table, {"up", "down", "left", "right", "quit"}, source, "bindings");
    if (table.contains("up")) {
        config.controls.bindings.up = read_binding(table, "up", source, "bindings.up");
    }
    if (table.contains("down")) {
        config.controls.bindings.down = read_binding(table, "down", source, "bindings.down");
    }
    if (table.contains("left")) {
        config.controls.bindings.left = read_binding(table, "left", source, "bindings.left");
    }
    if (table.contains("right")) {
        config.controls.bindings.right = read_binding(table, "right", source, "bindings.right");
    }
    if (table.contains("quit")) {
        config.controls.bindings.quit = read_binding(table, "quit", source, "bindings.quit");
    }
}

void parse_simulation(const toml::table& table,
                      ClientConfig& config,
                      const std::filesystem::path& source) {
    validate_keys(table, {"max_frame_delta_ms", "max_steps_per_frame"}, source, "simulation");
    if (table.contains("max_frame_delta_ms")) {
        const auto value =
            read_integer(table, "max_frame_delta_ms", source, "simulation.max_frame_delta_ms");
        if (value <= 0 || value > std::numeric_limits<int>::max()) {
            fail(source, "simulation.max_frame_delta_ms", "must be a positive integer");
        }
        config.simulation.max_frame_delta_ms = static_cast<int>(value);
    }
    if (table.contains("max_steps_per_frame")) {
        const auto value =
            read_integer(table, "max_steps_per_frame", source, "simulation.max_steps_per_frame");
        if (value <= 0) {
            fail(source, "simulation.max_steps_per_frame", "must be a positive integer");
        }
        config.simulation.max_steps_per_frame = static_cast<std::size_t>(value);
    }
}

float read_positive_float(const toml::table& table,
                          std::string_view name,
                          const std::filesystem::path& source,
                          std::string_view field) {
    const auto value = read_number(table, name, source, field);
    if (!std::isfinite(value) || value <= 0.0 ||
        value > static_cast<double>(std::numeric_limits<float>::max())) {
        fail(source, field, "must be a finite positive number");
    }
    return static_cast<float>(value);
}

void parse_view(const toml::table& table,
                ClientConfig& config,
                const std::filesystem::path& source) {
    validate_keys(
        table, {"pixels_per_world_unit", "draw_grid", "grid_spacing_world_units"}, source, "view");
    if (table.contains("pixels_per_world_unit")) {
        config.view.pixels_per_world_unit = read_positive_float(
            table, "pixels_per_world_unit", source, "view.pixels_per_world_unit");
    }
    if (table.contains("draw_grid")) {
        config.view.draw_grid = read_bool(table, "draw_grid", source, "view.draw_grid");
    }
    if (table.contains("grid_spacing_world_units")) {
        config.view.grid_spacing_world_units = read_positive_float(
            table, "grid_spacing_world_units", source, "view.grid_spacing_world_units");
    }
}

void parse_colors(const toml::table& table,
                  ClientConfig& config,
                  const std::filesystem::path& source) {
    validate_keys(
        table, {"background", "grid", "player", "player_growth", "food"}, source, "colors");
    if (table.contains("background")) {
        config.colors.background =
            parse_color(read_string(table, "background", source, "colors.background"),
                        source,
                        "colors.background");
    }
    if (table.contains("grid")) {
        config.colors.grid =
            parse_color(read_string(table, "grid", source, "colors.grid"), source, "colors.grid");
    }
    if (table.contains("player")) {
        config.colors.player = parse_color(
            read_string(table, "player", source, "colors.player"), source, "colors.player");
    }
    if (table.contains("player_growth")) {
        config.colors.player_growth =
            parse_color(read_string(table, "player_growth", source, "colors.player_growth"),
                        source,
                        "colors.player_growth");
    }
    if (table.contains("food")) {
        config.colors.food =
            parse_color(read_string(table, "food", source, "colors.food"), source, "colors.food");
    }
}

} // namespace

StartupError::StartupError(std::string message)
    : std::runtime_error(std::move(message)) {}

StartupError::StartupError(const std::filesystem::path& file,
                           std::string_view field,
                           std::string detail)
    : std::runtime_error(file.string() + ": " + std::string{field} + ": " + std::move(detail)) {}

ClientConfig default_client_config() {
    ClientConfig config;
    config.controls.bindings = {
        .up = {Key::W, Key::Up},
        .down = {Key::S, Key::Down},
        .left = {Key::A, Key::Left},
        .right = {Key::D, Key::Right},
        .quit = {Key::Escape},
    };
    return config;
}

ClientConfig parse_client_config(std::string_view toml_text, const std::filesystem::path& source) {
    toml::table root;
    try {
        root = toml::parse(toml_text, source.string());
    } catch (const toml::parse_error& error) {
        fail(source, "<document>", std::string{error.description()});
    }

    validate_keys(
        root, {"window", "input", "bindings", "simulation", "view", "colors"}, source, "");
    auto config = default_client_config();
    if (const auto* table = optional_table(root, "window", source)) {
        parse_window(*table, config, source);
    }
    if (const auto* table = optional_table(root, "input", source)) {
        parse_input(*table, config, source);
    }
    if (const auto* table = optional_table(root, "bindings", source)) {
        parse_bindings(*table, config, source);
    }
    if (const auto* table = optional_table(root, "simulation", source)) {
        parse_simulation(*table, config, source);
    }
    if (const auto* table = optional_table(root, "view", source)) {
        parse_view(*table, config, source);
    }
    if (const auto* table = optional_table(root, "colors", source)) {
        parse_colors(*table, config, source);
    }
    validate_binding_conflicts(config, source);
    return config;
}

ClientConfig load_client_config(const std::optional<std::filesystem::path>& explicit_path,
                                const std::filesystem::path& current_directory) {
    auto path = current_directory / "dots-client.toml";
    if (explicit_path) {
        path = explicit_path->is_absolute() ? *explicit_path : current_directory / *explicit_path;
    }

    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        fail(path, "<file>", "could not inspect configuration file: " + error.message());
    }
    if (!exists) {
        if (explicit_path) {
            fail(path, "<file>", "explicit configuration file does not exist");
        }
        return default_client_config();
    }
    const auto regular_file = std::filesystem::is_regular_file(path, error);
    if (error) {
        fail(path, "<file>", "could not inspect configuration file: " + error.message());
    }
    if (!regular_file) {
        fail(path, "<file>", "configuration path is not a regular file");
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        fail(path, "<file>", "could not open configuration file");
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    if (stream.bad()) {
        fail(path, "<file>", "could not read configuration file");
    }
    return parse_client_config(contents.str(), path);
}

ClientConfig load_client_config(const std::optional<std::filesystem::path>& explicit_path) {
    std::error_code error;
    const auto current_directory = std::filesystem::current_path(error);
    if (error) {
        fail("<current-directory>", "<file>", "could not resolve the current directory");
    }
    return load_client_config(explicit_path, current_directory);
}

} // namespace dots::client
