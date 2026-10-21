/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef GPU_SOFT_H
#define GPU_SOFT_H

#include "GPU.h"
#include "GPU2D_Soft.h"
#include "GPU3D_Soft.h"

namespace melonDS
{

class SoftRenderer : public Renderer
{
public:
    explicit SoftRenderer(melonDS::NDS& nds);
    ~SoftRenderer() override;
    bool Init() override { return true; }
    void Reset() override;
    void Stop() override;

    void PreSavestate() override;
    void PostSavestate() override;

    void SetRenderSettings(RendererSettings& settings) override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;

    void Start3DRendering() override;

    // Render control (the headless frontend): skip drawing to save time. Bits: SkipRender2D (BG/OBJ
    // drawing, compositing and the framebuffer), SkipRender3D (3D rasterising). Emulated state is not
    // touched either way: display capture writes VRAM, so whatever a capture needs is still drawn (the
    // whole line while capture is on; the 3D frame when a capture is likely, see Start3DRendering).
    // Change it between frames only (the frontend does it at the frame boundary).
    enum { SkipRender2D = 1, SkipRender3D = 2 };
    void SetRenderSkip(u32 mask) noexcept { SkipMask = mask & (SkipRender2D | SkipRender3D); }
    [[nodiscard]] u32 GetRenderSkip() const noexcept { return SkipMask; }
    void SetThreaded3D(bool threaded);
    [[nodiscard]] bool IsThreaded3D() const;

    struct RenderSkipStats
    {
        u64 Lines2DSkipped = 0;     // visible lines not drawn
        u64 Frames3DSkipped = 0;    // 3D frames not rasterised
        u64 CaptureLines = 0;       // lines drawn although 2D skipping was on, because display capture was on
        u64 Frames3DForCapture = 0; // 3D frames drawn although 3D skipping was on, because a capture was likely
        u64 Late3DFrames = 0;       // a capture started with the 3D frame skipped: it was drawn at the capture's
                                    // first line instead (exact unless texture VRAM changed after VCount 215)
    };
    [[nodiscard]] const RenderSkipStats& GetRenderSkipStats() const noexcept { return SkipStats; }

    void VBlank() override {};
    void VBlankEnd() override {};

    void AllocCapture(u32 bank, u32 start, u32 len) override {};
    void SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete) override {};

    bool GetFramebuffers(void** top, void** bottom) override;

private:
    friend class SoftRenderer2D;
    friend class SoftRenderer3D;

    u32* Framebuffer[2][2];

    u32 SkipMask = 0;
    bool CaptureSeen = false;   // display capture was on for a line since the last Start3DRendering
    RenderSkipStats SkipStats;

    u32* Output3D;
    alignas(8) u32 Output2D[2][256];

    void DrawScanlineA(u32 line, u32* dst);
    void DrawScanlineB(u32 line, u32* dst);

    void DoCapture(u32 line);

    void ApplyMasterBrightness(u16 regval, u32* dst);
    void ExpandColor(u32* dst);
};

}

#endif // GPU_SOFT_H
