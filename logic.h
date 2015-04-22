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

int do_logic();

#endif //LOGIC_H
