#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


void k230_tcp_init(void);


typedef struct {
    char session[24];
    char state[20];
    int action, seq, total, standard, nonstandard, score, feedback;
    int errors[10], incomplete, interrupted, short_hold, score_sum;

    bool running, confirmed, connected, idle_timeout, user_stop;
} k230_status_t;

bool k230_training_start(int action);
void k230_training_stop(void);
bool k230_get_status(k230_status_t *out);

bool k230_get_report(k230_status_t *out);

#ifdef __cplusplus
}
#endif
