#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <intrin.h>
#include <xmmintrin.h>
static volatile unsigned repeat_count=1;
static DWORD (WINAPI* stop_capture)(void*);
static DWORD WINAPI stopper(void*){Sleep(150);stop_capture(nullptr);return 0;}
__declspec(noinline) unsigned long long mix(unsigned long long v){return (v*17)^0x123456789abcdef0ull;}
__declspec(noinline) unsigned long long operation(unsigned long long* out,const unsigned long long* in){unsigned long long sum=0;for(unsigned pass=0;pass<repeat_count;++pass){sum=0;for(unsigned i=0;i<8;++i){out[i]=mix(in[i])+i;sum+=out[i];}}return sum;}
int wmain(int argc,wchar_t** argv){if(argc<3||argc>5)return 1;std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);DWORD64 image{};auto* fn=RtlLookupFunctionEntry(DWORD64(&operation),&image,nullptr);if(!fn)return 2;
 auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image+reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
 std::ofstream cfg(out/L"request.txt");cfg<<std::hex<<fn->BeginAddress<<' '<<fn->EndAddress-fn->BeginAddress<<' '<<nt->FileHeader.TimeDateStamp<<' '<<nt->OptionalHeader.SizeOfImage<<std::dec<<" 32768 "<<(argc>=4&&wcscmp(argv[3],L"training")==0?5000:100)<<" "<<(argc>=4&&wcscmp(argv[3],L"training")==0?"training":argc>=4?"boundary":"trace")<<"\n";cfg.close();
 std::ofstream code(out/L"expected-code.bin",std::ios::binary);code.write(reinterpret_cast<char*>(image+fn->BeginAddress),fn->EndAddress-fn->BeginAddress);code.close();
 auto dll=LoadLibraryW(argv[2]);if(!dll)return 3;using Init=DWORD(WINAPI*)(void*);auto init=reinterpret_cast<Init>(GetProcAddress(dll,"ArcInitialize"));auto path=(out/L"session.json").wstring();if(!init||init(path.data()))return 4;
 if(argc==5&&wcscmp(argv[4],L"stop")==0){repeat_count=5000;stop_capture=reinterpret_cast<decltype(stop_capture)>(GetProcAddress(dll,"ArcStopOptimizer"));auto h=CreateThread(nullptr,0,stopper,nullptr,0,nullptr);if(!h||!stop_capture)return 7;CloseHandle(h);}
 unsigned long long input[]={1,2,3,4,5,6,7,8},values[8]{},expected=0;for(unsigned i=0;i<8;++i)expected+=mix(input[i])+i;unsigned original_csr=_mm_getcsr();bool unmasked=argc==5&&wcscmp(argv[4],L"unmasked")==0;unsigned csr=unmasked?((original_csr&~0x3fu)&~0x1000u):original_csr;Sleep(100);
 for(int k=0;k<300;++k){_mm_setcsr(csr);SetLastError(0x12345678);if(operation(values,input)!=expected||_mm_getcsr()!=csr||GetLastError()!=0x12345678)return 5;for(unsigned i=0;i<8;++i)if(values[i]!=mix(input[i])+i)return 6;_mm_setcsr(original_csr);Sleep(5);}
 std::ofstream result(out/L"fixture.json");result<<"{\"test_only_explicit_function\":true,\"calls_checked\":300,\"outputs_match\":true,\"mxcsr_preserved\":true,\"last_error_preserved\":true}";return 0;
}
