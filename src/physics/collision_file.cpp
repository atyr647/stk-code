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
#include <cstddef>
#include <cstdlib>

const char *CollisionFile::MAGIC = "STKCOL\0\2";
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
void FilePageCache::read(uint32_t offset, void *dst, uint32_t size)
{
    uint8_t *out = (uint8_t*)dst;
    while (size > 0)
    {
        const uint32_t in_page = offset % m_page_size;
        const uint32_t n = std::min(size, m_page_size - in_page);
        memcpy(out, page(offset / m_page_size) + in_page, n);
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
    bool ok = memcmp(h.m_magic, MAGIC, 8) == 0 &&
              h.m_byte_order == BYTE_ORDER_MARK &&
              h.m_file_size == cf->m_cache.getFileSize() &&
              h.m_off_nodes + uint64_t(h.m_num_nodes) *
                              sizeof(btQuantizedBvhNode) <= h.m_file_size &&
              h.m_off_triangles + uint64_t(h.m_num_triangles) *
                              sizeof(CollisionTriangle) <= h.m_file_size;
    if (!ok)
    {
        Log::warn("CollisionFile", "Ignoring malformed '%s'.", path.c_str());
        delete cf;
        return NULL;
    }
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
void CollisionFile::readTriangle(unsigned triangle, btVector3 *p1,
                                 btVector3 *p2, btVector3 *p3) const
{
    float v[9];
    m_cache.read(m_header.m_off_triangles +
                 triangle * sizeof(CollisionTriangle), v, sizeof(v));
    *p1 = btVector3(v[0], v[1], v[2]);
    *p2 = btVector3(v[3], v[4], v[5]);
    *p3 = btVector3(v[6], v[7], v[8]);
}   // readTriangle

// ----------------------------------------------------------------------------
void CollisionFile::readNormals(unsigned triangle, uint32_t *normals) const
{
    m_cache.read(m_header.m_off_triangles +
                 triangle * sizeof(CollisionTriangle) +
                 offsetof(CollisionTriangle, m_normals), normals, 12);
}   // readNormals

// ----------------------------------------------------------------------------
unsigned CollisionFile::readMaterial(unsigned triangle) const
{
    uint16_t m;
    m_cache.read(m_header.m_off_triangles +
                 triangle * sizeof(CollisionTriangle) +
                 offsetof(CollisionTriangle, m_material), &m, 2);
    return m;
}   // readMaterial

// ----------------------------------------------------------------------------
void CollisionFile::fetchNode(int index, btQuantizedBvhNode &node) const
{
    m_cache.read(m_header.m_off_nodes + index * sizeof(btQuantizedBvhNode),
                 &node, sizeof(btQuantizedBvhNode));
}   // fetchNode

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
bool writeCollisionFileData(const std::string &path,
                            const CollisionFileHeader &header,
                            const std::vector<btQuantizedBvhNode> &nodes,
                            const std::vector<CollisionTriangle> &triangles)
{
    CollisionFileHeader h = header;
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
    h.m_off_triangles = (uint32_t)out.size();
    append(triangles.data(), triangles.size() * sizeof(CollisionTriangle));
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
