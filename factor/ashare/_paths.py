"""各脚本共用的路径配置"""
import os

from qr import paths

# 路径来自 qr/paths.py(环境变量 → qr.toml → 默认值), 不再写死在代码里。
LOB_ROOT  = str(paths.get('lob_root'))
FEAT_ROOT = str(paths.get('feat_root'))
RES_ROOT  = str(paths.get('res_root'))

def feat_dir(date):  return os.path.join(FEAT_ROOT, date)
def res_dir(date):   return os.path.join(RES_ROOT, date)
def plot_dir(date):  d = os.path.join(RES_ROOT, date, 'plots');    os.makedirs(d, exist_ok=True); return d
def gbt_dir(date):   d = os.path.join(RES_ROOT, date, 'group_bt'); os.makedirs(d, exist_ok=True); return d
