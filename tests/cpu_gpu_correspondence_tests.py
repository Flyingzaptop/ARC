import importlib.util,json,math,struct,tempfile,unittest
from pathlib import Path
p=Path(__file__).resolve().parents[1]/'scripts/cpu_gpu_correspondence.py';spec=importlib.util.spec_from_file_location('correspondence',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class CorrespondenceTests(unittest.TestCase):
    def fixture(self,root,constant=False,stale=False):
        frames=[]
        for f in range(2):
            cpu=bytearray(100000);gpu=bytearray(60000)
            for i in range(256):
                def value(frame):return struct.pack('<3f',i+1.125+frame*.125,i*2+frame*.25,math.sin(i+frame*.2))
                cpu[128+i*320:140+i*320]=value(0 if constant else f)
                gpu[52+i*192:64+i*192]=value(0 if constant or stale else f)
            (root/f'c{f}').write_bytes(cpu);(root/f'g{f}').write_bytes(gpu);(root/f'noise{f}').write_bytes(bytes(len(cpu)))
            frames.append({'gpu':f'g{f}','cpu':[{'id':'unknown-1','file':f'c{f}'},{'id':'unknown-2','file':f'noise{f}'}]})
        return {'frames':frames}
    def test_learns_strides_and_offsets(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);result=m.discover(self.fixture(root),root)
            self.assertTrue(any(r['cpu_stride']==320 and r['gpu_stride']==192 and r['cpu_offset']==128 and r['gpu_offset']==52 for r in result['relations']))
            self.assertTrue(all(not r['replacement_allowed'] for r in result['relations']))
    def test_stale_gpu_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.assertFalse(m.discover(self.fixture(root,stale=True),root)['relations'])
    def test_constants_do_not_prove_correspondence(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);self.assertFalse(m.discover(self.fixture(root,constant=True),root)['relations'])
    def test_names_are_not_features(self):
        with tempfile.TemporaryDirectory() as t:
            root=Path(t);spec=self.fixture(root);a=m.discover(spec,root)['relations']
            for f in spec['frames']:
                for r in f['cpu']:r['id']='relocated-'+r['id'];r['file']='renamed-'+r['file']
            for f in range(2):
                for n in [f'c{f}',f'noise{f}']:(root/('renamed-'+n)).write_bytes((root/n).read_bytes())
            b=m.discover(spec,root)['relations']
            for r in b:r['cpu_region']=r['cpu_region'].removeprefix('relocated-')
            self.assertEqual(a,b)
if __name__=='__main__':unittest.main()
