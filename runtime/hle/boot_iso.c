#include "boot_iso.h"
#include "iso9660.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_SIZE 3489696u
#define IMAGE_SIZE 3785392u
#define LOAD_BASE 0x08804000u

/* Fingerprint of ULUS10176's unencrypted PSP_GAME/SYSDIR/BOOT.BIN.
 * Native functions were compiled for this executable. Another region or a
 * modified executable would dispatch into the wrong code. */
static const u8 boot_sha[32] = {
    0x72,0xdb,0xf7,0xed,0xf3,0x40,0xff,0xe0,0x24,0x5d,0x2b,0xae,0x7c,0x38,0xa5,0x5b,
    0x80,0x12,0xc8,0xf6,0xbd,0x6c,0xde,0xee,0x83,0x1d,0xc0,0x01,0x2d,0xdf,0xe9,0x79
};
static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}
static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1]<<8; }
static void put32(u8 *p, u32 v)
{
    p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24);
}
static u32 ror(u32 x, unsigned n) { return (x>>n) | (x<<(32-n)); }

/* SHA-256, FIPS 180-4. Input size is checked before this function. */
static void sha256(const u8 *data, size_t size, u8 out[32])
{
    static const u32 k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    u32 h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t blocks=(size+9+63)/64;
    for (size_t block=0;block<blocks;++block) {
        u8 b[64]={0}; u32 w[64],a,c,d,e,f,g,t,v;
        size_t start=block*64;
        for (size_t j=0;j<64;++j) {
            size_t at=start+j;
            if (at<size) b[j]=data[at];
            else if (at==size) b[j]=0x80;
        }
        if (block+1==blocks) {
            unsigned long long bits=(unsigned long long)size*8;
            for (unsigned j=0;j<8;++j) b[63-j]=(u8)(bits>>(j*8));
        }
        for (unsigned j=0;j<16;++j)
            w[j]=(u32)b[4*j]<<24 | (u32)b[4*j+1]<<16 | (u32)b[4*j+2]<<8 | b[4*j+3];
        for (unsigned j=16;j<64;++j) {
            u32 x=w[j-15],y=w[j-2];
            w[j]=w[j-16]+(ror(x,7)^ror(x,18)^(x>>3))+w[j-7]+(ror(y,17)^ror(y,19)^(y>>10));
        }
        a=h[0]; v=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; t=h[7];
        for (unsigned j=0;j<64;++j) {
            u32 t1=t+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^(~e&g))+k[j]+w[j];
            u32 t2=(ror(a,2)^ror(a,13)^ror(a,22))+((a&v)^(a&c)^(v&c));
            t=g; g=f; f=e; e=d+t1; d=c; c=v; v=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=v; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=t;
    }
    for (unsigned j=0;j<8;++j)
        for (unsigned i=0;i<4;++i) out[4*j+i]=(u8)(h[j]>>(24-i*8));
}

typedef struct { u32 addr,hi; } PendingHi;
static int relocate(const u8 *elf,u8 *image,u32 capacity)
{
    u32 phoff=le32(elf+28),shoff=le32(elf+32);
    u32 phnum=le16(elf+44),shnum=le16(elf+48),bases[16];
    if (memcmp(elf,"\177ELF\1\1\1",7) || le16(elf+18)!=8
        || le32(elf+24)!=0x128 || le16(elf+42)!=32 || le16(elf+46)!=40
        || phnum>16 || phoff>BOOT_SIZE-phnum*32 || shoff>BOOT_SIZE-shnum*40
        || capacity<IMAGE_SIZE) return 0;
    memset(image,0,IMAGE_SIZE);
    for (u32 i=0;i<phnum;++i) {
        const u8 *p=elf+phoff+i*32;
        u32 off=le32(p+4),va=le32(p+8),filesz=le32(p+16),memsz=le32(p+20);
        bases[i]=va;
        if (le32(p)!=1) continue;
        if (filesz>memsz || va>IMAGE_SIZE || memsz>IMAGE_SIZE-va
            || off>BOOT_SIZE || filesz>BOOT_SIZE-off) return 0;
        memcpy(image+va,elf+off,filesz);
    }
    for (u32 i=0;i<shnum;++i) {
        const u8 *s=elf+shoff+i*40;
        u32 off=le32(s+16),len=le32(s+20),count=0;
        PendingHi *hi;
        /* This exact executable uses uncompressed SHT_PRXRELOC entries. */
        if (le32(s+4)!=0x700000a0) continue;
        if (off>BOOT_SIZE || len>BOOT_SIZE-off || len%8) return 0;
        hi=(PendingHi *)malloc((len/8+1)*sizeof *hi);
        if (!hi) return 0;
        for (u32 j=0;j<len;j+=8) {
            u32 info=le32(elf+off+j+4),ob=(info>>8)&255,ab=(info>>16)&255;
            u32 addr=le32(elf+off+j),value,w,base;
            if (ob>=phnum || ab>=phnum || bases[ob]>IMAGE_SIZE-4
                || addr>IMAGE_SIZE-4-bases[ob]) { free(hi); return 0; }
            addr+=bases[ob]; base=LOAD_BASE+bases[ab]; w=le32(image+addr);
            switch (info&255) {
            case 2: put32(image+addr,w+base); break;
            case 4:
                value=((w&0x03ffffff)<<2)+base;
                put32(image+addr,(w&0xfc000000)|((value>>2)&0x03ffffff)); break;
            case 5: hi[count++]=(PendingHi){addr,w&65535}; break;
            case 6: {
                /* LO16 consumes the pending stream queue once. */
                u32 low=(u32)(int32_t)(int16_t)(w&65535);
                for (u32 n=0;n<count;++n) {
                    value=(hi[n].hi<<16)+low+base;
                    put32(image+hi[n].addr,(le32(image+hi[n].addr)&0xffff0000)
                          | (((value+0x8000)>>16)&65535));
                }
                count=0; put32(image+addr,(w&0xffff0000)|((low+base)&65535)); break;
            }
            default: free(hi); return 0;
            }
        }
        free(hi);
        if (count) return 0;
    }
    return 1;
}

int boot_iso_load(u8 *image,u32 capacity,u32 *size,char *error,size_t error_size)
{
    s64 off,bytes; u8 *elf,digest[32];
    *size=0;
    if (!iso_find("PSP_GAME/SYSDIR/BOOT.BIN",&off,&bytes) || bytes!=BOOT_SIZE) {
        snprintf(error,error_size,"Unsupported game ISO. This build needs the US release of Ace Combat X (ULUS10176), with its original BOOT.BIN.");
        return 0;
    }
    elf=(u8 *)malloc(BOOT_SIZE);
    if (!elf) { snprintf(error,error_size,"Not enough memory to load the game."); return 0; }
    if (off<0 || off>iso_image_size() || bytes>iso_image_size()-off || !iso_pread(off,elf,BOOT_SIZE)) {
        snprintf(error,error_size,"The ISO is incomplete or unreadable. Copy a complete ISO from your own game disc.");
        free(elf); return 0;
    }
    sha256(elf,BOOT_SIZE,digest);
    if (memcmp(digest,boot_sha,32)) {
        snprintf(error,error_size,"Unsupported or modified game executable. This build supports the original US release (ULUS10176).");
        free(elf); return 0;
    }
    /* Assets are streamed by absolute sector: reject a repack with shifted
     * extents rather than letting a valid executable read unrelated data. */
    if (!iso_find("PSP_GAME/USRDIR/REGFILE.CDI",&off,&bytes)
        || off!=0x5f20LL*2048 || bytes!=1071140864LL || iso_image_size()<off+bytes) {
        snprintf(error,error_size,"The ISO is incomplete or has an unsupported disc layout. Use an original, uncompressed ULUS10176 ISO.");
        free(elf); return 0;
    }
    if (!relocate(elf,image,capacity)) {
        snprintf(error,error_size,"Could not prepare the game's executable for this runtime.");
        free(elf); return 0;
    }
    free(elf); *size=IMAGE_SIZE;
    printf("[boot] verified ULUS10176; prepared %u bytes directly from ISO\n",*size);
    return 1;
}
