#include "boot_iso.h"
#include "iso9660.h"
#include <stdio.h>
#include <stdlib.h>
/* Standalone importer check: no renderer, no game execution, no private
 * inputs embedded in the test. Compare output to Python prxload separately. */
int main(int argc,char **argv)
{
    char error[512]; u32 size=0,cap=4u*1024u*1024u;
    u8 *image=(u8 *)malloc(cap); FILE *out;
    if (argc<2 || !image) return 3;
    if (!iso_mount(argv[1])) { fputs("Cannot mount ISO\n",stderr); free(image); return 2; }
    if (!boot_iso_load(image,cap,&size,error,sizeof error)) {
        fprintf(stderr,"%s\n",error); free(image); return 2;
    }
    if (argc>2) {
        out=fopen(argv[2],"wb");
        if (!out) { free(image); return 3; }
        if (fwrite(image,1,size,out)!=size || fclose(out)) { free(image); return 3; }
    }
    free(image); return 0;
}
