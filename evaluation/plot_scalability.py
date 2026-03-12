import pandas as pd
import matplotlib.pyplot as plt

plt.style.use("seaborn-v0_8-paper")
plt.rcParams.update({'font.size': 11})

plt.figure(figsize=(6,4))

df1 = pd.read_csv("/home/long/leanstore/evaluation/throughput/trad.csv")
df2 = pd.read_csv("/home/long/leanstore/evaluation/throughput/flush.csv")
df3 = pd.read_csv("/home/long/leanstore/evaluation/throughput/bdr_trad.csv")
df4 = pd.read_csv("/home/long/leanstore/evaluation/throughput/bdr_flush.csv")

plt.plot(df1["threads"], df1["txn/s"], label="Trad", linewidth=2, marker='o', markersize=5)
plt.plot(df2["threads"], df2["txn/s"], label="Flush", linewidth=2, marker='o', markersize=5)
plt.plot(df3["threads"], df3["txn/s"], label="Trad_BDR", linewidth=2, marker='o', markersize=5)
plt.plot(df4["threads"], df4["txn/s"], label="Flush_BDR", linewidth=2, marker='o', markersize=5)

plt.xlabel("Number of worker threads")
plt.ylabel("Throughput [txn/s]")

plt.grid(True)

plt.xticks([1, 48, 96, 144, 192, 224])

plt.ylim(0, 9e6)

plt.yticks([1e6, 2e6, 3e6, 4e6, 5e6, 6e6, 7e6, 8e6], ["1M", "2M", "3M", "4M", "5M", "6M", "7M", "8M"])

plt.legend(loc="lower right")

plt.tight_layout()

plt.savefig("/home/long/leanstore/evaluation/throughput/scalability.pdf")