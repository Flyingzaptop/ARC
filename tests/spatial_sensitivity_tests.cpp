#include "arc/spatial_sensitivity.hpp"
#include <cassert>
#include <iostream>
int main(){
    arc::SpatialSensitivity model(9);assert(!model.observe(8,1,0,.01,false));assert(!model.observe(9,0,0,.01,false));
    model.observe(9,1,3,.001,true);for(unsigned i=0;i<5;++i)model.observe(9,2,3,.01,false);assert(model.snapshot().entries[3].samples==1);
    model.observe(9,3,3,.015,false);assert(model.snapshot().entries[3].error==1000);model.observe(9,4,3,.012,false);
    const auto trained=model.snapshot().entries[3];assert(trained.samples==3&&trained.error>=.017f&&trained.error<.017001f);
    for(unsigned i=1;i<=3;++i)model.observe(9,i,5,.001,false);assert(model.snapshot().entries[5].error==1000); // no noise reference
    model.observe(9,5,3,.01,false,false);assert(model.snapshot().entries[3].invalid&&model.snapshot().entries[3].error==1000);
    model.reset(10);assert(model.snapshot().key==10&&model.snapshot().entries[3].samples==0);assert(!model.observe(9,6,3,0,false));
    std::cout<<"Distinct-frame sensitivity, noise bounds, invalid coverage and generation reset PASS\n";
}
