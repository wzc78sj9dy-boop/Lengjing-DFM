/**
 * T3验证SDK - C++版本实现
 * 纯C++实现，不依赖OpenSSL
 */

#include "t3sdk.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <random>

/* 平台检测 */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    #include <iphlpapi.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <net/if.h>
    #ifdef __ANDROID__
        #include <curl/curl.h>
        #include <dlfcn.h>
    #endif
    #ifdef __APPLE__
        #include <sys/sysctl.h>
        #include <net/if_dl.h>
    #else
        #include <linux/if_packet.h>
    #endif
#endif

/* ========== MD5算法实现 ========== */
namespace {

struct MD5Context {
    uint32_t state[4];
    uint32_t count[2];
    unsigned char buffer[64];
};

#define MD5_S11 7
#define MD5_S12 12
#define MD5_S13 17
#define MD5_S14 22
#define MD5_S21 5
#define MD5_S22 9
#define MD5_S23 14
#define MD5_S24 20
#define MD5_S31 4
#define MD5_S32 11
#define MD5_S33 16
#define MD5_S34 23
#define MD5_S41 6
#define MD5_S42 10
#define MD5_S43 15
#define MD5_S44 21

#define MD5_F(x,y,z) (((x)&(y))|((~x)&(z)))
#define MD5_G(x,y,z) (((x)&(z))|((y)&(~z)))
#define MD5_H(x,y,z) ((x)^(y)^(z))
#define MD5_I(x,y,z) ((y)^((x)|(~z)))
#define MD5_ROL(x,n) (((x)<<(n))|((x)>>(32-(n))))

#define MD5_FF(a,b,c,d,x,s,ac) { (a)+=MD5_F((b),(c),(d))+(x)+(uint32_t)(ac); (a)=MD5_ROL((a),(s)); (a)+=(b); }
#define MD5_GG(a,b,c,d,x,s,ac) { (a)+=MD5_G((b),(c),(d))+(x)+(uint32_t)(ac); (a)=MD5_ROL((a),(s)); (a)+=(b); }
#define MD5_HH(a,b,c,d,x,s,ac) { (a)+=MD5_H((b),(c),(d))+(x)+(uint32_t)(ac); (a)=MD5_ROL((a),(s)); (a)+=(b); }
#define MD5_II(a,b,c,d,x,s,ac) { (a)+=MD5_I((b),(c),(d))+(x)+(uint32_t)(ac); (a)=MD5_ROL((a),(s)); (a)+=(b); }

static void md5Encode(unsigned char *out, const uint32_t *in, unsigned int len) {
    for (unsigned int i=0,j=0; j<len; i++,j+=4) {
        out[j]=(unsigned char)(in[i]&0xff);
        out[j+1]=(unsigned char)((in[i]>>8)&0xff);
        out[j+2]=(unsigned char)((in[i]>>16)&0xff);
        out[j+3]=(unsigned char)((in[i]>>24)&0xff);
    }
}

static void md5Decode(uint32_t *out, const unsigned char *in, unsigned int len) {
    for (unsigned int i=0,j=0; j<len; i++,j+=4)
        out[i]=((uint32_t)in[j])|(((uint32_t)in[j+1])<<8)|(((uint32_t)in[j+2])<<16)|(((uint32_t)in[j+3])<<24);
}

static void md5Transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],x[16];
    md5Decode(x,block,64);
    MD5_FF(a,b,c,d,x[0],MD5_S11,0xd76aa478); MD5_FF(d,a,b,c,x[1],MD5_S12,0xe8c7b756);
    MD5_FF(c,d,a,b,x[2],MD5_S13,0x242070db); MD5_FF(b,c,d,a,x[3],MD5_S14,0xc1bdceee);
    MD5_FF(a,b,c,d,x[4],MD5_S11,0xf57c0faf); MD5_FF(d,a,b,c,x[5],MD5_S12,0x4787c62a);
    MD5_FF(c,d,a,b,x[6],MD5_S13,0xa8304613); MD5_FF(b,c,d,a,x[7],MD5_S14,0xfd469501);
    MD5_FF(a,b,c,d,x[8],MD5_S11,0x698098d8); MD5_FF(d,a,b,c,x[9],MD5_S12,0x8b44f7af);
    MD5_FF(c,d,a,b,x[10],MD5_S13,0xffff5bb1); MD5_FF(b,c,d,a,x[11],MD5_S14,0x895cd7be);
    MD5_FF(a,b,c,d,x[12],MD5_S11,0x6b901122); MD5_FF(d,a,b,c,x[13],MD5_S12,0xfd987193);
    MD5_FF(c,d,a,b,x[14],MD5_S13,0xa679438e); MD5_FF(b,c,d,a,x[15],MD5_S14,0x49b40821);
    MD5_GG(a,b,c,d,x[1],MD5_S21,0xf61e2562); MD5_GG(d,a,b,c,x[6],MD5_S22,0xc040b340);
    MD5_GG(c,d,a,b,x[11],MD5_S23,0x265e5a51); MD5_GG(b,c,d,a,x[0],MD5_S24,0xe9b6c7aa);
    MD5_GG(a,b,c,d,x[5],MD5_S21,0xd62f105d); MD5_GG(d,a,b,c,x[10],MD5_S22,0x2441453);
    MD5_GG(c,d,a,b,x[15],MD5_S23,0xd8a1e681); MD5_GG(b,c,d,a,x[4],MD5_S24,0xe7d3fbc8);
    MD5_GG(a,b,c,d,x[9],MD5_S21,0x21e1cde6); MD5_GG(d,a,b,c,x[14],MD5_S22,0xc33707d6);
    MD5_GG(c,d,a,b,x[3],MD5_S23,0xf4d50d87); MD5_GG(b,c,d,a,x[8],MD5_S24,0x455a14ed);
    MD5_GG(a,b,c,d,x[13],MD5_S21,0xa9e3e905); MD5_GG(d,a,b,c,x[2],MD5_S22,0xfcefa3f8);
    MD5_GG(c,d,a,b,x[7],MD5_S23,0x676f02d9); MD5_GG(b,c,d,a,x[12],MD5_S24,0x8d2a4c8a);
    MD5_HH(a,b,c,d,x[5],MD5_S31,0xfffa3942); MD5_HH(d,a,b,c,x[8],MD5_S32,0x8771f681);
    MD5_HH(c,d,a,b,x[11],MD5_S33,0x6d9d6122); MD5_HH(b,c,d,a,x[14],MD5_S34,0xfde5380c);
    MD5_HH(a,b,c,d,x[1],MD5_S31,0xa4beea44); MD5_HH(d,a,b,c,x[4],MD5_S32,0x4bdecfa9);
    MD5_HH(c,d,a,b,x[7],MD5_S33,0xf6bb4b60); MD5_HH(b,c,d,a,x[10],MD5_S34,0xbebfbc70);
    MD5_HH(a,b,c,d,x[13],MD5_S31,0x289b7ec6); MD5_HH(d,a,b,c,x[0],MD5_S32,0xeaa127fa);
    MD5_HH(c,d,a,b,x[3],MD5_S33,0xd4ef3085); MD5_HH(b,c,d,a,x[6],MD5_S34,0x4881d05);
    MD5_HH(a,b,c,d,x[9],MD5_S31,0xd9d4d039); MD5_HH(d,a,b,c,x[12],MD5_S32,0xe6db99e5);
    MD5_HH(c,d,a,b,x[15],MD5_S33,0x1fa27cf8); MD5_HH(b,c,d,a,x[2],MD5_S34,0xc4ac5665);
    MD5_II(a,b,c,d,x[0],MD5_S41,0xf4292244); MD5_II(d,a,b,c,x[7],MD5_S42,0x432aff97);
    MD5_II(c,d,a,b,x[14],MD5_S43,0xab9423a7); MD5_II(b,c,d,a,x[5],MD5_S44,0xfc93a039);
    MD5_II(a,b,c,d,x[12],MD5_S41,0x655b59c3); MD5_II(d,a,b,c,x[3],MD5_S42,0x8f0ccc92);
    MD5_II(c,d,a,b,x[10],MD5_S43,0xffeff47d); MD5_II(b,c,d,a,x[1],MD5_S44,0x85845dd1);
    MD5_II(a,b,c,d,x[8],MD5_S41,0x6fa87e4f); MD5_II(d,a,b,c,x[15],MD5_S42,0xfe2ce6e0);
    MD5_II(c,d,a,b,x[6],MD5_S43,0xa3014314); MD5_II(b,c,d,a,x[13],MD5_S44,0x4e0811a1);
    MD5_II(a,b,c,d,x[4],MD5_S41,0xf7537e82); MD5_II(d,a,b,c,x[11],MD5_S42,0xbd3af235);
    MD5_II(c,d,a,b,x[2],MD5_S43,0x2ad7d2bb); MD5_II(b,c,d,a,x[9],MD5_S44,0xeb86d391);
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    memset(x,0,sizeof(x));
}

static void md5Init(MD5Context *ctx) {
    ctx->count[0]=ctx->count[1]=0;
    ctx->state[0]=0x67452301; ctx->state[1]=0xefcdab89;
    ctx->state[2]=0x98badcfe; ctx->state[3]=0x10325476;
}

static void md5Update(MD5Context *ctx, const unsigned char *input, unsigned int len) {
    unsigned int i,idx,partLen;
    idx=(unsigned int)((ctx->count[0]>>3)&0x3F);
    if((ctx->count[0]+=((uint32_t)len<<3))<((uint32_t)len<<3)) ctx->count[1]++;
    ctx->count[1]+=((uint32_t)len>>29);
    partLen=64-idx;
    if(len>=partLen){
        memcpy(&ctx->buffer[idx],input,partLen);
        md5Transform(ctx->state,ctx->buffer);
        for(i=partLen;i+63<len;i+=64) md5Transform(ctx->state,&input[i]);
        idx=0;
    } else i=0;
    memcpy(&ctx->buffer[idx],&input[i],len-i);
}

static void md5Final(unsigned char digest[16], MD5Context *ctx) {
    static unsigned char PADDING[64]={0x80};
    unsigned char bits[8]; unsigned int idx,padLen;
    md5Encode(bits,ctx->count,8);
    idx=(unsigned int)((ctx->count[0]>>3)&0x3f);
    padLen=(idx<56)?(56-idx):(120-idx);
    md5Update(ctx,PADDING,padLen);
    md5Update(ctx,bits,8);
    md5Encode(digest,ctx->state,16);
    memset(ctx,0,sizeof(*ctx));
}

std::string md5String(const std::string& str, bool upperCase=false) {
    MD5Context ctx; unsigned char digest[16];
    md5Init(&ctx);
    md5Update(&ctx,(const unsigned char*)str.c_str(),(unsigned int)str.size());
    md5Final(digest,&ctx);
    std::ostringstream oss;
    for(int i=0;i<16;i++)
        oss<<std::hex<<std::setfill('0')<<std::setw(2)<<(upperCase?std::uppercase:std::nouppercase)<<(int)digest[i];
    return oss.str();
}

/* ========== 字节/HEX转换工具 ========== */

std::string bytesToHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    for(auto b:data) oss<<std::hex<<std::uppercase<<std::setfill('0')<<std::setw(2)<<(int)b;
    return oss.str();
}

std::string stringToHex(const std::string& str) {
    std::ostringstream oss;
    for(unsigned char c:str) oss<<std::hex<<std::uppercase<<std::setfill('0')<<std::setw(2)<<(int)c;
    return oss.str();
}

int hexCharToInt(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

/* ========== 标准Base64解码(用于RSA公钥PEM解析) ========== */

static const char STD_B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::vector<uint8_t> stdBase64Decode(const std::string& in) {
    unsigned char tbl[256]; memset(tbl,0xFF,256);
    for(int i=0;i<64;i++) tbl[(unsigned char)STD_B64[i]]=i;
    std::vector<uint8_t> out;
    int val=0,bits=0;
    for(char c:in){
        if(c=='='||c=='\n'||c=='\r'||c==' ') continue;
        if(tbl[(unsigned char)c]==0xFF) continue;
        val=(val<<6)|tbl[(unsigned char)c]; bits+=6;
        if(bits>=8){ bits-=8; out.push_back((uint8_t)((val>>bits)&0xFF)); }
    }
    return out;
}

} /* end anonymous namespace */

/* ========== 大数运算(纯C++, 无OpenSSL RSA实现) ========== */

#define BN_MAX 72

struct RSACrypto::BigNum {
    uint32_t w[BN_MAX];
    int len;

    BigNum() { memset(w,0,sizeof(w)); len=1; }

    void set(uint32_t v) { memset(w,0,sizeof(w)); w[0]=v; len=1; }
    void trim() { while(len>1&&w[len-1]==0) len--; }
    bool isZero() const { return len==1&&w[0]==0; }

    static BigNum fromBE(const unsigned char *b, int blen) {
        BigNum a; a.len=(blen+3)/4; if(a.len<1)a.len=1; if(a.len>BN_MAX)a.len=BN_MAX;
        for(int i=0;i<blen;i++){
            int wi=(blen-1-i)/4, bi=(blen-1-i)%4;
            if(wi<BN_MAX) a.w[wi]|=((uint32_t)b[i])<<(bi*8);
        }
        a.trim(); return a;
    }

    void toBE(unsigned char *b, int blen) const {
        memset(b,0,blen);
        for(int i=0;i<len&&i*4<blen;i++)
            for(int j=0;j<4&&i*4+j<blen;j++)
                b[blen-1-(i*4+j)]=(w[i]>>(j*8))&0xFF;
    }

    int cmp(const BigNum& o) const {
        if(len!=o.len) return len>o.len?1:-1;
        for(int i=len-1;i>=0;i--)
            if(w[i]!=o.w[i]) return w[i]>o.w[i]?1:-1;
        return 0;
    }

    static BigNum add(const BigNum& a, const BigNum& b) {
        BigNum c; int ml=a.len>b.len?a.len:b.len; uint64_t carry=0;
        for(int i=0;i<ml||(int)carry;i++){
            uint64_t s=carry;
            if(i<a.len)s+=a.w[i]; if(i<b.len)s+=b.w[i];
            c.w[i]=(uint32_t)(s&0xFFFFFFFF); carry=s>>32;
            c.len=i+1;
        }
        if(c.len>BN_MAX) c.len=BN_MAX;
        c.trim(); return c;
    }

    static BigNum sub(const BigNum& a, const BigNum& b) {
        BigNum c; int64_t borrow=0;
        for(int i=0;i<a.len;i++){
            int64_t d=(int64_t)a.w[i]-borrow;
            if(i<b.len) d-=b.w[i];
            if(d<0){d+=0x100000000LL;borrow=1;} else borrow=0;
            c.w[i]=(uint32_t)d;
        }
        c.len=a.len; c.trim(); return c;
    }

    static BigNum shl(const BigNum& a, int bits) {
        BigNum c; if(a.isZero()) return c;
        int ws=bits/32, bs=bits%32;
        if(bs==0){
            for(int i=0;i<a.len&&i+ws<BN_MAX;i++) c.w[i+ws]=a.w[i];
        } else {
            uint32_t carry=0;
            for(int i=0;i<a.len&&i+ws<BN_MAX;i++){
                uint64_t v=((uint64_t)a.w[i]<<bs)|carry;
                c.w[i+ws]=(uint32_t)(v&0xFFFFFFFF); carry=(uint32_t)(v>>32);
            }
            if(carry&&a.len+ws<BN_MAX) c.w[a.len+ws]=carry;
        }
        c.len=a.len+ws+1; if(c.len>BN_MAX) c.len=BN_MAX; c.trim(); return c;
    }

    static BigNum shr(const BigNum& a, int bits) {
        BigNum c; if(a.isZero()) return c;
        int ws=bits/32, bs=bits%32;
        if(ws>=a.len) return c;
        c.len=a.len-ws;
        if(bs==0){
            for(int i=0;i<c.len;i++) c.w[i]=a.w[i+ws];
        } else {
            for(int i=0;i<c.len;i++){
                c.w[i]=a.w[i+ws]>>bs;
                if(i+ws+1<a.len) c.w[i]|=a.w[i+ws+1]<<(32-bs);
            }
        }
        c.trim(); return c;
    }

    static BigNum mul(const BigNum& a, const BigNum& b) {
        BigNum r; r.len=a.len+b.len; if(r.len>BN_MAX) r.len=BN_MAX;
        for(int i=0;i<a.len;i++){
            uint64_t carry=0;
            for(int j=0;j<b.len&&i+j<BN_MAX;j++){
                uint64_t p=(uint64_t)a.w[i]*b.w[j]+r.w[i+j]+carry;
                r.w[i+j]=(uint32_t)(p&0xFFFFFFFF); carry=p>>32;
            }
            if(carry&&i+b.len<BN_MAX) r.w[i+b.len]+=(uint32_t)carry;
        }
        r.trim(); return r;
    }

    static BigNum mod(const BigNum& a, const BigNum& m) {
        BigNum r=a;
        if(m.isZero()||a.cmp(m)<0) return r;
        int shift=0;
        { BigNum t=m; while(t.cmp(r)<0){t=shl(t,1);shift++;}
          if(t.cmp(r)>0) shift--; }
        for(int i=shift;i>=0;i--){
            BigNum sh=shl(m,i);
            if(r.cmp(sh)>=0) r=sub(r,sh);
        }
        return r;
    }

    static BigNum modpow(const BigNum& base, const BigNum& exp, const BigNum& mod_) {
        BigNum res; res.set(1);
        BigNum b=mod(base,mod_); BigNum e=exp;
        while(!e.isZero()){
            if(e.w[0]&1) res=mod(mul(res,b),mod_);
            b=mod(mul(b,b),mod_);
            e=shr(e,1);
        }
        return res;
    }
};

/* ========== RSA密钥结构 ========== */

struct RSACrypto::RSAKey {
    RSACrypto::BigNum n, e;
    int keySize;
};

/* ========== RSACrypto 实现 ========== */

RSACrypto::RSACrypto(const std::string& publicKeyPem) : key_(nullptr), keySize_(0) {
    key_ = new RSAKey();
    try {
        parsePEM(publicKeyPem);
    } catch (...) {
        delete key_;
        key_ = nullptr;
        throw;
    }
    encryptBlockSize_ = keySize_ - 11;
    decryptBlockSize_ = keySize_;
}

RSACrypto::~RSACrypto() {
    delete key_;
}

void RSACrypto::parsePEM(const std::string& pem) {
    /* 提取Base64内容 */
    auto s = pem.find("-----BEGIN PUBLIC KEY-----");
    auto e = pem.find("-----END PUBLIC KEY-----");
    if(s==std::string::npos||e==std::string::npos)
        throw std::runtime_error("Invalid PEM format");
    s += strlen("-----BEGIN PUBLIC KEY-----");
    std::string b64;
    for(size_t i=s;i<e;i++){
        char c=pem[i];
        if(c!='\n'&&c!='\r'&&c!=' '&&c!='\t') b64+=c;
    }

    /* Base64解码为DER */
    std::vector<uint8_t> der = stdBase64Decode(b64);
    if(der.empty()) throw std::runtime_error("PEM base64 decode failed");

    /* ASN.1 DER 解析 */
    int off=0;
    auto readLen=[&]()->int{
        unsigned char f=der[off++]; int n,l=0;
        if(f<0x80) return f;
        n=f&0x7F;
        for(int i=0;i<n;i++){l=(l<<8)|der[off];off++;}
        return l;
    };

    /* 外层SEQUENCE */
    if(der[off]!=0x30) throw std::runtime_error("DER: expected SEQUENCE");
    off++; readLen();
    /* AlgorithmIdentifier SEQUENCE */
    if(der[off]!=0x30) throw std::runtime_error("DER: expected AlgorithmIdentifier");
    off++; int alen=readLen(); off+=alen;
    /* BIT STRING */
    if(der[off]!=0x03) throw std::runtime_error("DER: expected BIT STRING");
    off++; readLen(); off++; /* skip unused bits byte */
    /* 内层SEQUENCE */
    if(der[off]!=0x30) throw std::runtime_error("DER: expected inner SEQUENCE");
    off++; readLen();
    /* INTEGER n */
    if(der[off]!=0x02) throw std::runtime_error("DER: expected INTEGER for n");
    off++; int nlen=readLen();
    if(der[off]==0x00){off++;nlen--;}
    key_->n = BigNum::fromBE(der.data()+off, nlen);
    key_->keySize = nlen;
    keySize_ = nlen;
    off+=nlen;
    /* INTEGER e */
    if(der[off]!=0x02) throw std::runtime_error("DER: expected INTEGER for e");
    off++; int elen=readLen();
    key_->e = BigNum::fromBE(der.data()+off, elen);
}

std::vector<uint8_t> RSACrypto::encrypt(const std::string& data) const {
    std::vector<uint8_t> result;
    const uint8_t* msg = (const uint8_t*)data.c_str();
    int mlen = (int)data.size();
    int nb = (mlen + encryptBlockSize_ - 1) / encryptBlockSize_;
    if(nb<1) nb=1;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(1,255);

    for(int i=0;i<nb;i++){
        int off=i*encryptBlockSize_;
        int cur=(off+encryptBlockSize_<=mlen)?encryptBlockSize_:(mlen-off);
        if(cur<0) cur=0;

        /* PKCS1 v1.5 填充 */
        std::vector<uint8_t> padded(keySize_,0);
        padded[0]=0x00; padded[1]=0x02;
        int pl=keySize_-cur-3;
        for(int j=0;j<pl;j++) padded[2+j]=(uint8_t)dist(rng);
        padded[2+pl]=0x00;
        if(cur>0) memcpy(padded.data()+3+pl, msg+off, cur);

        /* RSA: m^e mod n */
        BigNum m = BigNum::fromBE(padded.data(), keySize_);
        BigNum c = BigNum::modpow(m, key_->e, key_->n);
        std::vector<uint8_t> block(keySize_);
        c.toBE(block.data(), keySize_);
        result.insert(result.end(), block.begin(), block.end());
    }
    return result;
}

std::string RSACrypto::decrypt(const std::vector<uint8_t>& encryptedData) const {
    std::string result;
    int nb = (int)encryptedData.size() / decryptBlockSize_;

    for(int i=0;i<nb;i++){
        BigNum c = BigNum::fromBE(encryptedData.data()+i*decryptBlockSize_, decryptBlockSize_);
        BigNum m = BigNum::modpow(c, key_->e, key_->n);
        std::vector<uint8_t> blk(keySize_);
        m.toBE(blk.data(), keySize_);

        /* 去除PKCS1 v1.5 填充: 0x00 0x01 [0xFF...] 0x00 [data] */
        int padEnd=-1;
        for(int j=2;j<keySize_;j++){
            if(blk[j]==0x00){padEnd=j;break;}
        }
        if(padEnd!=-1){
            result.append((char*)blk.data()+padEnd+1, keySize_-padEnd-1);
        } else {
            result.append((char*)blk.data(), keySize_);
        }
    }
    return result;
}

std::string RSACrypto::encryptToHex(const std::string& data) const {
    return bytesToHex(encrypt(data));
}

std::string RSACrypto::decryptFromBase64(const std::string& base64Str) const {
    std::vector<uint8_t> enc = stdBase64Decode(base64Str);
    return decrypt(enc);
}

/* ========== CustomBase64 实现 ========== */

const std::string CustomBase64::STANDARD_CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

CustomBase64::CustomBase64(const std::string& customCharset) : customCharset_(customCharset) {
    if(customCharset.size()!=64)
        throw std::invalid_argument("自定义字符集必须是64位字符");
}

std::string CustomBase64::encode(const std::string& data) const {
    const unsigned char* d = (const unsigned char*)data.c_str();
    int dlen = (int)data.size();
    std::string out;
    for(int i=0;i<dlen;i+=3){
        int val=d[i]<<16;
        if(i+1<dlen) val|=d[i+1]<<8;
        if(i+2<dlen) val|=d[i+2];
        out+=STANDARD_CHARSET[(val>>18)&0x3F];
        out+=STANDARD_CHARSET[(val>>12)&0x3F];
        out+=(i+1<dlen)?STANDARD_CHARSET[(val>>6)&0x3F]:'=';
        out+=(i+2<dlen)?STANDARD_CHARSET[val&0x3F]:'=';
    }
    /* 转换为自定义字符集 */
    for(auto& c:out){
        if(c!='='){
            auto pos=STANDARD_CHARSET.find(c);
            if(pos!=std::string::npos) c=customCharset_[pos];
        }
    }
    return out;
}

std::string CustomBase64::decode(const std::string& data) const {
    /* 先转换回标准字符集 */
    std::string std64;
    for(char c:data){
        if(c=='='){std64+='=';continue;}
        auto pos=customCharset_.find(c);
        if(pos!=std::string::npos) std64+=STANDARD_CHARSET[pos];
        else std64+=c;
    }
    /* 标准Base64解码 */
    unsigned char tbl[256]; memset(tbl,0xFF,256);
    for(int i=0;i<64;i++) tbl[(unsigned char)STANDARD_CHARSET[i]]=i;
    std::string out;
    int len=(int)std64.size();
    for(int i=0;i<len;i+=4){
        int val=(tbl[(unsigned char)std64[i]]<<18)|
                (tbl[(unsigned char)std64[i+1]]<<12)|
                (tbl[(unsigned char)std64[i+2]]<<6)|
                 tbl[(unsigned char)std64[i+3]];
        out+=(char)((val>>16)&0xFF);
        if(std64[i+2]!='=') out+=(char)((val>>8)&0xFF);
        if(std64[i+3]!='=') out+=(char)(val&0xFF);
    }
    return out;
}

std::string CustomBase64::encodeToHex(const std::string& data) const {
    return stringToHex(encode(data));
}

/* ========== JSON解析辅助 ========== */

namespace {

int unicodeToUtf8(unsigned int cp, unsigned char *utf8) {
    if(cp<=0x7F){utf8[0]=(unsigned char)cp;return 1;}
    else if(cp<=0x7FF){utf8[0]=(unsigned char)(0xC0|(cp>>6));utf8[1]=(unsigned char)(0x80|(cp&0x3F));return 2;}
    else if(cp<=0xFFFF){utf8[0]=(unsigned char)(0xE0|(cp>>12));utf8[1]=(unsigned char)(0x80|((cp>>6)&0x3F));utf8[2]=(unsigned char)(0x80|(cp&0x3F));return 3;}
    else if(cp<=0x10FFFF){utf8[0]=(unsigned char)(0xF0|(cp>>18));utf8[1]=(unsigned char)(0x80|((cp>>12)&0x3F));utf8[2]=(unsigned char)(0x80|((cp>>6)&0x3F));utf8[3]=(unsigned char)(0x80|(cp&0x3F));return 4;}
    return 0;
}

std::string decodeJsonString(const std::string& in) {
    std::string out; size_t i=0;
    while(i<in.size()){
        if(in[i]=='\\'&&i+1<in.size()){
            i++;
            switch(in[i]){
                case '"': case '\\': case '/': out+=in[i]; break;
                case 'b': out+='\b'; break; case 'f': out+='\f'; break;
                case 'n': out+='\n'; break; case 'r': out+='\r'; break;
                case 't': out+='\t'; break;
                case 'u': {
                    if(i+4<in.size()){
                        unsigned int cp=0;
                        for(int j=0;j<4;j++){int v=hexCharToInt(in[i+1+j]);if(v<0)break;cp=(cp<<4)|v;}
                        unsigned char u[4]; int ul=unicodeToUtf8(cp,u);
                        for(int j=0;j<ul;j++) out+=(char)u[j];
                        i+=4;
                    }
                    break;
                }
                default: out+=in[i]; break;
            }
            i++;
        } else { out+=in[i]; i++; }
    }
    return out;
}

bool jsonGetString(const std::string& json, const std::string& key, std::string& value) {
    std::string sk="\""+key+"\"";
    auto pos=json.find(sk);
    if(pos==std::string::npos) return false;
    pos=json.find(':',pos); if(pos==std::string::npos) return false;
    pos++; while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t')) pos++;
    if(pos>=json.size()) return false;
    if(json[pos]=='"') {
        pos++;
        std::string raw;
        while(pos<json.size()&&json[pos]!='"'){
            if(json[pos]=='\\'&&pos+1<json.size()){raw+=json[pos];raw+=json[pos+1];pos+=2;}
            else {raw+=json[pos];pos++;}
        }
        value=decodeJsonString(raw);
        return true;
    }
    // 处理非字符串值（数字、布尔、null）
    if(json.substr(pos,4)=="null") { value=""; return true; }
    std::string raw;
    while(pos<json.size()&&json[pos]!=','&&json[pos]!='}'&&json[pos]!=']'&&json[pos]!='\n') {
        raw+=json[pos]; pos++;
    }
    // 去除尾部空白
    while(!raw.empty()&&(raw.back()==' '||raw.back()=='\t'||raw.back()=='\r')) raw.pop_back();
    if(raw.empty()) return false;
    value=raw;
    return true;
}

bool jsonGetInt(const std::string& json, const std::string& key, int& value) {
    std::string sk="\""+key+"\"";
    auto pos=json.find(sk);
    if(pos==std::string::npos) return false;
    pos=json.find(':',pos); if(pos==std::string::npos) return false;
    pos++; while(pos<json.size()&&(json[pos]==' '||json[pos]=='\t')) pos++;
    value=atoi(json.c_str()+pos);
    return true;
}

} /* end anonymous namespace */

/* ========== HTTP客户端实现 ========== */

namespace {

struct URLInfo {
    std::string protocol, host, path;
    int port;
};

using HttpClock = std::chrono::steady_clock;

int remainingMilliseconds(const HttpClock::time_point& deadline) noexcept {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - HttpClock::now());
    if (remaining.count() <= 0) return 0;
    return static_cast<int>(std::min<long long>(
        remaining.count(), std::numeric_limits<int>::max()));
}

bool parseUrl(const std::string& url, URLInfo& info) {
    std::size_t authorityStart = 0;
    if (url.compare(0, 8, "https://") == 0) {
        info.protocol = "https";
        info.port = 443;
        authorityStart = 8;
    } else if (url.compare(0, 7, "http://") == 0) {
        info.protocol = "http";
        info.port = 80;
        authorityStart = 7;
    } else {
        return false;
    }

    const std::size_t pathPosition = url.find('/', authorityStart);
    const std::size_t authorityEnd =
        pathPosition == std::string::npos ? url.size() : pathPosition;
    const std::string authority =
        url.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        authority.find('\r') != std::string::npos ||
        authority.find('\n') != std::string::npos) {
        return false;
    }

    std::string portText;
    if (authority.front() == '[') {
        const std::size_t bracket = authority.find(']');
        if (bracket == std::string::npos) return false;
        info.host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':') return false;
            portText = authority.substr(bracket + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            if (authority.find(':') != colon) return false;
            info.host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        } else {
            info.host = authority;
        }
    }
    if (info.host.empty()) return false;
    if (!portText.empty()) {
        if (!std::all_of(portText.begin(), portText.end(), [](char value) {
                return value >= '0' && value <= '9';
            })) {
            return false;
        }
        const long parsedPort = std::strtol(portText.c_str(), nullptr, 10);
        if (parsedPort <= 0 || parsedPort > 65535) return false;
        info.port = static_cast<int>(parsedPort);
    } else if (authority.back() == ':') {
        return false;
    }

    info.path = pathPosition == std::string::npos
        ? "/"
        : url.substr(pathPosition);
    return info.path.find('\r') == std::string::npos &&
        info.path.find('\n') == std::string::npos;
}

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
int lastSocketError() noexcept { return WSAGetLastError(); }
bool socketInterrupted(int error) noexcept { return error == WSAEINTR; }
bool socketWouldBlock(int error) noexcept {
    return error == WSAEWOULDBLOCK;
}
bool connectInProgress(int error) noexcept {
    return error == WSAEINPROGRESS || error == WSAEWOULDBLOCK ||
        error == WSAEINVAL;
}
void closeSocket(NativeSocket value) noexcept {
    if (value != kInvalidSocket) closesocket(value);
}
void shutdownSocket(NativeSocket value) noexcept {
    if (value != kInvalidSocket) shutdown(value, SD_BOTH);
}
bool setSocketBlocking(NativeSocket value, bool blocking) noexcept {
    u_long mode = blocking ? 0UL : 1UL;
    return ioctlsocket(value, FIONBIO, &mode) == 0;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
int lastSocketError() noexcept { return errno; }
bool socketInterrupted(int error) noexcept { return error == EINTR; }
bool socketWouldBlock(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}
bool connectInProgress(int error) noexcept {
    return error == EINPROGRESS || error == EWOULDBLOCK;
}
void closeSocket(NativeSocket value) noexcept {
    if (value != kInvalidSocket) close(value);
}
void shutdownSocket(NativeSocket value) noexcept {
    if (value != kInvalidSocket) shutdown(value, SHUT_RDWR);
}
bool setSocketBlocking(NativeSocket value, bool blocking) noexcept {
    const int flags = fcntl(value, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(value, F_SETFL,
                 blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
}
#endif

std::string socketFailure(const char* operation, int error) {
    return std::string(operation) + " (socket error " +
        std::to_string(error) + ")";
}

int waitForSocketIo(NativeSocket value, bool writable,
                    int timeoutMilliseconds, std::string& error) {
    fd_set descriptorSet;
    FD_ZERO(&descriptorSet);
    FD_SET(value, &descriptorSet);
    timeval timeout{timeoutMilliseconds / 1000,
                    (timeoutMilliseconds % 1000) * 1000};
#ifdef _WIN32
    const int selected = writable
        ? select(0, nullptr, &descriptorSet, nullptr, &timeout)
        : select(0, &descriptorSet, nullptr, nullptr, &timeout);
#else
    const int selected = writable
        ? select(value + 1, nullptr, &descriptorSet, nullptr, &timeout)
        : select(value + 1, &descriptorSet, nullptr, nullptr, &timeout);
#endif
    if (selected >= 0) return selected;
    const int socketError = lastSocketError();
    if (socketInterrupted(socketError)) return 0;
    error = socketFailure("HTTP socket wait failed", socketError);
    return -1;
}

bool waitForConnect(NativeSocket value, int timeoutMilliseconds,
                    std::string& error) {
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(value, &writeSet);
    FD_SET(value, &errorSet);
    timeval timeout{timeoutMilliseconds / 1000,
                    (timeoutMilliseconds % 1000) * 1000};
#ifdef _WIN32
    const int selected = select(0, nullptr, &writeSet, &errorSet, &timeout);
#else
    const int selected = select(value + 1, nullptr, &writeSet, &errorSet,
                                &timeout);
#endif
    if (selected == 0) {
        error = "HTTP connect timed out";
        return false;
    }
    if (selected < 0) {
        error = socketFailure("HTTP connect wait failed", lastSocketError());
        return false;
    }
    int connectError = 0;
#ifdef _WIN32
    int length = sizeof(connectError);
    const int socketResult = getsockopt(
        value, SOL_SOCKET, SO_ERROR,
        reinterpret_cast<char*>(&connectError), &length);
#else
    socklen_t length = sizeof(connectError);
    const int socketResult =
        getsockopt(value, SOL_SOCKET, SO_ERROR, &connectError, &length);
#endif
    if (socketResult != 0) {
        error = socketFailure("HTTP connect status failed", lastSocketError());
        return false;
    }
    if (connectError != 0) {
        error = socketFailure("HTTP connect failed", connectError);
        return false;
    }
    return true;
}

class SocketHttpTransport final : public T3HttpTransport {
public:
    explicit SocketHttpTransport(T3HttpTransportOptions options)
        : options_(options) {}

    T3HttpTransportResult post(const std::string& url,
                               const std::string& contentType,
                               const std::string& body) override {
        std::lock_guard<std::mutex> requestLock(requestMutex_);
        if (!options_.isValid()) {
            return {false, {}, "HTTP transport options are invalid"};
        }
        const HttpClock::time_point requestDeadline =
            HttpClock::now() + std::chrono::milliseconds(
                options_.requestTimeoutMilliseconds);
        if (cancelled_.load(std::memory_order_acquire)) {
            return {false, {}, "HTTP request cancelled"};
        }
        URLInfo info;
        if (!parseUrl(url, info)) {
            return {false, {}, "HTTP request URL is invalid"};
        }
        if (info.protocol == "https") {
            return {false, {},
                    "HTTPS transport is unavailable in this build"};
        }
        if (contentType.empty() ||
            contentType.find('\r') != std::string::npos ||
            contentType.find('\n') != std::string::npos) {
            return {false, {}, "HTTP content type is invalid"};
        }

#ifdef _WIN32
        WSADATA winsockData{};
        if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
            return {false, {}, "Winsock initialization failed"};
        }
        struct WinsockCleanup final {
            ~WinsockCleanup() { WSACleanup(); }
        } winsockCleanup;
#endif

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const std::string portText = std::to_string(info.port);
        const int resolveResult = getaddrinfo(
            info.host.c_str(), portText.c_str(), &hints, &addresses);
        if (resolveResult != 0 || addresses == nullptr) {
            return {false, {}, "HTTP host resolution failed"};
        }
        if (remainingMilliseconds(requestDeadline) == 0) {
            freeaddrinfo(addresses);
            return {false, {}, "HTTP request deadline exceeded"};
        }
        struct AddressCleanup final {
            addrinfo* value;
            ~AddressCleanup() { freeaddrinfo(value); }
        } addressCleanup{addresses};

        NativeSocket socketValue = kInvalidSocket;
        std::string connectError = "HTTP connect failed";
        for (addrinfo* address = addresses; address != nullptr;
             address = address->ai_next) {
            if (cancelled_.load(std::memory_order_acquire)) {
                connectError = "HTTP request cancelled";
                break;
            }
            const int deadlineRemaining =
                remainingMilliseconds(requestDeadline);
            if (deadlineRemaining == 0) {
                connectError = "HTTP request deadline exceeded";
                break;
            }
            socketValue = socket(address->ai_family, address->ai_socktype,
                                 address->ai_protocol);
            if (socketValue == kInvalidSocket) {
                connectError = socketFailure(
                    "HTTP socket creation failed", lastSocketError());
                continue;
            }
            if (!activate(socketValue)) {
                closeSocket(socketValue);
                socketValue = kInvalidSocket;
                connectError = "HTTP request cancelled";
                break;
            }
            if (!setSocketBlocking(socketValue, false)) {
                connectError = socketFailure(
                    "HTTP socket mode failed", lastSocketError());
                deactivate(socketValue);
                closeSocket(socketValue);
                socketValue = kInvalidSocket;
                continue;
            }
            const int connected = connect(
                socketValue, address->ai_addr,
                static_cast<int>(address->ai_addrlen));
            if (connected != 0) {
                const int socketError = lastSocketError();
                if (!connectInProgress(socketError) ||
                    !waitForConnect(socketValue,
                                    std::min(
                                        options_.connectTimeoutMilliseconds,
                                        deadlineRemaining),
                                    connectError)) {
                    deactivate(socketValue);
                    closeSocket(socketValue);
                    socketValue = kInvalidSocket;
                    continue;
                }
            }
            break;
        }
        if (socketValue == kInvalidSocket) {
            return {false, {}, std::move(connectError)};
        }
        struct SocketCleanup final {
            SocketHttpTransport* owner;
            NativeSocket value;
            ~SocketCleanup() {
                owner->deactivate(value);
                closeSocket(value);
            }
        } socketCleanup{this, socketValue};

        if (cancelled_.load(std::memory_order_acquire)) {
            return {false, {}, "HTTP request cancelled"};
        }
        if (remainingMilliseconds(requestDeadline) == 0) {
            return {false, {}, "HTTP request deadline exceeded"};
        }

        std::ostringstream request;
        request << "POST " << info.path << " HTTP/1.1\r\n"
                << "Host: " << info.host;
        if (info.port != 80) request << ':' << info.port;
        request << "\r\nContent-Type: " << contentType
                << "\r\nContent-Length: " << body.size()
                << "\r\nConnection: close\r\n\r\n"
                << body;
        const std::string requestBytes = request.str();

        std::size_t sent = 0;
        HttpClock::time_point lastSendProgress = HttpClock::now();
        while (sent < requestBytes.size()) {
            if (cancelled_.load(std::memory_order_acquire)) {
                return {false, {}, "HTTP request cancelled"};
            }
            const HttpClock::time_point now = HttpClock::now();
            const int deadlineRemaining =
                remainingMilliseconds(requestDeadline);
            if (deadlineRemaining == 0) {
                return {false, {}, "HTTP request deadline exceeded"};
            }
            const auto sendIdleMilliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastSendProgress).count();
            if (sendIdleMilliseconds >= options_.sendTimeoutMilliseconds) {
                return {false, {}, "HTTP send timed out"};
            }
            const int chunkSize = static_cast<int>(std::min<std::size_t>(
                requestBytes.size() - sent,
                static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef MSG_NOSIGNAL
            constexpr int sendFlags = MSG_NOSIGNAL;
#else
            constexpr int sendFlags = 0;
#endif
            const int sendResult = send(
                socketValue, requestBytes.data() + sent, chunkSize, sendFlags);
            if (sendResult > 0) {
                sent += static_cast<std::size_t>(sendResult);
                lastSendProgress = HttpClock::now();
                continue;
            }
            const int socketError = lastSocketError();
            if (sendResult < 0 && socketInterrupted(socketError)) continue;
            if (sendResult < 0 && socketWouldBlock(socketError)) {
                const int sendRemaining = options_.sendTimeoutMilliseconds -
                    static_cast<int>(sendIdleMilliseconds);
                std::string waitError;
                const int waitResult = waitForSocketIo(
                    socketValue, true,
                    std::max(1, std::min(
                        25, std::min(deadlineRemaining, sendRemaining))),
                    waitError);
                if (waitResult < 0) {
                    return {false, {}, std::move(waitError)};
                }
                continue;
            }
            return {false, {},
                    cancelled_.load(std::memory_order_acquire)
                        ? "HTTP request cancelled"
                        : socketFailure("HTTP send failed", socketError)};
        }

        std::string response;
        char buffer[4096];
        HttpClock::time_point lastReceiveProgress = HttpClock::now();
        for (;;) {
            if (cancelled_.load(std::memory_order_acquire)) {
                return {false, {}, "HTTP request cancelled"};
            }
            const HttpClock::time_point now = HttpClock::now();
            const int deadlineRemaining =
                remainingMilliseconds(requestDeadline);
            if (deadlineRemaining == 0) {
                return {false, {}, "HTTP request deadline exceeded"};
            }
            const auto receiveIdleMilliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastReceiveProgress).count();
            if (receiveIdleMilliseconds >=
                options_.receiveTimeoutMilliseconds) {
                return {false, {}, "HTTP receive timed out"};
            }
            const int received = recv(
                socketValue, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (cancelled_.load(std::memory_order_acquire)) {
                return {false, {}, "HTTP request cancelled"};
            }
            if (remainingMilliseconds(requestDeadline) == 0) {
                return {false, {}, "HTTP request deadline exceeded"};
            }
            if (received > 0) {
                const std::size_t bytes = static_cast<std::size_t>(received);
                if (bytes > options_.maximumResponseBytes - response.size()) {
                    return {false, {}, "HTTP response exceeds size limit"};
                }
                response.append(buffer, bytes);
                lastReceiveProgress = HttpClock::now();
                continue;
            }
            if (received == 0) break;
            const int socketError = lastSocketError();
            if (socketInterrupted(socketError)) continue;
            if (socketWouldBlock(socketError)) {
                const int receiveRemaining =
                    options_.receiveTimeoutMilliseconds -
                    static_cast<int>(receiveIdleMilliseconds);
                std::string waitError;
                const int waitResult = waitForSocketIo(
                    socketValue, false,
                    std::max(1, std::min(
                        25, std::min(deadlineRemaining, receiveRemaining))),
                    waitError);
                if (waitResult < 0) {
                    return {false, {}, std::move(waitError)};
                }
                continue;
            }
            return {false, {},
                    cancelled_.load(std::memory_order_acquire)
                        ? "HTTP request cancelled"
                        : socketFailure("HTTP receive failed", socketError)};
        }

        const std::size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            return {false, {}, "HTTP response header is incomplete"};
        }
        const std::size_t statusSpace = response.find(' ');
        if (response.compare(0, 5, "HTTP/") != 0 ||
            statusSpace == std::string::npos || statusSpace + 4 > headerEnd) {
            return {false, {}, "HTTP response status is invalid"};
        }
        const std::string statusText = response.substr(statusSpace + 1, 3);
        if (!std::all_of(statusText.begin(), statusText.end(), [](char value) {
                return value >= '0' && value <= '9';
            })) {
            return {false, {}, "HTTP response status is invalid"};
        }
        const int status = std::atoi(statusText.c_str());
        if (status < 200 || status >= 300) {
            return {false, {}, "HTTP server returned status " + statusText};
        }
        return {true, response.substr(headerEnd + 4), {}};
    }

    void cancelPendingRequests() noexcept override {
        cancelled_.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(activeSocketMutex_);
        shutdownSocket(activeSocket_);
    }

    void resetCancellation() noexcept override {
        cancelled_.store(false, std::memory_order_release);
    }

private:
    bool activate(NativeSocket value) noexcept {
        std::lock_guard<std::mutex> lock(activeSocketMutex_);
        if (cancelled_.load(std::memory_order_acquire)) return false;
        activeSocket_ = value;
        return true;
    }

    void deactivate(NativeSocket value) noexcept {
        std::lock_guard<std::mutex> lock(activeSocketMutex_);
        if (activeSocket_ == value) activeSocket_ = kInvalidSocket;
    }

    T3HttpTransportOptions options_;
    std::atomic_bool cancelled_{false};
    std::mutex requestMutex_;
    std::mutex activeSocketMutex_;
    NativeSocket activeSocket_ = kInvalidSocket;
};

#ifdef __ANDROID__

class CurlApi final {
public:
    using GlobalInitFn = CURLcode (*)(long);
    using EasyInitFn = CURL* (*)();
    using EasyCleanupFn = void (*)(CURL*);
    using EasySetoptFn = CURLcode (*)(CURL*, CURLoption, ...);
    using EasyGetinfoFn = CURLcode (*)(CURL*, CURLINFO, ...);
    using EasyStrerrorFn = const char* (*)(CURLcode);
    using SlistAppendFn = curl_slist* (*)(curl_slist*, const char*);
    using SlistFreeAllFn = void (*)(curl_slist*);
    using MultiInitFn = CURLM* (*)();
    using MultiCleanupFn = CURLMcode (*)(CURLM*);
    using MultiAddHandleFn = CURLMcode (*)(CURLM*, CURL*);
    using MultiRemoveHandleFn = CURLMcode (*)(CURLM*, CURL*);
    using MultiPerformFn = CURLMcode (*)(CURLM*, int*);
    using MultiPollFn = CURLMcode (*)(CURLM*, curl_waitfd*, unsigned int,
                                      int, int*);
    using MultiInfoReadFn = CURLMsg* (*)(CURLM*, int*);
    using MultiWakeupFn = CURLMcode (*)(CURLM*);
    using MultiStrerrorFn = const char* (*)(CURLMcode);

    static CurlApi& instance() {
        static CurlApi value;
        return value;
    }

    bool ready() const noexcept { return ready_; }
    const std::string& error() const noexcept { return error_; }

    GlobalInitFn globalInit = nullptr;
    EasyInitFn easyInit = nullptr;
    EasyCleanupFn easyCleanup = nullptr;
    EasySetoptFn easySetopt = nullptr;
    EasyGetinfoFn easyGetinfo = nullptr;
    EasyStrerrorFn easyStrerror = nullptr;
    SlistAppendFn slistAppend = nullptr;
    SlistFreeAllFn slistFreeAll = nullptr;
    MultiInitFn multiInit = nullptr;
    MultiCleanupFn multiCleanup = nullptr;
    MultiAddHandleFn multiAddHandle = nullptr;
    MultiRemoveHandleFn multiRemoveHandle = nullptr;
    MultiPerformFn multiPerform = nullptr;
    MultiPollFn multiPoll = nullptr;
    MultiInfoReadFn multiInfoRead = nullptr;
    MultiWakeupFn multiWakeup = nullptr;
    MultiStrerrorFn multiStrerror = nullptr;

private:
    CurlApi() {
        constexpr std::array<const char*, 2> kLibraryNames = {
            "/system/lib64/libcurl.so", "/system_ext/lib64/libcurl.so"};
        for (const char* libraryName : kLibraryNames) {
            handle_ = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
            if (handle_ != nullptr) break;
        }
        if (handle_ == nullptr) {
            const char* detail = dlerror();
            error_ = detail == nullptr
                ? "Android TLS library load failed"
                : std::string("Android TLS library load failed: ") + detail;
            return;
        }

        const bool loaded =
            load(globalInit, "curl_global_init") &&
            load(easyInit, "curl_easy_init") &&
            load(easyCleanup, "curl_easy_cleanup") &&
            load(easySetopt, "curl_easy_setopt") &&
            load(easyGetinfo, "curl_easy_getinfo") &&
            load(easyStrerror, "curl_easy_strerror") &&
            load(slistAppend, "curl_slist_append") &&
            load(slistFreeAll, "curl_slist_free_all") &&
            load(multiInit, "curl_multi_init") &&
            load(multiCleanup, "curl_multi_cleanup") &&
            load(multiAddHandle, "curl_multi_add_handle") &&
            load(multiRemoveHandle, "curl_multi_remove_handle") &&
            load(multiPerform, "curl_multi_perform") &&
            load(multiPoll, "curl_multi_poll") &&
            load(multiInfoRead, "curl_multi_info_read") &&
            load(multiWakeup, "curl_multi_wakeup") &&
            load(multiStrerror, "curl_multi_strerror");
        if (!loaded) return;

        const CURLcode initialization = globalInit(CURL_GLOBAL_DEFAULT);
        if (initialization != CURLE_OK) {
            error_ = std::string("Android TLS initialization failed: ") +
                easyStrerror(initialization);
            return;
        }
        ready_ = true;
    }

    ~CurlApi() = default;

    template <typename Function>
    bool load(Function& function, const char* name) {
        dlerror();
        function = reinterpret_cast<Function>(dlsym(handle_, name));
        const char* detail = dlerror();
        if (function != nullptr && detail == nullptr) return true;
        error_ = detail == nullptr
            ? std::string("Android TLS symbol load failed: ") + name
            : std::string("Android TLS symbol load failed: ") + detail;
        return false;
    }

    void* handle_ = nullptr;
    bool ready_ = false;
    std::string error_;
};

struct CurlTransferState final {
    const std::atomic_bool* cancelled = nullptr;
    const std::atomic<std::uint64_t>* cancellationSequence = nullptr;
    std::uint64_t initialCancellationSequence = 0;
    std::size_t maximumResponseBytes = 0;
    curl_off_t expectedUploadBytes = 0;
    int sendTimeoutMilliseconds = 0;
    int receiveTimeoutMilliseconds = 0;
    int requestTimeoutMilliseconds = 0;
    HttpClock::time_point started = HttpClock::now();
    HttpClock::time_point lastUploadProgress = started;
    HttpClock::time_point lastDownloadProgress = started;
    HttpClock::time_point receiveStarted = started;
    curl_off_t uploadedBytes = 0;
    curl_off_t downloadedBytes = 0;
    bool receivePhaseStarted = false;
    bool responseLimitExceeded = false;
    bool cancelledDuringTransfer = false;
    bool sendTimedOut = false;
    bool receiveTimedOut = false;
    bool requestTimedOut = false;
    std::string response;

    bool isCancelled() const noexcept {
        return cancelled->load(std::memory_order_acquire) ||
            cancellationSequence->load(std::memory_order_acquire) !=
                initialCancellationSequence;
    }

    void updateProgress(curl_off_t downloadNow, curl_off_t uploadNow,
                        const HttpClock::time_point& now) noexcept {
        if (uploadNow > uploadedBytes) {
            uploadedBytes = uploadNow;
            lastUploadProgress = now;
        }
        if (downloadNow > downloadedBytes) {
            downloadedBytes = downloadNow;
            lastDownloadProgress = now;
        }
        if (!receivePhaseStarted &&
            (expectedUploadBytes == 0 ||
             uploadNow >= expectedUploadBytes)) {
            receivePhaseStarted = true;
            receiveStarted = now;
            lastDownloadProgress = now;
        }
    }

    bool shouldAbort(const HttpClock::time_point& now) noexcept {
        if (isCancelled()) {
            cancelledDuringTransfer = true;
            return true;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - started).count() >= requestTimeoutMilliseconds) {
            requestTimedOut = true;
            return true;
        }
        if (!receivePhaseStarted) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastUploadProgress).count() >=
                sendTimeoutMilliseconds) {
                sendTimedOut = true;
                return true;
            }
            return false;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastDownloadProgress).count() >=
            receiveTimeoutMilliseconds) {
            receiveTimedOut = true;
            return true;
        }
        return false;
    }
};

std::size_t curlWriteCallback(char* data, std::size_t size,
                              std::size_t count, void* userData) {
    auto* state = static_cast<CurlTransferState*>(userData);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        state->responseLimitExceeded = true;
        return 0;
    }
    const std::size_t bytes = size * count;
    if (bytes > state->maximumResponseBytes - state->response.size()) {
        state->responseLimitExceeded = true;
        return 0;
    }
    state->response.append(data, bytes);
    state->updateProgress(
        state->downloadedBytes + static_cast<curl_off_t>(bytes),
        state->uploadedBytes, HttpClock::now());
    return bytes;
}

int curlProgressCallback(void* userData, curl_off_t, curl_off_t downloadNow,
                         curl_off_t, curl_off_t uploadNow) {
    auto* state = static_cast<CurlTransferState*>(userData);
    const HttpClock::time_point now = HttpClock::now();
    state->updateProgress(downloadNow, uploadNow, now);
    return state->shouldAbort(now) ? 1 : 0;
}

template <typename Value>
bool setCurlOption(CurlApi& api, CURL* easy, CURLoption option, Value value,
                   std::string& error) {
    const CURLcode result = api.easySetopt(easy, option, value);
    if (result == CURLE_OK) return true;
    error = std::string("HTTPS option setup failed: ") +
        api.easyStrerror(result);
    return false;
}

class AndroidCurlHttpTransport final : public T3HttpTransport {
public:
    explicit AndroidCurlHttpTransport(T3HttpTransportOptions options)
        : options_(options) {}

    T3HttpTransportResult post(const std::string& url,
                               const std::string& contentType,
                               const std::string& body) override {
        std::lock_guard<std::mutex> requestLock(requestMutex_);
        if (!options_.isValid()) {
            return {false, {}, "HTTP transport options are invalid"};
        }
        if (cancelled_.load(std::memory_order_acquire)) {
            return {false, {}, "HTTP request cancelled"};
        }
        URLInfo info;
        if (!parseUrl(url, info)) {
            return {false, {}, "HTTP request URL is invalid"};
        }
        if (contentType.empty() ||
            contentType.find('\r') != std::string::npos ||
            contentType.find('\n') != std::string::npos) {
            return {false, {}, "HTTP content type is invalid"};
        }

        CurlApi& api = CurlApi::instance();
        if (!api.ready()) return {false, {}, api.error()};

        CURL* easy = api.easyInit();
        if (easy == nullptr) {
            return {false, {}, "HTTPS request handle creation failed"};
        }
        struct EasyCleanup final {
            CurlApi* api;
            CURL* easy;
            ~EasyCleanup() { api->easyCleanup(easy); }
        } easyCleanup{&api, easy};

        curl_slist* headers = nullptr;
        const std::string contentTypeHeader = "Content-Type: " + contentType;
        headers = api.slistAppend(headers, contentTypeHeader.c_str());
        if (headers == nullptr) {
            return {false, {}, "HTTP request header allocation failed"};
        }
        curl_slist* completedHeaders = api.slistAppend(headers, "Expect:");
        if (completedHeaders == nullptr) {
            api.slistFreeAll(headers);
            return {false, {}, "HTTP request header allocation failed"};
        }
        headers = completedHeaders;
        struct HeaderCleanup final {
            CurlApi* api;
            curl_slist* headers;
            ~HeaderCleanup() { api->slistFreeAll(headers); }
        } headerCleanup{&api, headers};

        CurlTransferState state;
        state.cancelled = &cancelled_;
        state.cancellationSequence = &cancellationSequence_;
        state.initialCancellationSequence =
            cancellationSequence_.load(std::memory_order_acquire);
        state.maximumResponseBytes = options_.maximumResponseBytes;
        state.expectedUploadBytes = static_cast<curl_off_t>(body.size());
        state.sendTimeoutMilliseconds = options_.sendTimeoutMilliseconds;
        state.receiveTimeoutMilliseconds = options_.receiveTimeoutMilliseconds;
        state.requestTimeoutMilliseconds = options_.requestTimeoutMilliseconds;
        state.started = HttpClock::now();
        state.lastUploadProgress = state.started;
        state.lastDownloadProgress = state.started;
        state.receiveStarted = state.started;

        std::array<char, CURL_ERROR_SIZE> curlError{};
        std::string optionError;
        const long allowedProtocols = CURLPROTO_HTTP | CURLPROTO_HTTPS;
        const long tlsVersion = CURL_SSLVERSION_TLSv1_2;
        if (!setCurlOption(api, easy, CURLOPT_URL, url.c_str(), optionError) ||
            !setCurlOption(api, easy, CURLOPT_POST, 1L, optionError) ||
            !setCurlOption(api, easy, CURLOPT_POSTFIELDS, body.data(),
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_POSTFIELDSIZE_LARGE,
                           static_cast<curl_off_t>(body.size()), optionError) ||
            !setCurlOption(api, easy, CURLOPT_HTTPHEADER, headers,
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_WRITEFUNCTION,
                           &curlWriteCallback, optionError) ||
            !setCurlOption(api, easy, CURLOPT_WRITEDATA, &state,
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_XFERINFOFUNCTION,
                           &curlProgressCallback, optionError) ||
            !setCurlOption(api, easy, CURLOPT_XFERINFODATA, &state,
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_NOPROGRESS, 0L,
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_NOSIGNAL, 1L, optionError) ||
            !setCurlOption(api, easy, CURLOPT_CONNECTTIMEOUT_MS,
                           static_cast<long>(
                               options_.connectTimeoutMilliseconds),
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_TIMEOUT_MS,
                           static_cast<long>(
                               options_.requestTimeoutMilliseconds),
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_PROTOCOLS,
                           allowedProtocols, optionError) ||
            !setCurlOption(api, easy, CURLOPT_FOLLOWLOCATION, 0L,
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_PROXY, "", optionError) ||
            !setCurlOption(api, easy, CURLOPT_NOPROXY, "*", optionError) ||
            !setCurlOption(api, easy, CURLOPT_ERRORBUFFER, curlError.data(),
                           optionError) ||
            !setCurlOption(api, easy, CURLOPT_USERAGENT,
                           "Lengjing-T3/1", optionError)) {
            return {false, {}, std::move(optionError)};
        }
        if (info.protocol == "https") {
            if (!setCurlOption(api, easy, CURLOPT_SSL_VERIFYPEER, 1L,
                               optionError) ||
                !setCurlOption(api, easy, CURLOPT_SSL_VERIFYHOST, 2L,
                               optionError) ||
                !setCurlOption(api, easy, CURLOPT_CAPATH,
                               "/system/etc/security/cacerts",
                               optionError) ||
                !setCurlOption(api, easy, CURLOPT_SSLVERSION, tlsVersion,
                               optionError)) {
                return {false, {}, std::move(optionError)};
            }
        }

        CURLM* multi = api.multiInit();
        if (multi == nullptr) {
            return {false, {}, "HTTPS multi handle creation failed"};
        }
        struct MultiCleanup final {
            CurlApi* api;
            CURLM* multi;
            CURL* easy;
            bool added = false;
            ~MultiCleanup() {
                if (added) api->multiRemoveHandle(multi, easy);
                api->multiCleanup(multi);
            }
        } multiCleanup{&api, multi, easy};

        const CURLMcode addResult = api.multiAddHandle(multi, easy);
        if (addResult != CURLM_OK) {
            return {false, {}, std::string("HTTPS transfer setup failed: ") +
                api.multiStrerror(addResult)};
        }
        multiCleanup.added = true;
        if (!activate(multi)) {
            return {false, {}, "HTTP request cancelled"};
        }
        struct ActiveCleanup final {
            AndroidCurlHttpTransport* owner;
            CURLM* multi;
            ~ActiveCleanup() { owner->deactivate(multi); }
        } activeCleanup{this, multi};

        int runningHandles = 0;
        CURLMcode multiResult;
        do {
            multiResult = api.multiPerform(multi, &runningHandles);
        } while (multiResult == CURLM_CALL_MULTI_PERFORM);

        while (multiResult == CURLM_OK && runningHandles > 0) {
            if (state.shouldAbort(HttpClock::now())) break;
            int descriptorCount = 0;
            multiResult = api.multiPoll(multi, nullptr, 0, 25,
                                        &descriptorCount);
            if (multiResult != CURLM_OK) break;
            do {
                multiResult = api.multiPerform(multi, &runningHandles);
            } while (multiResult == CURLM_CALL_MULTI_PERFORM);
        }

        if (state.cancelledDuringTransfer || state.isCancelled()) {
            return {false, {}, "HTTP request cancelled"};
        }
        if (state.requestTimedOut) {
            return {false, {}, "HTTP request deadline exceeded"};
        }
        if (state.sendTimedOut) {
            return {false, {}, "HTTP send timed out"};
        }
        if (state.receiveTimedOut) {
            return {false, {}, "HTTP receive timed out"};
        }
        if (multiResult != CURLM_OK) {
            return {false, {}, std::string("HTTPS transfer failed: ") +
                api.multiStrerror(multiResult)};
        }

        CURLcode transferResult = CURLE_FAILED_INIT;
        bool transferFinished = false;
        int messagesRemaining = 0;
        while (CURLMsg* message =
                   api.multiInfoRead(multi, &messagesRemaining)) {
            if (message->msg == CURLMSG_DONE &&
                message->easy_handle == easy) {
                transferResult = message->data.result;
                transferFinished = true;
                break;
            }
        }
        if (!transferFinished) {
            return {false, {}, "HTTPS transfer result is missing"};
        }
        if (transferResult != CURLE_OK) {
            if (state.responseLimitExceeded) {
                return {false, {}, "HTTP response exceeds size limit"};
            }
            if (transferResult == CURLE_OPERATION_TIMEDOUT) {
                return {false, {}, "HTTP request deadline exceeded"};
            }
            if (transferResult == CURLE_PEER_FAILED_VERIFICATION ||
                transferResult == CURLE_SSL_ISSUER_ERROR ||
                transferResult == CURLE_SSL_CACERT_BADFILE) {
                return {false, {}, "HTTPS certificate verification failed"};
            }
            if (transferResult == CURLE_SSL_CONNECT_ERROR ||
                transferResult == CURLE_SSL_CERTPROBLEM ||
                transferResult == CURLE_SSL_CIPHER) {
                return {false, {}, "HTTPS handshake failed"};
            }
            const char* detail = curlError.front() == '\0'
                ? api.easyStrerror(transferResult)
                : curlError.data();
            return {false, {}, std::string("HTTPS transfer failed: ") +
                detail};
        }

        long status = 0;
        const CURLcode statusResult = api.easyGetinfo(
            easy, CURLINFO_RESPONSE_CODE, &status);
        if (statusResult != CURLE_OK) {
            return {false, {}, std::string("HTTP status read failed: ") +
                api.easyStrerror(statusResult)};
        }
        if (status < 200 || status >= 300) {
            return {false, {}, "HTTP server returned status " +
                std::to_string(status)};
        }
        return {true, std::move(state.response), {}};
    }

    void cancelPendingRequests() noexcept override {
        cancelled_.store(true, std::memory_order_release);
        cancellationSequence_.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(activeMultiMutex_);
        if (activeMulti_ != nullptr) {
            CurlApi& api = CurlApi::instance();
            if (api.ready()) api.multiWakeup(activeMulti_);
        }
    }

    void resetCancellation() noexcept override {
        cancelled_.store(false, std::memory_order_release);
    }

private:
    bool activate(CURLM* multi) noexcept {
        std::lock_guard<std::mutex> lock(activeMultiMutex_);
        if (cancelled_.load(std::memory_order_acquire)) return false;
        activeMulti_ = multi;
        return true;
    }

    void deactivate(CURLM* multi) noexcept {
        std::lock_guard<std::mutex> lock(activeMultiMutex_);
        if (activeMulti_ == multi) activeMulti_ = nullptr;
    }

    T3HttpTransportOptions options_;
    std::atomic_bool cancelled_{false};
    std::atomic<std::uint64_t> cancellationSequence_{0};
    std::mutex requestMutex_;
    std::mutex activeMultiMutex_;
    CURLM* activeMulti_ = nullptr;
};

#endif

} /* end anonymous namespace */

bool T3HttpTransportOptions::isValid() const noexcept {
    constexpr int kMaximumTimeoutMilliseconds = 120000;
    constexpr std::size_t kMaximumResponseLimit = 64U * 1024U * 1024U;
    return connectTimeoutMilliseconds > 0 &&
        connectTimeoutMilliseconds <= kMaximumTimeoutMilliseconds &&
        sendTimeoutMilliseconds > 0 &&
        sendTimeoutMilliseconds <= kMaximumTimeoutMilliseconds &&
        receiveTimeoutMilliseconds > 0 &&
        receiveTimeoutMilliseconds <= kMaximumTimeoutMilliseconds &&
        maximumResponseBytes > 0 &&
        maximumResponseBytes <= kMaximumResponseLimit &&
        requestTimeoutMilliseconds > 0 &&
        requestTimeoutMilliseconds <= kMaximumTimeoutMilliseconds;
}

std::shared_ptr<T3HttpTransport> createT3DefaultHttpTransport(
    const T3HttpTransportOptions& options) {
#ifdef __ANDROID__
    return std::make_shared<AndroidCurlHttpTransport>(options);
#else
    return std::make_shared<SocketHttpTransport>(options);
#endif
}

/* ========== 机器码获取 ========== */

std::string getMachineCode() {
    std::string macStr;

    #ifdef _WIN32
    IP_ADAPTER_INFO adapterInfo[16];
    DWORD bufLen=sizeof(adapterInfo);
    if(GetAdaptersInfo(adapterInfo,&bufLen)==ERROR_SUCCESS){
        PIP_ADAPTER_INFO adapter=adapterInfo;
        if(adapter){
            char buf[32];
            snprintf(buf,sizeof(buf),"%02X:%02X:%02X:%02X:%02X:%02X",
                adapter->Address[0],adapter->Address[1],adapter->Address[2],
                adapter->Address[3],adapter->Address[4],adapter->Address[5]);
            macStr=buf;
        }
    }
    #elif defined(__APPLE__)
    int mib[6]; size_t len; char *buf; unsigned char *ptr;
    struct if_msghdr *ifm; struct sockaddr_dl *sdl;
    mib[0]=CTL_NET; mib[1]=AF_ROUTE; mib[2]=0; mib[3]=AF_LINK; mib[4]=NET_RT_IFLIST;
    if((mib[5]=if_nametoindex("en0"))!=0){
        if(sysctl(mib,6,NULL,&len,NULL,0)==0){
            buf=(char*)malloc(len);
            if(buf&&sysctl(mib,6,buf,&len,NULL,0)==0){
                ifm=(struct if_msghdr*)buf;
                sdl=(struct sockaddr_dl*)(ifm+1);
                ptr=(unsigned char*)LLADDR(sdl);
                char tmp[32];
                snprintf(tmp,sizeof(tmp),"%02X:%02X:%02X:%02X:%02X:%02X",
                    ptr[0],ptr[1],ptr[2],ptr[3],ptr[4],ptr[5]);
                macStr=tmp;
            }
            free(buf);
        }
    }
    #else
    struct ifreq ifr; int sock=socket(AF_INET,SOCK_DGRAM,0);
    if(sock>=0){
        const char* ifaces[]={"eth0","ens33","enp0s3",NULL};
        for(int i=0;ifaces[i];i++){
            strncpy(ifr.ifr_name,ifaces[i],IFNAMSIZ-1);
            if(ioctl(sock,SIOCGIFHWADDR,&ifr)==0){
                unsigned char *mac=(unsigned char*)ifr.ifr_hwaddr.sa_data;
                char tmp[32];
                snprintf(tmp,sizeof(tmp),"%02X:%02X:%02X:%02X:%02X:%02X",
                    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
                macStr=tmp;
                break;
            }
        }
        close(sock);
    }
    #endif

    if(macStr.empty()) macStr="00:00:00:00:00:00";
    return md5String(macStr, true);
}

/* ========== T3Verify 实现 ========== */

T3Verify::T3Verify()
    : serverUrl_(T3_SERVER_URL), encodeType_(0), initialized_(false),
      encoder_(nullptr), rsaCrypto_(nullptr),
      httpTransport_(createT3DefaultHttpTransport()) {}

T3Verify::~T3Verify() {
    cancelPendingRequests();
    delete encoder_;
    delete rsaCrypto_;
}

void T3Verify::setHttpTransport(
    std::shared_ptr<T3HttpTransport> transport) {
    httpTransport_ = transport != nullptr
        ? std::move(transport)
        : createT3DefaultHttpTransport();
    lastTransportError_.clear();
}

void T3Verify::cancelPendingRequests() noexcept {
    if (httpTransport_ != nullptr) httpTransport_->cancelPendingRequests();
}

void T3Verify::resetPendingCancellation() noexcept {
    if (httpTransport_ != nullptr) httpTransport_->resetCancellation();
}

bool T3Verify::init(const std::string& loginCode, const std::string& noticeCode,
                    const std::string& versionCode, const std::string& heartbeatCode,
                    const std::string& appkey, const std::string& base64Charset) {
    if(base64Charset.size()!=64) return false;
    loginCode_=loginCode; noticeCode_=noticeCode;
    versionCode_=versionCode; heartbeatCode_=heartbeatCode;
    appkey_=appkey; encodeType_=0;
    try {
        CustomBase64* replacement = new CustomBase64(base64Charset);
        delete encoder_;
        encoder_ = replacement;
    } catch(...) { return false; }
    initialized_=true;
    return true;
}

bool T3Verify::initRSA(const std::string& loginCode, const std::string& noticeCode,
                       const std::string& versionCode, const std::string& heartbeatCode,
                       const std::string& appkey, const std::string& rsaPublicKey) {
    loginCode_=loginCode; noticeCode_=noticeCode;
    versionCode_=versionCode; heartbeatCode_=heartbeatCode;
    appkey_=appkey; encodeType_=1;
    try {
        RSACrypto* replacement = new RSACrypto(rsaPublicKey);
        delete rsaCrypto_;
        rsaCrypto_ = replacement;
    } catch(...) { return false; }
    initialized_=true;
    return true;
}

void T3Verify::checkInit() const {
    if(!initialized_) throw std::runtime_error("未初始化，请先调用 init() 或 initRSA()");
}

std::string T3Verify::buildUrl(const std::string& code) const {
    if(!serverUrl_.empty()&&serverUrl_.back()=='/') return serverUrl_+code;
    return serverUrl_+"/"+code;
}

std::string T3Verify::encodeValue(const std::string& value) const {
    if(encodeType_==0) return encoder_->encodeToHex(value);
    else return rsaCrypto_->encryptToHex(value);
}

std::string T3Verify::decodeResponse(const std::string& responseText) const {
    /* 清理响应 */
    std::string data=responseText;
    auto nl=data.find('\n');
    if(nl!=std::string::npos) data=data.substr(nl+1);
    /* 去除\r\n */
    std::string clean;
    for(char c:data) if(c!='\r'&&c!='\n') clean+=c;
    /* 去除末尾的0 */
    if(!clean.empty()&&clean.back()=='0') clean.pop_back();

    if(encodeType_==0) return encoder_->decode(clean);
    else return rsaCrypto_->decryptFromBase64(clean);
}

std::pair<std::vector<std::pair<std::string,std::string>>,std::string>
T3Verify::encodeParams(const std::vector<std::pair<std::string,std::string>>& params) const {
    /* 1. 每个值只编码一次，保持原始顺序 */
    std::vector<std::pair<std::string,std::string>> encoded;
    for(auto& p:params) encoded.push_back({p.first, encodeValue(p.second)});

    /* 2. 用已编码的值构建签名字符串 */
    std::string sStr;
    for(size_t i=0;i<encoded.size();i++){
        if(i>0) sStr+="&";
        sStr+=encoded[i].first+"="+encoded[i].second;
    }
    sStr+="&"+appkey_;

    /* 3. MD5签名再编码，追加到参数末尾 */
    std::string sValue=md5String(sStr);
    encoded.push_back({"s", encodeValue(sValue)});

    return {encoded, sStr};
}

std::string T3Verify::httpPost(const std::string& url,
                               const std::vector<std::pair<std::string,std::string>>& data) const {
    std::string postData;
    for(auto& p:data){
        if(!postData.empty()) postData+="&";
        postData+=p.first+"="+p.second;
    }
    if (httpTransport_ == nullptr) {
        lastTransportError_ = "HTTP transport is not configured";
        return {};
    }
    T3HttpTransportResult result = httpTransport_->post(
        url, "application/x-www-form-urlencoded", postData);
    if (!result.success) {
        lastTransportError_ = result.error.empty()
            ? "HTTP transport failed"
            : std::move(result.error);
        return {};
    }
    lastTransportError_.clear();
    return std::move(result.body);
}

std::string T3Verify::transportError() const {
    return lastTransportError_.empty()
        ? std::string("HTTP request failed")
        : lastTransportError_;
}

T3LoginResult T3Verify::login(const std::string& kami, const std::string& imei) {
    T3LoginResult result;
    try {
        checkInit();
        std::string url=buildUrl(loginCode_);
        time_t now=time(NULL);
        std::string tStr=std::to_string((long)now);

        std::vector<std::pair<std::string,std::string>> params={
            {"kami",kami},{"imei",imei},{"t",tStr}
        };
        auto [encoded, sOriginal]=encodeParams(params);

        std::string response=httpPost(url,encoded);
        if(response.empty()){result.error=transportError();return result;}

        std::string decoded;
        try { decoded=decodeResponse(response); }
        catch(...){result.error="响应解码失败";return result;}

        int code=0;
        if(!jsonGetInt(decoded,"code",code)){result.error="响应不是有效的JSON格式";return result;}
        if(code!=200){
            std::string msg; jsonGetString(decoded,"msg",msg);
            result.error=msg.empty()?"未知错误":msg; return result;
        }

        std::string kamiId,endTime,token,statecode;
        int responseTime=0;
        if(!jsonGetString(decoded,"id",kamiId)||!jsonGetString(decoded,"end_time",endTime)||
           !jsonGetString(decoded,"token",token)||!jsonGetString(decoded,"statecode",statecode)||
           !jsonGetInt(decoded,"time",responseTime)){
            result.error="响应数据缺少必要字段"; return result;
        }

        int timeDiff=abs((int)now-responseTime);
        if(timeDiff>5){
            result.error="时间戳校验失败，相差"+std::to_string(timeDiff)+"秒"; return result;
        }

        /* 生成预期token */
        struct tm *tmInfo=localtime(&now);
        char dateStr[16];
        strftime(dateStr,sizeof(dateStr),"%Y%m%d%H%M",tmInfo);
        std::string tokenSrc=kamiId+appkey_+sOriginal+endTime+dateStr;
        std::string expectedToken=md5String(tokenSrc);

        /* token比较(忽略大小写) */
        std::string tLower=token, eLower=expectedToken;
        std::transform(tLower.begin(),tLower.end(),tLower.begin(),::tolower);
        std::transform(eLower.begin(),eLower.end(),eLower.begin(),::tolower);
        if(tLower!=eLower){result.error="token校验失败";return result;}

        statecode_=statecode; endTime_=endTime;
        result.success=true; result.id=kamiId;
        result.end_time=endTime; result.statecode=statecode;
        jsonGetString(decoded, "recharge", result.recharge);
        jsonGetString(decoded, "use_time", result.use_time);
        jsonGetString(decoded, "available", result.available);
        jsonGetString(decoded, "imei", result.imei);
        jsonGetString(decoded, "change", result.change);
        jsonGetString(decoded, "core", result.core);
        jsonGetString(decoded, "amount", result.amount);
    } catch(const std::exception& e) {
        result.error=e.what();
    }
    return result;
}




T3Result T3Verify::heartbeat(const std::string& kami, const std::string& statecode) {
    return simpleRequest(heartbeatCode_, "单码心跳", {{"kami",kami},{"statecode",statecode}});
}

/* ========== 设置新增调用码 ========== */

void T3Verify::setCode(const std::string& field, const std::string& code) {
    std::map<std::string, std::string*> codeMap = {
        {"query", &queryCode_}, {"register", &registerCode_},
        {"user_login", &userLoginCode_}, {"user_heartbeat", &userHeartbeatCode_},
        {"qq_login", &qqLoginCode_}, {"bind_qq", &bindQQCode_},
        {"change_password", &changePasswordCode_}, {"user_cancel", &userCancelCode_},
        {"recharge", &rechargeCode_}, {"kami_recharge", &kamiRechargeCode_},
        {"unbind", &unbindCode_},
        {"ip_unbind", &ipUnbindCode_}, {"disable", &disableCode_},
        {"check_update", &checkUpdateCode_}, {"get_variable", &getVariableCode_},
        {"modify_variable", &modifyVariableCode_}, {"modify_core", &modifyCoreCode_},
        {"get_kami_core", &getKamiCoreCode_}, {"get_user_core", &getUserCoreCode_},
        {"online_kami", &onlineKamiCode_}, {"online_user", &onlineUserCode_},
        {"cloud_doc", &cloudDocCode_}, {"app_sign", &appSignCode_},
        {"qq_get_variable", &getVariableByQqCode_}, {"qq_heartbeat", &qqHeartbeatCode_},
        {"qq_get_core", &getUserCoreByQqCode_}, {"qq_modify_core", &modifyUserCoreByQqCode_},
        {"qq_unbind", &unbindQqCode_}, {"heartbeat_any", &heartbeatAnyCode_},
    };
    auto it = codeMap.find(field);
    if (it != codeMap.end()) *(it->second) = code;
}

/* ========== 通用简单请求 ========== */

T3Result T3Verify::simpleRequest(const std::string& code, const std::string& codeName,
                                  const std::vector<std::pair<std::string, std::string>>& params) {
    T3Result result;
    try {
        checkInit();
        if (code.empty()) { result.error = "未设置 " + codeName + " 调用码"; return result; }
        auto allParams = params;
        allParams.push_back({"t", std::to_string((long)time(NULL))});
        auto [encoded, _] = encodeParams(allParams);
        std::string response = httpPost(buildUrl(code), encoded);
        if (response.empty()) { result.error = transportError(); return result; }
        std::string decoded;
        try { decoded = decodeResponse(response); } catch (...) { result.error = "响应解码失败"; return result; }
        int c = 0;
        if (!jsonGetInt(decoded, "code", c)) { result.error = "响应不是有效的JSON格式"; return result; }
        if (c != 200) { std::string msg; jsonGetString(decoded, "msg", msg); result.error = msg.empty() ? "未知错误" : msg; return result; }
        std::string msg; jsonGetString(decoded, "msg", msg);
        result.success = true; result.msg = msg;
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

/* ========== 查询卡密 ========== */

T3QueryResult T3Verify::queryKami(const std::string& kami) {
    T3QueryResult result;
    try {
        checkInit();
        if (queryCode_.empty()) { result.error = "未设置查询卡密调用码"; return result; }
        auto [encoded, _] = encodeParams({{"kami", kami}, {"t", std::to_string((long)time(NULL))}});
        std::string response = httpPost(buildUrl(queryCode_), encoded);
        if (response.empty()) { result.error = transportError(); return result; }
        std::string decoded;
        try { decoded = decodeResponse(response); } catch (...) { result.error = "响应解码失败"; return result; }
        int c = 0;
        if (!jsonGetInt(decoded, "code", c)) { result.error = "响应不是有效的JSON格式"; return result; }
        if (c != 200) { std::string msg; jsonGetString(decoded, "msg", msg); result.error = msg.empty() ? "未知错误" : msg; return result; }
        result.success = true;
        jsonGetString(decoded, "state", result.state);
        jsonGetString(decoded, "use", result.use);
        jsonGetString(decoded, "id", result.id);
        jsonGetString(decoded, "use_time", result.use_time);
        jsonGetString(decoded, "end_time", result.end_time);
        jsonGetString(decoded, "line_time", result.line_time);
        jsonGetString(decoded, "line", result.line);
        jsonGetString(decoded, "amount", result.amount);
        jsonGetString(decoded, "available", result.available);
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

/* ========== 数据与内容 ========== */

T3NoticeResult T3Verify::getNotice() {
    T3NoticeResult result;
    auto r = simpleRequest(noticeCode_, "公告", {});
    if (r.success) { result.success = true; result.notice = r.msg; }
    else result.error = r.error;
    return result;
}

T3VersionResult T3Verify::getLatestVersion() {
    T3VersionResult result;
    auto r = simpleRequest(versionCode_, "版本号", {});
    if (r.success) { result.success = true; result.version = r.msg; }
    else result.error = r.error;
    return result;
}

T3UpdateResult T3Verify::checkUpdate(const std::string& ver) {
    T3UpdateResult result;
    try {
        checkInit();
        if (checkUpdateCode_.empty()) { result.error = "未设置检查更新调用码"; return result; }
        auto [encoded, _] = encodeParams({{"ver", ver}, {"t", std::to_string((long)time(NULL))}});
        std::string response = httpPost(buildUrl(checkUpdateCode_), encoded);
        if (response.empty()) { result.error = transportError(); return result; }
        std::string decoded;
        try { decoded = decodeResponse(response); } catch (...) { result.error = "响应解码失败"; return result; }
        int c = 0;
        if (!jsonGetInt(decoded, "code", c)) { result.error = "响应不是有效的JSON格式"; return result; }
        if (c == 200) {
            result.success = true; result.hasUpdate = true;
            jsonGetString(decoded, "ver", result.ver);
            jsonGetString(decoded, "version", result.version);
            jsonGetString(decoded, "uplog", result.uplog);
            jsonGetString(decoded, "upurl", result.upurl);
        } else if (c == 201) {
            result.success = true; result.hasUpdate = false;
            jsonGetString(decoded, "msg", result.msg);
        } else {
            std::string msg; jsonGetString(decoded, "msg", msg);
            result.error = msg.empty() ? "未知错误" : msg;
        }
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

T3CloudDocResult T3Verify::getCloudDoc(const std::string& token) {
    T3CloudDocResult result;
    auto r = simpleRequest(cloudDocCode_, "云文档", {{"token", token}});
    if (r.success) { result.success = true; result.content = r.msg; }
    else result.error = r.error;
    return result;
}

T3AppSignResult T3Verify::appSign(const std::string& autograph) {
    T3AppSignResult result;
    try {
        checkInit();
        if (appSignCode_.empty()) { result.error = "未设置应用签名调用码"; return result; }
        auto [encoded, _] = encodeParams({{"autograph", autograph}, {"t", std::to_string((long)time(NULL))}});
        std::string response = httpPost(buildUrl(appSignCode_), encoded);
        if (response.empty()) { result.error = transportError(); return result; }
        std::string decoded;
        try { decoded = decodeResponse(response); } catch (...) { result.error = "响应解码失败"; return result; }
        int c = 0;
        if (!jsonGetInt(decoded, "code", c)) { result.error = "响应不是有效的JSON格式"; return result; }
        if (c != 200) { std::string msg; jsonGetString(decoded, "msg", msg); result.error = msg.empty() ? "未知错误" : msg; return result; }
        result.success = true;
        jsonGetString(decoded, "msg", result.msg);
        jsonGetString(decoded, "autograph", result.autograph);
        int t = 0; jsonGetInt(decoded, "time", t); result.time = t;
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

/* ========== 用户体系 ========== */

T3Result T3Verify::userRegister(const std::string& user, const std::string& pass, const std::string& email) {
    std::vector<std::pair<std::string, std::string>> params = {{"user", user}, {"pass", pass}};
    if (!email.empty()) params.push_back({"email", email});
    return simpleRequest(registerCode_, "用户注册", params);
}

T3LoginResult T3Verify::userLogin(const std::string& user, const std::string& pass, const std::string& imei) {
    T3LoginResult result;
    try {
        checkInit();
        if (userLoginCode_.empty()) { result.error = "未设置用户登录调用码"; return result; }
        auto [encoded, _] = encodeParams({{"user", user}, {"pass", pass}, {"imei", imei}, {"t", std::to_string((long)time(NULL))}});
        std::string response = httpPost(buildUrl(userLoginCode_), encoded);
        if (response.empty()) { result.error = transportError(); return result; }
        std::string decoded;
        try { decoded = decodeResponse(response); } catch (...) { result.error = "响应解码失败"; return result; }
        int c = 0;
        if (!jsonGetInt(decoded, "code", c)) { result.error = "响应不是有效的JSON格式"; return result; }
        if (c != 200) { std::string msg; jsonGetString(decoded, "msg", msg); result.error = msg.empty() ? "未知错误" : msg; return result; }
        result.success = true;
        jsonGetString(decoded, "id", result.id);
        jsonGetString(decoded, "end_time", result.end_time);
        jsonGetString(decoded, "statecode", result.statecode);
        jsonGetString(decoded, "recharge", result.recharge);
        jsonGetString(decoded, "use_time", result.use_time);
        jsonGetString(decoded, "available", result.available);
        jsonGetString(decoded, "imei", result.imei);
        jsonGetString(decoded, "change", result.change);
        jsonGetString(decoded, "core", result.core);
        statecode_ = result.statecode; endTime_ = result.end_time;
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

T3Result T3Verify::userHeartbeat(const std::string& user, const std::string& pass, const std::string& statecode) {
    return simpleRequest(userHeartbeatCode_, "用户心跳", {{"user", user}, {"pass", pass}, {"statecode", statecode}});
}

T3LoginResult T3Verify::qqLogin(const std::string& openid, const std::string& accessToken) {
    T3LoginResult result;
    try {
        checkInit();
        if (qqLoginCode_.empty()) { result.error = "未设置QQ登录调用码"; return result; }
        auto [encoded, _] = encodeParams({{"openid", openid}, {"access_token", accessToken}, {"t", std::to_string((long)time(NULL))}});
        std::string response = httpPost(buildUrl(qqLoginCode_), encoded);
        if (response.empty()) { result.error = transportError(); return result; }
        std::string decoded;
        try { decoded = decodeResponse(response); } catch (...) { result.error = "响应解码失败"; return result; }
        int c = 0;
        if (!jsonGetInt(decoded, "code", c)) { result.error = "响应不是有效的JSON格式"; return result; }
        if (c != 200) { std::string msg; jsonGetString(decoded, "msg", msg); result.error = msg.empty() ? "未知错误" : msg; return result; }
        result.success = true;
        jsonGetString(decoded, "id", result.id);
        jsonGetString(decoded, "end_time", result.end_time);
        jsonGetString(decoded, "statecode", result.statecode);
        jsonGetString(decoded, "recharge", result.recharge);
        jsonGetString(decoded, "use_time", result.use_time);
        jsonGetString(decoded, "available", result.available);
        jsonGetString(decoded, "imei", result.imei);
        jsonGetString(decoded, "change", result.change);
        jsonGetString(decoded, "core", result.core);
        statecode_ = result.statecode; endTime_ = result.end_time;
    } catch (const std::exception& e) { result.error = e.what(); }
    return result;
}

T3Result T3Verify::bindQQ(const std::string& user, const std::string& pass, const std::string& openid, const std::string& accessToken) {
    return simpleRequest(bindQQCode_, "绑定QQ", {{"user", user}, {"pass", pass}, {"openid", openid}, {"access_token", accessToken}});
}

T3Result T3Verify::changePassword(const std::string& user, const std::string& oldpass, const std::string& newpass) {
    return simpleRequest(changePasswordCode_, "修改密码", {{"user", user}, {"oldpass", oldpass}, {"newpass", newpass}});
}

T3Result T3Verify::userCancel(const std::string& user, const std::string& pass) {
    return simpleRequest(userCancelCode_, "用户注销", {{"user", user}, {"pass", pass}});
}

T3Result T3Verify::recharge(const std::string& user, const std::string& card) {
    return simpleRequest(rechargeCode_, "用户充值", {{"user", user}, {"card", card}});
}

T3Result T3Verify::kamiRecharge(const std::string& targetKami, const std::string& sourceKami) {
    return simpleRequest(kamiRechargeCode_, "单码以卡充卡", {{"target_kami", targetKami}, {"source_kami", sourceKami}});
}

/* ========== 设备与安全 ========== */

T3Result T3Verify::unbindKami(const std::string& kami, const std::string& imei) {
    return simpleRequest(unbindCode_, "解绑设备", {{"kami", kami}, {"imei", imei}});
}

T3Result T3Verify::unbindUser(const std::string& user, const std::string& pass, const std::string& imei) {
    return simpleRequest(unbindCode_, "解绑设备", {{"user", user}, {"pass", pass}, {"imei", imei}});
}

T3Result T3Verify::ipUnbindKami(const std::string& kami) {
    return simpleRequest(ipUnbindCode_, "IP解绑", {{"kami", kami}});
}

T3Result T3Verify::ipUnbindUser(const std::string& user, const std::string& pass) {
    return simpleRequest(ipUnbindCode_, "IP解绑", {{"user", user}, {"pass", pass}});
}

T3Result T3Verify::disableKami(const std::string& kami) {
    return simpleRequest(disableCode_, "禁用", {{"kami", kami}});
}

T3Result T3Verify::disableUser(const std::string& user, const std::string& pass) {
    return simpleRequest(disableCode_, "禁用", {{"user", user}, {"pass", pass}});
}

/* ========== 远程变量 ========== */

T3VariableResult T3Verify::getVariableByKami(const std::string& kami, const std::string& valueid, const std::string& valuename) {
    T3VariableResult result;
    auto r = simpleRequest(getVariableCode_, "获取变量", {{"kami", kami}, {"valueid", valueid}, {"valuename", valuename}});
    if (r.success) { result.success = true; result.value = r.msg; }
    else result.error = r.error;
    return result;
}

T3VariableResult T3Verify::getVariableByUser(const std::string& user, const std::string& pass, const std::string& valueid, const std::string& valuename) {
    T3VariableResult result;
    auto r = simpleRequest(getVariableCode_, "获取变量", {{"user", user}, {"pass", pass}, {"valueid", valueid}, {"valuename", valuename}});
    if (r.success) { result.success = true; result.value = r.msg; }
    else result.error = r.error;
    return result;
}

T3Result T3Verify::modifyVariableByKami(const std::string& kami, const std::string& valueid, const std::string& valuecontent) {
    return simpleRequest(modifyVariableCode_, "修改变量", {{"kami", kami}, {"valueid", valueid}, {"valuecontent", valuecontent}});
}

T3Result T3Verify::modifyVariableByUser(const std::string& user, const std::string& pass, const std::string& valueid, const std::string& valuecontent) {
    return simpleRequest(modifyVariableCode_, "修改变量", {{"user", user}, {"pass", pass}, {"valueid", valueid}, {"valuecontent", valuecontent}});
}

/* ========== 核心数据 ========== */

T3Result T3Verify::modifyCoreByKami(const std::string& kami, const std::string& core) {
    return simpleRequest(modifyCoreCode_, "修改核心数据", {{"kami", kami}, {"core", core}});
}

T3Result T3Verify::modifyCoreByUser(const std::string& user, const std::string& pass, const std::string& core) {
    return simpleRequest(modifyCoreCode_, "修改核心数据", {{"user", user}, {"pass", pass}, {"core", core}});
}

T3CoreResult T3Verify::getCoreByKami(const std::string& kami) {
    T3CoreResult result;
    auto r = simpleRequest(getKamiCoreCode_, "获取卡密核心数据", {{"kami", kami}});
    if (r.success) { result.success = true; result.core = r.msg; }
    else result.error = r.error;
    return result;
}

T3CoreResult T3Verify::getCoreByUser(const std::string& user, const std::string& pass) {
    T3CoreResult result;
    auto r = simpleRequest(getUserCoreCode_, "获取用户核心数据", {{"user", user}, {"pass", pass}});
    if (r.success) { result.success = true; result.core = r.msg; }
    else result.error = r.error;
    return result;
}

/* ========== 在线数量 ========== */

T3OnlineResult T3Verify::getOnlineKamiCount() {
    T3OnlineResult result;
    auto r = simpleRequest(onlineKamiCode_, "获取在线卡密数量", {});
    if (r.success) { result.success = true; result.count = atoi(r.msg.c_str()); }
    else result.error = r.error;
    return result;
}

T3OnlineResult T3Verify::getOnlineUserCount() {
    T3OnlineResult result;
    auto r = simpleRequest(onlineUserCode_, "获取在线用户数量", {});
    if (r.success) { result.success = true; result.count = atoi(r.msg.c_str()); }
    else result.error = r.error;
    return result;
}

/* ========== QQ凭证 + 统一心跳 ========== */

T3VariableResult T3Verify::getVariableByQq(const std::string& openid, const std::string& accessToken, const std::string& valueid, const std::string& valuename) {
    T3VariableResult result;
    auto r = simpleRequest(getVariableByQqCode_, "QQ获取变量", {{"openid", openid}, {"access_token", accessToken}, {"valueid", valueid}, {"valuename", valuename}});
    if (r.success) { result.success = true; result.value = r.msg; }
    else result.error = r.error;
    return result;
}

T3Result T3Verify::qqHeartbeat(const std::string& openid, const std::string& accessToken, const std::string& statecode) {
    return simpleRequest(qqHeartbeatCode_, "QQ心跳", {{"openid", openid}, {"access_token", accessToken}, {"statecode", statecode}});
}

T3CoreResult T3Verify::getUserCoreByQq(const std::string& openid, const std::string& accessToken) {
    T3CoreResult result;
    auto r = simpleRequest(getUserCoreByQqCode_, "QQ获取核心数据", {{"openid", openid}, {"access_token", accessToken}});
    if (r.success) { result.success = true; result.core = r.msg; }
    else result.error = r.error;
    return result;
}

T3Result T3Verify::modifyUserCoreByQq(const std::string& openid, const std::string& accessToken, const std::string& core) {
    return simpleRequest(modifyUserCoreByQqCode_, "QQ修改核心数据", {{"openid", openid}, {"access_token", accessToken}, {"core", core}});
}

T3Result T3Verify::unbindQq(const std::string& user, const std::string& pass) {
    return simpleRequest(unbindQqCode_, "解绑QQ", {{"user", user}, {"pass", pass}});
}

T3Result T3Verify::heartbeatAny(const std::string& statecode) {
    return simpleRequest(heartbeatAnyCode_, "统一心跳", {{"statecode", statecode}});
}
