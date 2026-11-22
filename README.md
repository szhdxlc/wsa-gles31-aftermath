# WSA GLES 3.1 攻坚残留版

> 目标：在 Windows Subsystem for Android (WSA 2407 / Android 13) 里运行要求 **OpenGL ES 3.1** 的手游（率土之滨）。
>
> 结果：**GLES 3.1 拿不到硬件加速，WSA 路线最终放弃**。本仓库保留过程中编译出的 ANGLE、移植工具与全部实测结论，供后续参考。

---

## 一句话结论

WSA 的图形栈是一个**闭环的闭源系统**：guest 侧 EGL 只认 `ro.hardware.egl` 指定的驱动，宿主侧只有微软自己写的 `gfxstream_backend.dll`（D3D11 翻译层，**硬编码 GLES 3.0**）。要让 guest 拿到 3.1，只能自己塞一个 ANGLE 进去——而 ANGLE 的 Vulkan 后端在 WSA 里只能接软件 Vulkan（SwiftShader），性能不可用。

**核心矛盾：能拿到的 3.1 是 CPU 渲染，能用 GPU 的路径只有 3.0。**

---

## WSA 图形栈解剖（实测）

```
guest app
  └─ /system/lib64/libEGL.so            ← Android 通用 EGL 加载器
       ├─ 读 ro.hardware.egl
       └─ 加载 /vendor/lib64/egl/libEGL_<值>.so
            默认 "emulation" → libEGL_emulation.so
              └─ gfxstream 协议 → 宿主 gfxstream_backend.dll → D3D11 → GPU

host (Windows)
  gfxstream_backend.dll   6.29 MB   仅导入 d3d9/d3d11/d3d12/dxgi + CRT
  libEGL.dll / libGLESv2.dll        ANGLE 2.1.9690（只给 WsaClient 自己的 Qt 界面用）
  vulkan_dzn.dll          5.63 MB   Dawn：Vulkan-on-D3D12（**guest 根本调不到**）
  vulkan-1.dll
```

关键点：

| 观察 | 证据 |
|---|---|
| guest 的 GLES 不走宿主 ANGLE | 换掉宿主 `libGLESv2.dll` 为 ANGLE 5.15，`WsaClient` 确实加载了，guest 的 GLES 版本依然 3.0 |
| Vulkan 是空壳 | `cmd gpu vkjson` → `{}`，`"devices": []`，即使开启了 D3D12 设置 |
| guest 没有 GPU 版 Vulkan HAL | `/vendor/lib64/hw/` 只有 `hwcomposer.windows.so`、`gralloc`，**没有 `vulkan.windows.so`**（加载器日志里反复出现它 not found） |
| 宿主 GLES 3.0 是硬编码的 | 无 `0x821B/0x821C` 常量可打；`GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS` 和 `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` 返回 `INVALID_ENUM (0x500)` |

---

## 六条路线与结果

| # | 路线 | 结果 | 死因 |
|---|---|---|---|
| 1 | Magisk 模块覆盖 `ro.opengles.version=196609` | ❌ 无效 | 只是改框架上报值，驱动仍是 3.0 |
| 2 | 把 DSemulator 的宿主 ANGLE 5.15 换进 WSA | ❌ 无效 | guest 不走宿主 ANGLE（协议直连 D3D11） |
| 3 | 移植 Android 15 模拟器的 `vulkan.ranchu.so`（1.7 MB 真 HAL） | ❌ 加载失败 | 缺 `std::__libcpp_verbose_abort`（Android 14+ 符号）；补上后又缺 `gfxstream::guest::ChecksumCalculator`——**ABI 已整体换代** |
| 4 | Bliss OS 的 ANGLE 2.1.1 + SwiftShader `vulkan.pastel.so` | ⚠️ 半成功 | 系统级 ES 3.1 稳定运行，但游戏一出现 SF 就崩：`Gralloc4 lock failed` → `EGL image err 0x300c` → `Unable to generate SkSurface`（旧 SwiftShader 缺 `VK_ANDROID_external_memory_android_hardware_buffer`） |
| 5 | **从源码编译新版 ANGLE + SwiftShader（本仓库产物）** | ⚠️ ANGLE 成功、SwiftShader 未编过 | 见下方「产物」 |
| 6 | 二进制补丁宿主 DLL：`OpenGL ES 3.0` → `3.1` | ⚠️ 字符串通过、能力不通过 | `GL_VERSION` 变成 3.1（游戏第一道门槛过了），但 `GL_MAJOR/MINOR` 仍是 3/0，且 compute/SSBO 查询返回 `INVALID_ENUM` |

### 路线 6 的实测数据（最有参考价值）

在 WSA 里用 NDK 编的 `glesprobe` 直接查询：

```
GL_VENDOR   : Microsoft
GL_RENDERER : Subsystem OpenGL ES Translator
GL_VERSION  : OpenGL ES 3.1          ← 补丁生效
GL_MAJOR_VERSION : 3
GL_MINOR_VERSION : 0                 ← 仍是 3.0
GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS : -1  (err 0x500 INVALID_ENUM)
GL_MAX_SHADER_STORAGE_BLOCK_SIZE      : -1  (err 0x500 INVALID_ENUM)
GL_MAX_TEXTURE_SIZE : 16384
```

**结论：3.1 只是个字符串，不是能力。**

---

## 产物

### 1. 交叉编译的 ANGLE（android-x86_64）

从 ANGLE 主线（`2.1.29576`，git hash `76729448eb7d`）交叉编译，Vulkan 后端：

```
libEGL_angle.so          1.45 MB
libGLESv2_angle.so      12.41 MB
libGLESv1_CM_angle.so    0.58 MB
```

依赖干净：只需 `libc/libdl/liblog/libm/libnativewindow`（Vulkan 通过 dlopen 动态加载）。

**可直接用于任何 Android 13 x86_64 环境**：放进 `/vendor/lib64/egl/`，设 `ro.hardware.egl=angle`。

已验证：在 WSA 上配合 SwiftShader ICD 可提供稳定的 **OpenGL ES 3.1**。

### 2. `tools/patchm.py` — Android ELF 符号补丁器

解决「新编译的 .so 在旧系统上 dlopen 失败」的通用工具：

- 解析 `.dynstr` / `.dynsym`
- 找出目标 .so 引用但参考 libc++ 里不存在的符号
- **重建字符串表并修正全部 `st_name` 偏移**（不是简单覆盖，因为替换名长度不同）
- 默认把缺失符号映射到 `abort`（bionic 必有）

实测：在 1.7 MB 的 `vulkan.ranchu.so` 上处理 1951 个符号，零错位。

用法：
```bash
python patchm.py target.so reference_libc++.so out.so
```

### 3. `tools/glesprobe.c` — guest 侧 GLES 能力探针

NDK 编译的单文件命令行程序，直接打印 `GL_VERSION`、`GL_MAJOR/MINOR`、compute/SSBO 上限。**用来识破"版本号造假"**。

### 4. `tools/vulkan_lvp_hal.c` — Android Vulkan HAL 包装器

把只导出 `vk_icdGetInstanceProcAddr` 的桌面 ICD（lavapipe / SwiftShader）包装成 Android 要求的 `HMI` 导出格式。

注意 `hw_device_t` 的 ABI 陷阱：LP64 下 `reserved[]` 是 `uint64_t[12]`（96 字节），`close` 必须是**最后一个**成员。

### 5. `workflow/build.yml` — GitHub Actions 交叉编译流水线

在 ubuntu runner 上完整跑通：

```
gclient config --name src --custom-var=checkout_android=True https://chromium.googlesource.com/angle/angle
gclient sync
gn gen out/Rel --args='target_os="android" target_cpu="x64" angle_enable_vulkan=true android_ndk_path=...'
autoninja -C out/Rel libEGL_angle.so libGLESv2_angle.so
```

踩过的 5 个坑（都已修好，可直接复用）：

1. `gclient config --name .` → **报错**，必须 `--name src`
2. Android 目标需要 `checkout_android=True`，否则 `third_party/android_sdk/BUILD.gn` 加载失败
3. 构建目标名带 `.so` 后缀（`libEGL_angle.so`），不是 `libEGL_angle`
4. 必须用 `autoninja`，直接用 `ninja` 会因 `AUTONINJA_BUILD_ID` 缺失而失败
5. SwiftShader 的 Android 编译缺 `vulkan/vk_android_native_buffer.h`，要从 `vulkan-deps` 或 Khronos 仓库复制过去

---

## 未完成的部分

`libvk_swiftshader.so` 没编出来（缺上面第 5 个坑的头文件，最后一次修复未验证）。

即使编出来，**它仍然是 CPU 软件渲染**——WSA 的瓶颈不在 HAL，在于「没有硬件 Vulkan」。

---

## 最后的建议

如果你只是要玩游戏：用支持 GPU 直通的模拟器（LDPlayer / MuMu / DSemulator），它们自带 ANGLE + D3D11 宿主渲染，GLES 3.1 和性能都有。

如果一定要 WSA：
- 它已停止支持（2025-03-05）
- GLES 天花板 = 3.0（硬编码，无法绕过）
- Vulkan = 空壳

**别在它身上花时间了。**

---

## 许可

工具与文档：MIT。ANGLE 二进制遵循其原始 BSD 许可（Chromium 项目）。
