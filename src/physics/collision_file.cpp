//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2026 SuperTuxKart-Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "physics/collision_file.hpp"

#ifdef STK_ROM_COLLISION

#include "utils/log.hpp"

#include <algorithm>
#include <map>
#include <cstddef>
#include <cstdlib>

const char *CollisionFile::MAGIC = "STKCOL\0\3";
static const uint32_t BYTE_ORDER_MARK = 0x01020304;

// ============================================================================
FilePageCache::~FilePageCache()
{
    if (m_file)
        fclose(m_file);
}   // ~FilePageCache

// ----------------------------------------------------------------------------
bool FilePageCache::open(const std::string &path, unsigned cache_bytes,
                         unsigned page_size)
{
    m_page_size = page_size;
    m_file = fopen(path.c_str(), "rb");
    if (!m_file)
        return false;
    // The cache is the buffer: each miss is one read of exactly one page
    // (on the N64, one DMA from ROM)
    setvbuf(m_file, NULL, _IONBF, 0);
    if (fseek(m_file, 0, SEEK_END) != 0)
        return false;
    long size = ftell(m_file);
    if (size <= 0)
        return false;
    m_file_size = (uint32_t)size;
    m_sets = 1;
    while (2 * m_sets * WAYS * m_page_size <= cache_bytes)
        m_sets *= 2;
    m_pages.resize(m_sets * WAYS * m_page_size);
    m_tags.assign(m_sets * WAYS, 0);
    m_stamps.assign(m_sets * WAYS, 0);
    return true;
}   // open

// ----------------------------------------------------------------------------
const uint8_t* FilePageCache::page(uint32_t page_number)
{
    const unsigned first = (page_number & (m_sets - 1)) * WAYS;
    unsigned victim = first;
    for (unsigned w = first; w < first + WAYS; w++)
    {
        if (m_tags[w] == page_number + 1)
        {
            m_hits++;
            m_stamps[w] = ++m_clock;
            return &m_pages[w * m_page_size];
        }
        if (m_stamps[w] < m_stamps[victim])
            victim = w;
    }
    m_misses++;
    uint8_t *dst = &m_pages[victim * m_page_size];
    const uint32_t offset = page_number * m_page_size;
    const uint32_t n = std::min<uint32_t>(m_page_size, m_file_size - offset);
    if (fseek(m_file, offset, SEEK_SET) != 0 ||
        fread(dst, 1, n, m_file) != n)
    {
        Log::fatal("CollisionFile", "Read error at offset %u.", offset);
    }
    m_tags[victim] = page_number + 1;
    m_stamps[victim] = ++m_clock;
    return dst;
}   // page

// ----------------------------------------------------------------------------
void FilePageCache::readSlow(uint32_t offset, void *dst, uint32_t size)
{
    uint8_t *out = (uint8_t*)dst;
    while (size > 0)
    {
        const uint32_t in_page = offset % m_page_size;
        const uint32_t n = std::min(size, m_page_size - in_page);
        m_last_page = offset / m_page_size;
        m_last_data = page(m_last_page);
        memcpy(out, m_last_data + in_page, n);
        out += n;
        offset += n;
        size -= n;
    }
}   // read

// ============================================================================
CollisionFile* CollisionFile::open(const std::string &path,
                                   unsigned cache_bytes, unsigned page_size)
{
    CollisionFile *cf = new CollisionFile();
    cf->m_path = path;
    const CollisionFileHeader &h = cf->m_header;
    if (const char *kb = getenv("STK_COLLISION_CACHE_KB"))
        cache_bytes = atoi(kb) * 1024;
    if (const char *ps = getenv("STK_COLLISION_PAGE"))
        page_size = atoi(ps);
    if (!cf->m_cache.open(path, cache_bytes, page_size) ||
        cf->m_cache.getFileSize() < sizeof(CollisionFileHeader))
    {
        delete cf;
        return NULL;
    }
    cf->m_cache.read(0, &cf->m_header, sizeof(cf->m_header));
    const uint64_t tris = h.m_num_triangles;
    bool ok = memcmp(h.m_magic, MAGIC, 8) == 0 &&
              h.m_byte_order == BYTE_ORDER_MARK &&
              h.m_file_size == cf->m_cache.getFileSize() &&
              (h.m_vertex_index_size == 2 || h.m_vertex_index_size == 4) &&
              h.m_record_size >= 3 * h.m_vertex_index_size + 7 &&
              h.m_record_size <= 32 &&
              h.m_off_nodes + uint64_t(h.m_num_nodes) *
                              sizeof(btQuantizedBvhNode) <= h.m_file_size &&
              h.m_off_records + tris * h.m_record_size <= h.m_file_size &&
              h.m_off_vertices + uint64_t(h.m_num_vertices) * 12
                                                        <= h.m_file_size &&
              h.m_off_normals + uint64_t(h.m_num_normals) * 4
                                                        <= h.m_file_size;
    if (!ok)
    {
        if (h.m_byte_order == 0x04030201)
            Log::warn("CollisionFile", "Ignoring '%s': written for the other "
                      "byte order.", path.c_str());
        else
            Log::warn("CollisionFile", "Ignoring malformed '%s'.",
                      path.c_str());
        delete cf;
        return NULL;
    }
    cf->getBvh();
    return cf;
}   // open

// ----------------------------------------------------------------------------
CollisionFile::~CollisionFile()
{
    delete m_bvh;
}   // ~CollisionFile

// ----------------------------------------------------------------------------
btOptimizedBvh* CollisionFile::getBvh()
{
    if (!m_bvh)
    {
        const CollisionFileHeader &h = m_header;
        m_bvh = new btOptimizedBvh();
        m_bvh->setExternalNodes(this, (int)h.m_num_nodes,
            btVector3(h.m_bvh_min[0], h.m_bvh_min[1], h.m_bvh_min[2]),
            btVector3(h.m_bvh_max[0], h.m_bvh_max[1], h.m_bvh_max[2]),
            btVector3(h.m_bvh_quantization[0], h.m_bvh_quantization[1],
                      h.m_bvh_quantization[2]));
    }
    return m_bvh;
}   // getBvh

// ----------------------------------------------------------------------------
void CollisionFile::readRecord(unsigned triangle, uint32_t *vertices,
                               uint16_t *normals, unsigned *material) const
{
    uint8_t r[32];
    const unsigned size = m_header.m_record_size;
    m_cache.read(m_header.m_off_records + triangle * size, r, size);
    const unsigned vs = m_header.m_vertex_index_size;
    for (unsigned k = 0; k < 3; k++)
    {
        if (vs == 2)
        {
            uint16_t v;
            memcpy(&v, r + 2 * k, 2);
            vertices[k] = v;
        }
        else
            memcpy(&vertices[k], r + 4 * k, 4);
    }
    memcpy(normals, r + 3 * vs, 6);
    *material = r[3 * vs + 6];
}   // readRecord

// ----------------------------------------------------------------------------
void CollisionFile::readTriangle(unsigned triangle, btVector3 *p1,
                                 btVector3 *p2, btVector3 *p3) const
{
    Decoded &d = m_decoded[triangle & (DECODED - 1)];
    if (d.m_triangle != (int)triangle)
    {
        uint32_t v[3];
        uint16_t n[3];
        unsigned m;
        readRecord(triangle, v, n, &m);
        for (unsigned k = 0; k < 3; k++)
        {
            m_cache.read(m_header.m_off_vertices + 12 * v[k],
                         &d.m_vertices[3 * k], 12);
        }
        d.m_triangle = (int)triangle;
    }
    const float *f = d.m_vertices;
    *p1 = btVector3(f[0], f[1], f[2]);
    *p2 = btVector3(f[3], f[4], f[5]);
    *p3 = btVector3(f[6], f[7], f[8]);
}   // readTriangle

// ----------------------------------------------------------------------------
void CollisionFile::readNormals(unsigned triangle, uint32_t *normals) const
{
    uint32_t v[3];
    uint16_t n[3];
    unsigned m;
    readRecord(triangle, v, n, &m);
    for (unsigned k = 0; k < 3; k++)
        m_cache.read(m_header.m_off_normals + 4 * n[k], &normals[k], 4);
}   // readNormals

// ----------------------------------------------------------------------------
unsigned CollisionFile::readMaterial(unsigned triangle) const
{
    uint32_t v[3];
    uint16_t n[3];
    unsigned m;
    readRecord(triangle, v, n, &m);
    return m;
}   // readMaterial

// ----------------------------------------------------------------------------
void CollisionFile::fetchNode(int index, btQuantizedBvhNode &node) const
{
    m_cache.read(m_header.m_off_nodes + index * sizeof(btQuantizedBvhNode),
                 &node, sizeof(btQuantizedBvhNode));
}   // fetchNode

// ----------------------------------------------------------------------------
void CollisionFile::fetchLeaf(int triangle, btQuantizedBvhNode &node) const
{
    btVector3 t[3];
    readTriangle(triangle, t, t + 1, t + 2);
    m_bvh->quantizeTriangleLeaf(t, triangle, node);
}   // fetchLeaf

// ----------------------------------------------------------------------------
bool CollisionFile::getTriangleVertices(int subpart, int triangle,
                                        btVector3 *vertices) const
{
    readTriangle(triangle, vertices, vertices + 1, vertices + 2);
    return true;
}   // getTriangleVertices

// ----------------------------------------------------------------------------
void CollisionFile::getLockedVertexIndexBase(unsigned char **vertexbase,
                       int &numverts, PHY_ScalarType &type, int &stride,
                       unsigned char **indexbase, int &indexstride,
                       int &numfaces, PHY_ScalarType &indicestype,
                       int subpart)
{
    Log::fatal("CollisionFile", "Mesh data is not addressable.");
}   // getLockedVertexIndexBase

// ----------------------------------------------------------------------------
void CollisionFile::getLockedReadOnlyVertexIndexBase(
                       const unsigned char **vertexbase, int &numverts,
                       PHY_ScalarType &type, int &stride,
                       const unsigned char **indexbase, int &indexstride,
                       int &numfaces, PHY_ScalarType &indicestype,
                       int subpart) const
{
    Log::fatal("CollisionFile", "Mesh data is not addressable.");
}   // getLockedReadOnlyVertexIndexBase

// ----------------------------------------------------------------------------
void CollisionFile::getPremadeAabb(btVector3 *aabbMin,
                                   btVector3 *aabbMax) const
{
    const CollisionFileHeader &h = m_header;
    *aabbMin = btVector3(h.m_aabb_min[0], h.m_aabb_min[1], h.m_aabb_min[2]);
    *aabbMax = btVector3(h.m_aabb_max[0], h.m_aabb_max[1], h.m_aabb_max[2]);
}   // getPremadeAabb

// ----------------------------------------------------------------------------
/** Copies the subtree at tree[i] into out with every subtree of at most
 *  BUCKET leaves collapsed into one leaf. Leaves are numbered in order, so
 *  a subtree's triangles are consecutive. */
static void collapse(const btQuantizedBvhNode *tree, int i,
                     std::vector<btQuantizedBvhNode> *out)
{
    const btQuantizedBvhNode &n = tree[i];
    const int size = n.isLeafNode() ? 1 : n.getEscapeIndex();
    const int leaves = (size + 1) / 2;
    if (leaves <= (int)CollisionFile::BUCKET)
    {
        int first = i;
        while (!tree[first].isLeafNode())
            first++;
        btQuantizedBvhNode bucket = n;
        bucket.m_escapeIndexOrTriangleIndex =
            ((leaves - 1) << (31 - MAX_NUM_PARTS_IN_BITS)) |
            tree[first].getTriangleIndex();
        out->push_back(bucket);
        return;
    }
    const size_t at = out->size();
    out->push_back(n);
    const int left = i + 1;
    const int left_size = tree[left].isLeafNode()
                        ? 1 : tree[left].getEscapeIndex();
    collapse(tree, left, out);
    collapse(tree, left + left_size, out);
    (*out)[at].m_escapeIndexOrTriangleIndex = -(int)(out->size() - at);
}   // collapse

// ----------------------------------------------------------------------------
bool writeCollisionFileData(const std::string &path, CollisionFileHeader h,
                            const btQuantizedBvhNode *tree, int tree_nodes,
                            const std::vector<CollisionTriangle> &triangles)
{
    // Leaf triangle indices -> leaf order (positions in the leaf sequence)
    std::vector<btQuantizedBvhNode> renumbered(tree, tree + tree_nodes);
    int next = 0;
    for (auto &n : renumbered)
    {
        if (n.isLeafNode())
            n.m_escapeIndexOrTriangleIndex = next++;
    }
    std::vector<btQuantizedBvhNode> nodes;
    collapse(renumbered.data(), 0, &nodes);

    // Shared vertex and normal tables, first-use order
    std::vector<float> vertices;
    std::vector<uint32_t> normals;
    std::vector<uint32_t> tri_vertices, tri_normals;
    {
        struct Key
        {
            uint32_t v[3];
            bool operator<(const Key &o) const
            {
                return memcmp(v, o.v, sizeof(v)) < 0;
            }
        };
        std::map<Key, uint32_t> vertex_ids;
        std::map<uint32_t, uint32_t> normal_ids;
        for (const CollisionTriangle &t : triangles)
        {
            for (int k = 0; k < 3; k++)
            {
                Key key;
                memcpy(key.v, &t.m_vertices[3 * k], 12);
                auto v = vertex_ids.insert(std::make_pair(key,
                                     (uint32_t)vertex_ids.size()));
                if (v.second)
                    vertices.insert(vertices.end(), &t.m_vertices[3 * k],
                                    &t.m_vertices[3 * k] + 3);
                tri_vertices.push_back(v.first->second);
                auto n = normal_ids.insert(std::make_pair(t.m_normals[k],
                                     (uint32_t)normal_ids.size()));
                if (n.second)
                    normals.push_back(t.m_normals[k]);
                tri_normals.push_back(n.first->second);
            }
        }
    }
    if (normals.size() > 65536 || h.m_num_materials > 256)
        return false;
    h.m_num_nodes = (uint32_t)nodes.size();
    h.m_num_vertices = (uint32_t)(vertices.size() / 3);
    h.m_num_normals = (uint32_t)normals.size();
    h.m_vertex_index_size = h.m_num_vertices <= 65536 ? 2 : 4;
    h.m_record_size = (3 * h.m_vertex_index_size + 7 + 1) & ~1u;

    std::vector<uint8_t> out(sizeof(h));
    auto pad16 = [&out]() { while (out.size() % 16) out.push_back(0); };
    auto append = [&out](const void *p, size_t n)
    {
        out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    };
    pad16();
    h.m_off_nodes = (uint32_t)out.size();
    append(nodes.data(), nodes.size() * sizeof(btQuantizedBvhNode));
    pad16();
    h.m_off_records = (uint32_t)out.size();
    for (size_t t = 0; t < triangles.size(); t++)
    {
        uint8_t r[32] = { 0 };
        for (int k = 0; k < 3; k++)
        {
            const uint32_t v = tri_vertices[3 * t + k];
            if (h.m_vertex_index_size == 2)
            {
                const uint16_t v16 = (uint16_t)v;
                memcpy(r + 2 * k, &v16, 2);
            }
            else
                memcpy(r + 4 * k, &v, 4);
            const uint16_t n16 = (uint16_t)tri_normals[3 * t + k];
            memcpy(r + 3 * h.m_vertex_index_size + 2 * k, &n16, 2);
        }
        r[3 * h.m_vertex_index_size + 6] = (uint8_t)triangles[t].m_material;
        append(r, h.m_record_size);
    }
    pad16();
    h.m_off_vertices = (uint32_t)out.size();
    append(vertices.data(), vertices.size() * sizeof(float));
    pad16();
    h.m_off_normals = (uint32_t)out.size();
    append(normals.data(), normals.size() * sizeof(uint32_t));
    pad16();
    h.m_file_size = (uint32_t)out.size();
    memcpy(out.data(), &h, sizeof(h));

    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    return fclose(f) == 0 && ok;
}   // writeCollisionFileData

#endif   // STK_ROM_COLLISION
