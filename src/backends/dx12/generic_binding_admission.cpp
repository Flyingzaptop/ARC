#include "generic_binding_admission.hpp"
#include <algorithm>
#include <limits>

namespace arc::dx12::binding {
namespace {
bool allocation_known(const Allocation& a) {
    return a.id && (a.kind==AllocationKind::Committed ||
        (a.kind==AllocationKind::Placed&&a.heap&&a.bytes&&a.offset<=UINT64_MAX-a.bytes));
}
bool overlaps(const Allocation& a,const Allocation& b) {
    if(a.id==b.id)return true;
    if(a.kind==AllocationKind::Committed||b.kind==AllocationKind::Committed)return false;
    return a.heap==b.heap&&a.offset<b.offset+b.bytes&&b.offset<a.offset+a.bytes;
}
}
void BufferIndex::observe(const Allocation& a){
    retire(a.id);if(a.description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||!a.gpu_address)return;
    addresses_.emplace(a.gpu_address,a.id);bases_[a.id]=a.gpu_address;maximum_width_=std::max(maximum_width_,a.description.Width);
}
void BufferIndex::retire(std::uint64_t id){const auto base=bases_.find(id);if(base==bases_.end())return;auto [first,last]=addresses_.equal_range(base->second);for(auto it=first;it!=last;)if(it->second==id)it=addresses_.erase(it);else ++it;bases_.erase(base);}
const Allocation* BufferIndex::resolve(const std::map<std::uint64_t,Allocation>& allocations,std::uint64_t address,std::uint64_t bytes)const noexcept{
    const Allocation* found=nullptr;const auto end=addresses_.upper_bound(address);
    for(auto it=addresses_.lower_bound(address>maximum_width_?address-maximum_width_:0);it!=end;++it){
        const auto resource=allocations.find(it->second);if(resource==allocations.end())continue;const auto& a=resource->second;
        if(address<a.gpu_address)continue;const auto offset=address-a.gpu_address;
        if(offset>a.description.Width||bytes>a.description.Width-offset)continue;
        if(found)return nullptr;found=&a;
    }
    return found;
}
Admission admit_compute(const Arguments& arguments,const shader::Transform& transform,
    const DescriptorLedger& ledger,const std::vector<DescriptorHeap>& heaps,
    const std::map<std::uint64_t,Allocation>& allocations,UINT x,UINT y,UINT z,const shader::ResourceUsage* usage,const BufferIndex* buffers) {
    Admission out;auto reject=[&](const char* reason){out.reason=reason;return out;};
    if(!transform.admitted||!arguments.layout()||!arguments.layout()->complete)return reject("missing_shader_or_root_contract");
    if(!x||!y||z!=1||!transform.threads[0]||!transform.threads[1])return reject("dispatch_shape");
    unsigned bindings=0;
    for(const auto& contract:transform.resources){
        if(!contract.count)return reject("binding_capacity");
        if(contract.count!=UINT_MAX&&contract.count-1>UINT_MAX-contract.shader_register)return reject("register_overflow");
        std::vector<unsigned> registers;
        if(usage&&usage->complete){
            const auto range=usage->ranges.find({contract.resource_class,contract.range_id});
            if(range==usage->ranges.end())continue;
            if(!range->second.all){
                if(range->second.indices.size()>256)return reject("binding_capacity");
                for(const auto reg:range->second.indices){if(reg<contract.shader_register||(contract.count!=UINT_MAX&&reg-contract.shader_register>=contract.count))return reject("access_proof_out_of_range");registers.push_back(reg);}
            }else if(contract.count==UINT_MAX)return reject("unbounded_access_not_uniform");
            else{if(contract.count>256)return reject("binding_capacity");for(unsigned i=0;i<contract.count;++i)registers.push_back(contract.shader_register+i);}
        }else{
            if(contract.count==UINT_MAX)return reject("unbounded_access_proof_missing");
            if(contract.count>256)return reject("binding_capacity");for(unsigned i=0;i<contract.count;++i)registers.push_back(contract.shader_register+i);
        }
        if(bindings>1024-registers.size())return reject("binding_capacity");bindings+=static_cast<unsigned>(registers.size());
        const auto type=contract.resource_class==0?D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
            contract.resource_class==1?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
            contract.resource_class==2?D3D12_DESCRIPTOR_RANGE_TYPE_CBV:D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        for(const auto reg:registers){
            out.binding_class=contract.resource_class;out.binding_register=reg;out.binding_space=contract.space;
            auto location=arguments.locate(type,reg,contract.space,D3D12_SHADER_VISIBILITY_ALL);
            if(!location)return reject("unbound_register");
            if(location->static_sampler||location->constants)continue;
            const auto arg=arguments.argument(location->parameter);if(!arg)return reject("uninitialized_argument");
            DescriptorValue view;
            if(location->table){
                const DescriptorHeap* found=nullptr;
                for(const auto& h:heaps){
                    if(!h.gpu||!h.stride||arg->address<h.gpu)continue;
                    const auto distance=arg->address-h.gpu;
                    if(distance%h.stride||distance/h.stride>=h.count)continue;
                    if(found)return reject("ambiguous_heap");found=&h;
                }
                if(!found)return reject("unbound_heap");
                const auto expected=contract.resource_class==3?D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                if(found->type!=expected)return reject("heap_type");
                const auto index=(arg->address-found->gpu)/found->stride+location->table_offset;
                if(index>=found->count||index>(UINT64_MAX-found->cpu)/found->stride)return reject("descriptor_bounds");
                const auto value=ledger.read(found->cpu+index*found->stride);if(!value||!value->shape.known)return reject("unknown_descriptor");view=*value;
                const unsigned expected_kind=contract.resource_class==0?1:contract.resource_class==1?2:contract.resource_class==2?6:5;
                if(view.kind!=expected_kind)return reject("view_type");
                if(contract.resource_class==3)continue;
            } else {
                // This first pixel-compute contract admits root CBVs only;
                // texture resources cannot be bound as raw root descriptors.
                const bool acceleration=contract.resource_class==0&&contract.kind==16;
                const bool buffer_srv=contract.resource_class==0&&(contract.kind==11||contract.kind==12);
                if(contract.resource_class!=2&&!acceleration&&!buffer_srv)return reject("unsupported_root_view");
                const auto required_bytes=acceleration||buffer_srv?1u:contract.kind;
                const Allocation* found=nullptr;
                if(buffers)found=buffers->resolve(allocations,arg->address,required_bytes);
                else for(const auto& [id,a]:allocations){(void)id;if(a.description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||!a.gpu_address||arg->address<a.gpu_address)continue;
                    const auto offset=arg->address-a.gpu_address;
                    if(offset>a.description.Width||required_bytes>a.description.Width-offset)continue;
                    if(found)return reject("ambiguous_root_address");found=&a;
                }
                if(!found)return reject("unknown_root_address");
                view.resource=found->id;view.kind=acceleration||buffer_srv?1:6;view.shape.known=true;
                view.shape.byte_offset=arg->address-found->gpu_address;view.shape.byte_size=required_bytes;
                if(acceleration)view.shape.dimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                if(buffer_srv){view.shape.dimension=D3D12_SRV_DIMENSION_BUFFER;view.shape.byte_size=found->description.Width-view.shape.byte_offset;}
            }
            if(contract.resource_class==0&&contract.kind==16&&(!view.resource||view.shape.dimension!=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE))return reject("unknown_acceleration_structure");
            if(!view.resource){if(contract.resource_class==1)return reject("null_output");continue;}
            const auto resource=allocations.find(view.resource);
            if(resource==allocations.end()||!allocation_known(resource->second))return reject("unknown_allocation");
            BoundResource bound{contract,view,resource->second};
            if(contract.resource_class==0&&contract.kind==16&&
               (bound.allocation.description.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||!bound.allocation.gpu_address||
                view.shape.byte_offset>=bound.allocation.description.Width||
                (bound.allocation.gpu_address+view.shape.byte_offset)%D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT))return reject("acceleration_view_bounds");
            if(contract.resource_class!=1){out.inputs.push_back(bound);continue;}
            const auto& d=bound.allocation.description;
            if(view.shape.counter_resource||view.shape.dimension!=D3D12_UAV_DIMENSION_TEXTURE2D||
                d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||d.DepthOrArraySize!=1||
                view.first_mip>=d.MipLevels||view.first_mip>=32||view.shape.plane)
                return reject("output_view_shape");
            const UINT width=static_cast<UINT>(std::max<UINT64>(1,d.Width>>view.first_mip));
            const UINT height=std::max(1u,d.Height>>view.first_mip);
            if(!width||d.Width>16384||!height||height>16384)return reject("output_extent");
            if(out.width&&(out.width!=width||out.height!=height))return reject("mismatched_outputs");
            out.width=width;out.height=height;out.outputs.push_back(bound);
        }
    }
    if(out.outputs.empty())return reject("no_outputs");
    const bool rays=std::any_of(out.inputs.begin(),out.inputs.end(),[](const auto& input){return input.contract.resource_class==0&&input.contract.kind==16;});
    // TLAS/BLAS contain indirect resource references. Their complete allocation
    // graph is unavailable here, so outputs must have independent committed
    // storage: a placed output could alias a hidden acceleration input.
    if(rays&&std::any_of(out.outputs.begin(),out.outputs.end(),[](const auto& output){return output.allocation.kind!=AllocationKind::Committed;}))return reject("ray_indirect_alias_unproven");
    if(x!=(out.width+transform.threads[0]-1)/transform.threads[0]||y!=(out.height+transform.threads[1]-1)/transform.threads[1])return reject("not_full_output_dispatch");
    for(std::size_t i=0;i<out.outputs.size();++i){
        const auto& target=out.outputs[i].allocation;
        for(const auto& input:out.inputs)if(overlaps(target,input.allocation))return reject("input_output_alias");
        for(std::size_t j=0;j<i;++j)if(overlaps(target,out.outputs[j].allocation))return reject("output_output_alias");
    }
    out.admitted=true;out.reason="bindings_resolved_nonaliasing";return out;
}
}
