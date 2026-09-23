#pragma once
#include "arc/cheap_observer.hpp"
namespace arc::dx12::observation {
inline arc::CheapObserver& state(){static auto* value=new arc::CheapObserver;return *value;}
}
