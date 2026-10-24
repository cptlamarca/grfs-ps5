#pragma once

#include <string>

// A plain HTTP/1.0 GET of url (http://host[:port]/path); false if the server
// can't be reached or doesn't answer 200.
bool HttpGet(const std::string& url, std::string& body);

// A POST of post_body (plain text) to url, the same way.
bool HttpPost(const std::string& url, const std::string& post_body, std::string& body);
