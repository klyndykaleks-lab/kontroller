#ifndef F411_RECOVERY_H
#define F411_RECOVERY_H
#include <stdint.h>
#include <stdbool.h>
#define F411_RECOVERY_DELAY_MS 55000U
/* Only explicitly unhealthy work arms this timer. Polls/rejections are not progress. */
typedef struct {uint32_t since,progress;bool armed;} F411RecoveryTimer;
static inline bool F411RecoveryDue(F411RecoveryTimer *timer,uint32_t now,bool blocked,uint32_t progress){
 if(!blocked||timer->progress!=progress){timer->armed=false;timer->progress=progress;}
 if(!blocked)return false;
 if(!timer->armed){timer->armed=true;timer->since=now;return false;}
 return (uint32_t)(now-timer->since)>=F411_RECOVERY_DELAY_MS;
}
#endif
