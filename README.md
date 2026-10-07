# qi-minitools

一组 Windows 桌面小工具，纯 Win32 C + GDI+ 编写，无运行时依赖，每个工具编译为独立的静态链接 exe，可以单独拷贝到任何机器上运行。

![logo](logo.svg)

## 工具一览

| 工具 | 说明 |
|------|------|
| `envtool.exe` | 环境变量编辑器，内置开发环境预设 |
| `picker.exe` | 屏幕取色器 |
| `shot.exe`   | 截图工具，带标注编辑器 |
| `slice.exe`  | 图片切图工具 |

### envtool — 环境变量编辑器

以列表方式编辑用户 / 系统两个作用域的环境变量，支持过滤和查找，`Ctrl+S` 保存并广播 `WM_SETTINGCHANGE`，新开的进程立即可见。

内置常见开发环境的预设（JDK、Android SDK、Node.js、Go、Rust、Python、Maven、Gradle、Flutter、Zig 等），会自动探测本机安装位置并一键写入 `XXX_HOME` / `PATH`。带托盘图标和全局热键。

### picker — 屏幕取色器

全局热键唤起吸管，放大镜辅助精确取色；主窗口显示当前颜色、色轮、调色板和历史记录。

支持 11 种颜色格式，逐行一键复制：`HEX`、`HEXA`、`RGB`、`RGBA`、`HSL`、`HSV`、`CMYK`、`CIE-LAB`、`CIE-XYZ`、`CSS`、`DEC`。

### shot — 截图工具

常驻托盘，全局热键（可自定义，冲突时自动检测）触发截屏，支持延迟 3/5 秒。

截图后进入内置编辑器，可添加：画笔、直线、矩形、椭圆、箭头、文字、马赛克、模糊。完成后输出到剪贴板（含 `CF_HTML`，可直接粘贴到富文本编辑器）或保存为 PNG 文件。

### slice — 图片切图工具

打开一张图（JPG / PNG），在画布上直接拖线切片：左键拖动画切割线、按住边框拖动调整网格、右键（或 Ctrl+左键）框选多个单元格合并，没有需要来回切换的工具面板。

每个叶子区域按 `<前缀>_<序号>.png` 批量导出，序号按导出顺序零填充，文件列表可直接排序。也支持命令行批量切图：

```text
slice.exe --split <图片> --out <输出目录> [--prefix <前缀>]
```

## 构建

需要 [Zig](https://ziglang.org/) 工具链（默认路径 `F:\sdk\zig\zig.exe`，不在该路径时会回退到 PATH 中的 zig）。

```powershell
.\build.ps1                # 构建全部四个 exe 到 .\bin
.\build.ps1 -Only shot     # 只构建 shot.exe
.\build.ps1 -Dbg           # 未优化 + 调试信息
.\build.ps1 -RegenIcon     # 先从 logo.svg 重新渲染 res\logo-*.ico
```

公共代码（`src/common`）以静态方式链接进每个 exe，产物之间互不依赖。

## 测试

```powershell
.\test.ps1                 # 全部自测 + 探针
.\test.ps1 shot            # 只跑 shot 的测试
.\test.ps1 -SelfTestOnly   # 跳过需要真实桌面的探针（秒级完成）
.\test.ps1 -List           # 只打印测试计划
```

单条修改的快速循环：

```powershell
.\build.ps1 -Only shot ; .\test.ps1 shot
```

部分探针（托盘、编辑器）会移动真实鼠标并抢占前台，需要在有真实桌面的环境运行，无法跳过的探针在无桌面环境下会报 FAILED。

## 目录结构

```text
src/
  common/     共享代码：托盘、托盘图标、UIKit、窗口工具、关于框
  envtool/    环境变量编辑器（注册表读写、预设自动探测）
  picker/     取色器（吸管、色轮、调色板、格式转换）
  shot/       截图（热键、截屏、编辑器、输出）
  slice/      切图（画布、切片树、图像导出）
res/          各工具的图标
test/         自测与进程外探针脚本
build.ps1     构建脚本
test.ps1      测试脚本
```
