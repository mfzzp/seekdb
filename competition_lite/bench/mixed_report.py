#!/usr/bin/env python3
"""Generate the mixed-scale report tables from archived summaries."""
from pathlib import Path
import argparse,json

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('root',type=Path)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
root=args.root
s=json.loads((root/'summary.json').read_text());lines=[]
labels={'new-24':'新版 24d4262','scored-f885':'原 2k 版 f885c28','cpp-449':'旧 C++ 449bba1'}
def val(x,digits=3,scale=1):
 return '—' if not x else f'{x["median"]/scale:.{digits}f}'
def span(x,digits=3):
 return '—' if not x else f'{x["min"]:.{digits}f}–{x["max"]:.{digits}f}'
def table(header,rows):
 lines.extend(['','| '+' | '.join(header)+' |','|'+'|'.join(['---']*len(header))+'|'])
 lines.extend('| '+' | '.join(map(str,r))+' |' for r in rows);lines.append('')

lines+=['## 主计时：三轮中位数','', '写入计至所有 B100 批次持久提交确认。主流程是每轮写入、索引声明、独立预热、1000 条查询和 shutdown/checkpoint 的耗时相加后取中位数，不是各列中位数相加，也不是官方评分公式。启动、数据生成、独立真值和重启核验另列。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=s['primary'][f'{size}/{label}'];rows.append([size,title,f'{r["passed"]}/{r["attempted"]}',val(r['insert_wall_s']),val(r['insert_rows_s'],0),val(r['query_wall_s']),val(r['query_qps'],1),val(r['primary_stage_sum_s']),val(r['container_peak_bytes'],1,1048576)])
table(['总行数','版本','通过轮数','写入 s','写入行/s','1000 查询 s','QPS','主流程 s','容器峰值 MiB'],rows)
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=s['primary'][f'{size}/{label}'];rows.append([size,title,span(r['insert_wall_s']),span(r['query_wall_s']),val(r['index_s']),val(r['warmup_s']),val(r['checkpoint_s'])])
table(['总行数','版本','写入范围 s','查询范围 s','索引声明 s','18 次预热 s','关闭/checkpoint s'],rows)

lines+=['## 分维度及过滤的查询性能','', '每轮四组各 250 条不同查询；等值过滤选择率 5%，每张表的候选数为 2500 / 7500。均值、P95、QPS 均先按轮计算，再取三轮中位数。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=s['primary'][f'{size}/{label}']
  for group,g in r['query_groups'].items():rows.append([size,title,group,val(g['mean_ms']),val(g['p95_ms']),val(g['qps'],1)])
table(['总行数','版本','维度/过滤','平均 ms','P95 ms','组 QPS'],rows)

lines+=['## CPU、内存及读取/写入记账','', '容器峰值包含引擎、空闲 keeper 和文件页缓存；驱动、生成器、真值计算不在该 cgroup。RSS 是查询结束时的进程 VmHWM，不含关闭后的新增峰值。rootless 容器未委派 io controller，**实际设备分阶段 I/O 不可用**；下表 `/proc/<pid>/io` 只是进程记账。rchar 包含页缓存命中的读取请求，不能当作磁盘读取字节数。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=s['primary'][f'{size}/{label}'];w=r['resources']['write'];q=r['resources']['query']
  rows.append([size,title,val(w['cpu_s']),val(q['cpu_s']),val(r['rss_peak_kib'],1,1024),val(w['write_bytes'],2,1000000),val(w['read_bytes'],2,1000000),val(q['read_bytes'],2,1000000),val(q['write_bytes'],2,1000000),val(q['rchar'],3,1000000000)])
table(['总行数','版本','写 CPU s','查 CPU s','RSS MiB','写期 write_bytes MB','写期 read_bytes MB','查期 read_bytes MB','查期 write_bytes MB','查期 rchar GB'],rows)

lines+=['## 提交同步：独立诊断，不从主计时直接扣除','', '同样完整写入量、B100 和持久提交；同步 wrapper 使用单独二进制。统计包含写入期间自动 checkpoint/Manifest 的同步，仅汇总两个 INSERT 阶段。两张表共享 WAL/文件，同步按触发请求归属，不能把触发维度当成实际写入的数据维度。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  e=s['sync_diagnostic'].get(f'{size}/{label}',{});tot={}
  for phase in ['insert128','insert1024']:
   for key,v in (e.get('sync_diagnostic') or {}).get(phase,{}).items():
    if key.startswith('_'):continue
    t=tot.setdefault(key,{'count':0,'ms':0,'errors':0})
    for k in t:t[k]+=v[k]
  for key,v in sorted(tot.items()):rows.append([size,title,key,v['count'],f'{v["ms"]:.3f}',v['errors']])
table(['总行数','版本','文件类/调用','次数','累计 ms','错误数'],rows)

lines+=['## 内部阶段：独立诊断','', '每版使用源码副本插入函数边界计时；查询距离与 SST 扫描循环只增加计数，不逐向量调用时钟。向量解析函数仍逐次计时，仅用于诊断写入成本。函数计时包含子调用，不可相加作为互斥阶段，也不混入主计时。insert_apply_inclusive 在旧版对应 apply_insert_rows，在新版对应 insert，边界并不完全相同，不能按这一栏单独归因跨版收益。B100 使用自动提交，不触发显式 BEGIN，整表复制计数为零不表示旧版不存在复制实现。每组交替查询 8 条，随后分表连续查询 16 条，输入均未重复；它们用于定位，不替代三轮正式成绩。旧版的 LSM 专用计数未实现，不按零解释。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  e=s['internal_diagnostic'].get(f'{size}/{label}',{});tot={}
  for phase in ['insert128','insert1024']:
   for key,v in (e.get('sync_diagnostic') or {}).get(phase,{}).get('_functions',{}).items():
    t=tot.setdefault(key,{'count':0,'ms':0});t['count']+=v['count'];t['ms']+=v['ms']
  for key in ['json_parse','sql_lex','vector_parse','insert_apply_inclusive','journal_commit_inclusive','checkpoint_inclusive','whole_state_copy']:
   v=tot.get(key);rows.append([size,title,key,v['count'] if v else '—',f'{v["ms"]:.3f}' if v else '—'])
table(['总行数','版本','写入阶段函数','调用数','累计 ms（包含子调用）'],rows)
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  e=s['internal_diagnostic'].get(f'{size}/{label}',{})
  for key in ['128/none','128/category','1024/none','1024/category']:
   q=(e.get('query_groups') or {}).get(key,{})
   rows.append([size,title,key,size//2 if key.endswith('/none') else size//40,q.get('full_distance_evaluations_mean','—'),q.get('distance_dimensions_mean','—')])
lines.append('完整距离与维度只统计候选原向量；旧版 Block 下界和中心距离的额外运算不包含在该计数中。候选数由本次固定输入确定。')
table(['总行数','版本','查询组','输入预期候选','实际完整距离/查询','实际候选维度/查询'],rows)
rows=[]
for size in [100000,300000]:
 for group in ['128/none','128/category','1024/none','1024/category']:
  e=s['internal_diagnostic'].get(f'{size}/new-24',{});g=e.get('per_group',{}).get('alternating/'+group,{});c=g.get('lsm_counters',{});f=g.get('function_ms',{});q=(e.get('query_groups') or {}).get(group,{})
  rows.append([size,group,q.get('full_distance_evaluations_mean','—'),val(f.get('hot_view_prepare')),val(f.get('select_inclusive')),val(c.get('row_get'),0),val(c.get('sst_cursor_open'),0),val(c.get('sst_block_read'),0),val(c.get('sst_block_read_bytes'),2,1000000),val(c.get('sst_record_decode'),0)])
table(['总行数','新版查询组','完整距离数','热视图判断 ms','select ms','行点查数','SST cursor数','SST块读取数','块请求 MB','SST记录解码数'],rows)
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  e=s['sync_diagnostic'].get(f'{size}/{label}',{})
  for key in ['128/none','128/category','1024/none','1024/category']:
   a=(e.get('query_groups') or {}).get(key,{}).get('mean_ms');g=(e.get('grouped_query_groups') or {}).get(key,{}).get('mean_ms');rows.append([size,title,key,f'{a:.3f}' if a is not None else '—',f'{g:.3f}' if g is not None else '—'])
table(['总行数','版本','查询组','交替访问平均 ms','分表连续访问平均 ms'],rows)

lines+=['## 新版逻辑写入字节拆分','', '以下为源码诊断计数，单位 MB（10⁶ 字节）。SST 输出按 flush/compaction 归类，向量/WAL/Manifest 按实际编码长度累计；不是设备 I/O。旧版未添加同口径文件字节计数，以进程累计写入和同步对照为准。']
rows=[]
for size in [100000,300000]:
 e=s['internal_diagnostic'].get(f'{size}/new-24',{});c={}
 for phase in ['insert128','insert1024']:
  for key,value in (e.get('sync_diagnostic') or {}).get(phase,{}).get('_lsm_counters',{}).items():c[key]=c.get(key,0)+value
 rows.append([size,*[f'{c[k]/1e6:.3f}' if k in c else '—' for k in ['vector_append_bytes','wal_frame_bytes','sst_flush_bytes','sst_compact_bytes','manifest_bytes']]])
table(['总行数','向量追加 MB','WAL帧 MB','SST flush MB','SST合并 MB','Manifest MB'],rows)

lines+=['## 正确性、重启及失败','', '每轮全部正式查询与独立 C++ 暴力 Top-K 真值比对 ID、顺序，返回距离再用 Python math.fsum 核对。主流程关闭后以新进程逐批检查两表所有普通字段和原始 float32 字节，不抽样；这段验证时间不计入查询成绩。']
rows=[]
for size in [100000,300000]:
 for label,title in labels.items():
  r=s['primary'][f'{size}/{label}'];rows.append([size,title,f'{r["passed"]}/{r["attempted"]}',r['exact_verified'],r['restored_rows_verified'],val(r['restart_s']),val(r['restore_check_s']),val(r['restore_container_peak_bytes'],1,1048576)])
table(['总行数','版本','通过轮次','真值查询数','全量核验行数','重启 s','全量核验 s','核验容器峰值 MiB'],rows)
if s['failures']:
 for f in s['failures']:lines.append(f'- {f["label"]}: {f["phase"]}, {f["failure"]}')
else:lines.append('本次主对照未发现响应 ID 错配、提前退出、结果不一致或重启数据丢失。该结论仅覆盖本轮输入和资源限制，不等同于隐藏用例通过保证。')
(args.output or root/'tables.md').write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n')
