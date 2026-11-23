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

#ifndef HEADER_COLLISION_FILE_HPP
#define HEADER_COLLISION_FILE_HPP

#include "btBulletDynamicsCommon.h"

/** \file collision_file.hpp
 *  Track collision served from a file instead of RAM (memory-constrained
 *  targets, i.e. the N64 port, where the file lives in cartridge ROM).
 *
 *  A host build writes, for each track, exactly the collision data it built
 *  in RAM: Bullet's quantized BVH nodes, the indexed triangles, the packed
 *  normals and a per-triangle material index, plus a hash of the triangle
 *  stream it was built from. At run time the track is converted as usual,
 *  but only hashed; Bullet then walks the tree and reads triangles through
 *  a small page cache. The hash must match, so the game sees bit-for-bit
 *  the collision it would have built itself.
 */

#if defined(STK_LOW_MEMORY) && defined(BULLET_SMALL_MEMORY)
#define STK_ROM_COLLISION

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/** Running hash (64-bit FNV-1a over 32-bit words, so it does not depend on
 *  byte order) of the triangle stream a TriangleMesh is built from. */
class CollisionHash
{
    uint64_t m_hash;
public:
    CollisionHash() { reset(); }
    void reset() { m_hash = 0xcbf29ce484222325ull; }
    void add(uint32_t word)
    {
        m_hash ^= word;
        m_hash *= 0x100000001b3ull;
    }
    void add(float f)
    {
        uint32_t word;
        memcpy(&word, &f, sizeof(word));
        add(word);
    }
    uint64_t get() const { return m_hash; }
};   // CollisionHash

// ----------------------------------------------------------------------------
/** On-disk layout, native byte order (files are written by a host build of
 *  the same byte order as the target).
 *
 *  Triangles are numbered, and stored, in the order of the tree's leaves, so
 *  a query reads neighbouring records instead of hopping around the file.
 *  Subtrees of up to BUCKET leaves are collapsed into one leaf ("bucket",
 *  see btExternalBvhNodes); the triangles' own leaves are rebuilt on demand
 *  exactly as the tree build made them. Vertices and normals are shared
 *  through tables in first-use order. */
struct CollisionFileHeader
{
    char     m_magic[8];
    uint32_t m_byte_order;      // 0x01020304 as written
    uint32_t m_hash_lo, m_hash_hi;
    uint32_t m_num_triangles;
    uint32_t m_num_nodes;
    uint32_t m_num_materials;
    uint32_t m_num_vertices;
    uint32_t m_num_normals;
    uint32_t m_vertex_index_size;   // 2 or 4
    uint32_t m_record_size;
    float    m_aabb_min[3], m_aabb_max[3];   // btTriangleMeshShape local AABB
    float    m_bvh_min[3], m_bvh_max[3], m_bvh_quantization[3];
    uint32_t m_off_nodes;       // m_num_nodes x btQuantizedBvhNode
    uint32_t m_off_records;     // m_num_triangles x m_record_size: 3 vertex
                                // indices, 3 uint16 normal indices, uint8
                                // material (index in first-use order)
    uint32_t m_off_vertices;    // 3 floats each
    uint32_t m_off_normals;     // packed normals (see TriangleMesh)
    uint32_t m_file_size;
};   // CollisionFileHeader

/** One triangle as the writer receives it. */
struct CollisionTriangle
{
    float    m_vertices[9];
    uint32_t m_normals[3];
    uint16_t m_material;
};   // CollisionTriangle

// ----------------------------------------------------------------------------
/** Read-only, set-associative page cache over a file. */
class FilePageCache
{
public:
    static const unsigned WAYS      = 4;
private:
    FILE                 *m_file;
    unsigned              m_page_size;   // power of two
    uint32_t              m_file_size;
    unsigned              m_sets;
    std::vector<uint8_t>  m_pages;
    std::vector<uint32_t> m_tags;      // page number + 1, 0 = empty
    std::vector<uint32_t> m_stamps;
    uint32_t              m_clock;
    mutable unsigned      m_hits, m_misses;
    /** Most recently used page: most reads land there. */
    uint32_t              m_last_page;
    const uint8_t        *m_last_data;
    const uint8_t* page(uint32_t page_number);
    void readSlow(uint32_t offset, void *dst, uint32_t size);
public:
    FilePageCache() : m_file(NULL), m_page_size(1024), m_file_size(0),
                      m_sets(0), m_clock(0), m_hits(0), m_misses(0),
                      m_last_page(~0u), m_last_data(NULL) {}
    ~FilePageCache();
    bool open(const std::string &path, unsigned cache_bytes,
              unsigned page_size);
    unsigned getPageSize() const { return m_page_size; }
    unsigned getCacheBytes() const { return (unsigned)m_pages.size(); }
    void read(uint32_t offset, void *dst, uint32_t size)
    {
        const uint32_t in_page = offset & (m_page_size - 1);
        if (offset / m_page_size == m_last_page &&
            in_page + size <= m_page_size)
        {
            m_hits++;
            memcpy(dst, m_last_data + in_page, size);
            return;
        }
        readSlow(offset, dst, size);
    }
    uint32_t getFileSize() const { return m_file_size; }
    unsigned getHits() const { return m_hits; }
    unsigned getMisses() const { return m_misses; }
};   // FilePageCache

// ----------------------------------------------------------------------------
/** A track collision file: Bullet mesh interface (triangles on demand),
 *  external BVH node source, and STK's per-triangle data. */
class CollisionFile : public btStridingMeshInterface,
                      public btExternalBvhNodes
{
    mutable FilePageCache m_cache;
    CollisionFileHeader   m_header;
    /** Recently decoded triangles (direct mapped): a triangle is usually
     *  decoded for its leaf box, then again for the narrow phase, and
     *  neighbouring queries (four wheels) share most of their triangles. */
    static const unsigned DECODED = 256;
    struct Decoded
    {
        int   m_triangle;
        float m_vertices[9];
    };
    mutable Decoded       m_decoded[DECODED];
    btOptimizedBvh       *m_bvh;
    std::string           m_path;

    CollisionFile() : m_bvh(NULL)
    {
        for (unsigned i = 0; i < DECODED; i++)
            m_decoded[i].m_triangle = -1;
    }
public:
    static const char *MAGIC;
    /** Opens a collision file, NULL if it is missing or malformed. */
    static CollisionFile* open(const std::string &path,
                               unsigned cache_bytes = 128 * 1024,
                               unsigned page_size = 128);
    /** Writes a collision file (host builds). get_triangle(i, record)
     *  fills the record of the mesh's triangle i. On return order[k] is the
     *  mesh triangle stored as triangle k. */
    template<typename F>
    static bool write(const std::string &path, uint64_t hash,
                      const btBvhTriangleMeshShape &shape,
                      unsigned num_triangles, unsigned num_materials,
                      F get_triangle, std::vector<uint32_t> *order);
    virtual ~CollisionFile();

    const CollisionFileHeader& getHeader() const { return m_header; }
    const std::string& getPath() const { return m_path; }
    uint64_t getHash() const
    {
        return (uint64_t(m_header.m_hash_hi) << 32) | m_header.m_hash_lo;
    }
    /** The BVH, its nodes read on demand. Owned by this object. */
    btOptimizedBvh* getBvh();
    void readRecord(unsigned triangle, uint32_t *vertices, uint16_t *normals,
                    unsigned *material) const;
    void readTriangle(unsigned triangle, btVector3 *p1, btVector3 *p2,
                      btVector3 *p3) const;
    void readNormals(unsigned triangle, uint32_t *normals) const;
    unsigned readMaterial(unsigned triangle) const;
    const FilePageCache& getCache() const { return m_cache; }

    // btExternalBvhNodes
    virtual void fetchNode(int index, btQuantizedBvhNode &node) const;
    virtual void fetchLeaf(int triangle, btQuantizedBvhNode &node) const;
    static const unsigned BUCKET = 8;

    // btStridingMeshInterface: triangles are only available one at a time
    virtual bool getTriangleVertices(int subpart, int triangle,
                                     btVector3 *vertices) const;
    virtual void getLockedVertexIndexBase(unsigned char **vertexbase,
                       int &numverts, PHY_ScalarType &type, int &stride,
                       unsigned char **indexbase, int &indexstride,
                       int &numfaces, PHY_ScalarType &indicestype,
                       int subpart = 0);
    virtual void getLockedReadOnlyVertexIndexBase(
                       const unsigned char **vertexbase, int &numverts,
                       PHY_ScalarType &type, int &stride,
                       const unsigned char **indexbase, int &indexstride,
                       int &numfaces, PHY_ScalarType &indicestype,
                       int subpart = 0) const;
    virtual void unLockVertexBase(int subpart) {}
    virtual void unLockReadOnlyVertexBase(int subpart) const {}
    virtual int  getNumSubParts() const { return 1; }
    virtual void preallocateVertices(int numverts) {}
    virtual void preallocateIndices(int numindices) {}
    virtual bool hasPremadeAabb() const { return true; }
    virtual void setPremadeAabb(const btVector3 &aabbMin,
                                const btVector3 &aabbMax) const {}
    virtual void getPremadeAabb(btVector3 *aabbMin, btVector3 *aabbMax) const;
};   // CollisionFile

// ----------------------------------------------------------------------------
bool writeCollisionFileData(const std::string &path, CollisionFileHeader h,
                            const btQuantizedBvhNode *tree, int tree_nodes,
                            const std::vector<CollisionTriangle> &triangles);

template<typename F>
bool CollisionFile::write(const std::string &path, uint64_t hash,
                          const btBvhTriangleMeshShape &shape,
                          unsigned num_triangles, unsigned num_materials,
                          F get_triangle, std::vector<uint32_t> *order)
{
    btBvhTriangleMeshShape &s = const_cast<btBvhTriangleMeshShape&>(shape);
    btOptimizedBvh *bvh = s.getOptimizedBvh();
    if (!bvh || !bvh->isQuantized())
        return false;
    // Renumber triangles in leaf order
    const QuantizedNodeArray &tree = bvh->getQuantizedNodeArray();
    const int num_nodes = bvh->getNumNodes();
    std::vector<uint32_t> renumber(num_triangles, ~0u);
    order->clear();
    for (int i = 0; i < num_nodes; i++)
    {
        if (!tree[i].isLeafNode())
            continue;
        const int original = tree[i].getTriangleIndex();
        if (tree[i].getPartId() != 0 || original >= (int)num_triangles ||
            renumber[original] != ~0u)
            return false;
        renumber[original] = (uint32_t)order->size();
        order->push_back((uint32_t)original);
    }
    if (order->size() != num_triangles)
        return false;
    std::vector<CollisionTriangle> triangles(num_triangles);
    for (unsigned k = 0; k < num_triangles; k++)
        get_triangle((*order)[k], &triangles[k]);

    CollisionFileHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.m_magic, MAGIC, 8);
    h.m_byte_order    = 0x01020304;
    h.m_hash_lo       = (uint32_t)hash;
    h.m_hash_hi       = (uint32_t)(hash >> 32);
    h.m_num_triangles = num_triangles;
    h.m_num_materials = num_materials;
    for (int i = 0; i < 3; i++)
    {
        h.m_aabb_min[i]         = shape.getLocalAabbMin()[i];
        h.m_aabb_max[i]         = shape.getLocalAabbMax()[i];
        h.m_bvh_min[i]          = bvh->getBvhAabbMin()[i];
        h.m_bvh_max[i]          = bvh->getBvhAabbMax()[i];
        h.m_bvh_quantization[i] = bvh->getBvhQuantization()[i];
    }
    return writeCollisionFileData(path, h, &tree[0], num_nodes, triangles);
}   // write

#endif   // STK_LOW_MEMORY && BULLET_SMALL_MEMORY
#endif
