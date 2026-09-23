#pragma once
#include "wiPrimitive.h"
struct ArcChainResult { wi::primitive::AABB box; DirectX::XMFLOAT3 center; uint32_t visible; };
static_assert(sizeof(ArcChainResult)==48);
