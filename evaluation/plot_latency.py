import pandas as pd
import matplotlib.pyplot as plt

plt.style.use("seaborn-v0_8-paper")
plt.rcParams.update({'font.size': 11})

plt.figure(figsize=(6,4))

df1 = pd.read_csv("/home/long/leanstore/evaluation/tpcc/trad_latencies.csv")
df2 = pd.read_csv("/home/long/leanstore/evaluation/tpcc/flush_latencies.csv")
df3 = pd.read_csv("/home/long/leanstore/evaluation/tpcc/bdr_trad_latencies.csv")
df4 = pd.read_csv("/home/long/leanstore/evaluation/tpcc/bdr_flush_latencies.csv")

plt.plot(df1["latency"] / 1000, df1["percentile"] * 100, label="Trad", linewidth=2, marker='o', markersize=5)
plt.plot(df2["latency"] / 1000, df2["percentile"] * 100, label="Flush", linewidth=2, marker='o', markersize=5)
plt.plot(df3["latency"] / 1000, df3["percentile"] * 100, label="Trad_BDR", linewidth=2, marker='o', markersize=5)
plt.plot(df4["latency"] / 1000, df4["percentile"] * 100, label="Flush_BDR", linewidth=2, marker='o', markersize=5)

plt.xlabel("Latency")
plt.ylabel("%(Latency < X)")

plt.grid(True)

plt.xticks([200, 400, 600, 800, 1000, 1200, 1400], ["200ms", "400ms", "600ms", "800ms", "1s", "1.2s", "1.4s"])

plt.legend(loc="lower right")

plt.tight_layout()

plt.savefig("/home/long/leanstore/evaluation/tpcc/tpcc_latency.pdf")