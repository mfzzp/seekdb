#!/usr/bin/env python3
"""Summarize archived read-path evidence without mixing diagnostic and main runs."""
import argparse,json,statistics
from pathlib import Path
import mixed_summarize as mixed

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('root',type=Path);ap.add_argument('--baseline',type=Path,required=True)
a=ap.parse_args();root=a.root;release=root/'release'
mixed.LABELS=['readpath'];new=mixed.summarize(release)
old=mixed.load(a.baseline)
new['frozen_baseline_report_commit']='312f7e0e7039489d0c25688bff1ab0f274f1edff'
new['baseline_relation']='Historical three-round measurements; unchanged prepared input hashes, compiler, machine and limits. Old versions were not rerun for this iteration.'
new['runtime']=mixed.load(release/'runtime-identity.json.gz')
new['small_paired']=mixed.load(release/'small-paired/summary.json.gz')
new['diagnostics']={}
for p in sorted((release/'diagnostics').glob('*/result.json.gz')):
 r=mixed.load(p);assert r['passed'] and r['exact_check']['passed'],p
 item={k:r[k] for k in ['label','engine_env','exact_check','sync_diagnostic','query_groups','grouped_query_groups','warmup_s','warmup']}
 item['groups']={}
 for order,queries in [('alternating',r['queries']),('grouped',r['grouped_queries'])]:
  for group in ['128/none','128/category','1024/none','1024/category']:
   qs=[q for q in queries if str(q['dim'])+'/'+q['filter']==group]
   fields={'request_ms':mixed.sample([q['seconds']*1000 for q in qs])}
   for family in [None,'profile_ms','function_ms','function_counts','lsm_counters']:
    metrics=[q['metrics'] if family is None else q['metrics'].get(family,{}) for q in qs]
    if not metrics:continue
    for key in set.intersection(*[set(m) for m in metrics]):
     values=[m[key] for m in metrics]
     if all(isinstance(v,(int,float)) and not isinstance(v,bool) for v in values):fields[(family+'.' if family else '')+key]=mixed.sample(values)
   item['groups'][order+'/'+group]=fields
 new['diagnostics'][r['label']]=item
for key,r in new['primary'].items():
 assert r['passed']==r['attempted']==3 and r['exact_verified']==3000,key
 assert r['restored_rows_verified']==3*int(key.split('/')[0]),key
 for label in ['scored-f885','cpp-449']:
  b=old['primary'][key.split('/')[0]+'/'+label]
  for metric in ['insert_wall_s','query_wall_s','primary_stage_sum_s','container_peak_bytes']:assert r[metric]['median']<b[metric]['median'],(key,label,metric)
  for group,g in r['query_groups'].items():
   for metric in ['mean_ms','p95_ms']:assert g[metric]['median']<b['query_groups'][group][metric]['median'],(key,label,group,metric)
(root/'summary.json').write_text(json.dumps(new,indent=2)+'\n',encoding='utf-8',newline='\n')

lines=[]
def table(header,rows):
 lines.extend(['','| '+' | '.join(header)+' |','|'+'|'.join(['---']*len(header))+'|'])
 lines.extend('| '+' | '.join(map(str,row))+' |' for row in rows);lines.append('')
def val(x,d=3,scale=1):return '—' if x is None else f'{x["median"]/scale:.{d}f}'
def span(x):return f'{x["min"]:.3f}–{x["max"]:.3f}'
labels={'new-24':'修复前 24d4262','scored-f885':'原 2k f885c28','cpp-449':'旧 C++ 449bba1','readpath':'本轮修复'}
def primary(size,label):return (new if label=='readpath' else old)['primary'][f'{size}/{label}']
lines+=['## 正式计时与内存','', '同一服务器、GCC 12.2、2 CPU/2 GiB/no swap、CPU 0/1；总行数的一半为 128D、一半为 1024D，同一进程两张表。B100，1000/3000 次持久提交，1000 条独立 Exact/K10 查询。18 次不同的预热单列并计入主流程。以下均为各轮统计的三轮中位数；旧版沿用 312f7e0 冻结的三轮记录，本轮没有重新运行旧版。主流程按每轮写入、索引声明、预热、查询和关闭/checkpoint 求和后取中位数，不是官方综合分。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=primary(size,label);rows.append([size,title,val(r['insert_wall_s']),val(r['insert_rows_s'],0),val(r['query_wall_s']),val(r['query_qps'],1),val(r['primary_stage_sum_s']),val(r['container_peak_bytes'],1,1048576)])
table(['总行数','版本','写入 s','行/s','1000 查询 s','QPS','主流程 s','峰值 MiB'],rows)
table(['总行数','写入范围 s','查询范围 s','索引声明 s','预热 s','关闭/checkpoint s'],[[size,span(r['insert_wall_s']),span(r['query_wall_s']),val(r['index_s']),val(r['warmup_s']),val(r['checkpoint_s'])] for size in [100000,300000] for r in [primary(size,'readpath')]])

lines+=['## 分组查询','', '每组每轮 250 条查询；过滤选择率 5%。表中每个均值、P95、QPS 都先按轮计算，再取三轮中位数。']
table(['总行数','版本','维度/过滤','均值 ms','P95 ms','组 QPS','完整距离/查询'],[[size,title,g,val(m['mean_ms']),val(m['p95_ms']),val(m['qps'],1),val(m['full_distance_evaluations_mean'],0)] for size in [100000,300000] for label,title in labels.items() for g,m in primary(size,label)['query_groups'].items()])

lines+=['## CPU 与进程 I/O 记账','', '**设备级 io.stat 不可用。** rchar 是系统调用读取请求字节，包含页缓存命中；进程 read_bytes/write_bytes 也不代替官方设备指标。RSS 是查询结束时的进程 VmHWM，容器峰值包括文件页缓存及 keeper。驱动与真值校验在引擎 cgroup 外。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=primary(size,label);w=r['resources']['write'];q=r['resources']['query']
  rows.append([size,title,val(w['cpu_s']),val(q['cpu_s']),val(r['rss_peak_kib'],1,1024),val(w['read_bytes'],2,1e6),val(w['write_bytes'],2,1e6),val(q['read_bytes'],2,1e6),val(q['write_bytes'],2,1e6),val(q['rchar'],3,1e9)])
table(['行数','版本','写 CPU s','查 CPU s','RSS MiB','写期读取 MB','写期写入 MB','查期读取 MB','查期写入 MB','查期 rchar GB'],rows)

lines+=['## 同步与逻辑写入','', '以下为独立诊断运行，只累计 insert128/insert1024 两个写入阶段。函数包含子调用，不能相加或从主计时直接扣除。逻辑输出字节按编码长度计数；不代表设备流量。同步次数包括自动发布屏障。']
syncrows=[];writerows=[];functionrows=[]
for size in [100000,300000]:
 r=new['diagnostics'][f'diag-{size}-default'];tot={};counts={};functions={}
 for phase in ['insert128','insert1024']:
  part=r['sync_diagnostic'][phase]
  for key,v in part.items():
   if key.startswith('_'):continue
   target=tot.setdefault(key,{'count':0,'ms':0,'errors':0})
   for field in target:target[field]+=v[field]
  for key,v in part['_lsm_counters'].items():counts[key]=counts.get(key,0)+v
  for key,v in part['_functions'].items():
   t=functions.setdefault(key,{'count':0,'ms':0});t['count']+=v['count'];t['ms']+=v['ms']
 for key,v in tot.items():
  if v['count']:syncrows.append([size,key,v['count'],f'{v["ms"]:.3f}',v['errors']])
 writerows.append([size,*[f'{counts[k]/1e6:.3f}' for k in ['vector_append_bytes','wal_frame_bytes','sst_flush_bytes','sst_compact_bytes','manifest_bytes']]])
 for key in ['json_parse','sql_lex','vector_parse','insert_apply_inclusive','journal_commit_inclusive','checkpoint_inclusive','whole_state_copy']:
  v=functions[key];functionrows.append([size,key,v['count'],f'{v["ms"]:.3f}'])
table(['总行数','文件/同步','次数','累计 ms','错误'],syncrows)
table(['总行数','向量 MB','WAL MB','SST flush MB','Compaction MB','Manifest MB'],writerows)
table(['总行数','写入函数（包含子调用）','次数','累计 ms'],functionrows)

lines+=['## 读取路径诊断与关闭缓存对照','', '每种设置重新完整装载；显式 LITE_ROW_CACHE_MB=0。每组先交替 8 条查询，后分组连续 16 条，合计 96 条不同查询均核对真值。下表只展示交替组的逐请求中位数，不作为 1000 查询正式成绩。batch/scan 强制关闭紧凑视图；cache0/cache1 同时将保留块缓存设为 0/1 MiB；singlemap 仅关闭多窗口保留。扫描耗时包含元数据准备、过滤、向量访问/距离及 Top-K，尚未单独拆开；原 profile_ms 中占位的零值不视为真实测量。读取器次数与游标次数分别列出。unique-block 跟踪上限为 65536，原始记录保留精确性标志。']
rows=[]
for size in [100000,300000]:
 for group in ['128/none','128/category','1024/none','1024/category']:
  g=new['diagnostics'][f'diag-{size}-default']['groups']['alternating/'+group]
  rows.append([size,group,*[val(g.get(k)) for k in ['function_ms.json_parse','function_ms.sql_lex','function_ms.vector_parse','function_ms.select_inclusive','profile_ms.scan_total','profile_ms.result','function_ms.json_encode']]])
table(['行数','组','JSON解析 ms','SQL词法 ms','查询向量解析 ms','SELECT含子调用 ms','扫描/距离/TopK ms','结果组织 ms','JSON输出 ms'],rows)
rows=[]
for label,r in new['diagnostics'].items():
 for group in ['128/none','128/category','1024/none','1024/category']:
  g=r['groups']['alternating/'+group]
  rows.append([label.replace('diag-',''),group,val(g['request_ms']),val(g.get('sst_reader_initializations'),0),val(g.get('sst_cursor_initializations'),0),val(g.get('sst_index_bytes'),0),val(g.get('sst_block_requests'),0),val(g.get('sst_unique_blocks'),0),val(g.get('sst_block_reads'),0),val(g.get('sst_record_decodes'),0),val(g.get('full_row_decodes'),0),val(g.get('vector_map_creations'),0)])
table(['设置','组','请求 ms','Reader','Cursor','索引字节','块请求','不同块','块读取','SST记录解码','完整行解码','mmap次数'],rows)
rows=[]
for label,r in new['diagnostics'].items():
 g=r['groups']['alternating/128/category'];rows.append([label.replace('diag-',''),val(g.get('read_cache_bytes'),2,1048576),val(g.get('read_live_bytes'),2,1048576),val(g.get('read_peak_bytes'),2,1048576),val(g.get('query_view_bytes'),2,1048576),val(g.get('mapped_virtual_bytes'),2,1048576),val(g.get('read_open_files'),0)])
table(['设置','保留缓存 MiB','活动缓存估算 MiB','累计峰值估算 MiB','紧凑视图 MiB','虚拟映射 MiB','活动 SST FD'],rows)
lines+=['保留缓存预算与活动游标工作集分开：后者包含仍被查询持有的已驱逐块/Reader。估算不是精确分配器 RSS，也不含所有短期 CRC 字符串；主计时容器峰值才是实际内存测量。mmap 是虚拟映射，驻留页仍计入容器。诊断追踪集合本身另有字节计数，不用于正式计时。']

lines+=['## 默认 1 万行回归','', '与修复前 24d4262 在本轮相邻、轮换顺序运行三次，使用同一冻结 10k/128D/B100/1000 查询输入。']
small=new['small_paired']['summary_medians']
table(['版本','写入 s','1000 查询 s','查询范围 s','峰值 MiB'],[[label,f'{r["insert_wall_s"]:.6f}',f'{r["query_wall_s"]:.6f}',f'{r["ranges"]["query_wall_s"][0]:.6f}–{r["ranges"]["query_wall_s"][1]:.6f}',f'{r["peak_bytes"]/1048576:.2f}'] for label,r in small.items()])
lines+=['当前三轮查询中位数增加约 2.7%，区间重叠；不能称为小规模提速，也尚不足以证明稳定退化。峰值增加约 1 MiB，属于本次复用设施的代价。']

lines+=['## 正确性与证据','', '正式六轮查询均与独立 C++ 暴力真值核对主键及顺序，返回距离再用 Python math.fsum 检查；每轮重启后核对全部普通字段和 float32 原始向量字节。核验不计入性能成绩。30 万 r1/r2 使用旧的无上界核验请求，r3 使用固定 128 行主键区间，均全量检查；核验耗时包含两种驱动，不用于引擎速度比较。']
table(['总行数','正式通过','真值查询','重启全量行数','重启 s','核验 s'],[[size,'3/3',r['exact_verified'],r['restored_rows_verified'],val(r['restart_s']),val(r['restore_check_s'])] for size in [100000,300000] for r in [primary(size,'readpath')]])
lines+=['完整 Docker gate、专用缓存/批量/紧凑视图/映射检查、故障恢复、容量、迁移及 ASan/UBSan 记录在同目录 release 下。开发 phase3 未达到旧版查询门槛的结果也保留，未作为正式版本。所有结论只覆盖本轮测试，不构成隐藏用例或官方成绩保证。']
(root/'tables.md').write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n')
print(json.dumps({'main_runs':6,'exact_queries':sum(x['exact_verified'] for x in new['primary'].values()),'restored_rows':sum(x['restored_rows_verified'] for x in new['primary'].values()),'diagnostics':len(new['diagnostics'])}))
