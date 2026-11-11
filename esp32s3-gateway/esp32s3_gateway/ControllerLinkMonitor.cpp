#include "ControllerLinkMonitor.h"
#include <string.h>
#include <stdio.h>
namespace {
void copyText(char* out,size_t size,const char* text){if(!text)text="";size_t n=strlen(text);if(n>=size)n=size-1;memcpy(out,text,n);out[n]=0;}
bool number(const char*s,uint32_t&n){
 if(!s||!*s)return false;n=0;
 for(size_t i=0;s[i];++i){if(i>=10||s[i]<'0'||s[i]>'9')return false;const uint32_t d=s[i]-'0';if(n>(UINT32_MAX-d)/10U)return false;n=n*10U+d;}
 return true;
}
bool reasonValid(const char*s){
 if(!s||!*s||strlen(s)>24)return false;
 for(;*s;++s)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'||*s=='-'))return false;
 return true;
}
bool field(const char* token,const char* key,const char*&value){size_t n=strlen(key);if(strncmp(token,key,n)||token[n]!='=')return false;value=token+n+1;return true;}
}
void ControllerLinkMonitor::record(const char* event,const char* detail,uint32_t now,uint32_t tx,uint32_t boot){
 LinkEvent value;value.id=++nextId_;value.uptimeMs=now;value.boot=boot;value.tx=tx;
 copyText(value.event,sizeof(value.event),event);copyText(value.detail,sizeof(value.detail),detail);
 size_t slot;
 if(count_<Capacity){slot=(head_+count_)%Capacity;++count_;}else{slot=head_;head_=(head_+1)%Capacity;++overwritten_;}
 events_[slot]=value;
 if(strstr(value.event,"Rejected")||strstr(value.event,"Cancelled")||strstr(value.event,"Timeout")||!strcmp(value.event,"f411Reason"))lastRejection_=value;
 if(!strcmp(value.event,"f411Recovery")||!strcmp(value.event,"uartRestart")||!strcmp(value.event,"f411BootChanged")||!strcmp(value.event,"workerRestart")||!strcmp(value.event,"httpRestart"))lastRecovery_=value;
}
bool ControllerLinkMonitor::receive(const char* line,uint32_t now){
 ++rxLines_;
 const bool isState=!strncmp(line,"F411_STATE,",11),isRecovery=!strncmp(line,"F411_RECOVER,",13);
 if(isState||isRecovery){
  if(strlen(line)>127||strstr(line,",,")||line[strlen(line)-1]==','){++malformed_;return true;}
  char buffer[128];copyText(buffer,sizeof(buffer),line);
  const char* tokens[12];size_t count=0;char* cursor=nullptr;
  for(char*t=strtok_r(buffer,",",&cursor);t;t=strtok_r(nullptr,",",&cursor)){if(count==12){++malformed_;return true;}tokens[count++]=t;}
  const char* v=nullptr;uint32_t b=0,t=0,x=0,c=0,s=0,m=0,p=0;
  if(isState){
   if(count!=9||!field(tokens[1],"b",v)||!number(v,b)||!b||
      !field(tokens[2],"s",v)||!number(v,s)||s>5||
      !field(tokens[3],"m",v)||!number(v,m)||m>7||
      !field(tokens[4],"t",v)||!number(v,t)||
      !field(tokens[5],"p",v)||!number(v,p)||p>1||
      !field(tokens[6],"r",v)||!reasonValid(v)){++malformed_;return true;}
   char reason[25];copyText(reason,sizeof(reason),v);
   if(!field(tokens[7],"x",v)||!number(v,x)||!field(tokens[8],"c",v)||!number(v,c)){++malformed_;return true;}
   if(state_.seen&&b<state_.boot)return true; // late pre-reset UART telemetry
   const bool changed=state_.seen&&b>state_.boot;
   const bool newReason=strcmp(reason,"none")&&(!state_.seen||b!=state_.boot||x!=state_.reasonTx||strcmp(reason,state_.reason));
   if(changed)record("f411BootChanged","F411 reboot observed",now,t,b);
   if(newReason)record("f411Reason",reason,now,x,b);
   state_.seen=true;state_.boot=b;state_.receivedAtMs=now;state_.tx=t;
   state_.manager=static_cast<uint8_t>(s);state_.mdb=static_cast<uint8_t>(m);state_.reportPending=p!=0;
   state_.reasonTx=x;state_.recoveries=c;copyText(state_.reason,sizeof(state_.reason),reason);
  }else{
   if(count!=4||!field(tokens[1],"b",v)||!number(v,b)||!b||!field(tokens[2],"r",v)||!reasonValid(v)){++malformed_;return true;}
   char reason[25];copyText(reason,sizeof(reason),v);
   if(!field(tokens[3],"t",v)||!number(v,t)){++malformed_;return true;}
   if(!state_.seen||b>=state_.boot)record("f411Recovery",reason,now,t,b);
  }
  return true;
 }
 if(!strncmp(line,"CONTROLLER_DIAGNOSTIC,source=vendFlow,",sizeof("CONTROLLER_DIAGNOSTIC,source=vendFlow,")-1)){
  const char* e=strstr(line,",event=");
  if(e){e+=7;char kind[25];size_t n=0;while(e[n]&&e[n]!=','&&n<24){kind[n]=e[n];++n;}kind[n]=0;record(kind,line,now,0,state_.seen?state_.boot:0);}
 }else if(!strncmp(line,"PAYMENT_REQUEST,",16))record("paymentRequestUart",line,now);
 else if(!strncmp(line,"PAYMENT_ACK,",12))record("paymentAckUart",line,now);
 else if(!strncmp(line,"VEND_REPORT,",12))record("vendReportUart",line,now);
 else if(!strncmp(line,"ORDER_CANCELLED,",16))record("machineCancelled",line,now);
 else if(!strncmp(line,"DELIVERY_RESULT,",16))record("directResultUart",line,now);
 return false;
}
bool ControllerLinkMonitor::uartRestartDue(uint32_t now)const{
 return (uint32_t)(now-(state_.seen?state_.receivedAtMs:0U))>=55000U&&
        (uartRestarts_==0||(uint32_t)(now-lastUartRestartMs_)>=55000U);
}
void ControllerLinkMonitor::markUartRestart(uint32_t now){++uartRestarts_;lastUartRestartMs_=now;record("uartRestart","F411 heartbeat stale; UART driver restarted",now);}
