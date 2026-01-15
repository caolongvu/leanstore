# Intro plot

Command: `./benchmark/LeanStore_TPCC -worker_count=1 -worker_pin_thread=true -tpcc_warehouse_count=1 -tpcc_exec_seconds=20 -bm_virtual_gb=32 -bm_physical_gb=16 -txn_debug=true -wal_batch_write_kb=1 -wal_stealing_group_size=1 -txn_commit_group_size=1 -tpcc_neworder_only=true -db_path=/dev/nvme0n1`

**Local SSD**:

```shell
ts,tx,normal,rfa,commit_rounds,bm_rmb,bm_wmb,bm_evict,log_sz_mb,logio_mb,log_flush_cnt,gct_p1_us,gct_p2_us,gct_p3_us,db_size
0,9842,9842,0,9842,0.0000,0.0000,0,50.6576,68.7109,9873,3630,70,1178,0.2227
1,10070,10070,0,10070,0.0000,0.0000,0,51.9253,70.5625,10070,3801,72,1702,0.2423
2,10164,10164,0,10164,0.0000,0.0000,0,52.3248,71.2812,10164,3777,73,1797,0.2620
3,10168,10168,0,10168,0.0000,0.0000,0,52.3497,71.1055,10168,3746,73,1706,0.2818
4,10167,10167,0,10167,0.0000,0.0000,0,52.2565,71.0000,10167,3761,73,1845,0.3015
5,10171,10171,0,10171,0.0000,0.0000,0,52.1027,70.7891,10171,3768,73,1738,0.3211
6,10147,10147,0,10147,0.0000,0.0000,0,52.1119,70.4492,10147,3736,73,1739,0.3408
7,10193,10193,0,10193,0.0000,0.0000,0,52.6486,71.1172,10193,3770,73,1959,0.3606
8,10177,10177,0,10177,0.0000,0.0000,0,52.3184,71.0469,10177,3739,73,1609,0.3804
9,10232,10233,0,10232,0.0000,0.0000,0,52.7778,71.5547,10233,3772,73,1690,0.4003
[2026-01-14 13:17:19.567] [info] Transaction statistics: # completed txns: 101332 - # committed txns: 101331
[2026-01-14 13:17:19.567] [info] AvgGroupCommitTime: 0.5446us - No rounds 101331 - Txn per round 1.0000
[2026-01-14 13:17:19.567] [info] Halt LeanStore's Profiling thread
[2026-01-14 13:17:19.567] [info] Start measuring latency
[2026-01-14 13:17:19.567] [info] # data points: 91489
[2026-01-14 13:17:19.567] [info] Start evaluating latency data
[2026-01-14 13:17:19.573] [info] RFA transaction statistics:
	AvgLatency(98.0663 us)
	99.9thLatency(185.614 us)
[2026-01-14 13:17:19.574] [info] Statistics:
	AvgExecTime(37.4579 us)
	AvgQueue(60.5600 us)
	AvgLatencyInclWait(98.0663 us)
	AvgIOLatency(59.4727 us)
	AvgTxnPerCommitRound(1.0000 txns)
	99.9thTxnPerRound(1 txns)
	99.99thTxnPerRound(1 txns)
[2026-01-14 13:17:19.575] [info] Space used: 0.40034103 GB - WAL size: 0.6933594 GB
```

**EBS**:

```shell
ts,tx,normal,rfa,commit_rounds,bm_rmb,bm_wmb,bm_evict,log_sz_mb,logio_mb,log_flush_cnt,gct_p1_us,gct_p2_us,gct_p3_us,db_size
0,2127,2127,0,2127,0.0000,0.0000,0,10.7820,14.5898,2158,884,15,273,0.2077
1,1051,1051,0,1051,0.0000,0.0000,0,5.4349,7.4062,1051,460,7,214,0.2097
2,1052,1052,0,1052,0.0000,0.0000,0,5.4556,7.4102,1052,460,7,180,0.2118
3,1053,1053,0,1053,0.0000,0.0000,0,5.4795,7.4336,1053,465,7,176,0.2139
4,1052,1052,0,1052,0.0000,0.0000,0,5.5431,7.4961,1052,460,7,192,0.2159
5,1057,1057,0,1057,0.0000,0.0000,0,5.3854,7.3008,1057,463,7,176,0.2180
6,1054,1054,0,1054,0.0000,0.0000,0,5.4563,7.3477,1054,461,7,175,0.2200
7,1057,1057,0,1057,0.0000,0.0000,0,5.4284,7.3867,1057,460,7,180,0.2221
8,1056,1056,0,1056,0.0000,0.0000,0,5.4322,7.4219,1056,460,7,214,0.2241
9,1055,1056,0,1055,0.0000,0.0000,0,5.3714,7.3047,1056,451,7,176,0.2261
[2026-01-14 13:16:50.141] [info] Transaction statistics: # completed txns: 11615 - # committed txns: 11614
[2026-01-14 13:16:50.141] [info] AvgGroupCommitTime: 0.6077us - No rounds 11614 - Txn per round 1.0000
[2026-01-14 13:16:50.141] [info] Halt LeanStore's Profiling thread
[2026-01-14 13:16:50.141] [info] Start measuring latency
[2026-01-14 13:16:50.141] [info] # data points: 9487
[2026-01-14 13:16:50.141] [info] Start evaluating latency data
[2026-01-14 13:16:50.143] [info] RFA transaction statistics:
	AvgLatency(948.3205 us)
	99.9thLatency(1149.524 us)
[2026-01-14 13:16:50.143] [info] Statistics:
	AvgExecTime(54.9806 us)
	AvgQueue(893.2956 us)
	AvgLatencyInclWait(948.3205 us)
	AvgIOLatency(892.1413 us)
	AvgTxnPerCommitRound(1.0000 txns)
	99.9thTxnPerRound(1 txns)
	99.99thTxnPerRound(1 txns)
[2026-01-14 13:16:50.144] [info] Space used: 0.22614288 GB - WAL size: 0.078125 GB
```
