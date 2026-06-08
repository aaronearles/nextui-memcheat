#pragma once
#include "state.h"
#include "mongoose.h"

void api_handle(struct mg_connection *c, int ev, void *ev_data);
