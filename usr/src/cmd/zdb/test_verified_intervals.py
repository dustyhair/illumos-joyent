import copy,hashlib,unittest
from test_paired_intervals import fixture,F
import verified_intervals as v
from paired_intervals import canonical

def sample(nonallocated=None):
 rows=fixture(omit=nonallocated is not None);rows[0]['schema']='zdb-observed-verified-v1';bp=rows[3]
 if nonallocated:
  bp['bp_words'][:6]=['0000000000000000']*6
  bp.update(classification=nonallocated,dvas=[])
  if nonallocated=='embedded':
   bp['bp_words'][6]=f'{(19<<48)|(1<<39)|(2<<32)|(7<<25)|1023:016x}'
   bp.update(lsize='1024',psize='0',checksum_id=2,physical_birth='0',embedded_payload_bytes='8')
 check=dict(record='verification',sequence='1',status='verified',measured=True,error=0,requested_p='4096',completed_p='4096',**F)
 for k in ('bp_words','objset','object','level','blkid'):check[k]=copy.deepcopy(bp[k])
 rows.insert(4,check)
 rows.insert(-2,dict(record='verification_end',verification_complete=True,seen='1',submitted='1',completed='1',passed='1',failed='0',unsupported='0',not_applicable='0',outstanding='0',requested_p='4096',verified_p='4096',**F))
 if nonallocated:
  rows[4].update(status='not_applicable',measured=False,requested_p='0',completed_p='0')
  rows[-3].update(submitted='0',completed='0',passed='0',not_applicable='1',requested_p='0',verified_p='0')
 return rows

def run(rows,status='0'):
 data=b''.join(canonical(r)+b'\n' for r in rows)
 return v.analyze(data,status,hashlib.sha256(data).hexdigest())
class Tests(unittest.TestCase):
 def test_success(self):
  result=run(sample());self.assertEqual(result['measured_read_summary']['verified_raw_P_bytes'],4096);self.assertFalse(result['coverage_complete'])
 def test_nonallocated_positive(self):
  for kind in ('hole','embedded'):
   with self.subTest(kind=kind):
    result=run(sample(kind));self.assertEqual(result['measured_read_summary']['not_applicable_bp_references'],1)
 def test_nonallocated_native_preconditions(self):
  for kind in ('hole','embedded'):
   for typ,dedup in [(19,True),(42,False),(43,False),(49,False),(42,True),(43,True),(49,True)]:
    rows=sample(kind);bp=rows[3];bp['bp_type']=typ
    prop=int(bp['bp_words'][6],16);prop=(prop & ~(255<<48)) | (typ<<48)
    if dedup:prop |= 1<<62
    bp['bp_words'][6]=f'{prop:016x}';rows[4]['bp_words']=copy.deepcopy(bp['bp_words'])
    with self.subTest(kind=kind,typ=typ,dedup=dedup),self.assertRaisesRegex(ValueError,'native verification eligibility'):run(rows)
 def test_nonallocated_crypt_and_invalid_type(self):
  for kind in ('hole','embedded'):
   for mask in (1<<61,127<<48):
    rows=sample(kind);bp=rows[3];prop=int(bp['bp_words'][6],16)
    if mask==127<<48:prop=(prop & ~(255<<48))|mask;bp['bp_type']=127
    else:prop |= mask
    bp['bp_words'][6]=f'{prop:016x}';rows[4]['bp_words']=copy.deepcopy(bp['bp_words'])
    with self.subTest(kind=kind,mask=mask),self.assertRaises(ValueError):run(rows)
 def test_missing_duplicate(self):
  rows=sample();rows.pop(4)
  with self.assertRaises(ValueError):run(rows)
  rows=sample();rows.insert(5,copy.deepcopy(rows[4]))
  with self.assertRaises(ValueError):run(rows)
 def test_result_binding(self):
  for k,val in [('sequence','2'),('object','1'),('blkid','1'),('level','0'),('objset','1'),('bp_words',['0000000000000000']*16),('requested_p','512'),('completed_p','512'),('status','failed'),('measured',False),('error',5)]:
   with self.subTest(k=k):
    rows=sample();rows[4][k]=val
    with self.assertRaises(ValueError):run(rows)
 def test_counters(self):
  for k in ('seen','submitted','completed','passed','failed','unsupported','not_applicable','outstanding','requested_p','verified_p'):
   rows=sample();rows[-3][k]='99'
   with self.subTest(k=k),self.assertRaises(ValueError):run(rows)
 def test_terminal(self):
  rows=sample();rows[-3]['verification_complete']=False
  with self.assertRaises(ValueError):run(rows)
  with self.assertRaises(ValueError):run(sample(),'1')
  rows=sample();rows[-4]['txg']='31'
  with self.assertRaises(ValueError):run(rows)
 def test_checksum_off(self):
  rows=sample();rows[3]['checksum_id']=2
  with self.assertRaises(ValueError):run(rows)
 def test_global_claim(self):
  rows=sample();rows[4]['coverage_complete']=True
  with self.assertRaises(ValueError):run(rows)
if __name__=='__main__':unittest.main(verbosity=2)
