/*---DOC---
{
  "object": "demo.echo_c",
  "language": "c",
  "summary": "Echoes a JSON object with an added timestamp.",
  "entry": "stdio-json",
  "main": "main",
  "timeout_ms": 2000
}
---END---*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(void){
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int c;
    while((c=fgetc(stdin))!=EOF){
        if(len+1 >= cap){ cap = cap? cap*2 : 1024; buf = (char*)realloc(buf, cap);}
        buf[len++] = (char)c;
    }
    if(!buf){ puts("{}"); return 0; }
    buf[len] = '\0';
    char *last = strrchr(buf, '}');
    if(last){
        time_t t = time(NULL);
        char extra[128];
        snprintf(extra, sizeof(extra), ",\"echoed_by\":\"c\",\"ts\":%ld}", (long)t);
        *last = '\0';
        fputs(buf, stdout);
        fputs(extra, stdout);
    } else {
        fputs(buf, stdout);
    }
    free(buf);
    return 0;
}
