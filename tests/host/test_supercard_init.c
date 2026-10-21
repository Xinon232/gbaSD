/* SuperCard driver host test: init, CSD, addressing, errors, stop after a
 * failed read. A scripted card stands in for the bus routines (from gbamp3).
 * gbaSD, GPL-3.0-or-later. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "supercard_driver.h"
#include "crc.h"
#include "gbasd_config.h"
#define SC_RD_CAT2(a,b,c) a##b##c
#define SC_RD_CAT(a,b,c) SC_RD_CAT2(a,b,c)
#define SC_RD_MODE_SYM(w) SC_RD_CAT(sc_read_sectors_crc,GBASD_SC_READ_CRC,_##w)
bool isgba=true,slowsd=false;
static unsigned cmd,arg,hc,unready,badcsd,readfail,lastread,stops,calls,failat;
void send_empty_clocks(unsigned n){assert(n);}
bool wait_sdcard_idle(unsigned t){assert(t);return true;}
bool wait_dat0_idle(unsigned t){(void)t;return true;}
void send_sdcard_commandbuf(const uint8_t*p,unsigned n){assert(n==6&&p[5]==crc7(p,5));cmd=p[0]&63;arg=((unsigned)p[1]<<24)|((unsigned)p[2]<<16)|(p[3]<<8)|p[4];if(cmd==17||cmd==18)lastread=arg;if(cmd==12)stops++;}
bool receive_sdcard_response(uint8_t*p,unsigned n,unsigned t){assert(t);if(++calls==failat)return false;uint8_t r[20]={0};r[0]=cmd;r[3]=9;
 switch(cmd){case 8:r[3]=1;r[4]=0xaa;break;case 41:if(failat!=1)assert(arg&0x40000000);r[0]=63;r[1]=(unready?0:128)|(hc?64:0);break;case 3:r[1]=0x12;r[2]=0x34;r[3]=6;break;
 case 9:memset(r,0,20);r[0]=63;if(hc){r[1]=badcsd?128:64;r[9]=31;r[10]=255;}else{r[6]=10;r[7]=3;r[8]=255;r[9]=192;r[10]=3;r[11]=128;}break;}
 if(p)memcpy(p,r,n);return true;}
uintptr_t SC_RD_MODE_SYM(w0)(uint8_t*b,unsigned n,unsigned t){assert(n&&t);memset(b,0x5a,n*512);return readfail;}
uintptr_t SC_RD_MODE_SYM(w1)(uint8_t*b,unsigned n,unsigned t){return SC_RD_MODE_SYM(w0)(b,n,t);}
int sc_write_sectors_w0(const uint8_t*b,unsigned n,unsigned t){(void)b;(void)n;(void)t;assert(0);return 1;}
int sc_write_sectors_w1(const uint8_t*b,unsigned n,unsigned t){return sc_write_sectors_w0(b,n,t);}
int main(void){t_card_info i;hc=1;assert(sdcard_init(&i)==0);assert(sc_issdhc()&&i.sdhc&&i.block_cnt==8388608);unsigned total=calls;uint8_t b[1024];assert(sdcard_read_blocks(b,0x400123,2)==0&&lastread==0x400123&&b[1023]==0x5a);
 hc=0;assert(sdcard_init(&i)==0);assert(!sc_issdhc()&&!i.sdhc&&i.block_cnt==4194304);assert(sdcard_read_blocks(b,123,1)==0&&lastread==123*512);
 unready=1;assert(sdcard_init(&i)!=0);unready=0;hc=1;badcsd=1;assert(sdcard_init(&i)!=0);badcsd=0;
 for(failat=1;failat<=total;failat++){calls=0;unsigned err=sdcard_init(&i);/* CMD8 absence legitimately falls back, but CCS+CSD then disagree. */assert(err!=0);}failat=0;
 readfail=1;unsigned old=stops;assert(sdcard_read_blocks(b,1,2)!=0&&stops==old+1);puts("SuperFW production init/CSD/addressing/error/stop tests PASS");}
