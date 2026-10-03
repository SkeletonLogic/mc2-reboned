#include "renderer_webgpu_internal.h"

#include <cmath>

namespace mc2webgpu {

namespace {

bool IsAxisAlignedQuad(const gos_VERTEX* quad)
{
    const auto close = [](float left, float right)
    {
        return std::fabs(left - right) < 0.001f;
    };
    return close(quad[0].x, quad[1].x) && close(quad[2].x, quad[3].x) &&
           close(quad[0].y, quad[3].y) && close(quad[1].y, quad[2].y);
}

} // namespace

    void WebGPURenderer::AccountSubmittedGeometryDraw()
    {
        ++m_currentFrameDrawCalls;
        int alphaMode = m_state.Get(gos_State_AlphaMode);
        if (alphaMode == 0) alphaMode = gos_Alpha_OneZero;
        if (alphaMode < gos_Alpha_OneZero || alphaMode > gos_Alpha_OneInvAlpha)
            alphaMode = 0;
        ++m_geometryDrawsByAlphaMode[alphaMode];
        if (m_dumpOnAdditive && alphaMode == gos_Alpha_OneOne && !m_dumped && !m_dumpFrame)
        {
            m_dumpFrame = m_frameNumber;
            std::printf("[webgpu] first additive geometry draw at frame %lu; scheduling dump\n",
                        m_frameNumber);
        }
    }

    //-- drawing -----------------------------------------------------------//
    void WebGPURenderer::DrawPoints(gos_VERTEX* v, int n)
    {
        TallySpecular(v, (DWORD)n);
        TallyFog(v, (DWORD)n);
        TallyShade(v, (DWORD)n);
        if (n <= 0) return;
        ++m_stats.points;
        if (SubmitVertices(v, static_cast<DWORD>(n), wgpu::PrimitiveTopology::PointList))
        {
            ++m_stats.submittedPoints;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::DrawLines(gos_VERTEX* v, int n)
    {
        TallySpecular(v, (DWORD)n);
        TallyFog(v, (DWORD)n);
        TallyShade(v, (DWORD)n);
        if (n <= 0) return;
        ++m_stats.lines;
        if (SubmitVertices(v, static_cast<DWORD>(n), wgpu::PrimitiveTopology::LineList))
        {
            ++m_stats.submittedLines;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::DrawTriangles(gos_VERTEX* v, int n)
    {
        TallySpecular(v, (DWORD)n);
        TallyFog(v, (DWORD)n);
        TallyShade(v, (DWORD)n);
        if (n <= 0) return;
        ++m_stats.triangles;
        if (SubmitVertices(v, static_cast<DWORD>(n), wgpu::PrimitiveTopology::TriangleList))
        {
            ++m_stats.submittedTriangles;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::DrawQuads(gos_VERTEX* v, int n)
    {
        TallySpecular(v, (DWORD)n);
        TallyFog(v, (DWORD)n);
        TallyShade(v, (DWORD)n);
        if (n <= 0) return;
        ++m_stats.quads;
        if (SubmitQuads(v, static_cast<DWORD>(n)))
        {
            ++m_stats.submittedQuads;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::RenderIndexed(gos_VERTEX* v, DWORD nv, WORD* indices, DWORD ni)
    {
        TallySpecular(v, nv);
        TallyFog(v, nv);
        TallyShade(v, nv);
        CountIndexed(nv, ni);
        ++m_stats.indexed1UV;
        if (SubmitIndexed(v, nv, indices, ni, wgpu::PrimitiveTopology::TriangleList))
        {
            ++m_stats.submittedIndexed;
            ++m_stats.submittedIndexed1UV;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::RenderIndexed2UV(gos_VERTEX_2UV* v, DWORD nv, WORD* indices, DWORD ni)
    {
        CountIndexed(nv, ni);
        ++m_stats.indexed2UV;
        std::vector<gos_VERTEX> canonical;
        ConvertVertices(v, nv, canonical);
        if (SubmitIndexed(canonical.data(), nv, indices, ni,
                          wgpu::PrimitiveTopology::TriangleList))
        {
            ++m_stats.submittedIndexed;
            ++m_stats.submittedIndexed2UV;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::RenderIndexed3UV(gos_VERTEX_3UV* v, DWORD nv, WORD* indices, DWORD ni)
    {
        CountIndexed(nv, ni);
        ++m_stats.indexed3UV;
        std::vector<gos_VERTEX> canonical;
        ConvertVertices(v, nv, canonical);
        if (SubmitIndexed(canonical.data(), nv, indices, ni,
                          wgpu::PrimitiveTopology::TriangleList))
        {
            ++m_stats.submittedIndexed;
            ++m_stats.submittedIndexed3UV;
            AccountSubmittedGeometryDraw();
        }
    }



    void WebGPURenderer::RenderIndexedRaw(void* v, DWORD nv, WORD* indices, DWORD ni,
                          gosVERTEXTYPE vertexType, gosPRIMITIVETYPE primitiveType)
    {
        CountIndexed(nv, ni);
        ++m_stats.indexedRaw;
        const wgpu::PrimitiveTopology topology = TopologyFor(primitiveType);
        bool submitted = false;
        switch (vertexType)
        {
            case VERTEX_gos_VERTEX:
                submitted = SubmitIndexed(static_cast<gos_VERTEX*>(v), nv, indices, ni, topology);
                break;
            case VERTEX_gos_VERTEX_2UV:
            {
                std::vector<gos_VERTEX> canonical;
                ConvertVertices(static_cast<gos_VERTEX_2UV*>(v), nv, canonical);
                submitted = SubmitIndexed(canonical.data(), nv, indices, ni, topology);
                break;
            }
            case VERTEX_gos_VERTEX_3UV:
            {
                std::vector<gos_VERTEX> canonical;
                ConvertVertices(static_cast<gos_VERTEX_3UV*>(v), nv, canonical);
                submitted = SubmitIndexed(canonical.data(), nv, indices, ni, topology);
                break;
            }
            default:
                ++m_unsupportedRawDraws;
                break;
        }
        if (submitted)
        {
            ++m_stats.submittedIndexed;
            ++m_stats.submittedIndexedRaw;
            AccountSubmittedGeometryDraw();
        }
    }



    VertexArenaChunk& WebGPURenderer::VertexChunkFor(size_t count)
    {
        while (m_activeVertexChunk < m_vertexChunks.size() &&
               m_vertexChunks[m_activeVertexChunk].capacity -
                   m_vertexChunks[m_activeVertexChunk].cpu.size() < count)
            ++m_activeVertexChunk;

        if (m_activeVertexChunk == m_vertexChunks.size())
        {
            const size_t previous = m_vertexChunks.empty() ? 0 : m_vertexChunks.back().capacity;
            const size_t capacity = std::max(count, previous ? previous * 2 : size_t(1024));
            VertexArenaChunk chunk;
            chunk.capacity = capacity;
            chunk.cpu.reserve(capacity);
            wgpu::BufferDescriptor desc = {};
            desc.label = "MC2 frame vertex arena";
            desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
            desc.size = capacity * sizeof(gos_VERTEX);
            chunk.gpu = m_device.CreateBuffer(&desc);
            m_vertexChunks.push_back(std::move(chunk));
        }
        return m_vertexChunks[m_activeVertexChunk];
    }



    IndexArenaChunk& WebGPURenderer::IndexChunkFor(size_t count)
    {
        while (m_activeIndexChunk < m_indexChunks.size() &&
               m_indexChunks[m_activeIndexChunk].capacity -
                   m_indexChunks[m_activeIndexChunk].cpu.size() < count)
            ++m_activeIndexChunk;

        if (m_activeIndexChunk == m_indexChunks.size())
        {
            const size_t previous = m_indexChunks.empty() ? 0 : m_indexChunks.back().capacity;
            const size_t capacity = std::max(count, previous ? previous * 2 : size_t(32768));
            IndexArenaChunk chunk;
            chunk.capacity = capacity;
            chunk.cpu.reserve(capacity);
            wgpu::BufferDescriptor desc = {};
            desc.label = "MC2 frame index arena";
            desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
            desc.size = capacity * sizeof(WORD);
            chunk.gpu = m_device.CreateBuffer(&desc);
            m_indexChunks.push_back(std::move(chunk));
        }
        return m_indexChunks[m_activeIndexChunk];
    }



    VertexSlice WebGPURenderer::AppendVertices(const gos_VERTEX* vertices, DWORD count)
    {
        VertexSlice slice = {};
        if (!vertices || count == 0) return slice;
        VertexArenaChunk& chunk = VertexChunkFor(count);
        const size_t first = chunk.cpu.size();
        chunk.cpu.resize(first + count);
        std::memcpy(chunk.cpu.data() + first, vertices, count * sizeof(gos_VERTEX));
        slice.buffer = chunk.gpu;
        slice.offset = first * sizeof(gos_VERTEX);
        m_frameArenaBytes += count * sizeof(gos_VERTEX);
        m_peakArenaBytes = std::max(m_peakArenaBytes, m_frameArenaBytes);
        return slice;
    }



    IndexSlice WebGPURenderer::AppendIndices(const WORD* indices, DWORD count)
    {
        IndexSlice slice = {};
        if (!indices || count == 0) return slice;
        IndexArenaChunk& chunk = IndexChunkFor(count + 1);
        if (chunk.cpu.size() & 1) chunk.cpu.push_back(0);
        const size_t first = chunk.cpu.size();
        chunk.cpu.resize(first + count);
        std::memcpy(chunk.cpu.data() + first, indices, count * sizeof(WORD));
        slice.buffer = chunk.gpu;
        slice.offset = first * sizeof(WORD);
        return slice;
    }



    void WebGPURenderer::UploadFrameArenas()
    {
        for (VertexArenaChunk& chunk : m_vertexChunks)
            if (!chunk.cpu.empty())
                m_queue.WriteBuffer(chunk.gpu, 0, chunk.cpu.data(),
                                    chunk.cpu.size() * sizeof(gos_VERTEX));
        for (IndexArenaChunk& chunk : m_indexChunks)
            if (!chunk.cpu.empty())
            {
                if (chunk.cpu.size() & 1) chunk.cpu.push_back(0);
                m_queue.WriteBuffer(chunk.gpu, 0, chunk.cpu.data(),
                                    chunk.cpu.size() * sizeof(WORD));
            }
    }



    // PORT (M8-B11): which draws actually carry specular, and is it enabled for
    // them? Frame captures cannot answer this -- two runs of the SAME binary at
    // the same frame number differ in 46% of subpixels, so pixel deltas of this
    // size are noise, not evidence. This counts instead, and cannot drift.
    //
    // vertices carrying specular with the state OFF are MLR's effects, whose
    // GOSVertex constructor leaves frgb at 0xffffffff forever; those must stay
    // suppressed. Vertices carrying specular with the state ON are the terrain
    // pools, which must survive.
    // PORT (M8-3): the renderer inventory found gos_State_ShadeMode set 44,053
    // times per run and never read by this peer, so everything draws gouraud.
    // Before honouring it, establish whether honouring it would change any
    // pixel -- because flat and gouraud differ ONLY where a triangle carries
    // more than one vertex colour. GameOS.HPP even tells callers to set all
    // three the same in FLAT mode, so uniform triangles are the expected case.
    //
    // m_shadeVaryingTris on the FLAT bucket is the whole measurement: zero
    // means honouring the state is a no-op and this is not a gap; non-zero is
    // the count of triangles currently drawn smooth that retail drew faceted.
    //
    // NOTE the flat sites are mostly guarded by Environment.Renderer == 3, the
    // software rasteriser. Only txmmgr.cpp:660 and :772 reach the hardware
    // path, so a small FLAT bucket here is expected and is not a broken probe.
    void WebGPURenderer::TallyShade(const gos_VERTEX* vertices, DWORD count)
    {
        static const bool wantCensus = (getenv("MC2_SHADE_CENSUS") != NULL);
        if (!wantCensus || !vertices) return;

        const int bucket = (m_state.Get(gos_State_ShadeMode) == gos_ShadeFlat) ? 0 : 1;
        m_shadeDraws[bucket]++;
        m_shadeVerts[bucket] += count;

        if (count >= 3 && (count % 3) == 0)
        {
            for (DWORD t = 0; t + 2 < count; t += 3)
            {
                m_shadeTris[bucket]++;
                const DWORD a0 = vertices[t + 0].argb;
                if (vertices[t + 1].argb != a0 || vertices[t + 2].argb != a0)
                    m_shadeVaryingTris[bucket]++;
            }
        }

        if (((m_shadeDraws[0] + m_shadeDraws[1]) % 20000) == 0)
        {
            std::fprintf(stderr,
                "[shade-census] FLAT draws=%lu verts=%lu tris=%lu varyingTris=%lu | "
                "GOURAUD draws=%lu verts=%lu tris=%lu varyingTris=%lu\n",
                m_shadeDraws[0], m_shadeVerts[0], m_shadeTris[0], m_shadeVaryingTris[0],
                m_shadeDraws[1], m_shadeVerts[1], m_shadeTris[1], m_shadeVaryingTris[1]);
            std::fflush(stderr);
        }
    }

    void WebGPURenderer::TallySpecular(const gos_VERTEX* vertices, DWORD count)
    {
        static const bool wantCensus = (getenv("MC2_LIGHT_CENSUS") != NULL);
        if (!wantCensus || !vertices) return;

        const bool enabled = m_state.Get(gos_State_Specular) != 0;
        DWORD carrying = 0;
        for (DWORD i = 0; i < count; ++i)
            if ((vertices[i].frgb & 0x00FFFFFFu) != 0) ++carrying;

        m_specDraws[enabled ? 1 : 0]++;
        m_specVerts[enabled ? 1 : 0] += count;
        m_specCarrying[enabled ? 1 : 0] += carrying;

        if (((m_specDraws[0] + m_specDraws[1]) % 20000) == 0)
        {
            std::fprintf(stderr,
                "[light-census] specular draws: state ON draws=%lu verts=%lu carrying=%lu | "
                "state OFF draws=%lu verts=%lu carrying=%lu\n",
                m_specDraws[1], m_specVerts[1], m_specCarrying[1],
                m_specDraws[0], m_specVerts[0], m_specCarrying[0]);
            std::fflush(stderr);
        }
    }

    // PORT (M8-3): establish that the live draw stream reaches WebGPU with the
    // per-vertex fog factor in frgb's top byte before changing the shader. The
    // state buckets are the control: alpha below ff on a state-ON draw is fog
    // that should be visible; the same byte on a state-OFF draw must be ignored.
    void WebGPURenderer::TallyFog(const gos_VERTEX* vertices, DWORD count)
    {
        static const bool wantCensus = (getenv("MC2_FOG_CENSUS") != NULL);
        if (!wantCensus || !vertices) return;

        const int bucket = m_state.Get(gos_State_Fog) ? 1 : 0;
        DWORD carrying = 0;
        unsigned char minAlpha = 0xff;
        for (DWORD i = 0; i < count; ++i)
        {
            const unsigned char alpha = static_cast<unsigned char>(vertices[i].frgb >> 24);
            if (alpha < 0xff) ++carrying;
            if (alpha < minAlpha) minAlpha = alpha;
        }

        ++m_fogDraws[bucket];
        m_fogVerts[bucket] += count;
        m_fogCarrying[bucket] += carrying;
        if (minAlpha < m_fogMinAlpha[bucket]) m_fogMinAlpha[bucket] = minAlpha;

        if (((m_fogDraws[0] + m_fogDraws[1]) % 20000) == 0)
        {
            std::fprintf(stderr,
                "[fog-census] state ON draws=%lu verts=%lu carrying=%lu minAlpha=%u | "
                "state OFF draws=%lu verts=%lu carrying=%lu minAlpha=%u\n",
                m_fogDraws[1], m_fogVerts[1], m_fogCarrying[1],
                static_cast<unsigned>(m_fogMinAlpha[1]),
                m_fogDraws[0], m_fogVerts[0], m_fogCarrying[0],
                static_cast<unsigned>(m_fogMinAlpha[0]));
            std::fflush(stderr);
        }
    }

    bool WebGPURenderer::SubmitVertices(const gos_VERTEX* vertices, DWORD count,
                        wgpu::PrimitiveTopology topology)
    {
        if (!vertices || count == 0 || !BindDrawState(topology)) return false;
        const VertexSlice slice = AppendVertices(vertices, count);
        if (!slice.buffer) return false;
        m_pass.SetVertexBuffer(0, slice.buffer, slice.offset, count * sizeof(gos_VERTEX));
        m_pass.Draw(count);
        return true;
    }



    bool WebGPURenderer::EnsureQuadIndices(DWORD quadCount)
    {
        if (quadCount <= m_quadIndexCapacity) return true;
        if (quadCount > 16383)
        {
            std::fprintf(stderr, "[webgpu] quad batch too large for uint16 indices: %lu\n",
                         (unsigned long)quadCount);
            return false;
        }
        DWORD capacity = m_quadIndexCapacity ? m_quadIndexCapacity : 1;
        while (capacity < quadCount) capacity *= 2;
        std::vector<WORD> indices(static_cast<size_t>(capacity) * 6);
        for (DWORD q = 0; q < capacity; ++q)
        {
            const WORD base = static_cast<WORD>(q * 4);
            indices[q * 6 + 0] = base;
            indices[q * 6 + 1] = base + 1;
            indices[q * 6 + 2] = base + 2;
            indices[q * 6 + 3] = base;
            indices[q * 6 + 4] = base + 2;
            indices[q * 6 + 5] = base + 3;
        }
        wgpu::BufferDescriptor desc = {};
        desc.label = "MC2 reusable quad indices";
        desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
        desc.size = indices.size() * sizeof(WORD);
        m_quadIndexBuffer = m_device.CreateBuffer(&desc);
        m_queue.WriteBuffer(m_quadIndexBuffer, 0, indices.data(), desc.size);
        m_quadIndexCapacity = capacity;
        return !!m_quadIndexBuffer;
    }



    bool WebGPURenderer::SubmitQuads(const gos_VERTEX* vertices, DWORD count)
    {
        if (!vertices || count < 4 || count % 4 != 0) return false;
        const DWORD quadCount = count / 4;
        if (!EnsureQuadIndices(quadCount) ||
            !BindDrawState(wgpu::PrimitiveTopology::TriangleList)) return false;

        const gos_VERTEX* submittedVertices = vertices;
        std::vector<gos_VERTEX> snappedVertices;
        if (MC2_IsVirtualCanvasActive() && m_targetWidth && m_targetHeight)
        {
            // PORT: authored logistics screens assemble adjacent axis-aligned
            // atlas pieces on an 800x600 canvas. At 1920x1080 their shared
            // edges land at fractional physical pixels (for example y=387
            // becomes 696.6), which can leave a one-pixel raster crack between
            // otherwise touching quads. Snap only those quads' physical edges;
            // indexed world geometry and non-axis-aligned sprites are untouched.
            // PORT (M8-B52): must use the SAME extent the viewport uniform does,
            // or the snap moves edges to pixels the draw never lands on.
            float canvasW = 800.0f, canvasH = 600.0f;
            CanvasExtent(&canvasW, &canvasH);
            const float scale = std::min(static_cast<float>(m_targetWidth) / canvasW,
                                         static_cast<float>(m_targetHeight) / canvasH);
            const float offsetX = (static_cast<float>(m_targetWidth) - canvasW * scale) * 0.5f;
            const float offsetY = (static_cast<float>(m_targetHeight) - canvasH * scale) * 0.5f;
            snappedVertices.assign(vertices, vertices + count);
            for (DWORD q = 0; q < quadCount; ++q)
            {
                gos_VERTEX* quad = &snappedVertices[q * 4];
                if (!IsAxisAlignedQuad(quad)) continue;
                for (DWORD v = 0; v < 4; ++v)
                {
                    quad[v].x = (std::round(quad[v].x * scale + offsetX) - offsetX) / scale;
                    quad[v].y = (std::round(quad[v].y * scale + offsetY) - offsetY) / scale;
                }
            }
            submittedVertices = snappedVertices.data();
        }

        const VertexSlice slice = AppendVertices(submittedVertices, count);
        if (!slice.buffer) return false;
        m_pass.SetVertexBuffer(0, slice.buffer, slice.offset, count * sizeof(gos_VERTEX));
        m_pass.SetIndexBuffer(m_quadIndexBuffer, wgpu::IndexFormat::Uint16, 0,
                              quadCount * 6 * sizeof(WORD));
        m_pass.DrawIndexed(quadCount * 6);
        m_maxQuadBatch = std::max(m_maxQuadBatch, quadCount);
        return true;
    }



    void WebGPURenderer::SetQuad(gos_VERTEX* vertices, float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1, DWORD color)
    {
        const auto set = [color](gos_VERTEX& vertex, float x, float y, float u, float v)
        {
            vertex.x = x; vertex.y = y; vertex.z = 0.5f; vertex.rhw = 1.0f;
            vertex.argb = color; vertex.frgb = 0; vertex.u = u; vertex.v = v;
        };
        set(vertices[0], x0, y0, u0, v0);
        set(vertices[1], x0, y1, u0, v1);
        set(vertices[2], x1, y1, u1, v1);
        set(vertices[3], x1, y0, u1, v0);
    }



    bool WebGPURenderer::SubmitIndexed(const gos_VERTEX* vertices, DWORD vertexCount,
                       const WORD* indices, DWORD indexCount,
                       wgpu::PrimitiveTopology topology)
    {
        if (topology == wgpu::PrimitiveTopology::PointList)
            return SubmitVertices(vertices, vertexCount, topology);
        if (!vertices || !indices || vertexCount == 0 || indexCount == 0 ||
            !BindDrawState(topology)) return false;
        const VertexSlice verticesSlice = AppendVertices(vertices, vertexCount);
        const IndexSlice indicesSlice = AppendIndices(indices, indexCount);
        if (!verticesSlice.buffer || !indicesSlice.buffer) return false;
        m_pass.SetVertexBuffer(0, verticesSlice.buffer, verticesSlice.offset,
                               vertexCount * sizeof(gos_VERTEX));
        m_pass.SetIndexBuffer(indicesSlice.buffer, wgpu::IndexFormat::Uint16,
                              indicesSlice.offset, indexCount * sizeof(WORD));
        m_pass.DrawIndexed(indexCount);
        return true;
    }



    wgpu::PrimitiveTopology WebGPURenderer::TopologyFor(gosPRIMITIVETYPE primitive)
    {
        switch (primitive)
        {
            case PRIMITIVE_POINTLIST: return wgpu::PrimitiveTopology::PointList;
            case PRIMITIVE_LINELIST: return wgpu::PrimitiveTopology::LineList;
            default: return wgpu::PrimitiveTopology::TriangleList;
        }
    }



    void WebGPURenderer::ConvertVertices(const gos_VERTEX_2UV* input, DWORD count,
                                std::vector<gos_VERTEX>& output)
    {
        output.resize(count);
        for (DWORD i = 0; i < count; ++i)
        {
            output[i].x = input[i].x; output[i].y = input[i].y;
            output[i].z = input[i].z; output[i].rhw = input[i].rhw;
            output[i].argb = input[i].argb; output[i].frgb = input[i].frgb;
            output[i].u = input[i].u1; output[i].v = input[i].v1;
        }
    }



    void WebGPURenderer::ConvertVertices(const gos_VERTEX_3UV* input, DWORD count,
                                std::vector<gos_VERTEX>& output)
    {
        output.resize(count);
        for (DWORD i = 0; i < count; ++i)
        {
            output[i].x = input[i].x; output[i].y = input[i].y;
            output[i].z = input[i].z; output[i].rhw = input[i].rhw;
            output[i].argb = input[i].argb; output[i].frgb = input[i].frgb;
            output[i].u = input[i].u1; output[i].v = input[i].v1;
        }
    }



    void WebGPURenderer::CountIndexed(DWORD numVerts, DWORD numIndices)
    {
        ++m_stats.indexed;
        m_stats.indexedVerts   += numVerts;
        m_stats.indexedIndices += numIndices;
    }

} // namespace mc2webgpu
