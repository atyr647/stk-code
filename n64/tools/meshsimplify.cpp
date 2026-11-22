// meshsimplify: thin CLI over meshoptimizer's attribute-aware simplifier, used
// by n64/tools/stk_n64_assets.py to decimate STK .spm meshes for the N64.
//
// meshopt_simplify* never creates vertices -- it only drops triangles and
// re-indexes onto the vertices that survive -- so every surviving vertex keeps
// its exact UVs, colours and skinning weights. That is what makes it safe for
// skinned karts.
//
// stdin  (little-endian):
//   u32 vertex_count, u32 index_count, u32 attr_count, u32 target_index_count,
//   f32 target_error, u32 options
//   (options bit 31 = use meshopt_simplifySloppy: vertex clustering, which can
//    merge disconnected pieces the topology-preserving simplifier must keep;
//    it still only selects existing vertices, so attributes stay exact)
//   f32 positions[vertex_count*3]
//   f32 attrs[vertex_count*attr_count]
//   f32 attr_weights[attr_count]
//   u8  lock[vertex_count]
//   u32 indices[index_count]
// stdout:
//   u32 new_index_count, f32 result_error, u32 indices[new_index_count]
#include "meshoptimizer.h"
#include <cstdint>
#include <cstdio>
#include <vector>

template <typename T> static bool rd(T* p, size_t n)
{
    return fread(p, sizeof(T), n, stdin) == n;
}

int main()
{
    uint32_t vc, ic, ac, target, options;
    float err;
    if (!rd(&vc, 1) || !rd(&ic, 1) || !rd(&ac, 1) || !rd(&target, 1) ||
        !rd(&err, 1) || !rd(&options, 1))
        return 1;
    std::vector<float> pos(vc * 3), attrs(vc * ac), weights(ac);
    std::vector<unsigned char> lock(vc);
    std::vector<unsigned> idx(ic), out(ic);
    if (!rd(pos.data(), pos.size()) || !rd(attrs.data(), attrs.size()) ||
        !rd(weights.data(), weights.size()) || !rd(lock.data(), lock.size()) ||
        !rd(idx.data(), idx.size()))
        return 2;
    float result_error = 0.0f;
    size_t n;
    const bool sloppy = (options & 0x80000000u) != 0;
    options &= ~0x80000000u;
    if (sloppy)
        n = meshopt_simplifySloppy(out.data(), idx.data(), ic, pos.data(), vc,
            12, target, err, &result_error);
    else if (ac > 0)
        n = meshopt_simplifyWithAttributes(out.data(), idx.data(), ic,
            pos.data(), vc, 12, attrs.data(), ac * 4, weights.data(), ac,
            lock.data(), target, err, options, &result_error);
    else
        n = meshopt_simplify(out.data(), idx.data(), ic, pos.data(), vc, 12,
            target, err, options, &result_error);
    uint32_t n32 = (uint32_t)n;
    fwrite(&n32, 4, 1, stdout);
    fwrite(&result_error, 4, 1, stdout);
    fwrite(out.data(), 4, n, stdout);
    return 0;
}
