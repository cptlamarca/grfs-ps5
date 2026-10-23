#pragma once

#include <string>

// A plain HTTP/1.0 GET of url (http://host[:port]/path); false if the server
// can't be reached or doesn't answer 200.
bool HttpGet(const std::string& url, std::string& body);
