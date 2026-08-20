#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.c"
int main(){FILE*f=fopen(".tools/map_idat.bin","rb");fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);unsigned char*in=malloc(n);fread(in,1,n,f);fclose(f);unsigned char*dict=malloc(32768);tinfl_decompressor d;tinfl_init(&d);size_t io=0,off=0,total=0;for(;;){size_t ni=n-io,no=32768-off;tinfl_status s=tinfl_decompress(&d,in+io,&ni,dict,dict+off,&no,TINFL_FLAG_PARSE_ZLIB_HEADER);io+=ni;total+=no;printf("s=%d in=%zu/%zu out=%zu total=%zu off=%zu\n",s,io,n,no,total,off);if(s==TINFL_STATUS_DONE)break;if(s!=TINFL_STATUS_HAS_MORE_OUTPUT){return 2;}off=(off+no)&32767;}printf("OK total=%zu\n",total);return total==409920?0:3;}
