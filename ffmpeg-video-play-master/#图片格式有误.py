#图片格式有误.py

import cv2
import numpy as np
from matplotlib import pyplot as plt

# 中文标题正常显示（Windows自带微软雅黑/黑体）
plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei']
plt.rcParams['axes.unicode_minus'] = False

# ===================== 1. 加载原图 =====================
# 替换成你的图片路径，建议用带蓝天、人脸、红色物体的图
img_origin = cv2.imread(r"C:\Users\Admin\Desktop\R-C.png")
if img_origin is None:
    raise FileNotFoundError("无法读取图片，请检查路径是否存在")
h, w = img_origin.shape[:2]
# NV12/I420格式要求宽高为偶数，奇数则裁掉最后一行/列（不影响演示效果）
if h % 2 or w % 2:
    h -= h % 2
    w -= w % 2
    img_origin = img_origin[:h, :w]
img_origin_rgb = cv2.cvtColor(img_origin, cv2.COLOR_BGR2RGB)

# ===================== 2. BGR 误当作RGB解析（红蓝互换） =====================
# OpenCV读出来是BGR，直接当作RGB显示，不做转换
img_bgr_as_rgb = img_origin.copy()

# ===================== 3. YUV UV颠倒（NV12当成NV21，交换U/V） =====================
# OpenCV没有BGR直接转NV12的转换码，先转YUV三通道，再手动下采样并交错成NV12布局
img_yuv3 = cv2.cvtColor(img_origin, cv2.COLOR_BGR2YUV)
y_plane = img_yuv3[:, :, 0]
u_plane = cv2.resize(img_yuv3[:, :, 1], (w//2, h//2))
v_plane = cv2.resize(img_yuv3[:, :, 2], (w//2, h//2))
uv_interleaved = np.empty((h//2, w), dtype=np.uint8)
uv_interleaved[:, 0::2] = u_plane
uv_interleaved[:, 1::2] = v_plane
# 模拟NV12被当成NV21：U/V位置互换（必须copy，否则numpy视图别名会导致U平面数据丢失）
uv_swapped = uv_interleaved.copy()
uv_swapped[:, 0::2], uv_swapped[:, 1::2] = uv_swapped[:, 1::2].copy(), uv_swapped[:, 0::2].copy()
img_yuv_swap_uv = np.vstack([y_plane, uv_swapped])
img_uv_swap_rgb = cv2.cvtColor(img_yuv_swap_uv, cv2.COLOR_YUV2RGB_NV12)

# ===================== 4. 模拟花屏 stride错误，内存行读取错位 =====================
img_corrupt = img_origin_rgb.copy()
# 模拟行偏移错误：每一行横向偏移，产生撕裂马赛克
for row in range(0, h, 8):
    shift = (row * 20) % w
    img_corrupt[row:row+8, :, :] = np.roll(img_corrupt[row:row+8, :, :], shift, axis=1)

# ===================== 5. 模拟绿屏：解码器失败填充绿色 =====================
img_green_screen = np.full_like(img_origin_rgb, [0, 255, 0]) # RGB纯绿色

# ===================== 绘图展示 =====================
plt.figure(figsize=(20, 8), dpi=100)
plt.subplot(1,5,1)
plt.title("原图 正常", fontsize=12)
plt.imshow(img_origin_rgb)
plt.axis('off')

plt.subplot(1,5,2)
plt.title("BGR当作RGB(红蓝互换)", fontsize=12)
plt.imshow(img_bgr_as_rgb)
plt.axis('off')

plt.subplot(1,5,3)
plt.title("YUV UV颠倒(NV12当NV21)", fontsize=12)
plt.imshow(img_uv_swap_rgb)
plt.axis('off')

plt.subplot(1,5,4)
plt.title("花屏 stride/内存错位", fontsize=12)
plt.imshow(img_corrupt)
plt.axis('off')

plt.subplot(1,5,5)
plt.title("绿屏 解码失败填充底色", fontsize=12)
plt.imshow(img_green_screen)
plt.axis('off')

plt.tight_layout()
plt.savefig("image_error_compare.png")
plt.show()