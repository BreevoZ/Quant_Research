#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
单变量指数核 Hawkes 过程 —— 自实现(无外部库),O(N) 对数似然递推。

强度: λ(t) = μ + Σ_{tᵢ<t} α·β·e^(−β(t−tᵢ))     (核积分=α, 归一使 α=分支比)
参数: μ 基础到达率(外生) | α 分支比/自激发幅度(内生占比, <1 才平稳) | β 衰减率(1/记忆时长, 秒⁻¹)

对数似然(Ozaki 递推, O(N)):
  logL = Σ log(μ + α·β·R_i) − μ·T − α·Σ(1−e^(−β(T−tᵢ)))
  其中 R_i = e^(−β(tᵢ−t_{i-1}))·(1 + R_{i-1}), R_0=0     ← 递推免 O(N²)

用途: 拟合后 α=自激发强度、β=记忆衰减、branching=α 都可当因子;
      还能算任意时刻的瞬时强度 λ(t)(当前市场"有多热"的内生部分)。
"""
import numpy as np
from scipy.optimize import minimize


def _neg_loglik(params, t, T):
    mu, alpha, beta = params
    if mu <= 0 or alpha <= 0 or beta <= 0 or alpha >= 1:
        return 1e10
    n = len(t)
    # R_i 递推
    R = np.empty(n)
    R[0] = 0.0
    dt = np.diff(t)
    for i in range(1, n):
        R[i] = np.exp(-beta * dt[i-1]) * (1.0 + R[i-1])
    lam = mu + alpha * beta * R
    if np.any(lam <= 0):
        return 1e10
    term1 = np.sum(np.log(lam))
    term2 = mu * T
    term3 = alpha * np.sum(1.0 - np.exp(-beta * (T - t)))
    ll = term1 - term2 - term3
    return -ll


def fit(t, T=None):
    """拟合 Hawkes(t: 到达时刻升序, 秒). 返回 dict(mu, alpha, beta, branching, loglik, ok)."""
    t = np.asarray(t, float)
    t = t - t[0]                                  # 平移到 0 起点(数值稳定)
    if T is None:
        T = t[-1]
    n = len(t)
    if n < 50:
        return dict(ok=False)
    # 初值: μ≈半数事件外生, β≈1/中位间隔
    med_dt = np.median(np.diff(t))
    x0 = [0.5 * n / T, 0.3, 1.0 / max(med_dt, 1e-3)]
    bnds = [(1e-6, None), (1e-4, 0.999), (1e-3, 1e4)]
    try:
        res = minimize(_neg_loglik, x0, args=(t, T), method="L-BFGS-B", bounds=bnds,
                       options=dict(maxiter=200))
        mu, alpha, beta = res.x
        return dict(ok=res.success or res.status == 0, mu=float(mu), alpha=float(alpha),
                    beta=float(beta), branching=float(alpha), loglik=float(-res.fun),
                    half_life_ms=float(np.log(2)/beta*1000))
    except Exception:
        return dict(ok=False)


def intensity_at(t_events, tq, mu, alpha, beta):
    """在查询时刻 tq(升序)算内生激发强度的自激发部分 Σα·β·e^(−β(tq−tᵢ)), 用于因子。"""
    t_events = np.asarray(t_events, float); tq = np.asarray(tq, float)
    out = np.empty(len(tq))
    j = 0; S = 0.0; last = None
    ti = 0
    # 简单 O(N+Q) 扫描: 维护到当前查询点为止的衰减和
    S = 0.0; prev = t_events[0]
    ei = 0
    for k, q in enumerate(tq):
        while ei < len(t_events) and t_events[ei] <= q:
            S = np.exp(-beta * (t_events[ei] - prev)) * S + 1.0
            prev = t_events[ei]; ei += 1
        # 从 prev 衰减到 q
        out[k] = mu + alpha * beta * (np.exp(-beta * (q - prev)) * S if ei > 0 else 0.0)
    return out


if __name__ == "__main__":
    import sys, time
    from agg_events import extract
    tic = sys.argv[1] if len(sys.argv) > 1 else "000001"
    ev = extract(tic)
    for name, e in ev.items():
        t0 = time.time()
        r = fit(e["t"])
        if r["ok"]:
            print(f"[{name}] N={len(e['t']):,} 拟合 {time.time()-t0:.1f}s | "
                  f"μ={r['mu']:.2f}/s α(分支比)={r['alpha']:.3f} β={r['beta']:.2f}/s "
                  f"半衰期={r['half_life_ms']:.0f}ms")
        else:
            print(f"[{name}] 拟合失败")
