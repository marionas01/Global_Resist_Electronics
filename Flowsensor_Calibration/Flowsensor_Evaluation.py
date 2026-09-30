import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

data = pd.read_csv("Daten/flowlog_027.csv")
data["millis_ms"] = data["millis_ms"]/(10000*60)
data.plot("millis_ms", "ch1_flow_mean_ulmin")
plt.show()
print(data)
