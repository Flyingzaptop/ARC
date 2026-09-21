#pragma once
#include "generic_shader_transform.hpp"
namespace arc::dx12::shader {
struct SpatialPrelude {std::string ir,error,valid,block;};
SpatialPrelude spatial_importance(const Transform&,std::string error,std::string valid,std::string feature,std::string block);
SpatialPrelude spatial_lookup(const Transform&,std::string block);
}
