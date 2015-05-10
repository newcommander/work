#ifndef LOGIC_H
#define LOGIC_H

#include "json/json.h"
#include "curl/curl.h"
#include "mylog.h"

#include <string>
#include <map>
#include <set>

int report_init();

int report_clean();

int new_link(std::string name, std::string tag);

Json::Value new_tiny(std::string num);

int do_logic();

#endif //LOGIC_H
