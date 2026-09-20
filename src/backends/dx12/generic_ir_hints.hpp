#pragma once
#include <charconv>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace arc::dx12::shader {
inline std::string preserve_arc_branches(std::string text){
    while(!text.empty()&&text.back()=='\0')text.pop_back();
    const std::regex metadata(R"(^!([0-9]+) = )");unsigned next=0;std::smatch match;
    std::istringstream scan(text);std::string line;
    while(std::getline(scan,line))if(std::regex_search(line,match,metadata)){
        const auto number=match[1].str();unsigned value{};auto [end,e]=std::from_chars(number.data(),number.data()+number.size(),value);
        if(e!=std::errc{}||end!=number.data()+number.size()||value>=1000000)throw std::runtime_error("DXIL metadata capacity");
        if(value>=next)next=value+1;
    }
    std::istringstream source(text);std::ostringstream out;bool changed=false;
    while(std::getline(source,line)){
        if(line.find("br i1 %arc_")!=line.npos&&line.find("!dx.controlflow.hints")==line.npos){
            const auto comment=line.find(';');if(comment!=line.npos)line.resize(comment);
            line+=", !dx.controlflow.hints !"+std::to_string(next);changed=true;
        }
        out<<line<<'\n';
    }
    // Same metadata as HLSL [branch]. Without it a driver may flatten the new
    // branch and execute the work whose result ARC only intended to discard.
    if(changed)out<<'!'<<next<<" = distinct !{!"<<next<<", !\"dx.controlflow.hints\", i32 1}\n";
    return out.str();
}
}
