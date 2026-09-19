#include "arc/descriptor_ledger.hpp"
#include <cstdlib>
#include <iostream>
void check(bool ok,const char* reason){if(!ok){std::cerr<<reason<<'\n';std::exit(1);}}
int main(){
    arc::DescriptorLedger ledger;
    check(ledger.register_heap(1,0x100000,32,1000000),"million-slot heap");
    for(unsigned i=0;i<1000000;++i)check(ledger.write(0x100000+std::uint64_t(i)*32,{42,1,0,8}),"duplicate resource descriptor");
    check(ledger.value_count()==1&&ledger.known_count()==1000000,"duplicate values must be interned");
    check(ledger.write(0x100000,{})&&ledger.null_count()==1&&ledger.value_count()==2,"null replacement");
    ledger.forget(0x100000);check(!ledger.read(0x100000)&&ledger.null_count()==0,"unknown differs from null");
    ledger.retire_heap(1);check(ledger.value_count()==0&&ledger.known_count()==0&&ledger.slot_count()==0,"heap retirement releases all values");
    check(ledger.register_heap(2,0x100000,32,4)&&!ledger.read(0x100000),"native address reuse gets unknown contents");
    check(!ledger.register_heap(3,0x100020,32,4),"overlapping heap rejected");
    check(!ledger.write(0x100001,{99,1,0,1})&&!ledger.read(0x100001)&&ledger.orphan_count()==0,"misaligned known-heap address cannot become an orphan");
    check(ledger.write(0x900000,{99,2,0,1}),"late-attach orphan");
    check(ledger.register_heap(4,0x900000,32,4)&&!ledger.read(0x900000),"old orphan cannot silently populate a new heap");
    arc::DescriptorLedger small({8,2,1,2});
    check(small.register_heap(1,32,32,8)&&!small.register_heap(2,1000,32,1),"slot capacity");
    check(small.write(32,{1,1,0,1})&&small.write(64,{2,1,0,1})&&!small.write(96,{3,1,0,1}),"value capacity");
    small.forget(32);check(small.write(96,{3,1,0,1}),"value slots recycled");
    check(small.write(10000,{3,1,0,1})&&!small.write(10032,{3,1,0,1}),"orphan bound");
    for(int i=0;i<10000;++i){ledger.retire_heap(2);check(ledger.register_heap(2,0x100000,32,4),"heap churn");check(ledger.write(0x100000,{std::uint64_t(i+1),1,0,1}),"value churn");}
    check(ledger.value_count()==1,"no intern-table history leak");
    std::cout<<"Descriptor ledger: million-slot deduplication, reuse, null/unknown distinction and bounded churn PASS\n";
}
