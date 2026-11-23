// Equivalence test: CompactTriangleMesh vs btTriangleMesh under a quantized BVH.
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"
#include "physics/triangle_mesh.hpp"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

struct Hit { int part, tri; float frac; btVector3 n; };
struct RayCb : btTriangleRaycastCallback {
    std::vector<Hit> hits;
    RayCb(const btVector3 &f, const btVector3 &t) : btTriangleRaycastCallback(f, t) {}
    btScalar reportHit(const btVector3 &n, btScalar frac, int part, int tri) override
    { hits.push_back({part, tri, (float)frac, n}); return frac; }
};
struct AllCb : btTriangleCallback {
    std::vector<float> out;
    void processTriangle(btVector3 *t, int part, int tri) override {
        out.push_back((float)part); out.push_back((float)tri);
        for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) out.push_back((float)t[i][k]);
    }
};

static int run(int grid, unsigned seed, bool refinalize = false)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-1, 1);
    // Terrain grid (shared vertices) + a few duplicated "instances" + degenerate tris
    std::vector<float> h((grid + 1) * (grid + 1));
    for (auto &x : h) x = u(rng) * 3;
    auto V = [&](int i, int j, float ox) { return btVector3(i * 2.0f + ox, h[i * (grid + 1) + j], j * 2.0f); };
    std::vector<btVector3> tris;
    for (int inst = 0; inst < 2; inst++)
        for (int i = 0; i < grid; i++) for (int j = 0; j < grid; j++) {
            float ox = inst * 0.5f;
            tris.push_back(V(i, j, ox)); tris.push_back(V(i + 1, j, ox)); tris.push_back(V(i, j + 1, ox));
            tris.push_back(V(i + 1, j, ox)); tris.push_back(V(i + 1, j + 1, ox)); tris.push_back(V(i, j + 1, ox));
        }
    tris.push_back(btVector3(0, -0.0f, 0)); tris.push_back(btVector3(0, 0.0f, 0)); tris.push_back(btVector3(1, 0, 0));
    const int nt = (int)tris.size() / 3;

    btTriangleMesh ref(true, false);
    CompactTriangleMesh cmp;
    for (int t = 0; t < nt; t++) {
        ref.addTriangle(tris[3 * t], tris[3 * t + 1], tris[3 * t + 2]);
        cmp.addTriangle(tris[3 * t], tris[3 * t + 1], tris[3 * t + 2]);
        if (refinalize && t == nt / 2) cmp.finalize();
    }
    cmp.finalize();
    int bad = 0;
    if (cmp.getNumTriangles() != nt) { printf("count mismatch\n"); return 1; }
    for (int t = 0; t < nt; t++) {
        btVector3 a, b, c; cmp.getTriangle(t, &a, &b, &c);
        if (memcmp(&a, &tris[3*t], 12) || memcmp(&b, &tris[3*t+1], 12) || memcmp(&c, &tris[3*t+2], 12)) bad++;
    }
    btBvhTriangleMeshShape sref(&ref, true), scmp(&cmp, true);
    // Serialized trees must be byte-identical
    btOptimizedBvh *br = sref.getOptimizedBvh(), *bc = scmp.getOptimizedBvh();
    unsigned sz = br->calculateSerializeBufferSize();
    std::vector<char> m1(sz + 16), m2(sz + 16);
    char *p1 = (char*)(((size_t)m1.data() + 15) & ~15), *p2 = (char*)(((size_t)m2.data() + 15) & ~15);
    br->serialize(p1, sz, false); bc->serialize(p2, sz, false);
    bool tree_same = sz == bc->calculateSerializeBufferSize() && !memcmp(p1, p2, sz);
    // Rays
    int rays = 0, hits = 0;
    for (int r = 0; r < 20000; r++) {
        btVector3 f(u(rng) * grid * 1.2f + grid, 10 + u(rng) * 5, u(rng) * grid * 1.2f + grid);
        btVector3 t = f + btVector3(u(rng) * 8, -25, u(rng) * 8);
        if (r % 4 == 0) t = f + btVector3(u(rng) * grid, u(rng) * 3, u(rng) * grid); // grazing
        RayCb c1(f, t), c2(f, t);
        sref.performRaycast(&c1, f, t); scmp.performRaycast(&c2, f, t);
        rays++; hits += !c1.hits.empty();
        if (c1.hits.size() != c2.hits.size()) { bad++; continue; }
        for (size_t k = 0; k < c1.hits.size(); k++)
            if (c1.hits[k].tri != c2.hits[k].tri || c1.hits[k].part != c2.hits[k].part ||
                memcmp(&c1.hits[k].frac, &c2.hits[k].frac, 4) || memcmp(&c1.hits[k].n, &c2.hits[k].n, 12)) bad++;
    }
    // AABB queries (what contact generation uses)
    for (int q = 0; q < 2000; q++) {
        btVector3 c(u(rng) * grid + grid, u(rng) * 3, u(rng) * grid + grid), e(1 + u(rng), 1 + u(rng), 1 + u(rng));
        AllCb a1, a2; sref.processAllTriangles(&a1, c - e, c + e); scmp.processAllTriangles(&a2, c - e, c + e);
        if (a1.out.size() != a2.out.size() || memcmp(a1.out.data(), a2.out.data(), a1.out.size() * 4)) bad++;
    }
    int verts = (int)cmp.getIndexedMeshArray()[0].m_numVertices;
    printf("grid %3d: %6d tris, %6d unique verts (%s indices), tree %s, %d rays (%d hit), mismatches %d\n",
           grid, nt, verts, cmp.getIndexedMeshArray()[0].m_indexType == PHY_SHORT ? "16-bit" : "32-bit",
           tree_same ? "identical" : "DIFFERENT", rays, hits, bad);
    return bad || !tree_same;
}
static int chunked() {
    ChunkedArray<uint32_t> a; std::vector<uint32_t> r; int bad = 0;
    for (uint32_t i = 0; i < 5000; i++) { a.push_back(i * 7); r.push_back(i * 7); if (i == 2047) a.compact(); }
    for (size_t i = 0; i < r.size(); i++) bad += a[i] != r[i];
    a.compact(); for (size_t i = 0; i < r.size(); i++) bad += a.data()[i] != r[i];
    printf("chunked array: %d mismatches\n", bad); return bad != 0 || a.size() != r.size();
}
int main() { int f = chunked(); f |= run(90, 4, true); f |= run(20, 1); f |= run(90, 2); f |= run(200, 3); puts(f ? "FAIL" : "PASS"); return f; }
