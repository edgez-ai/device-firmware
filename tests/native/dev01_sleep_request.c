#include "dev01_sleep_request.h"
#include <assert.h>
int main(void) {
  uint8_t p[] = {0x42, 2, 0x12, 0x34, 0xab, 0xcd, 0xff};
  struct dev01_sleep_request a = dev01_sleep_request_decode(1,p,sizeof(p));
  assert(a.valid && a.mid==0x1234 && a.token_length==2);
  struct dev01_sleep_request b = dev01_sleep_request_decode(1,p,sizeof(p));
  assert(dev01_sleep_request_same(&a,&b));
  b.peer=2;assert(!dev01_sleep_request_same(&a,&b));
  b=a;b.mid++;assert(!dev01_sleep_request_same(&a,&b));
  b=a;b.token[1]++;assert(!dev01_sleep_request_same(&a,&b));
  b=a;b.token_length=9;assert(!dev01_sleep_request_same(&a,&b));
  for(size_t n=0;n<6;n++)assert(!dev01_sleep_request_decode(1,p,n).valid);
  p[0]=0x49;assert(!dev01_sleep_request_decode(1,p,sizeof(p)).valid);
  p[0]=0x82;assert(!dev01_sleep_request_decode(1,p,sizeof(p)).valid);
  p[0]=0x62;assert(!dev01_sleep_request_decode(1,p,sizeof(p)).valid);
  p[0]=0x42;p[1]=1;assert(!dev01_sleep_request_decode(1,p,sizeof(p)).valid);
  p[0]=0x50;p[1]=2;assert(dev01_sleep_request_decode(1,p,4).valid);
  assert(!dev01_sleep_request_decode(1,NULL,10).valid);
  return 0;
}
