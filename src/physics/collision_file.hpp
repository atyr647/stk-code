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

/** The triangle stream hashed three ways, so that a mismatch says what
 *  differs. Positions and normals go through floating point (transforms,
 *  i.e. sin/cos from the platform's math library, and normalisation), and
 *  can differ in a last bit between the build that wrote a file and the
 *  one reading it; the material sequence is integer-exact. */
struct CollisionHashes
{
    CollisionHash m_positions, m_normals, m_materials;
    void reset()
    {
        m_positions.reset();
        m_normals.reset();
        m_materials.reset();
    }
};   // CollisionHashes

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
    uint32_t m_hash_positions[2];   // low, high word
    uint32_t m_hash_normals[2];
    uint32_t m_hash_materials[2];
    uint32_t m_num_triangles;
    uint32_t m_num_nodes;
    uint32_t m_num_materials;
    uint32_t m_num_vertices;
    uint32_t m_num_normals;
    uint32_t m_vertex_index_size;   // 2 or 4
    uint32_t m_record_size;
    uint32_t m_world_objects;       // most collision objects the physics
                                    // world held while the track loaded
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
/** Read-only, set-associative page cache over a file.
 *
 *  Reads go through cursors: a cursor remembers the cache slot of the page
 *  it last used, so a stream of reads (tree nodes, triangle records,
 *  vertices) that stays in a page costs a compare, even when other streams
 *  are read in between. A cursor's slot is checked against the slot's tag
 *  before use: a page evicted by another stream is never read stale. */
class FilePageCache
{
public:
    static const unsigned WAYS      = 4;
    struct Cursor
    {
        uint32_t m_page;   // page number + 1 the slot held when last used
        unsigned m_slot;
        Cursor() : m_page(0), m_slot(0) {}
    };
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
    Cursor                m_cursor;    // for read() and peek()
    /** The slot holding page_number, reading it in on a miss. */
    unsigned slot(uint32_t page_number)
    {
        const unsigned first = (page_number & (m_sets - 1)) * WAYS;
        for (unsigned w = first; w < first + WAYS; w++)
        {
            if (m_tags[w] == page_number + 1)
            {
                m_hits++;
                m_stamps[w] = ++m_clock;
                return w;
            }
        }
        return load(page_number);
    }
    /** Reads page_number into the least recently used slot of its set. */
    unsigned load(uint32_t page_number);
    void readSlow(Cursor &c, uint32_t offset, void *dst, uint32_t size);
public:
    FilePageCache() : m_file(NULL), m_page_size(1024), m_file_size(0),
                      m_sets(0), m_clock(0), m_hits(0), m_misses(0) {}
    ~FilePageCache();
    bool open(const std::string &path, unsigned cache_bytes,
              unsigned page_size);
    unsigned getPageSize() const { return m_page_size; }
    unsigned getCacheBytes() const { return (unsigned)m_pages.size(); }
    /** The bytes at offset, in place: valid until the next read through any
     *  cursor. The caller guarantees they do not cross a page. */
    const uint8_t* at(Cursor &c, uint32_t offset)
    {
        // a cursor hit does not refresh the slot's LRU stamp: a page in use
        // is stamped whenever a stream enters it through slot()
        const uint32_t tag = offset / m_page_size + 1;
        if (tag == c.m_page && m_tags[c.m_slot] == tag)
            m_hits++;
        else
        {
            c.m_slot = slot(tag - 1);
            c.m_page = tag;
        }
        return &m_pages[c.m_slot * m_page_size +
                        (offset & (m_page_size - 1))];
    }
    const uint8_t* at(uint32_t offset) { return at(m_cursor, offset); }
    void read(Cursor &c, uint32_t offset, void *dst, uint32_t size)
    {
        if ((offset & (m_page_size - 1)) + size <= m_page_size)
            memcpy(dst, at(c, offset), size);
        else
            readSlow(c, offset, dst, size);
    }
    void read(uint32_t offset, void *dst, uint32_t size)
    {
        read(m_cursor, offset, dst, size);
    }
    /** read() for a size known at compile time: the copy is inlined. */
    template<unsigned N>
    void readFixed(Cursor &c, uint32_t offset, void *dst)
    {
        if ((offset & (m_page_size - 1)) + N <= m_page_size)
            memcpy(dst, at(c, offset), N);
        else
            readSlow(c, offset, dst, N);
    }
    /** The bytes at offset in place when they lie in one page (valid until
     *  the next read), else copied to bounce. */
    const uint8_t* peek(Cursor &c, uint32_t offset, uint32_t size,
                        uint8_t *bounce)
    {
        if ((offset & (m_page_size - 1)) + size <= m_page_size)
            return at(c, offset);
        readSlow(c, offset, bounce, size);
        return bounce;
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
    /** One cursor per stream of reads (see FilePageCache). */
    mutable FilePageCache::Cursor m_node_cursor, m_record_cursor,
                                  m_vertex_cursor, m_normal_cursor;
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
    /** Recently expanded buckets (direct mapped): karts query the same
     *  neighbourhood frame after frame, and expanding a bucket means
     *  decoding and quantizing all its triangles. */
    struct Bucket
    {
        int                m_first;
        btQuantizedBvhNode m_leaves[btExternalBvhNodes::BUCKET_MAX];
    };
    mutable std::vector<Bucket> m_buckets;
    mutable unsigned      m_bucket_hits, m_bucket_misses;
    btOptimizedBvh       *m_bvh;
    std::string           m_path;

    CollisionFile() : m_bucket_hits(0), m_bucket_misses(0), m_bvh(NULL)
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
    static bool write(const std::string &path, const CollisionHashes &hash,
                      const btBvhTriangleMeshShape &shape,
                      unsigned num_triangles, unsigned num_materials,
                      unsigned world_objects,
                      F get_triangle, std::vector<uint32_t> *order);
    /** Sets m_world_objects in an existing file (host builds). */
    static bool setWorldObjects(const std::string &path, uint32_t n);
    virtual ~CollisionFile();

    const CollisionFileHeader& getHeader() const { return m_header; }
    const std::string& getPath() const { return m_path; }
    static uint64_t join(const uint32_t *w)
    {
        return (uint64_t(w[1]) << 32) | w[0];
    }
    bool samePositions(const CollisionHashes &h) const
    {
        return join(m_header.m_hash_positions) == h.m_positions.get();
    }
    bool sameNormals(const CollisionHashes &h) const
    {
        return join(m_header.m_hash_normals) == h.m_normals.get();
    }
    bool sameMaterials(const CollisionHashes &h) const
    {
        return join(m_header.m_hash_materials) == h.m_materials.get();
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
    virtual const btQuantizedBvhNode* nodeAt(int index) const;
    virtual const btQuantizedBvhNode* nodeSpan(int index, int *first,
                                               int *count) const;
    virtual void fetchLeaf(int triangle, btQuantizedBvhNode &node) const;
    virtual void fetchBucket(int first, int count,
                             btQuantizedBvhNode *leaves) const;
    unsigned getBucketHits() const { return m_bucket_hits; }
    unsigned getBucketMisses() const { return m_bucket_misses; }
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
bool CollisionFile::write(const std::string &path,
                          const CollisionHashes &hash,
                          const btBvhTriangleMeshShape &shape,
                          unsigned num_triangles, unsigned num_materials,
                          unsigned world_objects,
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
    const uint64_t hp = hash.m_positions.get(), hn = hash.m_normals.get(),
                   hm = hash.m_materials.get();
    h.m_hash_positions[0] = (uint32_t)hp;
    h.m_hash_positions[1] = (uint32_t)(hp >> 32);
    h.m_hash_normals[0]   = (uint32_t)hn;
    h.m_hash_normals[1]   = (uint32_t)(hn >> 32);
    h.m_hash_materials[0] = (uint32_t)hm;
    h.m_hash_materials[1] = (uint32_t)(hm >> 32);
    h.m_num_triangles = num_triangles;
    h.m_num_materials = num_materials;
    h.m_world_objects = world_objects;
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
