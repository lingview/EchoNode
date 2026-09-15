// stb_image_write 的唯一实现翻译单元。
// ScreenshotExecutor（JPEG 截图）与 RemoteDeskExecutor（JPEG 桌面帧 / PNG 光标）
// 共用 stb 编码函数；实现宏只能在一个 .cpp 里定义，否则链接期重复符号。
// 凡链接上述任一执行器的目标都须一并链接本文件。
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
