import pandas as pd
import matplotlib.pyplot as plt

# CSV laden
df = pd.read_csv("/home/long/leanstore/evaluation/tatp/throughput.csv")

plt.style.use("seaborn-v0_8-paper")
plt.rcParams.update({'font.size': 11})

plt.figure(figsize=(6,4))

# Balkenplot
plt.bar(df['version'], df['txn/s'], color=['blue', 'orange', 'green', 'red'], edgecolor='black', linewidth=2 )

# Achsenbeschriftung
plt.ylabel("Throughput (txn/s)")

plt.ylim(1.14e7, 1.30e7)

plt.xticks([0,1,2,3], ["Trad", "Flush", "Trad_BDR", "Flush_BDR"])

# Eigene Y-Ticks (in Millionen)
plt.yticks([1.14e7, 1.18e7, 1.22e7, 1.26e7, 1.30e7], ["11.4M", "11.8M", "12.2M", "12.6M", "13.0M"])

plt.tight_layout()

# Plot speichern
plt.savefig("/home/long/leanstore/evaluation/tatp/tatp_throughput.pdf")
