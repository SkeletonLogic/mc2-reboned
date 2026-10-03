//===========================================================================//
// The D3D8-era fixed-function render state machine, shared by every backend.
//
// WHY THIS IS SHARED RATHER THAN COPIED
//
// The null backend's job is to MEASURE the state space so the WebGPU backend can
// be planned against a real number instead of Phase 0's "691 gos_SetRenderState
// call sites" (which is a call count, not a state space). That only works if
// both backends account for state IDENTICALLY -- the moment each keeps its own
// copy of the stack and the census, a disagreement between them stops being
// evidence about WebGPU and becomes a bug in the bookkeeping.
//
// So the state block, the push/pop stack and the census live here once, and both
// backends hold one. Comparing `DumpStats` output between backends is then a
// meaningful A/B.
//
// SIZED FROM THE GAME'S OWN ENUM. gos_MaxState is Microsoft's end marker, so it
// is the correct bound. The previous hard-coded 64 silently accepted states that
// cannot exist and silently dropped nothing; out-of-range values are now counted
// and reported instead, because a state the game sets and the renderer ignores
// is exactly the kind of thing that shows up as a wrong picture much later.
//===========================================================================//

#pragma once

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include <gameos.hpp>

#include "renderer_font.h"

namespace mc2gfx {

// gos_State_Texture is 1, so slot 0 is never used. Keeping the array 1-based
// rather than biasing the index keeps the printed census readable against the
// header.
const int kRenderStateCount = (int)gos_MaxState;

// Names matter here: a census that prints "5:2 x14" cannot be planned against,
// while "ZCompare:2 x14" says immediately that two depth-test modes are needed.
inline const char* RenderStateName(int state)
{
    switch (state)
    {
        case gos_State_Texture:          return "Texture";
        case gos_State_Texture2:         return "Texture2";
        case gos_State_Texture3:         return "Texture3";
        case gos_State_Filter:           return "Filter";
        case gos_State_ZCompare:         return "ZCompare";
        case gos_State_ZWrite:           return "ZWrite";
        case gos_State_AlphaTest:        return "AlphaTest";
        case gos_State_Perspective:      return "Perspective";
        case gos_State_Specular:         return "Specular";
        case gos_State_Dither:           return "Dither";
        case gos_State_Clipping:         return "Clipping";
        case gos_State_WireframeMode:    return "Wireframe";
        case gos_State_AlphaMode:        return "AlphaMode";
        case gos_State_TextureAddress:   return "TextureAddress";
        case gos_State_ShadeMode:        return "ShadeMode";
        case gos_State_TextureMapBlend:  return "TextureMapBlend";
        case gos_State_MipMapBias:       return "MipMapBias";
        case gos_State_Fog:              return "Fog";
        case gos_State_MonoEnable:       return "MonoEnable";
        case gos_State_Culling:          return "Culling";
        case gos_State_StencilEnable:    return "StencilEnable";
        case gos_State_StencilFunc:      return "StencilFunc";
        case gos_State_StencilRef:       return "StencilRef";
        case gos_State_StencilMask:      return "StencilMask";
        case gos_State_StencilZFail:     return "StencilZFail";
        case gos_State_StencilFail:      return "StencilFail";
        case gos_State_StencilPass:      return "StencilPass";
        case gos_State_Multitexture:     return "Multitexture";
        case gos_State_Ambient:          return "Ambient";
        case gos_State_Lighting:         return "Lighting";
        case gos_State_NormalizeNormals: return "NormalizeNormals";
        case gos_State_VertexBlend:      return "VertexBlend";
        default:                         return "?";
    }
}

inline const char* AlphaModeName(int mode)
{
    switch (mode)
    {
        case gos_Alpha_OneZero:       return "OneZero";
        case gos_Alpha_OneOne:        return "OneOne";
        case gos_Alpha_AlphaInvAlpha: return "AlphaInvAlpha";
        case gos_Alpha_OneInvAlpha:   return "OneInvAlpha";
        case gos_Alpha_AlphaOne:      return "AlphaOne";
        default:                      return "?";
    }
}

class RenderStateTracker
{
public:
    // tag appears in diagnostics so a stack error is attributable to a backend.
    explicit RenderStateTracker(const char* tag)
        : m_tag(tag ? tag : "renderer"), m_setCalls(0), m_stackErrors(0), m_outOfRange(0)
    {
        std::memset(&m_state, 0, sizeof(m_state));
        std::memset(m_uses, 0, sizeof(m_uses));

        // Defaults documented by Microsoft's GameOS header. They do not count
        // as SetRenderState calls, but Get() must still return the state a draw
        // sees before the game explicitly changes it.
        m_state.value[gos_State_Filter]          = gos_FilterNone;
        m_state.value[gos_State_ZCompare]        = 1;
        m_state.value[gos_State_ZWrite]          = 1;
        m_state.value[gos_State_Perspective]     = 1;
        m_state.value[gos_State_AlphaMode]       = gos_Alpha_OneZero;
        m_state.value[gos_State_TextureAddress]  = gos_TextureWrap;
        m_state.value[gos_State_ShadeMode]       = gos_ShadeGouraud;
        m_state.value[gos_State_TextureMapBlend] = gos_BlendModulateAlpha;
    }

    void Set(gos_RenderState state, int value)
    {
        ++m_setCalls;
        const int i = (int)state;
        if (i <= 0 || i >= kRenderStateCount)
        {
            // Not silently dropped: an unhandled state is a rendering difference
            // waiting to be blamed on something else.
            ++m_outOfRange;
            return;
        }
        m_state.value[i] = value;
        ++m_uses[i];
        // Distinct VALUES per state, not just which states were touched. A WebGPU
        // pipeline is built per state COMBINATION, so the value spread is what
        // sizes the pipeline/shader permutation work.
        m_values[i].insert(value);
    }

    void Push() { m_stack.push_back(m_state); }

    void Pop()
    {
        if (m_stack.empty())
        {
            std::fprintf(stderr, "[%s] PopRenderStates with an empty stack\n", m_tag);
            ++m_stackErrors;
            return;
        }
        m_state = m_stack.back();
        m_stack.pop_back();
    }

    int Get(gos_RenderState state) const
    {
        const int i = (int)state;
        return (i > 0 && i < kRenderStateCount) ? m_state.value[i] : 0;
    }

    unsigned long SetCalls()   const { return m_setCalls; }
    unsigned long StackErrors() const { return m_stackErrors; }
    unsigned long OutOfRange()  const { return m_outOfRange; }

    unsigned DistinctStates() const
    {
        unsigned n = 0;
        for (int i = 0; i < kRenderStateCount; ++i)
            if (m_uses[i]) ++n;
        return n;
    }

    // The line that actually sizes the renderer work.
    void DumpCensus(const char* tag) const
    {
        std::printf("[%s] render state space: %u distinct states, %lu calls",
                    tag, DistinctStates(), m_setCalls);
        if (m_outOfRange) std::printf(", %lu OUT OF RANGE", m_outOfRange);
        std::printf("\n");

        std::printf("[%s] state:distinctValues xUses:", tag);
        bool any = false;
        for (int i = 0; i < kRenderStateCount; ++i)
        {
            if (!m_uses[i]) continue;
            std::map<int, std::set<int> >::const_iterator it = m_values.find(i);
            const unsigned distinct = (it == m_values.end()) ? 0u : (unsigned)it->second.size();
            std::printf(" %s(%d):%u x%lu", RenderStateName(i), i, distinct, m_uses[i]);
            any = true;
        }
        if (!any) std::printf(" none");
        std::printf("\n");

        std::printf("[%s] alpha modes seen:", tag);
        std::map<int, std::set<int> >::const_iterator alpha =
            m_values.find(gos_State_AlphaMode);
        if (alpha == m_values.end() || alpha->second.empty())
        {
            std::printf(" none");
        }
        else
        {
            for (std::set<int>::const_iterator it = alpha->second.begin();
                 it != alpha->second.end(); ++it)
                std::printf(" %d(%s)", *it, AlphaModeName(*it));
        }
        std::printf("\n");
    }

private:
    struct Block { int value[kRenderStateCount]; };

    const char*                   m_tag;
    Block                         m_state;
    std::vector<Block>            m_stack;
    unsigned long                 m_setCalls;
    unsigned long                 m_stackErrors;
    unsigned long                 m_outOfRange;
    unsigned long                 m_uses[kRenderStateCount];
    std::map<int, std::set<int> > m_values;
};

} // namespace mc2gfx
