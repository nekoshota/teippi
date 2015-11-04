#include "image.h"

#include "offsets.h"
#include "sprite.h"
#include "log.h"
#include "lofile.h"
#include "warn.h"
#include "rng.h"
#include "unit.h"
#include "yms.h"
#include "draw.h"
#include "perfclock.h"
#include "strings.h"

#include <atomic>

bool GrpFrameHeader::IsDecoded() const
{
    return ((uint16_t *)frame)[0] == 0;
}

int GrpFrameHeader::GetWidth() const
{
    if (IsDecoded())
    {
        int width = w;
        // Round upwards to grp_padding_size
        width += (grp_padding_size - 1);
        width &= ~(grp_padding_size - 1);
        // And add safety padding
        width += 2 * (grp_padding_size - 1);
        return width;
    }
    else
        return w;
}

uint8_t GrpFrameHeader::GetPixel(x32 x, y32 y) const
{
    if (IsDecoded())
    {
        return frame[2 + (grp_padding_size - 1) + x + y * GetWidth()];
    }
    else
    {
        uint16_t *line_offsets = (uint16_t *)frame;
        uint8_t *line = frame + line_offsets[y];
        int pos = 0;
        while (true)
        {
            uint8_t val = *line++;
            if (val & 0x80)
            {
                val &= ~0x80;
                if (x < pos + val)
                    return 0;
                pos += val;
            }
            else if (val & 0x40)
            {
                val &= ~0x40;
                uint8_t color = *line++;
                if (x < pos + val)
                    return color;
                pos += val;
            }
            else
            {
                if (x < pos + val)
                    return line[x - pos];
                pos += val;
                line += val;
            }
        }
    }
}

Image::Image()
{
    list.prev = nullptr;
    list.next = nullptr;
}

Image::Image(Sprite *parent, int image_id, int x, int y) : image_id(image_id), x_off(x), y_off(y), parent(parent)
{
    list.prev = nullptr;
    list.next = nullptr;
    grp = bw::image_grps[image_id];
    flags = 0;
    frameset = 0;
    frame = 0;
    direction = 0;
    grp_bounds = Rect16(0, 0, 0, 0);
    iscript.header = 0;
    iscript.pos = 0;
    iscript.return_pos = 0;
    iscript.animation = 0;
    iscript.wait = 0;

    if (images_dat_turning_graphic[image_id] & 0x1)
        flags |= ImageFlags::CanTurn;
    if (images_dat_clickable[image_id] & 0x1)
        flags |= ImageFlags::Clickable;
    if (images_dat_use_full_iscript[image_id] & 0x1)
        flags |= ImageFlags::FullIscript;

    SetDrawFunc(images_dat_drawfunc[image_id], nullptr);
    if (drawfunc == OverrideColor)
        drawfunc_param = (void *)(uintptr_t)parent->player;
    else if (drawfunc == Remap)
        drawfunc_param = bw::blend_palettes[images_dat_remapping[image_id]].data;
}

bool Image::InitIscript()
{
    int iscript_header = images_dat_iscript_header[image_id];
    bool success = iscript.Initialize(iscript_header);
    if (!success)
    {
        Warning("Image %s has an invalid iscript header: %d (0x%x)",
                DebugStr().c_str(), iscript_header, iscript_header);
        return false;
    }
    IscriptContext ctx;
    ctx.bullet = *bw::active_iscript_bullet;
    ctx.unit = *bw::active_iscript_unit;
    auto cmds = SetIscriptAnimation(IscriptAnim::Init, &ctx, main_rng);
    if (!Empty(cmds))
        Warning("Image::InitIscript did not handle all iscript commands for image %x", image_id);
    PrepareDrawImage(this);
    return true;
}

#ifdef SYNC
void *Image::operator new(size_t size)
{
    auto ret = new uint8_t[size];
    if (SyncTest)
        ScrambleStruct(ret, size);
    return ret;
}
#endif

void Image::SingleDelete()
{
    if (~*bw::image_flags & 1)
        MarkImageAreaForRedraw(this);

    if (list.next)
        list.next->list.prev = list.prev;
    else
    {
        Assert(parent->last_overlay == this);
        parent->last_overlay = list.prev;
    }

    if (list.prev)
        list.prev->list.next = list.next;
    else
    {
        Assert(parent->first_overlay == this);
        parent->first_overlay = list.next;
    }

    // Bw does not do this and works fine...
    // Actually, bw would crash in some cases otherwise,
    // now they have to be found and fixed ._.
    if (parent->main_image == this)
        parent->main_image = nullptr;

    delete this;
}

void Image::UpdateSpecialOverlayPos()
{
    if (parent->main_image)
        LoFile::GetOverlay(parent->main_image->image_id, 2).SetImageOffset(this);
}

void Image::SetFlipping(bool set)
{
    bool state = (flags & ImageFlags::Flipped) != 0;
    if (state != set)
    {
        flags = flags | (set << 1) | ImageFlags::Redraw;
        SetDrawFunc(drawfunc, drawfunc_param);
    }
}

Image::ProgressFrame_C Image::SetIscriptAnimation(int anim, IscriptContext *ctx, Rng *rng)
{
    if (anim == IscriptAnim::Death && iscript.animation == IscriptAnim::Death)
        return ProgressFrame_C();
    if (~flags & 0x10 && anim != IscriptAnim::Death && anim != IscriptAnim::Init) // 0x10 = full iscript
        return ProgressFrame_C();
    if (anim == iscript.animation && (anim == IscriptAnim::Walking || anim == IscriptAnim::Working))
        return ProgressFrame_C();
    if (anim == IscriptAnim::GndAttkRpt)
    {
        if (iscript.animation != IscriptAnim::GndAttkRpt && iscript.animation != IscriptAnim::GndAttkInit)
            anim = IscriptAnim::GndAttkInit;
    }
    else if (anim == IscriptAnim::AirAttkRpt)
    {
        if (iscript.animation != IscriptAnim::AirAttkRpt && iscript.animation != IscriptAnim::AirAttkInit)
            anim = IscriptAnim::AirAttkInit;
    }
    uint8_t *pos = *bw::iscript + iscript.header;
    if (*(int32_t *)(pos + 4) >= anim)
    {
        int anim_off = *(uint16_t *)(pos + 8 + anim * sizeof(uint16_t));
        if (anim_off != 0)
        {
            iscript.animation = anim;
            iscript.pos = anim_off;
            iscript.return_pos = 0;
            iscript.wait = 0;
            return ProgressFrame(ctx, rng, false, nullptr);
        }
    }
    Warning("SetIscriptAnimation: Image %x does not have animation %x", image_id, anim);
    return ProgressFrame_C();
}

Image::ProgressFrame_C Image::ProgressFrame()
{
    IscriptContext ctx;
    ctx.unit = *bw::active_iscript_unit;
    ctx.bullet = *bw::active_iscript_bullet;
    ctx.iscript = *bw::iscript;
    return ProgressFrame(&ctx, main_rng, false, nullptr);
}

void Image::UpdateFrameToDirection()
{
    if (frameset + direction != frame)
    {
        frame = frameset + direction;
        flags |= ImageFlags::Redraw;
    }
}

void Image::Show()
{
    if (IsHidden())
    {
        flags &= ~ImageFlags::Hidden;
        flags |= ImageFlags::Redraw;
    }
}

void Image::Hide()
{
    if (!IsHidden())
    {
        if (~*bw::image_flags & 0x1)
            MarkImageAreaForRedraw(this);
        flags |= ImageFlags::Hidden;
    }
}

void Image::SetOffset(int x, int y)
{
    if (x_off != x || y_off != y)
        flags |= ImageFlags::Redraw;
    x_off = x;
    y_off = y;
}

void Image::SetFrame(int new_frame)
{
    if (frameset != new_frame)
    {
        frameset = new_frame;
        if (frame != frameset + direction)
        {
            frame = frameset + direction;
            flags |= ImageFlags::Redraw;
        }
    }
}

void Image::SetDrawFunc(int drawfunc_, void *param)
{
    drawfunc = drawfunc_;
    drawfunc_param = param;
    if (IsFlipped())
        Render = bw::image_renderfuncs[drawfunc].flipped;
    else
        Render = bw::image_renderfuncs[drawfunc].nonflipped;
    if (drawfunc == WarpFlash)
        drawfunc_param = (void *)0x230;
    if (flags & ImageFlags::UseParentLo)
        UpdateSpecialOverlayPos();
    flags |= ImageFlags::Redraw;
}

void Image::MakeDetected()
{
    // See comment in Test_DrawFuncSync why making every image detected is dangerous
    // Then again, disable doodad state cloaks units without animation, so we'll have to allow that
    bool can_detect_noncloaked = parent->main_image != nullptr && parent->main_image->drawfunc == Image::Normal;
    if (can_detect_noncloaked && drawfunc == DrawFunc::Normal)
        SetDrawFunc(DrawFunc::DetectedCloak, 0);
    else if (drawfunc >= DrawFunc::Cloaking && drawfunc <= DrawFunc::Decloaking)
        SetDrawFunc(drawfunc + 3, drawfunc_param);
}

static std::atomic<Tbl *> images_tbl;
/// Thread-safely loads/gives loaded images.tbl and keeps it in memory forever.
static Tbl *GetImagesTbl()
{
    const auto release = std::memory_order_release;
    const auto acquire = std::memory_order_acquire;
    Tbl *tbl = images_tbl.load(acquire);
    if (tbl == nullptr)
    {
        uint32_t size;
        Tbl *read_tbl = (Tbl *)ReadMpqFile("arr\\images.tbl", 0, 0, "storm", 0, 0, &size);
        Assert(read_tbl != nullptr);
        if (images_tbl.compare_exchange_strong(tbl, read_tbl, release, acquire) == false)
            SMemFree(read_tbl, __FILE__, __LINE__, 0);
        else
            tbl = read_tbl;
    }
    return tbl;
}

std::string Image::DebugStr() const
{
    Tbl *tbl = GetImagesTbl();
    int grp_id = images_dat_grp[image_id];
    char buf[128];
    snprintf(buf, sizeof buf / sizeof(buf[0]), "%x [unit\\%s]", image_id, tbl->GetTblString(grp_id));
    return buf;
}

void Image::DrawFunc_ProgressFrame(IscriptContext *ctx, Rng *rng)
{
    if (drawfunc == DrawFunc::Cloaking || drawfunc == DrawFunc::DetectedCloaking)
    {
        uint32_t val = ((uint32_t)drawfunc_param);
        uint8_t counter = (val & 0xff00) >> 8;
        uint8_t state = val & 0xff;
        if (counter-- != 0)
        {
            drawfunc_param = (void *)((counter << 8) | state);
            return;
        }
        drawfunc_param = (void *)((state + 1) | 0x300);
        flags |= ImageFlags::Redraw;
        if (state + 1 >= 8)
        {
            SetDrawFunc(drawfunc + 1, 0);
            if (ctx->unit)
                ctx->unit->flags |= (UnitStatus::InvisibilityDone | UnitStatus::BeginInvisibility);
        }
    }
    else if (drawfunc == DrawFunc::Decloaking || drawfunc == DrawFunc::DetectedDecloaking)
    {
        uint32_t val = ((uint32_t)drawfunc_param);
        uint8_t counter = (val & 0xff00) >> 8;
        uint8_t state = val & 0xff;
        if (counter-- != 0)
        {
            drawfunc_param = (void *)((counter << 8) | state);
            return;
        }
        drawfunc_param = (void *)((state - 1) | 0x300);
        flags |= ImageFlags::Redraw;
        if (state - 1 == 0)
        {
            SetDrawFunc(DrawFunc::Normal, 0);
            if (ctx->unit)
                ctx->unit->flags &= ~(UnitStatus::InvisibilityDone | UnitStatus::BeginInvisibility);
        }
    }
    else if (drawfunc == DrawFunc::WarpFlash)
    {
        uint32_t val = ((uint32_t)drawfunc_param);
        uint8_t counter = (val & 0xff00) >> 8;
        uint8_t state = val & 0xff;
        if (counter-- != 0)
        {
            drawfunc_param = (void *)((counter << 8) | state);
            return;
        }
        flags |= ImageFlags::Redraw;
        if (state < 0x3f)
        {
            drawfunc_param = (void *)((state + 1) | 0x300);
            return;
        }
        auto cmds = SetIscriptAnimation(IscriptAnim::Death, ctx, rng);
        if (!Empty(cmds))
            Warning("Image warp flash drawfunc progress did not handle all iscript commands for image %x", image_id);
        Unit *entity = ctx->unit != nullptr ? ctx->unit : (Unit *)ctx->bullet;
        if (entity)
            entity->order_signal |= 0x1;
    }
}

// Could only have one padding between lines,
// it can work as both left/right padding
template <class Operation>
void Render_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, Operation op)
{
    const int loop_unroll_count = grp_padding_size;

    Surface *surface = *bw::current_canvas;
    uint8_t *surface_pos = surface->image + x + y * surface->w;
    x32 skip = rect->left;
    x32 draw_width = ((rect->right + (loop_unroll_count - 1)) & ~(loop_unroll_count - 1));
    int surface_add = surface->w - draw_width;
    uint8_t *img = frame_header->frame;
    int img_width = frame_header->GetWidth();
    int img_add = img_width - draw_width;
    uint8_t *image_pos = img + 2 + img_width * rect->top + loop_unroll_count - 1; // + 2 is for the two zeroes signifying decoded img
    image_pos += skip;
    if (x + draw_width >= surface->w)
    {
        int sub = x + draw_width - surface->w;
        image_pos -= sub;
        surface_pos -= sub;
    }
    for (y32 line_count = rect->bottom; line_count != 0; line_count--)
    {
        for (x32 x = 0; x < draw_width; x += loop_unroll_count)
        {
            // Clang seems only unroll up to 8 iterations, which might be better
            for (int i = 0; i < loop_unroll_count / 2; i++)
            {
                op(image_pos, surface_pos);
                image_pos++;
                surface_pos++;
            }
            for (int i = 0; i < loop_unroll_count / 2; i++)
            {
                op(image_pos, surface_pos);
                image_pos++;
                surface_pos++;
            }
        }
        image_pos += img_add;
        surface_pos += surface_add;
    }
}

template <class Operation>
void Render_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, Operation op)
{
    const int loop_unroll_count = grp_padding_size;

    Surface *surface = *bw::current_canvas;
    x32 skip = rect->left;
    x32 draw_width = ((rect->right + (loop_unroll_count - 1)) & ~(loop_unroll_count - 1));
    uint8_t *surface_pos = surface->image + x + y * surface->w;
    int surface_add = surface->w - draw_width;
    uint8_t *img = frame_header->frame;
    int img_width = frame_header->GetWidth();
    int img_add = img_width + draw_width;
    uint8_t *image_pos = img + 2 + img_width * rect->top + loop_unroll_count - 1 + frame_header->w - 1; // + 2 is for the two zeroes signifying decoded img
    image_pos -= skip;
    if (x + draw_width >= surface->w)
    {
        int sub = x + draw_width - surface->w;
        image_pos += sub;
        surface_pos -= sub;
    }
    for (y32 line_count = rect->bottom; line_count != 0; line_count--)
    {
        for (x32 x = 0; x < draw_width; x += loop_unroll_count)
        {
            for (int i = 0; i < loop_unroll_count / 2; i++)
            {
                op(image_pos, surface_pos);
                image_pos--;
                surface_pos++;
            }
            for (int i = 0; i < loop_unroll_count / 2; i++)
            {
                op(image_pos, surface_pos);
                image_pos--;
                surface_pos++;
            }
        }
        image_pos += img_add;
        surface_pos += surface_add;
    }
}

void __fastcall DrawBlended_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *param)
{
    uint8_t *blend_table = (uint8_t *)param;
    Render_NonFlipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        *out = blend_table[*in << 8 | *out];
    });
}

void __fastcall DrawBlended_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *param)
{
    uint8_t *blend_table = (uint8_t *)param;
    Render_Flipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        *out = blend_table[*in << 8 | *out];
    });
}

void __fastcall DrawNormal_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *unused)
{
    STATIC_PERF_CLOCK(Dn2);
    uint8_t *remap = (uint8_t *)bw::default_grp_remap.raw_pointer();
    Render_NonFlipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        *out = *in ? remap[*in] : *out;
    });
}

void __fastcall DrawNormal_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *unused)
{
    STATIC_PERF_CLOCK(Dn2);
    uint8_t *remap = (uint8_t *)bw::default_grp_remap.raw_pointer();
    Render_Flipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        *out = *in ? remap[*in] : *out;
    });
}

void __fastcall DrawUncloakedPart_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, int state)
{
    uint8_t *remap = (uint8_t *)bw::default_grp_remap.raw_pointer();
    Render_NonFlipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        uint8_t color = remap[*in];
        if (bw::cloak_distortion[color] > state)
            *out = color;
    });
}

void __fastcall DrawUncloakedPart_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, int state)
{
    uint8_t *remap = (uint8_t *)bw::default_grp_remap.raw_pointer();
    Render_Flipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        uint8_t color = remap[*in];
        if (bw::cloak_distortion[color] > state)
            *out = color;
    });
}

void __fastcall DrawCloaked_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *unused)
{
    uint8_t *remap = (uint8_t *)bw::cloak_remap_palette.raw_pointer();
    uint8_t *surface = (*bw::current_canvas)->image;
    uint8_t *surface_end = surface + resolution::screen_width * resolution::screen_height;
    Render_NonFlipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        uint8_t pos = remap[*in];
        uint8_t *out_pos = out + pos;
        if (out_pos >= surface_end)
            out_pos -= resolution::screen_width * resolution::screen_height;
        *out = *out_pos;
    });
}

void __fastcall DrawCloaked_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *unused)
{
    uint8_t *remap = (uint8_t *)bw::cloak_remap_palette.raw_pointer();
    uint8_t *surface = (*bw::current_canvas)->image;
    uint8_t *surface_end = surface + resolution::screen_width * resolution::screen_height;
    Render_Flipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        uint8_t pos = remap[*in];
        uint8_t *out_pos = out + pos;
        if (out_pos >= surface_end)
            out_pos -= resolution::screen_width * resolution::screen_height;
        *out = *out_pos;
    });
}

static GrpFrameHeader *GetGrpFrameHeader(int image_id, int frame)
{
    GrpSprite *sprite = bw::image_grps[image_id];
    return (GrpFrameHeader *)(((uint8_t *)sprite) + 6 + frame * sizeof(GrpFrameHeader));
}

// TODO: million slow divisions here
void __fastcall DrawWarpTexture_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *param)
{
    Assert(frame_header->IsDecoded());
    int texture_frame = (int)param;
    GrpFrameHeader *warp_texture_header = GetGrpFrameHeader(Image::WarpTexture, texture_frame);
    int warp_texture_width = warp_texture_header->GetWidth();
    int frame_width = frame_header->GetWidth();
    Render_NonFlipped(x, y, warp_texture_header, rect, [&](uint8_t *in, uint8_t *out) {
        int y = (in - warp_texture_header->frame - 2) / warp_texture_width;
        int x = (in - warp_texture_header->frame - 2) % warp_texture_width;
        *out = frame_header->frame[2 + y * frame_width + x] ? *in : *out;
    });
}

void __fastcall DrawWarpTexture_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *param)
{
    Assert(frame_header->IsDecoded());
    int texture_frame = (int)param;
    GrpFrameHeader *warp_texture_header = GetGrpFrameHeader(Image::WarpTexture, texture_frame);
    int warp_texture_width = warp_texture_header->GetWidth();
    int frame_width = frame_header->GetWidth();
    Render_Flipped(x, y, warp_texture_header, rect, [&](uint8_t *in, uint8_t *out) {
        int y = (in - warp_texture_header->frame - 2) / warp_texture_width;
        int x = (in - warp_texture_header->frame - 2) % warp_texture_width;
        *out = frame_header->frame[2 + y * frame_width + x] ? *in : *out;
    });
}

void __fastcall DrawShadow_NonFlipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *unused)
{
    STATIC_PERF_CLOCK(Ds1);
    // Dark.pcx
    uint8_t *remap = (uint8_t *)bw::shadow_remap.raw_pointer();
    Render_NonFlipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        *out = *in ? remap[*out] : *out;
    });
}

void __fastcall DrawShadow_Flipped(int x, int y, GrpFrameHeader *frame_header, Rect32 *rect, void *unused)
{
    STATIC_PERF_CLOCK(Ds2);
    uint8_t *remap = (uint8_t *)bw::shadow_remap.raw_pointer();
    Render_Flipped(x, y, frame_header, rect, [&](uint8_t *in, uint8_t *out) {
        *out = *in ? remap[*out] : *out;
    });
}
