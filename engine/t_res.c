#include "scumm.h"
#include <stdio.h>
#include <stdlib.h>
static u8 *rd(const char*p,u32*n){FILE*f=fopen(p,"rb");fseek(f,0,SEEK_END);*n=ftell(f);rewind(f);u8*b=malloc(*n);fread(b,1,*n,f);fclose(f);return b;}
int main(){u32 a,b;const char*D="/home/user/monkeyislanddemo/mi_demo2/";char p[256];
 sprintf(p,"%sDISK01.LEC",D);u8*l=rd(p,&a);sprintf(p,"%s000.LFL",D);u8*i=rd(p,&b);
 printf("init %d\n",res_init(l,a,i,b));
 for(int s=0;s<NUM_GLOBAL_SCRIPTS;s++){u32 sz;const u8*x=res_script(s,&sz);if(x)printf("script %d size %u\n",s,sz); else if(g_dir_script[s].room)printf("script %d MISSING room %d off %x tag %.2s\n",s,g_dir_script[s].room,g_dir_script[s].off,g_disk+g_lf_off[g_dir_script[s].room]+8+g_dir_script[s].off+4);}
 for(int s=0;s<g_n_cost;s++) if(g_dir_cost[s].room){u32 sz;const u8*x=res_costume(s,&sz);printf("cost %d %s %u\n",s,x?"ok":"MISSING",x?sz:0);}
 for(int s=0;s<g_n_sound;s++) if(g_dir_sound[s].room){u32 sz;const u8*x=res_sound(s,&sz);printf("sound %d %s %u tag %.2s\n",s,x?"ok":"MISSING",x?sz:0,g_disk+g_lf_off[g_dir_sound[s].room]+8+g_dir_sound[s].off+4);}
}
