#include "generic_edge_transform.hpp"
#include "generic_spatial_transform.hpp"
#include <sstream>
#include <regex>
#include <algorithm>

namespace arc::dx12::shader {
EdgeTransform protect_input_edges(std::string_view input,const Transform& contract){
    EdgeTransform result;result.ir=std::string(input);
    if(input.find("arc_edge_")!=input.npos||input.find("%arc_coarse_control =")==input.npos)return result;
    std::vector<ResourceContract> sources;
    std::array<std::string,2> coordinates;std::smatch coordinate_match;
    for(unsigned axis=0;axis<2;++axis){const std::regex coordinate("(%[A-Za-z0-9_.$]+) = add i32 %arc_coarse_coord_"+std::string(axis?"y":"x")+", 0");if(!std::regex_search(result.ir,coordinate_match,coordinate))return result;coordinates[axis]=coordinate_match[1];}
    for(const auto& r:contract.resources)if(r.resource_class==0&&r.kind==2&&r.count==1&&r.range_id<31){
        // A float resource read must already exist for this declared range.
        const std::regex handle("(%[A-Za-z0-9_.$]+) = call %dx.types.Handle @dx.op.createHandle\\(i32 57, i8 0, i32 "+std::to_string(r.range_id)+", i32 "+std::to_string(r.shader_register)+", i1 false\\)");
        std::smatch match;if(!std::regex_search(result.ir,match,handle))continue;
        const auto h=match[1].str();const std::regex read("@dx.op.textureLoad.f32\\(i32 66, %dx.types.Handle "+h+", i32 0, i32 "+coordinates[0]+", i32 "+coordinates[1]+", i32 undef");
        if(!std::regex_search(result.ir,read))continue;sources.push_back(r);result.input_mask|=1u<<r.range_id;
    }
    if(sources.empty())return result;
    std::ostringstream declarations;
    if(contract.execution_marker){
        if(input.find("%dx.types.ResRet.i32 = type")==input.npos)declarations<<"%dx.types.ResRet.i32 = type { i32, i32, i32, i32, i32 }\n";
        if(input.find("declare %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(")==input.npos)declarations<<"declare %dx.types.ResRet.i32 @dx.op.bufferLoad.i32(i32, %dx.types.Handle, i32, i32)\n";
        if(input.find("declare i32 @dx.op.atomicBinOp.i32(")==input.npos)declarations<<"declare i32 @dx.op.atomicBinOp.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32)\n";
    }
    if(input.find("declare %dx.types.ResRet.f32 @dx.op.textureLoad.f32(")==input.npos)declarations<<"declare %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32)\n";
    if(input.find("declare float @dx.op.binary.f32(")==input.npos)declarations<<"declare float @dx.op.binary.f32(i32, float, float)\n";
    if(input.find("declare float @dx.op.unary.f32(")==input.npos)declarations<<"declare float @dx.op.unary.f32(i32, float)\n";
    if(input.find("declare i32 @dx.op.groupId.i32(")==input.npos)declarations<<"declare i32 @dx.op.groupId.i32(i32, i32)\n";
    std::ostringstream prelude;
    prelude<<"  %arc_edge_controls = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_coarse_control, i32 1)\n"
        <<"  %arc_edge_mask = extractvalue %dx.types.CBufRet.i32 %arc_edge_controls, 2\n"
        <<"  %arc_edge_threshold_bits = extractvalue %dx.types.CBufRet.i32 %arc_edge_controls, 3\n"
        <<"  %arc_edge_threshold = bitcast i32 %arc_edge_threshold_bits to float\n"
        <<"  %arc_edge_enable_bit = and i32 %arc_edge_mask, -2147483648\n"
        <<"  %arc_edge_enabled = icmp ne i32 %arc_edge_enable_bit, 0\n"
        <<"  %arc_edge_source_bits = and i32 %arc_edge_mask, "<<result.input_mask<<"\n"
        <<"  %arc_edge_has_sources = icmp ne i32 %arc_edge_source_bits, 0\n";
    if(contract.execution_marker){
        prelude<<"  %arc_prepass_controls = call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(i32 59, %dx.types.Handle %arc_coarse_control, i32 6)\n"
            <<"  %arc_prepass_flags = extractvalue %dx.types.CBufRet.i32 %arc_prepass_controls, 2\n"
            <<"  %arc_prepass_bit = and i32 %arc_prepass_flags, 16\n"
            <<"  %arc_prepass_enabled = icmp ne i32 %arc_prepass_bit, 0\n"
            <<"  br i1 %arc_prepass_enabled, label %arc_prepass_check, label %arc_edge_main\narc_prepass_check:\n";
        for(unsigned a=0;a<2;++a){const char axis=a?'y':'x';prelude
            <<"  %arc_prepass_raw_"<<axis<<" = call i32 @dx.op.threadId.i32(i32 93, i32 "<<a<<")\n"
            <<"  %arc_prepass_local_"<<axis<<" = urem i32 %arc_prepass_raw_"<<axis<<", "<<contract.threads[a]*2<<"\n"
            <<"  %arc_prepass_first_"<<axis<<" = icmp eq i32 %arc_prepass_local_"<<axis<<", 0\n";}
        prelude<<"  %arc_prepass_writer = and i1 %arc_prepass_first_x, %arc_prepass_first_y\n"
            <<"  %arc_prepass_scan = and i1 %arc_prepass_writer, %arc_edge_enabled\n"
            <<"  br i1 %arc_prepass_scan, label %arc_edge_scan, label %arc_prepass_exit\narc_prepass_exit:\n  ret void\narc_edge_main:\n"
            <<"  br i1 %arc_edge_enabled, label %arc_edge_cached, label %arc_edge_done\narc_edge_cached:\n";
        const auto cached=spatial_lookup(contract,"arc_edge_cached");prelude<<cached.ir
            <<"  %arc_cached_small = fcmp ole float "<<cached.error<<", %arc_edge_threshold\n"
            <<"  %arc_cached_inputs = and i1 "<<cached.valid<<", %arc_edge_has_sources\n"
            <<"  %arc_cached_allow = and i1 %arc_cached_inputs, %arc_cached_small\n"
            <<"  br label %arc_edge_done\narc_edge_scan:\n";
    }else prelude<<"  br i1 %arc_edge_enabled, label %arc_edge_scan, label %arc_edge_done\narc_edge_scan:\n";
    for(unsigned axis=0;axis<2;++axis){const auto name=axis?"y":"x",extent=axis?"height":"width";prelude
        <<"  %arc_edge_group_"<<name<<" = call i32 @dx.op.groupId.i32(i32 94, i32 "<<axis<<")\n"
        <<"  %arc_edge_even_"<<name<<" = and i32 %arc_edge_group_"<<name<<", -2\n"
        <<"  %arc_edge_origin_"<<name<<" = mul i32 %arc_edge_even_"<<name<<", "<<contract.threads[axis]<<"\n"
        <<"  %arc_edge_empty_"<<name<<" = icmp eq i32 %arc_coarse_"<<extent<<", 0\n"
        <<"  %arc_edge_extent_"<<name<<" = select i1 %arc_edge_empty_"<<name<<", i32 1, i32 %arc_coarse_"<<extent<<"\n"
        <<"  %arc_edge_last_"<<name<<" = sub i32 %arc_edge_extent_"<<name<<", 1\n";
        for(unsigned j=0;j<3;++j){const auto offset=j==0?0:j==1?contract.threads[axis]-1:contract.threads[axis]*2-1;prelude
            <<"  %arc_edge_raw_"<<name<<j<<" = add i32 %arc_edge_origin_"<<name<<", "<<offset<<"\n"
            <<"  %arc_edge_in_"<<name<<j<<" = icmp ult i32 %arc_edge_raw_"<<name<<j<<", %arc_edge_extent_"<<name<<"\n"
            <<"  %arc_edge_"<<name<<j<<" = select i1 %arc_edge_in_"<<name<<j<<", i32 %arc_edge_raw_"<<name<<j<<", i32 %arc_edge_last_"<<name<<"\n";
        }
    }
    std::string previous_block="arc_edge_scan",previous_error="0.000000e+00",previous_valid="true",previous_feature="0.000000e+00";
    for(const auto& r:sources){const auto id=std::to_string(r.range_id),body="arc_edge_source"+id,join="arc_edge_join"+id;
        prelude<<"  %arc_edge_bit"<<id<<" = and i32 %arc_edge_mask, "<<(1u<<r.range_id)<<"\n"
            <<"  %arc_edge_read"<<id<<" = icmp ne i32 %arc_edge_bit"<<id<<", 0\n"
            <<"  br i1 %arc_edge_read"<<id<<", label %"<<body<<", label %"<<join<<"\n"<<body<<":\n"
            <<"  %arc_edge_handle"<<id<<" = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 "<<r.range_id<<", i32 "<<r.shader_register<<", i1 false)\n";
        std::array<std::string,4> minimum,maximum;std::string valid=previous_valid;unsigned point=0;
        for(unsigned y=0;y<3;++y)for(unsigned x=0;x<3;++x,++point){const auto key=id+"_"+std::to_string(point);
            prelude<<"  %arc_edge_sample"<<key<<" = call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32 66, %dx.types.Handle %arc_edge_handle"<<id<<", i32 0, i32 %arc_edge_x"<<x<<", i32 %arc_edge_y"<<y<<", i32 undef, i32 undef, i32 undef, i32 undef)\n";
            for(unsigned channel=0;channel<4;++channel){const auto suffix=key+"_"+std::to_string(channel),v="%arc_edge_value"+suffix;
                prelude<<"  "<<v<<" = extractvalue %dx.types.ResRet.f32 %arc_edge_sample"<<key<<", "<<channel<<"\n"
                    <<"  %arc_edge_finite_delta"<<suffix<<" = fsub float "<<v<<", "<<v<<"\n"
                    <<"  %arc_edge_finite"<<suffix<<" = fcmp oeq float %arc_edge_finite_delta"<<suffix<<", 0.000000e+00\n"
                    <<"  %arc_edge_valid"<<suffix<<" = and i1 "<<valid<<", %arc_edge_finite"<<suffix<<"\n";valid="%arc_edge_valid"+suffix;
                if(!point)minimum[channel]=maximum[channel]=v;
                else{prelude<<"  %arc_edge_min"<<suffix<<" = call float @dx.op.binary.f32(i32 36, float "<<minimum[channel]<<", float "<<v<<")\n"
                    <<"  %arc_edge_max"<<suffix<<" = call float @dx.op.binary.f32(i32 35, float "<<maximum[channel]<<", float "<<v<<")\n";minimum[channel]="%arc_edge_min"+suffix;maximum[channel]="%arc_edge_max"+suffix;}
            }
        }
        std::string error=previous_error;
        for(unsigned channel=0;channel<4;++channel){const auto key=id+"_"+std::to_string(channel);prelude
            <<"  %arc_edge_range"<<key<<" = fsub float "<<maximum[channel]<<", "<<minimum[channel]<<"\n"
            <<"  %arc_edge_absmin"<<key<<" = call float @dx.op.unary.f32(i32 6, float "<<minimum[channel]<<")\n"
            <<"  %arc_edge_absmax"<<key<<" = call float @dx.op.unary.f32(i32 6, float "<<maximum[channel]<<")\n"
            <<"  %arc_edge_scale0_"<<key<<" = call float @dx.op.binary.f32(i32 35, float %arc_edge_absmin"<<key<<", float %arc_edge_absmax"<<key<<")\n"
            <<"  %arc_edge_scale"<<key<<" = call float @dx.op.binary.f32(i32 35, float %arc_edge_scale0_"<<key<<", float 0x3F847AE140000000)\n"
            <<"  %arc_edge_ratio"<<key<<" = fdiv float %arc_edge_range"<<key<<", %arc_edge_scale"<<key<<"\n"
            <<"  %arc_edge_error"<<key<<" = call float @dx.op.binary.f32(i32 35, float "<<error<<", float %arc_edge_ratio"<<key<<")\n";error="%arc_edge_error"+key;
        }
        prelude<<"  %arc_edge_feature"<<id<<" = fadd float "<<previous_feature<<", %arc_edge_value"<<id<<"_4_0\n";
        prelude<<"  br label %"<<join<<"\n"<<join<<":\n"
            <<"  %arc_edge_accum"<<id<<" = phi float [ "<<previous_error<<", %"<<previous_block<<" ], [ "<<error<<", %"<<body<<" ]\n"
            <<"  %arc_edge_feature_sum"<<id<<" = phi float [ "<<previous_feature<<", %"<<previous_block<<" ], [ %arc_edge_feature"<<id<<", %"<<body<<" ]\n"
            <<"  %arc_edge_ok"<<id<<" = phi i1 [ "<<previous_valid<<", %"<<previous_block<<" ], [ "<<valid<<", %"<<body<<" ]\n";
        previous_error="%arc_edge_accum"+id;previous_valid="%arc_edge_ok"+id;previous_block=join;previous_feature="%arc_edge_feature_sum"+id;
    }
    const auto spatial=spatial_importance(contract,previous_error,previous_valid,previous_feature,previous_block);
    prelude<<spatial.ir;previous_error=spatial.error;previous_valid=spatial.valid;previous_block=spatial.block;
    if(contract.execution_marker)prelude<<"  ret void\narc_edge_done:\n  %arc_edge_allow = phi i1 [ true, %arc_edge_main ], [ %arc_cached_allow, %arc_cached_done ]\n";
    else prelude<<"  %arc_edge_small = fcmp ole float "<<previous_error<<", %arc_edge_threshold\n"
        <<"  %arc_edge_ok_inputs = and i1 "<<previous_valid<<", %arc_edge_has_sources\n"
        <<"  %arc_edge_safe = and i1 %arc_edge_ok_inputs, %arc_edge_small\n"
        <<"  br label %arc_edge_done\narc_edge_done:\n"
        <<"  %arc_edge_allow = phi i1 [ true, %arc_coarse_entry ], [ %arc_edge_safe, %"<<previous_block<<" ]\n";
    std::istringstream source(result.ir);std::ostringstream out;out<<declarations.str();std::string line,pcf_dependents;bool inserted=false;
    const bool spatial_pcf=input.find("%arc_pcf_enabled =")!=input.npos;
    while(std::getline(source,line)){
        // PCF counts and normalization depend on the final spatial permission.
        // Keep them after its definition, in the same dominating block.
        if(spatial_pcf&&(line.find("%arc_pcf_count =")!=line.npos||line.find("%arc_pcf_count_float =")!=line.npos||line.find("%arc_pcf_reciprocal =")!=line.npos)){pcf_dependents+=line+'\n';continue;}
        if(spatial_pcf&&line.find("%arc_pcf_enabled =")!=line.npos)line.replace(line.find("%arc_pcf_enabled"),16,"%arc_pcf_requested");
        if(line.find("%arc_coarse_requested_x =")!=line.npos&&!inserted){out<<prelude.str();if(spatial_pcf)out<<"  %arc_pcf_enabled = and i1 %arc_pcf_requested, %arc_edge_allow\n"<<pcf_dependents;inserted=true;}
        const bool x=line.find("%arc_coarse_enabled_x =")!=line.npos,y=line.find("%arc_coarse_enabled_y =")!=line.npos;
        if(x||y){const std::string axis=x?"x":"y",old="%arc_coarse_enabled_"+axis,new_name="%arc_edge_requested_"+axis;
            line.replace(line.find(old),old.size(),new_name);out<<line<<"\n  "<<old<<" = and i1 "<<new_name<<", %arc_edge_allow\n";
        }else out<<line<<'\n';
    }
    if(inserted)result.ir=out.str();else result.input_mask=0;
    return result;
}
}
