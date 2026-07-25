#include "compositor_utils.h"
#include <algorithm>
#include <cmath>
#include <unistd.h>

uint64_t MakeSurfaceKey(uint32_t clientPid, uint32_t surfaceId)
{
    return (static_cast<uint64_t>(clientPid) << 32) | surfaceId;
}

uint32_t GetWaylandClientPid(wl_client* client)
{
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    if (client) wl_client_get_credentials(client, &pid, &uid, &gid);
    return pid > 0 ? static_cast<uint32_t>(pid) : 0;
}

bool IsTaskbarLike(int top, int height, int outputHeight)
{
    return height > 0 && height < compositor_consts::kTaskbarMaxHeight &&
           top + height >= outputHeight;
}

void BlitScaled(uint8_t* dst, int rootW, int rootH,
                const uint8_t* src, int srcStride, int srcW, int srcH,
                int dstX, int dstY, int dstW, int dstH, bool alphaBlend)
{
    if (!dst || !src || srcStride <= 0 || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    if (srcW > srcStride) srcW = srcStride;

    const int x0 = std::max(0, dstX), y0 = std::max(0, dstY);
    const int x1 = std::min(rootW, dstX + dstW), y1 = std::min(rootH, dstY + dstH);
    if (x1 <= x0 || y1 <= y0) return;

    const int64_t stepX = (static_cast<int64_t>(srcW) << 16) / dstW;
    const int64_t stepY = (static_cast<int64_t>(srcH) << 16) / dstH;
    const int64_t maxFx = static_cast<int64_t>(srcW - 1) << 16;
    const int64_t maxFy = static_cast<int64_t>(srcH - 1) << 16;

    std::vector<int> sx0(x1 - x0), sx1(x1 - x0), wx0(x1 - x0), wx1(x1 - x0);
    for (int i = 0; i < x1 - x0; ++i) {
        int64_t fx = static_cast<int64_t>(x0 + i - dstX) * stepX + (stepX >> 1) - (1 << 15);
        fx = std::max<int64_t>(0, std::min(maxFx, fx));
        sx0[i] = static_cast<int>(fx >> 16);
        sx1[i] = std::min(sx0[i] + 1, srcW - 1);
        wx1[i] = static_cast<int>((fx >> 8) & 0xFF);
        wx0[i] = 256 - wx1[i];
    }

    for (int y = y0; y < y1; ++y) {
        int64_t fy = static_cast<int64_t>(y - dstY) * stepY + (stepY >> 1) - (1 << 15);
        fy = std::max<int64_t>(0, std::min(maxFy, fy));
        const int sy = static_cast<int>(fy >> 16);
        const int sy1 = std::min(sy + 1, srcH - 1);
        const unsigned wy1 = static_cast<unsigned>((fy >> 8) & 0xFF);
        const unsigned wy0 = 256 - wy1;
        const uint8_t* row0 = src + static_cast<size_t>(sy) * srcStride * 4;
        const uint8_t* row1 = src + static_cast<size_t>(sy1) * srcStride * 4;
        uint8_t* drow = dst + (static_cast<size_t>(y) * rootW + x0) * 4;
        for (int i = 0; i < x1 - x0; ++i) {
            const uint8_t* p00 = row0 + sx0[i] * 4;
            const uint8_t* p01 = row0 + sx1[i] * 4;
            const uint8_t* p10 = row1 + sx0[i] * 4;
            const uint8_t* p11 = row1 + sx1[i] * 4;
            const unsigned w00 = static_cast<unsigned>(wx0[i]) * wy0;
            const unsigned w01 = static_cast<unsigned>(wx1[i]) * wy0;
            const unsigned w10 = static_cast<unsigned>(wx0[i]) * wy1;
            const unsigned w11 = static_cast<unsigned>(wx1[i]) * wy1;
            uint8_t* dpx = drow + i * 4;
            const unsigned b = (p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) >> 16;
            const unsigned g = (p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) >> 16;
            const unsigned r = (p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) >> 16;
            if (!alphaBlend) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
                continue;
            }
            const unsigned a = (p00[3] * w00 + p01[3] * w01 + p10[3] * w10 + p11[3] * w11) >> 16;
            if (a == 0) continue;
            if (a >= 255) {
                dpx[0] = static_cast<uint8_t>(b);
                dpx[1] = static_cast<uint8_t>(g);
                dpx[2] = static_cast<uint8_t>(r);
                dpx[3] = 255;
            } else {
                const unsigned inv = 255 - a;
                const unsigned nb = b + (dpx[0] * inv) / 255;
                const unsigned ng = g + (dpx[1] * inv) / 255;
                const unsigned nr = r + (dpx[2] * inv) / 255;
                dpx[0] = static_cast<uint8_t>(std::min(nb, 255u));
                dpx[1] = static_cast<uint8_t>(std::min(ng, 255u));
                dpx[2] = static_cast<uint8_t>(std::min(nr, 255u));
                dpx[3] = 255;
            }
        }
    }
}
