#include "imaging/synthetic/scenario_loader.h"

#include <fstream>
#include <sstream>

#include "imaging/synthetic/scenario_loader_convert.h"
#include "imaging/synthetic/scenario_loader_parse.h"
#include "imaging/synthetic/scenario_loader_validate.h"

namespace cambang {

bool load_synthetic_canonical_scenario_from_json_text(
    const std::string& text,
    SyntheticCanonicalScenario& out,
    std::string* error) {
  SyntheticScenarioLoaderParsedDocument parsed{};
  if (!parse_synthetic_scenario_loader_json_text(text, parsed, error)) {
    return false;
  }
  if (!validate_parsed_synthetic_scenario_loader_document(parsed, error)) {
    return false;
  }
  return convert_parsed_synthetic_scenario_loader_document_to_canonical(parsed, out, error);
}

bool load_synthetic_canonical_scenario_from_json_file(
    const std::string& path,
    SyntheticCanonicalScenario& out,
    std::string* error) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    if (error) {
      *error = "failed to open scenario file: " + path;
    }
    return false;
  }

  std::ostringstream oss;
  oss << in.rdbuf();
  if (!in.good() && !in.eof()) {
    if (error) {
      *error = "failed to read scenario file: " + path;
    }
    return false;
  }

  return load_synthetic_canonical_scenario_from_json_text(oss.str(), out, error);
}

} // namespace cambang
