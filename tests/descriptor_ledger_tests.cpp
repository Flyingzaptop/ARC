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
    arc::DescriptorLedger copied;
    check(copied.register_heap(1,32,32,4)&&copied.register_heap(2,1024,32,4),"copy heaps");
    check(copied.write(32,{7,1,0,4})&&copied.copy_one(1024,32)&&copied.copy_one(1056,1024),"copy interned identity across heaps");
    check(copied.known_count()==3&&copied.value_count()==1&&copied.read(1056)->resource==7,"copy accounting");
    check(copied.copy_one(32,32)&&copied.known_count()==3,"self copy");
    check(copied.write(64,{})&&copied.copy_one(1024,64)&&copied.null_count()==2,"known null copied");
    check(copied.copy_one(1056,96)&&!copied.read(1056)&&copied.known_count()==3,"unknown source invalidates old destination");
    check(!copied.copy_one(33,32),"misaligned copy destination rejected");
    check(copied.copy_one(4096,32)&&copied.read(4096)->resource==7,"copy to orphan");
    copied.retire_heap(1);copied.retire_heap(2);check(copied.known_count()==1&&copied.null_count()==0&&copied.value_count()==1,"copy references survive source retirement");
    copied.forget(4096);check(copied.known_count()==0&&copied.value_count()==0,"copy reference retirement complete");
    arc::DescriptorLedger replacement;
    check(copied.register_heap(11,32,32,4)&&copied.write(32,{21,1,0,1}),"cached old heap");
    check(copied.read(32)->resource==21,"warm lookup hint");
    check(replacement.register_heap(11,8192,16,4)&&replacement.write(8208,{22,1,0,1}),"replacement layout");
    copied=replacement;
    check(!copied.read(32)&&copied.read(8208)->resource==22,"assignment cannot reuse another heap's cached range");
    auto moved=std::move(copied);
    check(moved.read(8208)->resource==22&&!copied.read(8208),"moved-from lookup cannot retain stale heap pointers");
    std::cout<<"Descriptor ledger: million-slot deduplication, reuse, null/unknown distinction and bounded churn PASS\n";
    copied.forget_all();check(copied.known_count()==0&&copied.value_count()==0,"bulk invalidation drops all values");
    arc::DescriptorLedger reset;check(reset.register_heap(99,3200,32,4),"reset heap");reset.write(3200,{7,1,0,1});reset.write(99999,{8,1,0,1});
    reset.forget_all();check(reset.heap_at(3200)==99&&reset.slot_count()==4&&!reset.read(3200)&&!reset.read(99999),"bulk invalidation preserves heaps, not stale descriptors");
    check(reset.write(3200,{9,1,0,1})&&reset.read(3200)->resource==9,"observations recover after invalidation");
}
