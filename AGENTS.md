# qimgv fork — agent 操作手册

本文件存在的理由:凡是"需要记住"的规则都是隐性状态,而记忆是最差的存储。
这里的每一条都对应一次真实事故。改动本文件前先读完。

## 这是什么仓库

qimgv 的 fork(KDE/Qt6 优先,GPL),`rustcore` 分支是集成主干:Rust 引擎
(`engine/qimgv-engine*`)是目录与解码调度的唯一写者,C++ 侧是只读镜像与
Qt 渲染。架构规则由 `qimgv/tests/architecture_rules.py` 执行(R1 gui 隔离、
R2 FFI 说话者白名单、R3 rust 封印、R4 settings 解引用、R5 ABI 钉、R6 信号
重声明)——**结构规则不靠记忆,靠这 5 个测试套件**。

## 常用命令

```bash
# 构建(警告即合同:-Wall -Wextra -Wpedantic 零警告是常态)
cmake --build build -j$(nproc)

# 全部测试(5 套件;ARCH_RULES 是架构合同,失败先读它输出)
cd build && ctest

# 静态分析门禁(计数只许缩;新族即失败;降数须与修复同提交)
python3 scripts/lint.py build --gate

# clazy 独立构建树(build-clazy 已配置,clazy 1.17 + level0/level1/detaching)
CLAZY_CHECKS="level0,level1,detaching" cmake --build build-clazy -j$(nproc)
```

## 纪律(每条对应一次事故)

1. **bash 每条命令以 `cd /home/dropguard/try/qimgv` 开头。** cwd 跨调用持久;
   在 build/ 里跑下一条命令,相对路径全部碎裂。曾在一天内失败十余次。
2. **多 agent 并行时必须用 git worktree 分工**(共享一棵树 = 提交互相卷入 +
   分支被对方 amend,已发生)。单 agent 独占时不必。
3. **提交只落在 rustcore**(集成主干);特性工作开分支,完工合回。
   push 只在用户明说时执行。
4. **修复与钉住它的测试必须同一提交。** 检测延迟是学费的大头。
5. **不引入隐式上下文**:cwd 依赖、单例顺序依赖、未初始化成员、靠调用顺序
   才成立的协议——已在 CacheItem/settings/lint.py 各修过一次,别再新增。

## 已知债务(有基线,只许缩)

- `scripts/lint-baseline.txt`:静态分析计数,`--gate` 强制只缩不涨。
- settings 的 R4 DEBT 名单(architecture_rules.py 内):off-thread 解引用。
- suggest-override 等风格族:机械修一轮、基线同 commit 下调。

## 挂起决议(动了要说理由)

- 镜像读取路径:批量 arena 按性能账**保留**,arena 即存储(选项二)挂起,
  触发条件 = 百万级目录实测或 profile 打进 parseListing。
- `--gen-thumbs` 已砍;wallpaper 为 KDE 专属是**有意为之**(注释在
  Core::setWallpaper)。
- pimpl 不用:应用不是库,白盒可测性优先于隐藏。
