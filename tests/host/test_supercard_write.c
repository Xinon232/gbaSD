/* SuperCard driver host test: multi-block write status, stop, busy, retries
 * and the block limit, for the configuration it is compiled with.
 * From gbamp3's test_write_driver.c. gbaSD, GPL-3.0-or-later. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "supercard_driver.h"
#include "gbasd_config.h"
#define SC_RD_CAT2(a,b,c) a##b##c
#define SC_RD_CAT(a,b,c) SC_RD_CAT2(a,b,c)
#define SC_RD_MODE_SYM(w) SC_RD_CAT(sc_read_sectors_crc,GBASD_SC_READ_CRC,_##w)
bool isgba=true,slowsd=false;
static unsigned cmd,arg,failcmd,busy,writefail,stops,writes;
void send_empty_clocks(unsigned n){assert(n);}
bool wait_sdcard_idle(unsigned n){return n!=0;}
bool wait_dat0_idle(unsigned n){assert(n);return !busy;}
void send_sdcard_commandbuf(const uint8_t*p,unsigned n){assert(n==6);cmd=p[0]&63;arg=(unsigned)p[1]<<24|(unsigned)p[2]<<16|p[3]<<8|p[4];if(cmd==12)stops++;}
bool receive_sdcard_response(uint8_t*p,unsigned n,unsigned t){assert(t);if(cmd==failcmd)return false;if(p){memset(p,0,n);p[0]=cmd;if(cmd==13){p[3]=9;p[4]=0;}}return true;}
uintptr_t SC_RD_MODE_SYM(w0)(uint8_t*b,unsigned n,unsigned t){(void)b;(void)n;(void)t;return 1;}
uintptr_t SC_RD_MODE_SYM(w1)(uint8_t*b,unsigned n,unsigned t){return SC_RD_MODE_SYM(w0)(b,n,t);}
int sc_write_sectors_w0(const uint8_t*b,unsigned n,unsigned t){assert(b&&n&&t&&arg==123*512);writes++;return writefail;}
int sc_write_sectors_w1(const uint8_t*b,unsigned n,unsigned t){return sc_write_sectors_w0(b,n,t);}
int main(void){
  uint8_t b[512]={0};
  assert(!sdcard_write_blocks(b,123,1));assert(writes==1&&stops==1);
  busy=1;assert(sdcard_write_blocks(b,123,1));busy=0;
  writefail=1;unsigned old=stops,oldw=writes;
  assert(sdcard_write_blocks(b,123,1)&&stops>old);
  assert(writes==oldw+1+GBASD_SC_WRITE_RETRIES);           /* every attempt sent the data */
  writefail=0;failcmd=25;old=writes;assert(sdcard_write_blocks(b,123,1)&&writes==old);failcmd=0;
  assert(sdcard_write_blocks(0,0,1));assert(sdcard_write_blocks(b,0,0));
#if GBASD_SC_MAX_WRITE_BLOCKS
  assert(sdcard_write_blocks(b,0,GBASD_SC_MAX_WRITE_BLOCKS+1));
#endif
  printf("SD write bounded errors/stop/busy/SDSC, %u retries PASS\n",(unsigned)GBASD_SC_WRITE_RETRIES);
}
