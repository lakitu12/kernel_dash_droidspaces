# kernel_dash_droidspaces

GKI android15-6.6 自定义内核 — Redmi Turbo 5 Max (dash, MT6991Z, 天玑 9500s)。

DroidSpaces（宿主容器）+ SukiSU Ultra（KPM + SUSFS）。仓库结构/方法论复用
[kernel_marble_droidspaces](https://github.com/lakitu12/kernel_marble_droidspaces)
（marble 5.10.236 同配方已实机验证 7 轮 CI + 开机）。

## Stock 基线（实机 boot.img 实测，2026-08-30）

| 项 | 值 | 来源 |
|---|---|---|
| 内核版本 | `6.6.118-android15-8-gc44b714366cc-abogki519650608-4k` | extract-ikconfig + strings，本地 stock boot.img |
| GKI 代 | **GKI 3.0**（boot.img header v4, RAMDISK_SZ=0, kernel LZ4） | magiskboot 头解析 |
| `abogki` 前缀 | MTK 用 AOSP GKI 产物做的 fork 构建 | 版本字符串 |
| KMI | `android15-8`（KMI generation 8） | 版本字符串 |
| page size | 4K（`CONFIG_ARM64_PAGE_SHIFT=12`，`-4k` 后缀） | ikconfig |
| LTO | **`CONFIG_LTO_NONE=y`**（与 marble 的 LTO_FULL 不同！） | ikconfig |
| CFI | `CONFIG_CFI_CLANG=y` | ikconfig |
| MODVERSIONS | y + `MODULE_SIG_PROTECT=y` | ikconfig |
| ANDROID_KABI | `ANDROID_KABI_RESERVE=y`（kABI 补丁有保留区可用） | ikconfig |
| SCSS | `CONFIG_SHADOW_CALL_STACK=y` | ikconfig |
| 源码 tag 对应 | `android15-6.6-2026-01_r1`（Makefile SUBLEVEL=118 已核实） | AOSP common |

`c44b714366cc` 本身不在 AOSP common 上（MTK fork 内部提交），但 base tag 锁
`android15-6.6-2026-01_r1`（同为 6.6.118）。

## "小米 6.6/6.12 不能直接刷 GKI" 传言考证

**结论：传言把三类真实失败混为一谈，6.6 设备刷自建 GKI 内核是成熟路线
（WildKernels 对 android15-6.6 有完整 release 矩阵；官方 DroidSpaces 文档
明确支持 5.4–6.12+），dash 这台没有任何结构性障碍。**

| 传言里的"不能刷" | 真实原因 | 对 dash |
|---|---|---|
| 刷官方 GKI boot（如 KernelSU GKI 包）不开机 | 官方 GKI 是 AOSP 全量内核 + GKI 模块（新 Android 15/16 设备拆到 `system_dlkm`/`vendor_kernel_boot`，还带 Google 签名/MODULE_SIG_PROTECT），直接刷与设备系统不配套 | **不刷官方 boot**，只替换 boot 内的 kernel 载荷（AK3 split_boot），ramdisk/vendor_boot 一律不动；本内核配置贴 stock ikconfig |
| 5.10 时代"必须 LTO_THIN 不能 LTO_NONE" | marble stock 是 LTO_FULL+CFI，LTO_NONE 改变 codegen → vendor_dlkm CRC 失配 → 无限重启（实测两轮） | dash stock 本来就是 **LTO_NONE** → 照抄 stock 反而最安全；CFI 保持 =y |
| 开 SYSVIPC 等就 brick | kABI：task_struct 偏移变了 vendor 模块即崩 | DroidSpaces 官方 kABI 补丁（6.6 用 below-6.12 的 `001..._6_7_8` 变体，走 `ANDROID_KABI_RESERVE` padding，dash 有保留区）+ 仅启用官方认证的 GKI-safe 配置集 |

真正的 dash 特有风险与对策（workflow 已内置）：

1. **KMI 尾缀 `-android15-8` 必须保持**：vermagic 首段不参与 MODVERSIONS 匹配，
   但 KMI 8 的符号列表/CRC 环境必须与 stock 一致 → 钉 2026-01_r1 tag。
2. **MTK fork 漂移**：`abogki519650608` 与 AOSP tag 之间的差异不可枚举 →
   靠 ikconfig 断言（LTO_NONE/CFI/SYSVIPC…）+ 首次刷机守 A/B 槽回退。
3. **MODULE_SIG_PROTECT=y**：内核自身签名保护仅约束加载 GKI 模块
   （system_dlkm 场景）；本路线不动 system_dlkm/vendor_dlkm，无需处理。
4. **system_dlkm/vendor_kernel_boot**：GKI 3.0 新分区，本路线完全不触碰。

## Workflow

`.github/workflows/build.yml`（marble 模板改造，差异点见文件内注释）：

- repo sync 钉 `common-android15-6.6-2026-01` + local_manifest 锁 tag
  `android15-6.6-2026-01_r1`，SUBLEVEL=118 门禁
- DroidSpaces kABI 补丁 `001.GKI-below-6.12-fix_sysvipc_kabi_6_7_8.patch`
  （6.6 属 below-6.12；无 002 MQUEUE 补丁——那是 ≤5.10 专属）
- gki_defconfig：官方 GKI-safe 集 + KPROBES/KALLSYMS；**LTO/CFI 保持 stock**
- 版本尾巴：`.scmversion` 在 6.x 已死（setlocalversion 不再读）→ 改
  `CONFIG_LOCALVERSION="-droidspaces-lakitu"` → `6.6.118-android15-8-droidspaces-lakitu`
- SukiSU builtin 分支（SUSFS 符号必需）+ susfs4ksu `gki-android15-6.6`
  分支补丁（已核实存在）
- AK3：Numbersf 大写变量基底；dash boot 无 ramdisk → `flash_boot` 路径
- 产物：matrix `ksu` / `bypass` 双变体，Image + AK3 zip

## 首刷安全规程（A/B 守护，同 marble）

```bash
fastboot getvar current-slot           # 记住当前槽
fastboot flash boot_<当前槽> boot.img   # 只刷当前槽
# 开机验证 uname -r；异常时 fastboot --set-active <另一槽> 回滚
```

vbmeta：解锁后镜像未签名，若卡第一屏 →
`fastboot flash vbmeta_<槽> --disable-verity --disable-verification vbmeta.img`。

## DroidSpaces 要求核查（stock → 目标）

`droidspaces check` 致命项：PID_NS/MNT/UTS/IPC_NS/CGROUP_DEVICE/DEVTMPFS。
stock 缺失项（ikconfig 实测）：SYSVIPC、POSIX_MQUEUE、IPC_NS、PID_NS、DEVTMPFS、
USER_NS、TMPFS_XATTR/POSIX_ACL、NETFILTER_XT_MATCH_ADDRTYPE、IP_SET —
全部由本内核补齐；UTS_NS/OVERLAY_FS/SECCOMP/VETH/CGROUP_FREEZER/MEMCG stock 已有。
