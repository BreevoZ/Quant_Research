#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
纯 numpy 实现的 GRU 序列回归器(无外部深度学习框架依赖)。

结构:  x[B,T,F] → Dense+ReLU(嵌入) → GRU(hidden) → 取末隐状态 → Dense → 标量
优化:  Adam; 损失 MSE(目标已是截面 rank, 直接回归即可)

GRU 单元(标准公式):
  z_t = σ(x_t W_z + h_{t-1} U_z + b_z)          更新门: 新信息占多少
  r_t = σ(x_t W_r + h_{t-1} U_r + b_r)          重置门: 旧记忆保留多少
  n_t = tanh(x_t W_n + r_t*(h_{t-1} U_n) + b_n) 候选状态
  h_t = (1-z_t)*n_t + z_t*h_{t-1}               混合

为什么 GRU 而非 CNN: 递归结构使近期事件对末隐状态影响更大(旧信息逐步衰减),
与"微观信号快速衰减"的先验一致; CNN 平移不变, 分不清"刚发生"与"很久前"。
"""
import numpy as np


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -30, 30)))


class GRURegressor:
    def __init__(self, f_in, hid=48, seed=7):
        rng = np.random.default_rng(seed)
        s = lambda a, b: rng.standard_normal((a, b)).astype(np.float32) * np.sqrt(2.0/a)
        H = hid
        self.p = dict(
            We=s(f_in, H), be=np.zeros(H, np.float32),                    # 逐事件嵌入
            Wz=s(H, H), Uz=s(H, H), bz=np.zeros(H, np.float32),           # 更新门
            Wr=s(H, H), Ur=s(H, H), br=np.zeros(H, np.float32),           # 重置门
            Wn=s(H, H), Un=s(H, H), bn=np.zeros(H, np.float32),           # 候选
            Wo=s(H, H//2), bo=np.zeros(H//2, np.float32),                 # 输出头
            Wy=s(H//2, 1), by=np.zeros(1, np.float32),
        )
        self.H = H
        self.m = {k: np.zeros_like(v) for k, v in self.p.items()}         # Adam 一阶
        self.v = {k: np.zeros_like(v) for k, v in self.p.items()}         # Adam 二阶
        self.t = 0

    def forward(self, X, cache=False):
        """X[B,T,F] → pred[B]; cache=True 时保存反向传播所需中间量"""
        p = self.p
        B, T, _ = X.shape
        E = np.maximum(X @ p["We"] + p["be"], 0.0)                        # ReLU 嵌入 [B,T,H]
        h = np.zeros((B, self.H), np.float32)
        cs = [] if cache else None
        for t in range(T):
            x = E[:, t]
            z = sigmoid(x @ p["Wz"] + h @ p["Uz"] + p["bz"])
            r = sigmoid(x @ p["Wr"] + h @ p["Ur"] + p["br"])
            hu = h @ p["Un"]
            n = np.tanh(x @ p["Wn"] + r * hu + p["bn"])
            h_new = (1 - z) * n + z * h
            if cache:
                cs.append((x, h, z, r, n, hu))
            h = h_new
        o = np.maximum(h @ p["Wo"] + p["bo"], 0.0)                        # ReLU
        y = (o @ p["Wy"] + p["by"]).ravel()
        if cache:
            self._c = (X, E, cs, h, o)
        return y

    def backward(self, y_true, y_pred):
        """MSE 损失的反向传播, 返回梯度 dict"""
        X, E, cs, hT, o = self._c
        p = self.p
        B, T, _ = X.shape
        g = {k: np.zeros_like(v) for k, v in p.items()}
        dy = (2.0 * (y_pred - y_true) / B).astype(np.float32)[:, None]    # [B,1]
        g["Wy"] = o.T @ dy; g["by"] = dy.sum(0)
        do = dy @ p["Wy"].T
        do = do * (o > 0)                                                 # ReLU 反传
        g["Wo"] = hT.T @ do; g["bo"] = do.sum(0)
        dh = do @ p["Wo"].T                                               # [B,H]
        dE = np.zeros_like(E)
        for t in range(T-1, -1, -1):
            x, h_prev, z, r, n, hu = cs[t]
            dn = dh * (1 - z) * (1 - n*n)
            dz = dh * (h_prev - n) * z * (1 - z)
            g["Wn"] += x.T @ dn; g["Un"] += h_prev.T @ (dn * r); g["bn"] += dn.sum(0)
            g["Wz"] += x.T @ dz; g["Uz"] += h_prev.T @ dz; g["bz"] += dz.sum(0)
            dr = dn * hu * r * (1 - r)
            g["Wr"] += x.T @ dr; g["Ur"] += h_prev.T @ dr; g["br"] += dr.sum(0)
            dx = dn @ p["Wn"].T + dz @ p["Wz"].T + dr @ p["Wr"].T
            dE[:, t] = dx
            dh = (dh * z + (dn * r) @ p["Un"].T + dz @ p["Uz"].T + dr @ p["Ur"].T)
        dE = dE * (E > 0)
        g["We"] = np.einsum("btf,bth->fh", X, dE, optimize=True)
        g["be"] = dE.sum((0, 1))
        return g

    def step(self, g, lr=2e-3, b1=0.9, b2=0.999, eps=1e-8, clip=5.0):
        """Adam 更新(带梯度裁剪)"""
        self.t += 1
        gn = np.sqrt(sum(float((v**2).sum()) for v in g.values())) + 1e-12
        scale = min(1.0, clip / gn)
        for k in self.p:
            gk = g[k] * scale
            self.m[k] = b1*self.m[k] + (1-b1)*gk
            self.v[k] = b2*self.v[k] + (1-b2)*gk*gk
            mh = self.m[k] / (1 - b1**self.t)
            vh = self.v[k] / (1 - b2**self.t)
            self.p[k] -= (lr * mh / (np.sqrt(vh) + eps)).astype(np.float32)

    def fit_epoch(self, X, y, batch=512, lr=2e-3, rng=None):
        rng = rng or np.random.default_rng(0)
        idx = rng.permutation(len(X))
        tot = 0.0
        for i in range(0, len(idx), batch):
            b = idx[i:i+batch]
            pred = self.forward(X[b], cache=True)
            tot += float(((pred - y[b])**2).sum())
            self.step(self.backward(y[b], pred), lr=lr)
        return tot / len(idx)

    def predict(self, X, batch=4096):
        return np.concatenate([self.forward(X[i:i+batch]) for i in range(0, len(X), batch)])
