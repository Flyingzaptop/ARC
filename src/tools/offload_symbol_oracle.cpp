// Offline symbol oracle only. Never used for production candidate selection.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dia2.h>
#include <cvconst.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

static void require(HRESULT hr,const char* operation) {
    if(FAILED(hr)){std::fprintf(stderr,"%s failed: 0x%08lx\n",operation,(unsigned long)hr);std::exit(2);}
}
int wmain(int argc,wchar_t** argv) {
    if(argc<6){std::fputs("msdia.dll file.pdb {PE-RSDS-GUID} age pattern...\n",stderr);return 1;}
    require(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
    HMODULE dll=LoadLibraryW(argv[1]); if(!dll)return 2;
    using Factory=HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**);
    auto factory=reinterpret_cast<Factory>(GetProcAddress(dll,"DllGetClassObject"));if(!factory)return 2;
    IClassFactory* cf{};require(factory(__uuidof(DiaSource),__uuidof(IClassFactory),reinterpret_cast<void**>(&cf)),"factory");
    IDiaDataSource* source{};require(cf->CreateInstance(nullptr,__uuidof(IDiaDataSource),reinterpret_cast<void**>(&source)),"source");cf->Release();
    GUID guid{};require(CLSIDFromString(argv[3],&guid),"GUID");
    require(source->loadAndValidateDataFromPdb(argv[2],&guid,0,wcstoul(argv[4],nullptr,10)),"PE/PDB identity");
    IDiaSession* session{};require(source->openSession(&session),"session");
    IDiaSymbol* global{};require(session->get_globalScope(&global),"global");
    std::wprintf(L"query\trva\tlength\tname\n");
    for(int query=5;query<argc;++query){
        IDiaEnumSymbols* symbols{};
        require(global->findChildren(SymTagFunction,argv[query],nsfRegularExpression,&symbols),"functions");
        IDiaSymbol* symbol{};ULONG got{};unsigned count=0;
        while(symbols->Next(1,&symbol,&got)==S_OK&&got){
            DWORD rva{};ULONGLONG length{};BSTR name{};
            if(symbol->get_relativeVirtualAddress(&rva)==S_OK&&rva){
                symbol->get_length(&length);symbol->get_name(&name);
                std::wprintf(L"%ls\t0x%lx\t%llu\t%ls\n",argv[query],rva,length,name?name:L"");
                SysFreeString(name);++count;
            }
            symbol->Release();if(count==32)break;
        }
        symbols->Release();
    }
    global->Release();session->Release();source->Release();FreeLibrary(dll);CoUninitialize();
}
