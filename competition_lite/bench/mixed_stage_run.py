#!/usr/bin/env python3
"""Use the same diagnostic workload with extended shared function counters."""
import mmap,struct
import mixed_compare as base
FUNCTIONS=['json_parse','sql_lex','vector_parse','insert_apply_inclusive','select_inclusive','matrix_prepare','scalar_index_prepare','block_prepare','whole_state_copy','journal_commit_inclusive','checkpoint_inclusive','json_encode','hot_view_prepare']
COUNTERS=['row_get','row_decode','sst_cursor_open','sst_block_read','sst_block_read_bytes','sst_record_decode','sst_flush_bytes','sst_compact_bytes','wal_frame_bytes','vector_append_bytes','manifest_bytes','store_point_get']
class StageProbe(base.Probe):
 active=None
 def __init__(self,data,enabled):
  super().__init__(data,enabled)
  if enabled:self.map.close();self.fd.truncate(8192);self.fd.flush();self.map=mmap.mmap(self.fd.fileno(),8192)
  StageProbe.active=self
 def snapshot_functions(self):
  if self.map is None:return None
  phase=struct.unpack_from('<Q',self.map,0)[0];return struct.unpack_from('<26Q',self.map,(400+phase*26)*8)
 def snapshot_counters(self):
  if self.map is None:return None
  phase=struct.unpack_from('<Q',self.map,0)[0];return struct.unpack_from('<12Q',self.map,(700+phase*12)*8)
 def result(self):
  result=super().result()
  if result is None:return None
  vals=struct.unpack_from('<1024Q',self.map)
  for i,phase in enumerate(base.PHASES):
   result[phase]['_functions']={name:{'count':vals[400+(i*13+j)*2],'ms':vals[401+(i*13+j)*2]/1e6} for j,name in enumerate(FUNCTIONS)}
   result[phase]['_lsm_counters']={name:vals[700+i*12+j] for j,name in enumerate(COUNTERS)}
  return result
base.Probe=StageProbe
class StageClient(base.MixedClient):
 def send(self,request):
  probe=StageProbe.active;before=probe.snapshot_functions();bc=probe.snapshot_counters();r=super().send(request);after=probe.snapshot_functions();ac=probe.snapshot_counters()
  if before is not None:
   metrics=r.setdefault('metrics',{});metrics['function_ms']={name:(after[j*2+1]-before[j*2+1])/1e6 for j,name in enumerate(FUNCTIONS)};metrics['function_counts']={name:after[j*2]-before[j*2] for j,name in enumerate(FUNCTIONS)}
   metrics['lsm_counters']={name:ac[j]-bc[j] for j,name in enumerate(COUNTERS)}
  return r
base.MixedClient=StageClient
if __name__=='__main__':raise SystemExit(base.main())
