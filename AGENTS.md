# qimgv fork — agent 操作手册

本文件存在的理由:凡是"需要记住"的规则都是隐性状态,而记忆是最差的存储。
这里的每一条都对应一次真实事故。改动本文件前先读完。

## 这是什么仓库

qimgv 的 fork(KDE/Qt6 优先,GPL),`rustcore` 分支是集成主干:Rust 引擎
(`engine/qimgv-engine*`)是目录与解码调度的唯一写者,C++ 侧是只读镜像与
Qt 渲染。mirror 的内容与顺序都由引擎决定:mutation 事件携带引擎自序索引与
watcher 已 stat 的数据,C++ 只做机械拼接与替换(收敛契约 =
`mirrorStaysAFaithfulProjectionOfTheEngineListing`)。已知的两个本地例外,
均有注释说明理由:forceInsert 的同步 push_back(Core 打开路径需要 index
立即可用),与 setSubdirsSync 的旁路扫描。架构规则由
`qimgv/tests/architecture_rules.py` 执行(R1 gui 隔离、R2 FFI 说话者白名单、
R3 rust 封印、R4 settings 解引用、R5 ABI 钉、R6 信号重声明)——**结构规则
不靠记忆,靠这 5 个测试套件**。

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

# 消毒树(独立目录,默认构建不变):ASan+UBSan 抓堆错误/泄漏/UB。
# 注意:断言全过时套件仍可能因 LSan 退出泄漏而红——证据在
# Testing/Temporary/LastTest.log,不在 ctest 输出里
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQIMGV_SANITIZERS=address,undefined
cmake --build build-asan -j4 && cd build-asan && ASAN_OPTIONS=detect_leaks=1 ctest
```

## 纪律(每条对应一次事故)

1. **bash 每条命令以 `cd /home/dropguard/try/qimgv` 开头。** cwd 跨调用持久;
   在 build/ 里跑下一条命令,相对路径全部碎裂。曾在一天内失败十余次。
2. **提交只落在 rustcore**(集成主干);特性工作开分支,完工合回。
   push 只在用户明说时执行。
3. **修复与钉住它的测试必须同一提交。** 检测延迟是学费的大头。
4. **不引入隐式上下文**:cwd 依赖、单例顺序依赖、未初始化成员、靠调用顺序
   才成立的协议——已在 CacheItem/settings/lint.py 各修过一次,别再新增。
5. **签名类修复必须带着函数体走。** f8e8b216 只改签名行就宣称"纯改名无行为
   变化",实际制造了六处行为回归(含 FSEntry 静默丢数据)。改参数名/局部名时
   `git show` 完整 diff 逐行自查函数体引用;shadow 清零不等于行为不变——家族
   之间会互相制造(修 shadow 造出 unused-parameter)。凡行为可能变的修复,按
   纪律 3 钉住。
6. **只修能编译的路径;事实平台只有 Linux。** 引擎 watcher 是 inotify(整条
   `cfg(target_os = "linux")`),非 Linux 时事件通道是 `never()`,C++ 的后备
   `QFileSystemWatcher` 已随迁移删除(全仓零命中),`inotify`/`libc` 还是无条件
   依赖——别的平台过不了构建。README 的 `## Windows`/`## BSD`、19 个带
   `Q_OS_WIN`/`_WIN32` 的文件和 `supportsWatching()`(定义有、调用者无)都是上游
   继承的痕迹,不是本分支的承诺;曾有 agent 给一条 `cfg(windows)` 分支"顺手补了缺失的 import"并
   写进提交信息,而那段代码在 crate 层面就够不着。要动别的平台先拿事实:
   `rustup target add x86_64-pc-windows-gnu` + `cargo check --target ...`
   (check 不链接,编译验证在这里做得到,做不到的只有运行时)。
7. **state 的锁不得跨等待持有。** guard 活到所在块结束,不是分号;轮询循环
   里抱着读锁 sleep,std 的 RwLock 无写者优先,引擎写者被饿死整个 deadline
   (b583e1aa 的验收测试就是这么红的,trace 加了时间戳才照出来)。正确参照:
   同文件里 wait_for 的写法。
8. **统计式测量("跑 N 遍数失败")只是诊断桥,不是交付物。** 它回答"多频
   繁",不回答"为什么";必须尽快收尾成确定性产物:proptest 收缩出的种子、
   注入式复现(applyArena/arenaFor 这道缝就是为此拆的——喂 mirror 一份引擎
   不持有的 listing,不赌文件系统时机),或 R7 式结构规则。失败必须留名:
   1/200 那次连失败者是谁都没留下,留不下名字的测量等于没测。

## 已知债务(有基线,只许缩)

- `scripts/lint-baseline.txt`:静态分析计数,`--gate` 强制只缩不涨。
- suggest-override / old-style-cast / shadow:已清零;unused-parameter 已升格
  `-Werror`(CMakeLists)。风格族不再主动扫荡:只缩不涨 + 新族即失败自动守门。

## 挂起决议(动了要说理由)

- 镜像读取路径:批量 arena 按性能账**保留**,arena 即存储(选项二)挂起,
  触发条件 = 百万级目录实测或 profile 打进 parseListing。
- `--gen-thumbs` 已砍;wallpaper 为 KDE 专属是**有意为之**(注释在
  Core::setWallpaper)。
- pimpl 不用:应用不是库,白盒可测性优先于隐藏。
- 反模式燃烧阶段已于 2026-11 关闭:不再主动全库扫风格族。策略冻结族
  (conversion/float-conversion/passedByValue/duplInheritedMember)已退役出
  扫描(lint.py 的 FROZEN 与 REPORT 裁剪),理由留在 lint-baseline.txt 头部。
  重启条件:无——新族即失败与只缩不涨已经自动守门,重启只在有人拿实测证据
  证明某个冻结族值得修时发生。
- state 锁的机器化守卫(行级启发式 R7)于 2026-11-05 否决:启发式扫描把
  规则文件的 green 从"验证过"降级成"抽样过",其余六条的承诺因此被稀释。
  根治路径是 with_state 闭包(guard 不可逃逸,"持锁睡觉"必须显式写进闭包
  体),待测试面下次变动时评估;在那之前靠纪律 7 的散文与 wait_for 参照。
- 所有权战役于 2026-11-05 关闭:**可变者唯一所有,共享者只读,依赖者具名,
  跨界者带数据**。最后一块(R4 DEBT:thumbnailerrunnable/documentinfo/
  imagestatic/video 的 off-thread settings 读取)已搬进 LoadPolicy 与请求
  参数,DEBT 清零,R4 成为纯断言。重启条件:新事故。
- 消毒树首轮(2026-11-05)收获已修:CentralWidget::mode 与
  OverlayWidget::position 未初始化枚举读、qApp 错误下转型(测试跑的是
  QGuiApplication)、Cache 无析构泄漏全部条目。挂账:测试退出仍有 ~58KB
  对象图泄漏(9 Scaler + 6 DirectoryModel + 连接分配,fixture 环/所有权
  嫌疑);TSan 树未建(Rust 需 -Zsanitizer 才有完整覆盖,且不能与 ASan
  同树)。
