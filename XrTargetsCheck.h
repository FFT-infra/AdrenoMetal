// SPDX-License-Identifier: MIT
// Apple's extended-range render targets through the frontend: a kSize x kSize texture of a format is cleared, blended over in its left
// half, then read and sampled (filtered) into a BGRA8 target, side by side. QuartzCore on a device that is not virtual makes its
// extended-range surfaces RGB10A8_2P_XR10 (550) where a virtual one makes RGBA16Float (CA::OGL::MetalContext::
// create_surface_with_properties), and the tablet drew translucent light blue blocks once it did (i22e9aa895). The host check
// (bridge/xr_planes_host.mm) compares the bridge with the host's Metal; the plugin runs the same passes on the tablet and compares them
// with the bytes Metal gave (kXrExpected).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace xrcheck {
static const uint32_t kSize = 64;
static NSString *const kShaders = @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "vertex float4 xr_vs(uint vid [[vertex_id]]) {\n"
    "    const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};\n"
    "    return float4(p[vid], 0, 1);\n"
    "}\n"
    "fragment half4 xr_color(constant float4 &c [[buffer(0)]]) {return half4(c);}\n"
    "fragment float4 xr_read(float4 pos [[position]], texture2d<float> t [[texture(0)]]) {return t.read(uint2(pos.xy));}\n"
    "fragment float4 xr_sample(float4 pos [[position]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "    return t.sample(s, float2(pos.x - 64.0, pos.y) / 64.0);\n"
    "}\n";
// 115 RGBA16Float (a virtual device's), 550 RGB10A8_2P_XR10 and 551 its sRGB twin, 552 BGRA10_XR, 554 BGR10_XR.
static const MTLPixelFormat kFormats[5] = {(MTLPixelFormat)115, (MTLPixelFormat)550, (MTLPixelFormat)551, (MTLPixelFormat)552,
                                           (MTLPixelFormat)554};
// Read blended, read cleared, sampled blended, sampled cleared.
static const uint32_t kPoints[4][2] = {{8, 32}, {48, 32}, {kSize + 8, 32}, {kSize + 48, 32}};
// The BGRA bytes Metal gave at the points, as one word (byte 0 first), per format (an Apple M-series Mac under macOS 26.5).
static const uint32_t kExpected[5][4] = {{0x60409fff, 0xbf8040ff, 0x60409fff, 0xbf8040ff}, {0x60409fff, 0xbf8040ff, 0x60409fff, 0xbf8040ff},
                                         {0x60409fff, 0xbf7f40ff, 0x60409fff, 0xbf7f40ff}, {0x60409fff, 0xbf8040ff, 0x60409fff, 0xbf8040ff},
                                         {0x60409fff, 0xbf8040ff, 0x60409fff, 0xbf8040ff}};

static uint32_t word(const std::vector<uint8_t> &bytes, int point) {
    const size_t at = (kPoints[point][1] * kSize * 2 + kPoints[point][0]) * 4;
    return (uint32_t)bytes[at] << 24 | (uint32_t)bytes[at + 1] << 16 | (uint32_t)bytes[at + 2] << 8 | bytes[at + 3];
}
// The largest difference of one byte between two words.
static int distance(uint32_t a, uint32_t b) {
    int worst = 0;
    for (int shift = 0; shift < 32; shift += 8) worst = std::max(worst, std::abs((int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff)));
    return worst;
}

// The BGRA8 bytes of the passes over `format` (empty, with *error, when a step was refused).
static std::vector<uint8_t> render(id<MTLDevice> device, MTLPixelFormat format, NSString **error) {
    NSError *e = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kShaders options:nil error:&e];
    if (!library) {*error = [NSString stringWithFormat:@"library: %@", e]; return {};}
    auto pipeline = [&](NSString *fragment, MTLPixelFormat target, bool blend) -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library newFunctionWithName:@"xr_vs"];
        d.fragmentFunction = [library newFunctionWithName:fragment];
        d.colorAttachments[0].pixelFormat = target;
        if (blend) {
            d.colorAttachments[0].blendingEnabled = YES;
            d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
            d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        }
        NSError *pe = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&pe];
        if (!state) *error = [NSString stringWithFormat:@"pipeline %@ %lu: %@", fragment, (unsigned long)target, pe];
        return state;
    };
    id<MTLRenderPipelineState> over = pipeline(@"xr_color", format, true);
    id<MTLRenderPipelineState> readOut = over ? pipeline(@"xr_read", MTLPixelFormatBGRA8Unorm, false) : nil;
    id<MTLRenderPipelineState> sampleOut = readOut ? pipeline(@"xr_sample", MTLPixelFormatBGRA8Unorm, false) : nil;
    if (!sampleOut) return {};
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:kSize height:kSize mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> xr = [device newTextureWithDescriptor:td];
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:kSize * 2 height:kSize
                                                                             mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget;
    od.storageMode = MTLStorageModeShared;
    id<MTLTexture> out = [device newTextureWithDescriptor:od];
    if (!xr || !out) {*error = @"texture"; return {};}
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *a = [MTLRenderPassDescriptor renderPassDescriptor];
    a.colorAttachments[0].texture = xr;
    a.colorAttachments[0].loadAction = MTLLoadActionClear;
    a.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
    a.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:a];
    [enc setRenderPipelineState:over];
    [enc setViewport:(MTLViewport){0, 0, kSize / 2.0, (double)kSize, 0, 1}];
    const float color[4] = {1.0f, 0.0f, 0.0f, 0.5f};
    [enc setFragmentBytes:color length:sizeof color atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    MTLRenderPassDescriptor *b = [MTLRenderPassDescriptor renderPassDescriptor];
    b.colorAttachments[0].texture = out;
    b.colorAttachments[0].loadAction = MTLLoadActionClear;
    b.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    b.colorAttachments[0].storeAction = MTLStoreActionStore;
    enc = [cb renderCommandEncoderWithDescriptor:b];
    [enc setFragmentTexture:xr atIndex:0];
    [enc setFragmentSamplerState:sampler atIndex:0];
    [enc setRenderPipelineState:readOut];
    [enc setViewport:(MTLViewport){0, 0, (double)kSize, (double)kSize, 0, 1}];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc setRenderPipelineState:sampleOut];
    [enc setViewport:(MTLViewport){(double)kSize, 0, (double)kSize, (double)kSize, 0, 1}];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {*error = [NSString stringWithFormat:@"command buffer: %@", cb.error]; return {};}
    std::vector<uint8_t> bytes(kSize * 2 * kSize * 4);
    [out getBytes:bytes.data() bytesPerRow:kSize * 2 * 4 fromRegion:MTLRegionMake2D(0, 0, kSize * 2, kSize) mipmapLevel:0];
    return bytes;
}

// Every format's passes against Metal's bytes, one short line each into `line(text)`: "xr F ok" or "xr F p0 got/want ..." or the
// refusal. Returns how many formats differ or were refused.
template <typename Line>
static int checkAgainstExpected(id<MTLDevice> device, Line line) {
    int failed = 0;
    for (int f = 0; f < 5; ++f) {
        NSString *error = nil;
        const std::vector<uint8_t> got = render(device, kFormats[f], &error);
        char text[200];
        if (got.empty()) {
            snprintf(text, sizeof text, "xr %lu refused: %.150s", (unsigned long)kFormats[f], error.UTF8String ?: "?");
            line(text);++failed;continue;
        }
        int n = snprintf(text, sizeof text, "xr %lu", (unsigned long)kFormats[f]);
        bool wrong = false;
        for (int p = 0; p < 4; ++p) {
            const uint32_t have = word(got, p);
            if (distance(have, kExpected[f][p]) <= 2) continue;
            wrong = true;
            n += snprintf(text + n, sizeof text - n, " p%d %08x/%08x", p, have, kExpected[f][p]);
        }
        if (!wrong) snprintf(text + n, sizeof text - n, " ok");
        line(text);
        if (wrong) ++failed;
    }
    return failed;
}
}
