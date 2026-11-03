#ifndef WEB_SERVER_H
#define WEB_SERVER_H
#include "k230_tcp.h"

#ifdef __cplusplus
extern "C" {
#endif

void web_server_init(void);
void web_server_session_started(const char *session);
void web_server_record_snapshot(void);
void web_server_record_training(const k230_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
