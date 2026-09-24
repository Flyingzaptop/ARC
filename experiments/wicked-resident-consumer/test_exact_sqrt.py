"""Independent host oracle for the shader's midpoint correction (not a GPU timing test)."""
import math
import random
import struct
import unittest

def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]
def value(word):return struct.unpack('<f',struct.pack('<I',word))[0]
def compare(low,high,radicand):
    le,he=(low>>23)&255,(high>>23)&255
    midpoint=((low&0x7fffff)|0x800000)+(((high&0x7fffff)|0x800000)<<(he-le))
    square=midpoint*midpoint
    se=(radicand>>23)&255
    mantissa=(radicand&0x7fffff)|(0x800000 if se else 0)
    shift=(se if se else 1)-2*le+152
    assert 0<=shift<64
    scaled=mantissa<<shift
    assert max(square,scaled)<1<<64
    return (scaled>square)-(scaled<square)
def correct(radicand,seed):
    for _ in range(4):
        lower,upper=compare(seed-1,seed,radicand),compare(seed,seed+1,radicand)
        if lower<0 or (lower==0 and seed&1):seed-=1
        elif upper>0 or (upper==0 and seed&1):seed+=1
        else:break
    return seed

class ExactSqrt(unittest.TestCase):
    def test_normal_inputs_and_neighboring_hardware_results(self):
        rng=random.Random(20260924)
        corpus=[bits(x*x) for x in [1.0,2.0,4.0,16.0,value(1100353536)]]
        corpus.extend(rng.randrange(0x00800000,0x7f7fffff) for _ in range(10000))
        for source in corpus:
            expected=bits(math.sqrt(value(source)))
            for delta in [-3,-2,-1,0,1,2,3]:
                self.assertEqual(correct(source,expected+delta),expected,(hex(source),delta))

if __name__=='__main__':unittest.main()
