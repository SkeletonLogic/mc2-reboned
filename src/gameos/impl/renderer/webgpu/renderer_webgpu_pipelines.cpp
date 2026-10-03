#include "renderer_webgpu_internal.h"

namespace mc2webgpu {


    bool WebGPURenderer::EnsureSharedResources()
    {
        if (m_pipelineLayout) return true;

        static const char shaderSource[] = R"WGSL(
struct Viewport {
    size: vec2<f32>,
    scale: f32,
    unused: f32,
    offset: vec2<f32>,
    padding: vec2<f32>,
};

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    // Colour stays screen-linear, which is what it has always been here and
    // what D3D's Gouraud shading specifies. Only uv gains perspective
    // correction, so nothing but texture mapping changes.
    @location(0) @interpolate(linear) color: vec4<f32>,
    @location(1) uv: vec2<f32>,
};

@group(0) @binding(0) var<uniform> viewport: Viewport;
@group(0) @binding(1) var proofSampler: sampler;
@group(0) @binding(2) var proofTexture: texture_2d<f32>;

@vertex
fn vs_main(@location(0) position: vec3<f32>,
           @location(1) packedBgra: vec4<f32>,
           @location(2) uv: vec2<f32>,
           @location(3) rhw: f32) -> VertexOut {
    var out: VertexOut;
    let canvasPosition = position.xy * viewport.scale + viewport.offset;
    let ndc = vec2<f32>(canvasPosition.x / viewport.size.x * 2.0 - 1.0,
                        1.0 - canvasPosition.y / viewport.size.y * 2.0);
    // gos_VERTEX is D3D's pre-transformed XYZRHW form: x,y are already screen
    // pixels and rhw is the 1/w the CPU projection divided them by. Emitting
    // w = 1 threw that away, so the rasteriser interpolated u,v linearly in
    // screen space -- affine texture mapping, which bows straight features
    // painted into a texture across each triangle's diagonal. Scaling the clip
    // position by w hands w back: the hardware interpolates u,v
    // perspective-correctly and then divides the position straight back to the
    // same pixels and the same depth.
    var w = 1.0;
    if (rhw > 0.0) { w = 1.0 / rhw; }
    out.position = vec4<f32>(ndc.x * w, ndc.y * w, position.z * w, w);
    // D3D's packed ARGB DWORD is B,G,R,A in little-endian memory. Unorm8x4
    // therefore arrives as BGRA and must be explicitly swizzled to RGBA.
    out.color = packedBgra.bgra;
    out.uv = uv;
    return out;
}

@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
    return textureSample(proofTexture, proofSampler, in.uv) * in.color;
}
)WGSL";

        wgpu::ShaderSourceWGSL wgsl = {};
        wgsl.code = std::string_view(shaderSource);
        wgpu::ShaderModuleDescriptor shaderDesc = {};
        shaderDesc.label = "MC2 M3-1 proof shader";
        shaderDesc.nextInChain = &wgsl;
        const wgpu::ShaderModule shader = m_device.CreateShaderModule(&shaderDesc);
        if (!shader) return false;

        wgpu::BindGroupLayoutEntry layoutEntries[3] = {};
        layoutEntries[0].binding = 0;
        layoutEntries[0].visibility = wgpu::ShaderStage::Vertex;
        layoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        layoutEntries[0].buffer.minBindingSize = sizeof(ViewportUniform);
        layoutEntries[1].binding = 1;
        layoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        layoutEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
        layoutEntries[2].binding = 2;
        layoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        layoutEntries[2].texture.sampleType = wgpu::TextureSampleType::Float;
        layoutEntries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;

        wgpu::BindGroupLayoutDescriptor bindLayoutDesc = {};
        bindLayoutDesc.label = "MC2 M3-1 proof bind layout";
        bindLayoutDesc.entryCount = 3;
        bindLayoutDesc.entries = layoutEntries;
        m_proofBindLayout = m_device.CreateBindGroupLayout(&bindLayoutDesc);
        if (!m_proofBindLayout) return false;

        wgpu::PipelineLayoutDescriptor pipelineLayoutDesc = {};
        pipelineLayoutDesc.label = "MC2 M3-1 proof pipeline layout";
        pipelineLayoutDesc.bindGroupLayoutCount = 1;
        pipelineLayoutDesc.bindGroupLayouts = &m_proofBindLayout;
        const wgpu::PipelineLayout pipelineLayout =
            m_device.CreatePipelineLayout(&pipelineLayoutDesc);

        wgpu::VertexAttribute attributes[4] = {};
        attributes[0].format = wgpu::VertexFormat::Float32x3;
        attributes[0].offset = offsetof(gos_VERTEX, x);
        attributes[0].shaderLocation = 0;
        attributes[1].format = wgpu::VertexFormat::Unorm8x4;
        attributes[1].offset = offsetof(gos_VERTEX, argb);
        attributes[1].shaderLocation = 1;
        attributes[2].format = wgpu::VertexFormat::Float32x2;
        attributes[2].offset = offsetof(gos_VERTEX, u);
        attributes[2].shaderLocation = 2;
        attributes[3].format = wgpu::VertexFormat::Float32;
        attributes[3].offset = offsetof(gos_VERTEX, rhw);
        attributes[3].shaderLocation = 3;

        wgpu::VertexBufferLayout vertexLayout = {};
        vertexLayout.arrayStride = sizeof(gos_VERTEX);
        vertexLayout.stepMode = wgpu::VertexStepMode::Vertex;
        vertexLayout.attributeCount = 4;
        vertexLayout.attributes = attributes;

        wgpu::ColorTargetState colorTarget = {};
        colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
        colorTarget.blend = nullptr;
        colorTarget.writeMask = wgpu::ColorWriteMask::All;

        wgpu::FragmentState fragment = {};
        fragment.module = shader;
        fragment.entryPoint = "fs_main";
        fragment.targetCount = 1;
        fragment.targets = &colorTarget;

        wgpu::DepthStencilState depth = {};
        depth.format = wgpu::TextureFormat::Depth24Plus;
        depth.depthWriteEnabled = true;
        depth.depthCompare = wgpu::CompareFunction::LessEqual;

        wgpu::RenderPipelineDescriptor pipelineDesc = {};
        pipelineDesc.label = "MC2 M3-1 proof pipeline";
        pipelineDesc.layout = pipelineLayout;
        pipelineDesc.vertex.module = shader;
        pipelineDesc.vertex.entryPoint = "vs_main";
        pipelineDesc.vertex.bufferCount = 1;
        pipelineDesc.vertex.buffers = &vertexLayout;
        pipelineDesc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipelineDesc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
        pipelineDesc.depthStencil = &depth;
        pipelineDesc.multisample.count = 1;
        pipelineDesc.fragment = &fragment;
        m_proofPipeline = m_device.CreateRenderPipeline(&pipelineDesc);
        if (!m_proofPipeline) return false;

        wgpu::BufferDescriptor uniformDesc = {};
        uniformDesc.label = "MC2 viewport uniform";
        uniformDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniformDesc.size = sizeof(ViewportUniform);
        m_viewportBuffer = m_device.CreateBuffer(&uniformDesc);
        uniformDesc.label = "MC2 800x600 canvas viewport uniform";
        m_canvasViewportBuffer = m_device.CreateBuffer(&uniformDesc);

        gos_VERTEX vertices[6] = {};
        const auto setVertex = [](gos_VERTEX& vertex, float x, float y, float u, float v)
        {
            vertex.x = x;
            vertex.y = y;
            vertex.z = 0.5f;
            vertex.rhw = 1.0f;
            vertex.argb = 0xFFFFFFFFu;
            vertex.frgb = 0;
            vertex.u = u;
            vertex.v = v;
        };
        setVertex(vertices[0], 300.0f, 250.0f, 0.0f, 0.0f);
        setVertex(vertices[1], 300.0f, 350.0f, 0.0f, 1.0f);
        setVertex(vertices[2], 500.0f, 350.0f, 1.0f, 1.0f);
        setVertex(vertices[3], 300.0f, 250.0f, 0.0f, 0.0f);
        setVertex(vertices[4], 500.0f, 350.0f, 1.0f, 1.0f);
        setVertex(vertices[5], 500.0f, 250.0f, 1.0f, 0.0f);

        wgpu::BufferDescriptor vertexDesc = {};
        vertexDesc.label = "MC2 M3-1 proof quad";
        vertexDesc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vertexDesc.size = sizeof(vertices);
        m_proofVertexBuffer = m_device.CreateBuffer(&vertexDesc);
        m_queue.WriteBuffer(m_proofVertexBuffer, 0, vertices, sizeof(vertices));

        const unsigned char checkerPixels[16] = {
            255,   0,   0, 255,      0, 255,   0, 255,
              0,   0, 255, 255,    255, 255,   0, 255
        };
        wgpu::TextureDescriptor checkerDesc = {};
        checkerDesc.label = "MC2 M3-1 checkerboard";
        checkerDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        checkerDesc.dimension = wgpu::TextureDimension::e2D;
        checkerDesc.size = { 2, 2, 1 };
        checkerDesc.format = wgpu::TextureFormat::RGBA8Unorm;
        checkerDesc.mipLevelCount = 1;
        checkerDesc.sampleCount = 1;
        m_proofTexture = m_device.CreateTexture(&checkerDesc);
        m_proofTextureView = m_proofTexture.CreateView();

        wgpu::TexelCopyTextureInfo checkerDestination = {};
        checkerDestination.texture = m_proofTexture;
        checkerDestination.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyBufferLayout checkerLayout = {};
        checkerLayout.bytesPerRow = 8;
        checkerLayout.rowsPerImage = 2;
        const wgpu::Extent3D checkerSize = { 2, 2, 1 };
        m_queue.WriteTexture(&checkerDestination, checkerPixels, sizeof(checkerPixels),
                             &checkerLayout, &checkerSize);

        wgpu::SamplerDescriptor samplerDesc = {};
        samplerDesc.label = "MC2 M3-1 nearest sampler";
        samplerDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
        samplerDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
        samplerDesc.addressModeW = wgpu::AddressMode::ClampToEdge;
        samplerDesc.magFilter = wgpu::FilterMode::Nearest;
        samplerDesc.minFilter = wgpu::FilterMode::Nearest;
        samplerDesc.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
        m_proofSampler = m_device.CreateSampler(&samplerDesc);

        wgpu::BindGroupEntry bindEntries[3] = {};
        bindEntries[0].binding = 0;
        bindEntries[0].buffer = m_viewportBuffer;
        bindEntries[0].offset = 0;
        bindEntries[0].size = sizeof(ViewportUniform);
        bindEntries[1].binding = 1;
        bindEntries[1].sampler = m_proofSampler;
        bindEntries[2].binding = 2;
        bindEntries[2].textureView = m_proofTextureView;

        wgpu::BindGroupDescriptor bindGroupDesc = {};
        bindGroupDesc.label = "MC2 M3-1 proof bind group";
        bindGroupDesc.layout = m_proofBindLayout;
        bindGroupDesc.entryCount = 3;
        bindGroupDesc.entries = bindEntries;
        m_proofBindGroup = m_device.CreateBindGroup(&bindGroupDesc);

        if (!m_viewportBuffer || !m_proofVertexBuffer || !m_proofTexture ||
            !m_proofTextureView || !m_proofSampler || !m_proofBindGroup)
            return false;

        wgpu::BindGroupLayoutEntry viewportEntry = {};
        viewportEntry.binding = 0;
        viewportEntry.visibility = wgpu::ShaderStage::Vertex;
        viewportEntry.buffer.type = wgpu::BufferBindingType::Uniform;
        viewportEntry.buffer.minBindingSize = sizeof(ViewportUniform);
        wgpu::BindGroupLayoutDescriptor viewportLayoutDesc = {};
        viewportLayoutDesc.label = "MC2 viewport bind layout";
        viewportLayoutDesc.entryCount = 1;
        viewportLayoutDesc.entries = &viewportEntry;
        m_viewportBindLayout = m_device.CreateBindGroupLayout(&viewportLayoutDesc);

        wgpu::BindGroupLayoutEntry samplerEntry = {};
        samplerEntry.binding = 0;
        samplerEntry.visibility = wgpu::ShaderStage::Fragment;
        samplerEntry.sampler.type = wgpu::SamplerBindingType::Filtering;
        wgpu::BindGroupLayoutDescriptor samplerLayoutDesc = {};
        samplerLayoutDesc.label = "MC2 sampler bind layout";
        samplerLayoutDesc.entryCount = 1;
        samplerLayoutDesc.entries = &samplerEntry;
        m_samplerBindLayout = m_device.CreateBindGroupLayout(&samplerLayoutDesc);

        wgpu::BindGroupLayoutEntry textureEntry = {};
        textureEntry.binding = 0;
        textureEntry.visibility = wgpu::ShaderStage::Fragment;
        textureEntry.texture.sampleType = wgpu::TextureSampleType::Float;
        textureEntry.texture.viewDimension = wgpu::TextureViewDimension::e2D;
        wgpu::BindGroupLayoutDescriptor textureLayoutDesc = {};
        textureLayoutDesc.label = "MC2 texture bind layout";
        textureLayoutDesc.entryCount = 1;
        textureLayoutDesc.entries = &textureEntry;
        m_textureBindLayout = m_device.CreateBindGroupLayout(&textureLayoutDesc);

        wgpu::BindGroupLayoutEntry fogEntry = {};
        fogEntry.binding = 0;
        fogEntry.visibility = wgpu::ShaderStage::Fragment;
        fogEntry.buffer.type = wgpu::BufferBindingType::Uniform;
        fogEntry.buffer.minBindingSize = sizeof(FogUniform);
        wgpu::BindGroupLayoutDescriptor fogLayoutDesc = {};
        fogLayoutDesc.label = "MC2 fog bind layout";
        fogLayoutDesc.entryCount = 1;
        fogLayoutDesc.entries = &fogEntry;
        m_fogBindLayout = m_device.CreateBindGroupLayout(&fogLayoutDesc);

        const wgpu::BindGroupLayout groupLayouts[4] = {
            m_viewportBindLayout, m_samplerBindLayout, m_textureBindLayout, m_fogBindLayout
        };
        wgpu::PipelineLayoutDescriptor realPipelineLayoutDesc = {};
        realPipelineLayoutDesc.label = "MC2 draw pipeline layout";
        realPipelineLayoutDesc.bindGroupLayoutCount = 4;
        realPipelineLayoutDesc.bindGroupLayouts = groupLayouts;
        m_pipelineLayout = m_device.CreatePipelineLayout(&realPipelineLayoutDesc);

        wgpu::BindGroupEntry viewportBindEntry = {};
        viewportBindEntry.binding = 0;
        viewportBindEntry.buffer = m_viewportBuffer;
        viewportBindEntry.offset = 0;
        viewportBindEntry.size = sizeof(ViewportUniform);
        wgpu::BindGroupDescriptor viewportBindDesc = {};
        viewportBindDesc.label = "MC2 viewport bind group";
        viewportBindDesc.layout = m_viewportBindLayout;
        viewportBindDesc.entryCount = 1;
        viewportBindDesc.entries = &viewportBindEntry;
        m_viewportBindGroup = m_device.CreateBindGroup(&viewportBindDesc);
        viewportBindEntry.buffer = m_canvasViewportBuffer;
        viewportBindDesc.label = "MC2 800x600 canvas viewport bind group";
        m_canvasViewportBindGroup = m_device.CreateBindGroup(&viewportBindDesc);

        const unsigned char whitePixel[4] = { 255, 255, 255, 255 };
        wgpu::TextureDescriptor whiteDesc = {};
        whiteDesc.label = "MC2 untextured white";
        whiteDesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        whiteDesc.dimension = wgpu::TextureDimension::e2D;
        whiteDesc.size = { 1, 1, 1 };
        whiteDesc.format = wgpu::TextureFormat::RGBA8Unorm;
        whiteDesc.mipLevelCount = 1;
        whiteDesc.sampleCount = 1;
        m_whiteTexture = m_device.CreateTexture(&whiteDesc);
        m_whiteTextureView = m_whiteTexture.CreateView();
        wgpu::TexelCopyTextureInfo whiteDestination = {};
        whiteDestination.texture = m_whiteTexture;
        whiteDestination.aspect = wgpu::TextureAspect::All;
        wgpu::TexelCopyBufferLayout whiteLayout = {};
        whiteLayout.bytesPerRow = 4;
        whiteLayout.rowsPerImage = 1;
        const wgpu::Extent3D whiteSize = { 1, 1, 1 };
        m_queue.WriteTexture(&whiteDestination, whitePixel, sizeof(whitePixel),
                             &whiteLayout, &whiteSize);

        wgpu::BindGroupEntry whiteEntry = {};
        whiteEntry.binding = 0;
        whiteEntry.textureView = m_whiteTextureView;
        wgpu::BindGroupDescriptor whiteBindDesc = {};
        whiteBindDesc.label = "MC2 untextured bind group";
        whiteBindDesc.layout = m_textureBindLayout;
        whiteBindDesc.entryCount = 1;
        whiteBindDesc.entries = &whiteEntry;
        m_whiteBindGroup = m_device.CreateBindGroup(&whiteBindDesc);
        ++m_textureBindGroupCount;

        if (!m_viewportBindLayout || !m_samplerBindLayout || !m_textureBindLayout ||
            !m_fogBindLayout ||
            !m_pipelineLayout || !m_viewportBindGroup || !m_canvasViewportBuffer ||
            !m_canvasViewportBindGroup || !m_whiteTexture ||
            !m_whiteTextureView || !m_whiteBindGroup)
            return false;

        std::printf("[webgpu] shared draw resources ready (M3-1 proof resources retained, proof draw disabled)\n");
        return true;
    }



    PipelineKey WebGPURenderer::CurrentPipelineKey(wgpu::PrimitiveTopology topology) const
    {
        PipelineKey key = {};
        key.topology = topology;
        key.alphaMode = m_state.Get(gos_State_AlphaMode);
        key.zCompare = m_state.Get(gos_State_ZCompare);
        key.zWrite = m_state.Get(gos_State_ZWrite) ? 1 : 0;
        if (m_disableDepth) { key.zCompare = 0; key.zWrite = 0; }
        key.alphaTest = m_state.Get(gos_State_AlphaTest) ? 1 : 0;
        key.textureBlend = m_state.Get(gos_State_TextureMapBlend);
        const DWORD textureHandle = static_cast<DWORD>(m_state.Get(gos_State_Texture));
        const std::map<DWORD, Texture*>::const_iterator texture =
            m_textures.find(textureHandle);
        key.textureSolid = textureHandle == 0 ||
            (texture != m_textures.end() &&
             texture->second->format == gos_Texture_Solid);
        key.perspective = !m_noPerspective &&
            m_state.Get(gos_State_Perspective) != 0;
        // PORT (M8-B11): honour gos_State_Specular, exactly as D3D's
        // D3DRS_SPECULARENABLE does.
        //
        // Binding frgb unconditionally made every effect a white square, and
        // the reason is MLR: GOSVertex.cpp:36 and GOSVertex2UV.cpp:26
        // initialise frgb to 0xffffffff, and MLR thereafter writes only BYTE 3
        // -- the fog byte -- leaving the RGB at full white forever. Retail got
        // away with that because it drew those primitives with specular
        // DISABLED, so the white was never read.
        //
        // The game already draws this distinction for us and always did:
        // objmgr.cpp:1493 and MechCmd2.cpp:910 enable specular for the
        // hardware world pass, while the GUI, tacmap, movie, cevfx, celine and
        // terrain-object passes disable it. Keying on the state is therefore
        // not a special case for terrain -- it is the contract the whole
        // codebase was written against.
        key.specular = m_state.Get(gos_State_Specular) ? 1 : 0;
        // gos_State_Fog supplies the colour; frgb alpha supplies the already
        // computed per-vertex visibility factor (ff = clear, 00 = full fog).
        key.fog = m_state.Get(gos_State_Fog) ? 1 : 0;
        if (key.alphaMode == 0) key.alphaMode = gos_Alpha_OneZero;
        if (key.textureBlend == 0) key.textureBlend = gos_BlendModulateAlpha;
        return key;
    }



    wgpu::RenderPipeline WebGPURenderer::PipelineFor(wgpu::PrimitiveTopology topology)
    { return PipelineForKey(CurrentPipelineKey(topology)); }



    wgpu::RenderPipeline WebGPURenderer::PipelineForKey(const PipelineKey& key)
    {
        std::map<PipelineKey, wgpu::RenderPipeline>::iterator found = m_pipelines.find(key);
        if (found != m_pipelines.end()) return found->second;

        std::string combine;
        switch (key.textureBlend)
        {
            case gos_BlendDecal:
                combine = "var outColor = texel;\n";
                break;
            case gos_BlendModulate:
                // PORT: GameOS specifies that Modulate takes alpha from vertices
                // for a solid texture, and from the texture for keyed/alpha
                // textures. Solid uploads are XRGB-compatible and therefore
                // have opaque texels, so this distinction must be in the shader.
                combine = key.textureSolid ?
                    "var outColor = vec4<f32>(texel.rgb * in.color.rgb, in.color.a);\n" :
                    "var outColor = vec4<f32>(texel.rgb * in.color.rgb, texel.a);\n";
                break;
            case gos_BlendModulateAlpha:
            default:
                combine = "var outColor = texel * in.color;\n";
                break;
        }
        std::string shaderSource = R"WGSL(
struct Viewport {
    size: vec2<f32>,
    scale: f32,
    unused: f32,
    offset: vec2<f32>,
    padding: vec2<f32>,
};
struct VertexOut {
    @builtin(position) position: vec4<f32>,
    // Colour stays screen-linear, which is what it has always been here and
    // what D3D's Gouraud shading specifies. Only uv gains perspective
    // correction, so nothing but texture mapping changes.
    @location(0) @interpolate(linear) color: vec4<f32>,
    @location(1) uv: vec2<f32>,
    // Specular is Gouraud-shaded exactly like colour, so it interpolates the
    // same screen-linear way.
    @location(2) @interpolate(linear) specular: vec3<f32>,
    @location(3) @interpolate(linear) fog: f32,
};
@group(0) @binding(0) var<uniform> viewport: Viewport;
@group(1) @binding(0) var drawSampler: sampler;
@group(2) @binding(0) var drawTexture: texture_2d<f32>;
struct Fog { color: vec4<f32> };
@group(3) @binding(0) var<uniform> fogUniform: Fog;
@vertex
fn vs_main(@location(0) position: vec3<f32>,
           @location(1) packedBgra: vec4<f32>,
           @location(2) uv: vec2<f32>,
           @location(3) rhw: f32,
           @location(4) packedSpecularBgra: vec4<f32>) -> VertexOut {
    var out: VertexOut;
    let canvasPosition = position.xy * viewport.scale + viewport.offset;
    let ndc = vec2<f32>(canvasPosition.x / viewport.size.x * 2.0 - 1.0,
                        1.0 - canvasPosition.y / viewport.size.y * 2.0);
    // gos_VERTEX is D3D's pre-transformed XYZRHW form: x,y are already screen
    // pixels and rhw is the 1/w the CPU projection divided them by. Emitting
    // w = 1 threw that away, so the rasteriser interpolated u,v linearly in
    // screen space -- affine texture mapping, which bows straight features
    // painted into a texture across each triangle's diagonal. Scaling the clip
    // position by w hands w back: the hardware interpolates u,v
    // perspective-correctly and then divides the position straight back to the
    // same pixels and the same depth.
    var w = 1.0;
    if (rhw > 0.0) { w = 1.0 / rhw; }
    out.position = vec4<f32>(ndc.x * w, ndc.y * w, position.z * w, w);
    out.color = packedBgra.bgra;
    out.uv = uv;
    // Same little-endian BGRA unpack as the diffuse colour. RGB is additive
    // specular; alpha is MC2's already-computed per-vertex fog factor.
    out.specular = packedSpecularBgra.bgr;
    out.fog = packedSpecularBgra.a;
    return out;
}
@fragment
fn fs_main(in: VertexOut) -> @location(0) vec4<f32> {
)WGSL";
        shaderSource += m_flatTextures ?
            "let texel = vec4<f32>(1.0, 1.0, 1.0, 1.0);\n" :
            "let texel = textureSample(drawTexture, drawSampler, in.uv);\n";
        shaderSource += combine;
        // D3D fixed-function adds specular AFTER the texture/diffuse blend and
        // leaves alpha alone, so alpha test and blending are unaffected. Only
        // when the state enables it -- see CurrentPipelineKey.
        if (key.specular)
            shaderSource += "outColor = vec4<f32>(outColor.rgb + in.specular, outColor.a);\n";
        if (key.fog)
            shaderSource += "outColor = vec4<f32>(mix(fogUniform.color.rgb, outColor.rgb, clamp(in.fog, 0.0, 1.0)), outColor.a);\n";
        if (key.alphaTest) shaderSource += "if (outColor.a <= 0.0) { discard; }\n";
        shaderSource += "return outColor;\n}\n";

        // gos_State_Perspective controls texture correction, not projection:
        // gos_VERTEX is already projected screen-space XYZRHW. Keep the clip
        // position reconstruction for correct depth, but request screen-linear
        // UV interpolation for affine draws such as MC2's projected shadows.
        if (!key.perspective)
        {
            const std::string uv = "    @location(1) uv: vec2<f32>,\n";
            const size_t at = shaderSource.find(uv);
            if (at != std::string::npos)
                shaderSource.replace(at, uv.size(),
                    "    @location(1) @interpolate(linear) uv: vec2<f32>,\n");
        }

        // MC2_NO_PERSPECTIVE remains a whole-process discriminator by forcing
        // key.perspective false in CurrentPipelineKey above.

        wgpu::ShaderSourceWGSL wgsl = {};
        wgsl.code = std::string_view(shaderSource);
        wgpu::ShaderModuleDescriptor shaderDesc = {};
        shaderDesc.label = "MC2 state pipeline shader";
        shaderDesc.nextInChain = &wgsl;
        const wgpu::ShaderModule shader = m_device.CreateShaderModule(&shaderDesc);

        // PORT (M8-B11): gos_VERTEX::frgb is D3D's SPECULAR colour, and this
        // layout never bound it. Every value written there was uploaded to
        // nothing. That is where MC2 puts all of its additive light:
        // quad.cpp packs each terrain vertex's accumulated point/spot/terrain
        // light contribution into fogRGB -> frgb, and tgl.cpp does the same for
        // shape specular. None of it has ever reached the screen in this port.
        //
        // MEASURED on mc2_05 before the fix: 60,806 vertex-light contributions
        // per 400,000 tests, 22,653 of them from TG_LIGHT_SPOT, at falloff up
        // to 1.0 -- all computed, all discarded here.
        //
        // Safe to add because nothing writes white into this field: across the
        // 99 assignments in the game the values are 0, 0xff000000 (alpha-only,
        // RGB zero), fogRGB, and explicit specular. Geometry that carries no
        // specular therefore adds exactly zero and does not change.
        wgpu::VertexAttribute attributes[5] = {};
        attributes[0].format = wgpu::VertexFormat::Float32x3;
        attributes[0].offset = offsetof(gos_VERTEX, x);
        attributes[0].shaderLocation = 0;
        attributes[1].format = wgpu::VertexFormat::Unorm8x4;
        attributes[1].offset = offsetof(gos_VERTEX, argb);
        attributes[1].shaderLocation = 1;
        attributes[2].format = wgpu::VertexFormat::Float32x2;
        attributes[2].offset = offsetof(gos_VERTEX, u);
        attributes[2].shaderLocation = 2;
        attributes[3].format = wgpu::VertexFormat::Float32;
        attributes[3].offset = offsetof(gos_VERTEX, rhw);
        attributes[3].shaderLocation = 3;
        attributes[4].format = wgpu::VertexFormat::Unorm8x4;
        attributes[4].offset = offsetof(gos_VERTEX, frgb);
        attributes[4].shaderLocation = 4;
        wgpu::VertexBufferLayout vertexLayout = {};
        vertexLayout.arrayStride = sizeof(gos_VERTEX);
        vertexLayout.stepMode = wgpu::VertexStepMode::Vertex;
        vertexLayout.attributeCount = 5;
        vertexLayout.attributes = attributes;

        wgpu::BlendState blend = {};
        blend.color.operation = wgpu::BlendOperation::Add;
        blend.alpha.operation = wgpu::BlendOperation::Add;
        bool blending = key.alphaMode != gos_Alpha_OneZero;
        switch (key.alphaMode)
        {
            case gos_Alpha_OneOne:
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::One;
                blend.alpha.srcFactor = wgpu::BlendFactor::One;
                blend.alpha.dstFactor = wgpu::BlendFactor::One;
                break;
            case gos_Alpha_AlphaInvAlpha:
                blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
                blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                blend.alpha.srcFactor = wgpu::BlendFactor::One;
                blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                break;
            case gos_Alpha_OneInvAlpha:
                blend.color.srcFactor = wgpu::BlendFactor::One;
                blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                blend.alpha.srcFactor = wgpu::BlendFactor::One;
                blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
                break;
            case gos_Alpha_AlphaOne:
                blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
                blend.color.dstFactor = wgpu::BlendFactor::One;
                blend.alpha.srcFactor = wgpu::BlendFactor::One;
                blend.alpha.dstFactor = wgpu::BlendFactor::One;
                break;
            default:
                blending = false;
                break;
        }

        wgpu::ColorTargetState colorTarget = {};
        colorTarget.format = wgpu::TextureFormat::RGBA8Unorm;
        colorTarget.blend = blending ? &blend : nullptr;
        colorTarget.writeMask = wgpu::ColorWriteMask::All;
        wgpu::FragmentState fragment = {};
        fragment.module = shader;
        fragment.entryPoint = "fs_main";
        fragment.targetCount = 1;
        fragment.targets = &colorTarget;

        wgpu::DepthStencilState depth = {};
        depth.format = wgpu::TextureFormat::Depth24Plus;
        depth.depthWriteEnabled = key.zWrite != 0;
        depth.depthCompare = key.zCompare == 2 ? wgpu::CompareFunction::Less :
                             key.zCompare == 0 ? wgpu::CompareFunction::Always :
                                                 wgpu::CompareFunction::LessEqual;

        wgpu::RenderPipelineDescriptor pipelineDesc = {};
        pipelineDesc.label = "MC2 cached draw pipeline";
        pipelineDesc.layout = m_pipelineLayout;
        pipelineDesc.vertex.module = shader;
        pipelineDesc.vertex.entryPoint = "vs_main";
        pipelineDesc.vertex.bufferCount = 1;
        pipelineDesc.vertex.buffers = &vertexLayout;
        pipelineDesc.primitive.topology = key.topology;
        pipelineDesc.primitive.frontFace = wgpu::FrontFace::CCW;
        pipelineDesc.primitive.cullMode = wgpu::CullMode::None;
        pipelineDesc.depthStencil = &depth;
        pipelineDesc.multisample.count = 1;
        pipelineDesc.fragment = &fragment;

        wgpu::RenderPipeline pipeline = m_device.CreateRenderPipeline(&pipelineDesc);
        if (!pipeline) return nullptr;
        m_pipelines[key] = pipeline;
        std::printf("[webgpu] pipeline[%u]: topology=%u alpha=%d z=%d/%d alphaTest=%d texBlend=%d solid=%d perspective=%d\n",
                    (unsigned)m_pipelines.size(), (unsigned)key.topology, key.alphaMode,
                    key.zCompare, key.zWrite, key.alphaTest, key.textureBlend,
                    key.textureSolid ? 1 : 0, key.perspective ? 1 : 0);
        return pipeline;
    }



    SamplerBinding* WebGPURenderer::SamplerForCurrentState()
    {
        SamplerKey key = { m_state.Get(gos_State_Filter),
                           m_state.Get(gos_State_TextureAddress) };

        // PORT: MC2_UI_POINT_SAMPLING forces nearest filtering on draws made
        // through the authored 800x600 canvas. Those are magnified by roughly
        // 1.8 at 1920x1080, and bilinear magnification of a packed atlas blends
        // in whatever texel sits next door -- which shows up as a thin coloured
        // line along an icon edge. Point sampling cannot do that. So if a line
        // survives this switch it is not filter bleed, and the cause is the
        // geometry, the UVs or the source art instead.
        if (m_uiPointSampling && MC2_IsVirtualCanvasActive())
        {
            // Counted, and reported once, because the first version of this
            // switch produced a byte-identical frame and there was no way to
            // tell whether it had fired and changed nothing or never fired at
            // all. An instrument that cannot distinguish those two is useless.
            if (key.filter != gos_FilterNone)
            {
                ++m_uiPointForced;
                if (m_uiPointForced == 1)
                    std::fprintf(stderr, "[webgpu] MC2_UI_POINT_SAMPLING: first draw "
                                         "forced from filter %d to point\n", key.filter);
            }
            else
            {
                ++m_uiPointAlreadyPoint;
            }
            key.filter = gos_FilterNone;
        }

        return SamplerForKey(key);
    }



    SamplerBinding* WebGPURenderer::SamplerForKey(SamplerKey key)
    {
        if (key.filter == 0) key.filter = gos_FilterNone;
        if (key.address == 0) key.address = gos_TextureWrap;
        std::map<SamplerKey, SamplerBinding>::iterator found = m_samplers.find(key);
        if (found != m_samplers.end()) return &found->second;

        const bool point = key.filter == gos_FilterNone;
        const wgpu::AddressMode address = key.address == gos_TextureClamp ?
            wgpu::AddressMode::ClampToEdge : wgpu::AddressMode::Repeat;
        wgpu::SamplerDescriptor samplerDesc = {};
        samplerDesc.label = "MC2 cached sampler";
        samplerDesc.addressModeU = address;
        samplerDesc.addressModeV = address;
        samplerDesc.addressModeW = address;
        samplerDesc.magFilter = point ? wgpu::FilterMode::Nearest : wgpu::FilterMode::Linear;
        samplerDesc.minFilter = point ? wgpu::FilterMode::Nearest : wgpu::FilterMode::Linear;
        samplerDesc.mipmapFilter = key.filter == gos_FilterTriLinear ?
            wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest;

        SamplerBinding binding;
        binding.sampler = m_device.CreateSampler(&samplerDesc);
        wgpu::BindGroupEntry entry = {};
        entry.binding = 0;
        entry.sampler = binding.sampler;
        wgpu::BindGroupDescriptor bindDesc = {};
        bindDesc.label = "MC2 sampler bind group";
        bindDesc.layout = m_samplerBindLayout;
        bindDesc.entryCount = 1;
        bindDesc.entries = &entry;
        binding.bindGroup = m_device.CreateBindGroup(&bindDesc);
        m_samplers[key] = binding;
        return &m_samplers.find(key)->second;
    }



    wgpu::BindGroup WebGPURenderer::TextureBindGroupForCurrentState()
    {
        const DWORD handle = static_cast<DWORD>(m_state.Get(gos_State_Texture));
        return TextureBindGroupForHandle(handle);
    }



    wgpu::BindGroup WebGPURenderer::TextureBindGroupForHandle(DWORD handle)
    {
        if (handle == 0) return m_whiteBindGroup;
        std::map<DWORD, Texture*>::iterator found = m_textures.find(handle);
        if (found == m_textures.end() || !found->second->gpu) return m_whiteBindGroup;

        Texture& texture = *found->second;
        if (texture.bindGroup) return texture.bindGroup;
        texture.view = texture.gpu.CreateView();
        wgpu::BindGroupEntry entry = {};
        entry.binding = 0;
        entry.textureView = texture.view;
        wgpu::BindGroupDescriptor bindDesc = {};
        bindDesc.label = texture.name.empty() ? "MC2 texture bind group" : texture.name.c_str();
        bindDesc.layout = m_textureBindLayout;
        bindDesc.entryCount = 1;
        bindDesc.entries = &entry;
        texture.bindGroup = m_device.CreateBindGroup(&bindDesc);
        if (texture.bindGroup) ++m_textureBindGroupCount;
        return texture.bindGroup ? texture.bindGroup : m_whiteBindGroup;
    }



    wgpu::BindGroup WebGPURenderer::FogBindGroupForCurrentState()
    {
        const DWORD tracked = static_cast<DWORD>(m_state.Get(gos_State_Fog));
        const DWORD color = tracked & 0x00ffffffu;
        std::map<DWORD, FogBinding>::iterator found = m_fogBindings.find(color);
        if (found != m_fogBindings.end()) return found->second.bindGroup;

        const FogUniform uniform = {
            static_cast<float>((color >> 16) & 0xffu) / 255.0f,
            static_cast<float>((color >> 8) & 0xffu) / 255.0f,
            static_cast<float>(color & 0xffu) / 255.0f,
            0.0f
        };
        FogBinding binding;
        wgpu::BufferDescriptor bufferDesc = {};
        bufferDesc.label = "MC2 fog color uniform";
        bufferDesc.size = sizeof(FogUniform);
        bufferDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        binding.buffer = m_device.CreateBuffer(&bufferDesc);
        if (!binding.buffer) return nullptr;
        m_queue.WriteBuffer(binding.buffer, 0, &uniform, sizeof(uniform));

        wgpu::BindGroupEntry entry = {};
        entry.binding = 0;
        entry.buffer = binding.buffer;
        entry.offset = 0;
        entry.size = sizeof(FogUniform);
        wgpu::BindGroupDescriptor bindDesc = {};
        bindDesc.label = "MC2 fog color bind group";
        bindDesc.layout = m_fogBindLayout;
        bindDesc.entryCount = 1;
        bindDesc.entries = &entry;
        binding.bindGroup = m_device.CreateBindGroup(&bindDesc);
        m_fogBindings[color] = binding;
        return m_fogBindings.find(color)->second.bindGroup;
    }



    bool WebGPURenderer::BindDrawState(wgpu::PrimitiveTopology topology)
    {
        if (!m_frameOpen) return false;
        const PipelineKey key = CurrentPipelineKey(topology);
        const wgpu::RenderPipeline pipeline = PipelineForKey(key);
        SamplerBinding* sampler = SamplerForCurrentState();
        const wgpu::BindGroup texture = TextureBindGroupForCurrentState();
        const wgpu::BindGroup fog = FogBindGroupForCurrentState();
        if (!pipeline || !sampler || !sampler->bindGroup || !texture || !fog)
            return false;
        m_pass.SetPipeline(pipeline);
        m_pass.SetBindGroup(0, ViewportBindGroupForCurrentCanvas());
        m_pass.SetBindGroup(1, sampler->bindGroup);
        m_pass.SetBindGroup(2, texture);
        m_pass.SetBindGroup(3, fog);
        return true;
    }

    wgpu::BindGroup WebGPURenderer::ViewportBindGroupForCurrentCanvas()
    {
        if (MC2_IsVirtualCanvasActive())
            return m_canvasViewportBindGroup;
        return m_viewportBindGroup;
    }

} // namespace mc2webgpu
