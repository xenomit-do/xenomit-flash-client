#pragma once

#include <string>
#include <vector>

namespace as_bridge {

std::string request_xml(const std::string &name, const std::vector<std::string> &args);
std::string parse_response(const std::string &response);

}
