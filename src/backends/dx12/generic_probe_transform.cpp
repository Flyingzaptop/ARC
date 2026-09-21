#include "generic_probe_transform.hpp"
#include <sstream>
#include <regex>
#include <map>
#include <algorithm>
#include <stdexcept>
namespace arc::dx12::shader {
namespace {
std::string trim(std::string s){const auto a=s.find_first_not_of(" \t\r\n");return a==s.npos?std::string{}:s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
std::vector<std::string> split(const std::string& s){std::vector<std::string> out;std::string v;unsigned depth=0;bool quoted=false,escape=false;for(char c:s){if(quoted){v+=c;if(escape)escape=false;else if(c=='\\')escape=true;else if(c=='"')quoted=false;continue;}if(c=='"')quoted=true;if(c=='['||c=='{'||c=='('||c=='<')++depth;if(c==']'||c=='}'||c==')'||c=='>'){if(!depth)throw std::runtime_error("fields");--depth;}if(c==','&&!depth){out.push_back(trim(v));v.clear();}else v+=c;}out.push_back(trim(v));return out;}
unsigned metadata_ref(const std::string& s){if(!s.starts_with('!'))throw std::runtime_error("metadata");return std::stoul(s.substr(1));}
std::string node(unsigned id,const std::vector<std::string>& v){std::string s="!"+std::to_string(id)+" = !{";for(unsigned i=0;i<v.size();++i){if(i)s+=", ";s+=v[i];}return s+"}";}
}
ProbeTransform sparse_probe(const Transform& c){
    ProbeTransform out;auto decline=[&](const char* reason){out.reason=reason;return out;};
    if(!c.admitted||!c.execution_marker||c.ir.empty()||c.ir.size()>8*1024*1024)return decline("probe_contract");
    try{
        std::map<unsigned,unsigned> outputs;
        for(const auto& r:c.resources)if(r.resource_class==1){if(r.kind!=2||r.count!=1)return decline("probe_output_shape");outputs.emplace(r.range_id,static_cast<unsigned>(outputs.size()));}
        if(outputs.empty()||outputs.size()>8)return decline("probe_output_capacity");out.outputs=static_cast<unsigned>(outputs.size());
        std::vector<std::string> lines;std::map<unsigned,std::vector<std::string>> metadata;unsigned maximum=0,resources=UINT32_MAX,control_range=UINT32_MAX;std::string original_entry;
        std::istringstream input(c.ir);std::string line;std::smatch m;
        const std::regex md(R"(^!([0-9]+) = !\{(.*)\}$)"),resource_root(R"(^!dx.resources = !\{!([0-9]+)\}$)"),control(R"(^\s*%arc_coarse_control = call %dx.types.Handle @dx.op.createHandle\(i32 57, i8 2, i32 ([0-9]+), i32 0, i1 false\).*$)");
        const std::regex active(R"(^\s*br i1 %arc_coarse_active, label %([A-Za-z0-9_.$]+), label %arc_coarse_exit(?:, !.*)?$)");
        const std::regex any_metadata(R"(^!([0-9]+) = )");
        while(std::getline(input,line)){const auto t=trim(line);if(std::regex_search(t,m,any_metadata))maximum=std::max(maximum,unsigned(std::stoul(m[1])));if(std::regex_match(t,m,md)){const auto id=unsigned(std::stoul(m[1]));metadata[id]=split(m[2]);}if(std::regex_match(t,m,resource_root))resources=metadata_ref("!"+m[1].str());if(std::regex_match(t,m,control))control_range=std::stoul(m[1]);if(std::regex_match(t,m,active))original_entry=m[1];lines.push_back(line);}
        if(resources==UINT32_MAX||control_range==UINT32_MAX||original_entry.empty()||maximum>999990)return decline("probe_ir_shape");
        const auto scratch_range=c.execution_marker_range+1;auto lists=metadata.at(resources);auto uavs=metadata.at(metadata_ref(lists.at(1)));const unsigned raw_node=maximum+1,list_node=maximum+2;
        uavs.push_back("!"+std::to_string(raw_node));lists[1]="!"+std::to_string(list_node);
        std::map<std::string,unsigned> handles;const std::regex handle(R"(^\s*(%[A-Za-z0-9_.$]+) = call %dx.types.Handle @dx.op.createHandle\(i32 57, i8 1, i32 ([0-9]+), i32 [^,]+, i1 (?:true|false)\).*$)");
        for(const auto& l:lines)if(std::regex_match(l,m,handle)&&outputs.contains(std::stoul(m[2])))handles[m[1]]=outputs.at(std::stoul(m[2]));
        std::ostringstream result;
        if(c.ir.find("declare i32 @dx.op.atomicBinOp.i32(")==c.ir.npos)result<<"declare i32 @dx.op.atomicBinOp.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32)\n";
        unsigned serial=0,rewritten=0;
        // All selected tiles have a compact ordinal; every representative owns
        // its whole replicated footprint, so clearing masks cannot race a peer.
        auto address=[&](std::ostringstream& s,const std::string& x,const std::string& y,unsigned output){const auto id=std::to_string(serial++);s
            <<"  %arc_probe_lx"<<id<<" = urem i32 "<<x<<", %arc_probe_tile_x\n"
            <<"  %arc_probe_ly"<<id<<" = urem i32 "<<y<<", %arc_probe_tile_y\n"
            <<"  %arc_probe_row"<<id<<" = mul i32 %arc_probe_ly"<<id<<", %arc_probe_tile_x\n"
            <<"  %arc_probe_local"<<id<<" = add i32 %arc_probe_row"<<id<<", %arc_probe_lx"<<id<<"\n"
            <<"  %arc_probe_pixel"<<id<<" = add i32 %arc_probe_pixel_base, %arc_probe_local"<<id<<"\n"
            <<"  %arc_probe_outputs"<<id<<" = mul i32 %arc_probe_pixel"<<id<<", "<<out.outputs<<"\n"
            <<"  %arc_probe_output"<<id<<" = add i32 %arc_probe_outputs"<<id<<", "<<output<<"\n"
            <<"  %arc_probe_address"<<id<<" = mul i32 %arc_probe_output"<<id<<", 32\n";return "%arc_probe_address"+id;};
        const std::regex store(R"(^\s*call void @dx.op.textureStore.f32\((.*)\)(?: #[0-9]+)?(?:, !.*)?(?:\s*;.*)?$)");
        for(const auto& l:lines){const auto t=trim(l);const auto instruction=trim(l.substr(0,l.find(';')));
            if(t.starts_with("declare void @dx.op.textureStore.f32("))continue;
            if(t.starts_with("!"+std::to_string(resources)+" = ")){result<<node(resources,lists)<<'\n';continue;}
            if(t=="arc_proof_entry:"){
                result<<"arc_probe_entry:\n  %arc_probe_cb = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 "<<control_range<<", i32 0, i1 false)\n"
                    <<"  %arc_probe_data = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_probe_cb, i32 7)\n"
                    <<"  %arc_probe_epoch_lo = extractvalue %dx.types.CBufRet.i32 %arc_probe_data, 0\n  %arc_probe_epoch_hi = extractvalue %dx.types.CBufRet.i32 %arc_probe_data, 1\n"
                    <<"  %arc_probe_stride = extractvalue %dx.types.CBufRet.i32 %arc_probe_data, 2\n  %arc_probe_phase = extractvalue %dx.types.CBufRet.i32 %arc_probe_data, 3\n"
                    <<"  %arc_probe_epoch = or i32 %arc_probe_epoch_lo, %arc_probe_epoch_hi\n  %arc_probe_enabled = icmp ne i32 %arc_probe_epoch, 0\n"
                    <<"  %arc_probe_stride_min = icmp uge i32 %arc_probe_stride, 16\n  %arc_probe_stride_max = icmp ule i32 %arc_probe_stride, 8192\n"
                    <<"  %arc_probe_phase_ok = icmp ult i32 %arc_probe_phase, %arc_probe_stride\n"
                    <<"  %arc_probe_tiles = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_probe_cb, i32 6)\n"
                    <<"  %arc_probe_tile_x = extractvalue %dx.types.CBufRet.i32 %arc_probe_tiles, 0\n  %arc_probe_tile_y = extractvalue %dx.types.CBufRet.i32 %arc_probe_tiles, 1\n"
                    <<"  %arc_probe_tx = icmp ugt i32 %arc_probe_tile_x, 0\n  %arc_probe_ty = icmp ugt i32 %arc_probe_tile_y, 0\n"
                    <<"  %arc_probe_ok0 = and i1 %arc_probe_enabled, %arc_probe_stride_min\n  %arc_probe_ok1 = and i1 %arc_probe_stride_max, %arc_probe_phase_ok\n"
                    <<"  %arc_probe_ok2 = and i1 %arc_probe_tx, %arc_probe_ty\n  %arc_probe_ok3 = and i1 %arc_probe_ok0, %arc_probe_ok1\n  %arc_probe_ok = and i1 %arc_probe_ok3, %arc_probe_ok2\n"
                    <<"  br i1 %arc_probe_ok, label %arc_probe_validate, label %arc_probe_exit\narc_probe_exit:\n  ret void\narc_probe_validate:\n"
                    <<"  %arc_probe_extent = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_probe_cb, i32 0)\n"
                    <<"  %arc_probe_width = extractvalue %dx.types.CBufRet.i32 %arc_probe_extent, 2\n  %arc_probe_height = extractvalue %dx.types.CBufRet.i32 %arc_probe_extent, 3\n"
                    <<"  %arc_probe_rate_x = extractvalue %dx.types.CBufRet.i32 %arc_probe_extent, 0\n  %arc_probe_rate_y = extractvalue %dx.types.CBufRet.i32 %arc_probe_extent, 1\n"
                    <<"  %arc_probe_rate_x2 = icmp eq i32 %arc_probe_rate_x, 2\n  %arc_probe_rate_x4 = icmp eq i32 %arc_probe_rate_x, 4\n  %arc_probe_rate_xok = or i1 %arc_probe_rate_x2, %arc_probe_rate_x4\n  %arc_probe_rx_safe = select i1 %arc_probe_rate_xok, i32 %arc_probe_rate_x, i32 1\n"
                    <<"  %arc_probe_rate_y2 = icmp eq i32 %arc_probe_rate_y, 2\n  %arc_probe_rate_y4 = icmp eq i32 %arc_probe_rate_y, 4\n  %arc_probe_rate_yok = or i1 %arc_probe_rate_y2, %arc_probe_rate_y4\n  %arc_probe_ry_safe = select i1 %arc_probe_rate_yok, i32 %arc_probe_rate_y, i32 1\n"
                    <<"  %arc_probe_group_x = mul i32 %arc_probe_rx_safe, "<<c.threads[0]<<"\n  %arc_probe_group_y = mul i32 %arc_probe_ry_safe, "<<c.threads[1]<<"\n"
                    <<"  %arc_probe_mod_x = urem i32 %arc_probe_tile_x, %arc_probe_group_x\n  %arc_probe_mod_y = urem i32 %arc_probe_tile_y, %arc_probe_group_y\n  %arc_probe_mod_xy = or i32 %arc_probe_mod_x, %arc_probe_mod_y\n  %arc_probe_aligned = icmp eq i32 %arc_probe_mod_xy, 0\n"
                    <<"  %arc_probe_limit_x = icmp ule i32 %arc_probe_tile_x, 16384\n  %arc_probe_limit_y = icmp ule i32 %arc_probe_tile_y, 16384\n  %arc_probe_limit_xy = and i1 %arc_probe_limit_x, %arc_probe_limit_y\n  %arc_probe_bounded = and i1 %arc_probe_limit_xy, %arc_probe_aligned\n"
                    <<"  br i1 %arc_probe_bounded, label %arc_probe_select, label %arc_probe_exit\narc_probe_select:\n"
                    <<"  %arc_probe_x = call i32 @dx.op.threadId.i32(i32 93, i32 0)\n  %arc_probe_y = call i32 @dx.op.threadId.i32(i32 93, i32 1)\n"
                    <<"  %arc_probe_gx = udiv i32 %arc_probe_x, %arc_probe_tile_x\n  %arc_probe_gy = udiv i32 %arc_probe_y, %arc_probe_tile_y\n"
                    <<"  %arc_probe_columns0 = add i32 %arc_probe_width, %arc_probe_tile_x\n  %arc_probe_columns1 = sub i32 %arc_probe_columns0, 1\n  %arc_probe_columns = udiv i32 %arc_probe_columns1, %arc_probe_tile_x\n"
                    <<"  %arc_probe_tile_row = mul i32 %arc_probe_gy, %arc_probe_columns\n  %arc_probe_tile = add i32 %arc_probe_tile_row, %arc_probe_gx\n"
                    <<"  %arc_probe_mod = urem i32 %arc_probe_tile, %arc_probe_stride\n  %arc_probe_selected = icmp eq i32 %arc_probe_mod, %arc_probe_phase\n"
                    <<"  %arc_probe_slot = udiv i32 %arc_probe_tile, %arc_probe_stride\n  %arc_probe_area = mul i32 %arc_probe_tile_x, %arc_probe_tile_y\n  %arc_probe_pixel_base = mul i32 %arc_probe_slot, %arc_probe_area\n"
                    <<"  %arc_probe_raw = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 1, i32 "<<scratch_range<<", i32 1, i1 false)\n"
                    <<"  br i1 %arc_probe_selected, label %arc_proof_entry, label %arc_probe_exit\n";
            }
            if(std::regex_match(t,m,active)){
                result<<"  br i1 %arc_coarse_active, label %arc_probe_init, label %arc_coarse_exit\narc_probe_init:\n";
                for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto id=std::to_string(serial++);result
                    <<"  %arc_probe_ix"<<id<<" = add i32 %arc_coarse_coord_x, "<<x<<"\n  %arc_probe_iy"<<id<<" = add i32 %arc_coarse_coord_y, "<<y<<"\n"
                    <<"  %arc_probe_rx"<<id<<" = icmp ugt i32 %arc_coarse_rate_x, "<<x<<"\n  %arc_probe_ry"<<id<<" = icmp ugt i32 %arc_coarse_rate_y, "<<y<<"\n"
                    <<"  %arc_probe_bx"<<id<<" = icmp ult i32 %arc_probe_ix"<<id<<", %arc_probe_width\n  %arc_probe_by"<<id<<" = icmp ult i32 %arc_probe_iy"<<id<<", %arc_probe_height\n"
                    <<"  %arc_probe_r"<<id<<" = and i1 %arc_probe_rx"<<id<<", %arc_probe_ry"<<id<<"\n  %arc_probe_b"<<id<<" = and i1 %arc_probe_bx"<<id<<", %arc_probe_by"<<id<<"\n  %arc_probe_write"<<id<<" = and i1 %arc_probe_r"<<id<<", %arc_probe_b"<<id<<"\n"
                    <<"  br i1 %arc_probe_write"<<id<<", label %arc_probe_clear"<<id<<", label %arc_probe_next"<<id<<"\narc_probe_clear"<<id<<":\n";
                    for(unsigned output=0;output<out.outputs;++output){const auto address_id=address(result,"%arc_probe_ix"+id,"%arc_probe_iy"+id,output);const auto a=std::to_string(serial++);result<<"  %arc_probe_epoch_address"<<a<<" = add i32 "<<address_id<<", 16\n  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %arc_probe_raw, i32 %arc_probe_epoch_address"<<a<<", i32 undef, i32 %arc_probe_epoch_lo, i32 %arc_probe_epoch_hi, i32 0, i32 0, i8 15)\n";}
                    result<<"  br label %arc_probe_next"<<id<<"\narc_probe_next"<<id<<":\n";
                }result<<"  br label %"<<original_entry<<"\n";continue;
            }
            if(std::regex_match(instruction,m,store)){const auto args=split(m[1]);if(args.size()!=10||!args[1].starts_with("%dx.types.Handle "))return decline("probe_store_shape");const auto h=args[1].substr(17);if(!handles.contains(h))return decline("probe_store_handle");
                const auto a=address(result,args[2].substr(4),args[3].substr(4),handles.at(h));const auto id=std::to_string(serial++);
                for(unsigned component=0;component<4;++component)result<<"  %arc_probe_bits"<<id<<'_'<<component<<" = bitcast "<<args[component+5]<<" to i32\n";
                result<<"  call void @dx.op.bufferStore.i32(i32 69, %dx.types.Handle %arc_probe_raw, i32 "<<a<<", i32 undef";for(unsigned component=0;component<4;++component)result<<", i32 %arc_probe_bits"<<id<<'_'<<component;result<<", "<<args[9]<<")\n"
                    <<"  %arc_probe_mask_address"<<id<<" = add i32 "<<a<<", 24\n  %arc_probe_mask"<<id<<" = call i32 @dx.op.atomicBinOp.i32(i32 78, %dx.types.Handle %arc_probe_raw, i32 2, i32 %arc_probe_mask_address"<<id<<", i32 undef, i32 undef, i32 "<<args[9].substr(3)<<")\n";++rewritten;continue;
            }
            result<<l<<'\n';
        }
        if(!rewritten)return decline("probe_no_outputs");result<<node(list_node,uavs)<<'\n'<<"!"<<raw_node<<" = !{i32 "<<scratch_range<<", %arc_execution_buffer* undef, !\"\", i32 "<<c.control_space<<", i32 1, i32 1, i32 11, i1 false, i1 false, i1 false, null}\n";
        out.ir=result.str();out.admitted=true;out.reason="sparse_private_raw_outputs";return out;
    }catch(...){return decline("probe_ir_unsupported");}
}
}
