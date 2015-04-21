#ifndef LOGIC_H
#define LOGIC_H

#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "event2/event.h"
#include "json/json.h"
#include "evhttp.h"
#include "mylog.h"
#include "curl/curl.h"

#include <string>
#include <vector>
#include <map>

int report_init();

int report_clean();

int do_logic();

#endif //LOGIC_H
