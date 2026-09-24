#include "arc/cpu/changed_words.hpp"
#include <array>
int main(){std::array<std::uint64_t,162> input{},previous{},decoded{};std::array<unsigned char,1320> data{};
for(unsigned pass=0;pass<100;++pass){input[pass%162]=pass*123456789ull;auto n=arc::cpu::encode_changed_words(input.data(),previous.data(),162,data.data(),data.size());if(!n)return 1;unsigned offset=21;for(unsigned i=0;i<162;++i)if(data[i/8]&(1u<<(i%8))){std::memcpy(&decoded[i],data.data()+offset,8);offset+=8;}if(offset!=n||decoded!=input)return 2;}
if(arc::cpu::encode_changed_words(input.data(),previous.data(),162,data.data(),1))return 3;return 0;}
