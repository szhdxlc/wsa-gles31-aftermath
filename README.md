# WSA 图形能力实测档案：GLES 3.1 / Vulkan 可行性研究

> 平台：Windows Subsystem for Android `2407.40000.4.0`（Android 13, x86_64）
>
> 结论：**WSA 的 GLES 天花板是 3.0（硬编码），Vulkan 是空壳（0 个设备）。两条路都走不通，本仓库保留实测数据、可复用工具与交叉编译产物。**

---

## 一、结论速览

| 问题 | 答案 |
|---|---|
| WSA 能提供 OpenGL ES 3.1 吗？ | 软件渲染可以，硬件加速不行 |
| 为什么不行？ | 宿主渲染器 `gfxstream_backend.dll` 闭源且硬编码 GLES 3.0，无法替换 |
| WSA 的 Vulkan 能用吗？ | **不能**。`vkjson` 返回 `{}`，`"devices": []` |
| 为什么 Vulkan 是空的？ | guest 侧没有 GPU 版 Vulkan HAL（`vulkan.windows.so` 压根不存在） |
| 有解吗？ | 除非微软重新实现宿主渲染器，否则无解 |

---

## 二、图形栈解剖（实测）

```
guest app
  └─ /system/lib64/libEGL.so                Android 通用 EGL 加载器
       ├─ 读 ro.hardware.egl（默认 "emulation"）
       └─ 加载 /vendor/lib64/egl/libEGL_<值>.so
            libEGL_emulation.so
              └─ gfxstream 协议 → 宿主 gfxstream_backend.dll → D3D11 → GPU

host (Windows)
  gfxstream_backend.dll    6.29 MB   仅导入 d3d9/d3d11/d3d12/dxgi + CRT
  libEGL.dll / libGLESv2.dll         ANGLE 2.1.9690（仅供 WsaClient 的 Qt 界面）
  vulkan_dzn.dll           5.63 MB   Dawn：Vulkan-on-D3D12（guest 无法触达）
  vulkan-1.dll
```

### 关键观察

| 观察 | 证据 |
|---|---|
| guest 的 GLES **不经过**宿主 ANGLE | 把宿主 `libGLESv2.dll` 换成 ANGLE 5.15 并确认 `WsaClient` 已加载，guest 侧 GLES 版本依然是 3.0 |
| Vulkan 为空 | `cmd gpu vkjson` → `{}`，即使开启 D3D12 设置 |
| 缺少 GPU Vulkan HAL | `/vendor/lib64/hw/` 只有 `hwcomposer.windows.so`、`gralloc`；加载器日志反复报 `vulkan.windows.so not found` |
| 宿主 GLES 3.0 硬编码 | DLL 内无 `0x821B/0x821C` 处理常量；`GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS` 与 `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` 均返回 `INVALID_ENUM (0x500)` |
| guest 内 `libvulkan_enc.so` 存在（4.2 MB） | 说明 guest Vulkan 编码器是齐的，缺的是宿主侧对接 |

---

## 三、让 guest 拿到 GLES 3.1 的六种尝试

| # | 方案 | 结果 | 失败原因 |
|---|---|---|---|
| 1 | Magisk 模块覆盖 `ro.opengles.version=196609` | ❌ | 只改框架上报值，驱动仍是 3.0 |
| 2 | 替换宿主 `libGLESv2.dll` 为桌面版 ANGLE | ❌ | guest 不走宿主 ANGLE，协议直连 D3D11 |
| 3 | 移植 Android 15 模拟器的 `vulkan.ranchu.so`（1.7 MB 真 HAL） | ❌ | 缺 `std::__libcpp_verbose_abort`（Android 14+ 符号）；补上后又缺 `gfxstream::guest::ChecksumCalculator` —— ABI 已整体换代 |
| 4 | 第三方 ANGLE 2.1.1 + SwiftShader `vulkan.pastel.so` | ⚠️ 半成功 | 系统级 ES 3.1 可稳定运行；但合成器在导入 gralloc buffer 时崩：`Gralloc4 lock failed` → `EGL image err 0x300c` → `Unable to generate SkSurface`。旧版 SwiftShader 缺 `VK_ANDROID_external_memory_android_hardware_buffer` |
| 5 | **源码交叉编译新版 ANGLE + SwiftShader** | ⚠️ ANGLE 成功 | 见「产物」；SwiftShader 未编完（仍为 CPU 软件渲染，性能不可用） |
| 6 | 二进制补丁宿主 DLL：`"OpenGL ES 3.0"` → `"3.1"` | ⚠️ 只骗过字符串 | 见下方实测数据 |

### 方案 6 的实测数据（最值得参考）

用 NDK 编译的探针在 guest 内直接查询：

```
GL_VENDOR   : Microsoft
GL_RENDERER : Subsystem OpenGL ES Translator
GL_VERSION  : OpenGL ES 3.1            ← 补丁生效
GL_MAJOR_VERSION : 3
GL_MINOR_VERSION : 0                   ← 实际仍是 3.0
GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS : -1  (err 0x500 INVALID_ENUM)
GL_MAX_SHADER_STORAGE_BLOCK_SIZE      : -1  (err 0x500 INVALID_ENUM)
GL_MAX_TEXTURE_SIZE : 16384
```

**结论：版本号可以伪装，能力不能。**

---

## 四、产物

### 1. 交叉编译的 ANGLE（android-x86_64）

ANGLE 主线 `2.1.29576`（git hash `76729448eb7d`），Vulkan 后端：

```
libEGL_angle.so          1.45 MB
libGLESv2_angle.so      12.41 MB
libGLESv1_CM_angle.so    0.58 MB
```

依赖干净：仅 `libc/libdl/liblog/libm/libnativewindow`（Vulkan 通过 dlopen 动态加载）。

**可用于任意 Android 13 x86_64 环境**：放入 `/vendor/lib64/egl/`，设置 `ro.hardware.egl=angle`。
配合软件 Vulkan ICD 可提供稳定的 OpenGL ES 3.1。

### 2. `tools/patchm.py` — Android ELF 符号补丁器

解决「新编译的 .so 在旧 Android 上 dlopen 失败」的通用工具：

- 解析 `.dynstr` / `.dynsym`
- 找出目标 .so 引用、但参考 libc++ 中不存在的符号
- **重建字符串表并修正全部 `st_name` 偏移**（替换名长度不同，不能简单覆盖）
- 默认把缺失符号映射到 `abort`（bionic 必有）

实测：在 1.7 MB 的 `vulkan.ranchu.so` 上处理 1951 个符号，零错位。

```bash
python patchm.py target.so reference_libc++.so out.so
```

### 3. `tools/glesprobe.c` — guest 侧 GLES 能力探针

单文件命令行程序，NDK 直接编译。打印 `GL_VENDOR/RENDERER/VERSION`、真实 `GL_MAJOR/MINOR`、compute/SSBO 上限。**用于识破"版本号造假"。**

### 4. `tools/vulkan_lvp_hal.c` — Android Vulkan HAL 包装器

把只导出 `vk_icdGetInstanceProcAddr` 的桌面 ICD（lavapipe / SwiftShader）包装为 Android 所需的 `HMI` 导出格式。

**ABI 陷阱**：LP64 下 `hw_device_t.reserved[]` 是 `uint64_t[12]`（96 字节），`close` 必须是最后一个成员，否则 `driver::OpenHAL()` 立即段错误。

### 5. `workflow/build.yml` — GitHub Actions 交叉编译流水线

在 ubuntu runner 上完整跑通 ANGLE 的 Android 交叉编译：

```
gclient config --name src --custom-var=checkout_android=True https://chromium.googlesource.com/angle/angle
gclient sync
gn gen out/Rel --args='target_os="android" target_cpu="x64" angle_enable_vulkan=true android_ndk_path=...'
autoninja -C out/Rel libEGL_angle.so libGLESv2_angle.so
```

已修复 5 个坑，可直接复用：

1. `gclient config --name .` 会报错，必须 `--name src`
2. Android 目标需要 `checkout_android=True`，否则 `third_party/android_sdk/BUILD.gn` 加载失败
3. 构建目标名带 `.so` 后缀（`libEGL_angle.so`），不是 `libEGL_angle`
4. 必须用 `autoninja`；直接 `ninja` 会因 `AUTONINJA_BUILD_ID` 缺失而失败
5. SwiftShader 的 Android 编译缺 `vulkan/vk_android_native_buffer.h`，需从 `vulkan-deps` 或 Khronos 仓库复制

---

## 五、给 WSA 用户与开发者的建议

**如果你的应用只要求 GLES 2.0 / 3.0**：WSA 完全够用，无需任何改动。

**如果要求 GLES 3.1+ 或 Vulkan**：不要在 WSA 上投入时间。

- WSA 已于 **2025-03-05 停止支持**
- GLES 天花板 = 3.0（硬编码，无法绕过）
- Vulkan = 空壳（无设备）
- 可用的替代方案：支持 GPU 直通的 Android 模拟器（LDPlayer / MuMu / DSemulator 等），它们自带宿主侧 ANGLE + D3D11，GLES 3.1 与性能都有

**如果你只是想提升 WSA 的图形能力**：唯一可行方向是替换 guest 的 EGL 驱动（本仓库的 ANGLE 产物），但只能接软件 Vulkan，性能不可接受。

---

## 六、未完成项

`libvk_swiftshader.so` 未能编出（缺上述第 5 条的头文件，最后一次修复未验证）。
即使编出，**它仍是 CPU 软件渲染** —— WSA 的瓶颈不在 HAL，而在于「没有硬件 Vulkan」。

---

## 七、许可

工具与文档：MIT。ANGLE 二进制遵循其原始 BSD 许可（Chromium 项目）。
