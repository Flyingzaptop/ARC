// Isolated timing of mechanically validated original captured instruction span.
// Input is a saved snapshot; callback production and consumers are excluded.
#include <windows.h>
#include <bcrypt.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Clock=std::chrono::steady_clock;
static double ms(Clock::time_point a,Clock::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
static std::vector<char> read(const wchar_t* path){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("open file");auto n=f.tellg();if(n<0||n>16*1024*1024)throw std::runtime_error("file size");std::vector<char> b(size_t(n),0);f.seekg(0);f.read(b.data(),n);if(!f)throw std::runtime_error("read file");return b;}
static std::string sha(const std::vector<char>& b){BCRYPT_ALG_HANDLE a{};BCRYPT_HASH_HANDLE h{};if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash provider");DWORD n{},got{};BCryptGetProperty(a,BCRYPT_OBJECT_LENGTH,(PUCHAR)&n,4,&got,0);std::vector<UCHAR> object(n);UCHAR digest[32]{};if(BCryptCreateHash(a,&h,object.data(),n,nullptr,0,0)<0)throw std::runtime_error("hash create");BCryptHashData(h,(PUCHAR)b.data(),ULONG(b.size()),0);BCryptFinishHash(h,digest,32,0);BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);static const char* hex="0123456789abcdef";std::string s;for(UCHAR v:digest){s+=hex[v>>4];s+=hex[v&15];}return s;}
int wmain(int argc,wchar_t** argv){try{
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 if(argc!=7){std::cerr<<"validated-thunk gather.bin expected.bin count expected-sha256 csv\n";return 2;}
 auto code=read(argv[1]),gather=read(argv[2]),expected=read(argv[3]);uint32_t count=std::stoul(argv[4]);
 std::wstring wideHash=argv[5];std::string expectedHash;for(wchar_t c:wideHash){if(c>127)throw std::runtime_error("hash encoding");expectedHash.push_back(char(c));}if(expectedHash.size()!=64||code.size()<64||code.size()>512||sha(code)!=expectedHash)throw std::runtime_error("thunk hash mismatch");
 if(gather.size()<24||memcmp(gather.data(),"AIRG",4))throw std::runtime_error("gather magic");uint32_t header[5];memcpy(header,gather.data()+4,20);uint32_t version=header[0],items=header[1],stride=header[2],words=header[3],shared=header[4];if(version!=1||items!=count||count<1||count>65536||stride<28||stride>4096||words<1||words>64||shared>words||expected.size()!=uint64_t(count)*12)throw std::runtime_error("fixture shape");size_t rowsAt=24+size_t(words+shared)*4;if(gather.size()!=rowsAt+uint64_t(count)*stride)throw std::runtime_error("gather size");
 void* memory=VirtualAlloc(nullptr,code.size(),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!memory)throw std::runtime_error("allocate code");memcpy(memory,code.data(),code.size());DWORD old{};if(!VirtualProtect(memory,code.size(),PAGE_EXECUTE_READ,&old)||!FlushInstructionCache(GetCurrentProcess(),memory,code.size()))throw std::runtime_error("protect code");using Fn=void(*)(const void*,void*);auto fn=reinterpret_cast<Fn>(memory);
 std::vector<char> output(expected.size());std::ofstream csv(argv[6]);csv<<std::setprecision(10)<<"iteration,count,original_slice_ms,mismatch_words\n";
 for(int repetition=-1;repetition<100;++repetition){auto t0=Clock::now();for(uint32_t i=0;i<count;++i)fn(gather.data()+rowsAt+size_t(i)*stride,output.data()+size_t(i)*12);auto t1=Clock::now();size_t mismatch=0;if(repetition==-1||repetition==99)for(size_t j=0;j<expected.size();j+=4)mismatch+=memcmp(output.data()+j,expected.data()+j,4)!=0;csv<<repetition<<','<<count<<','<<ms(t0,t1)<<','<<mismatch<<'\n';if(mismatch)throw std::runtime_error("original slice output mismatch");}
 VirtualFree(memory,0,MEM_RELEASE);std::cout<<"original_captured_slice_exact=1 callback_cost_included=0 frame_gain_measured=0\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
