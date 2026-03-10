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

plt.ylim(11e6, 13e6)

plt.xticks([0,1,2,3], ["Trad", "Flush", "Trad_BDR", "Flush_BDR"])

# Eigene Y-Ticks (in Millionen)
plt.yticks([11.2e6, 11.6e6, 12.0e6, 12.4e6, 12.8e6], ["11.2M", "11.6M", "12.0M", "12.4M", "12.8M"])

plt.tight_layout()

# Plot speichern
plt.savefig("/home/long/leanstore/evaluation/tatp/tatp_throughput_128.pdf")
