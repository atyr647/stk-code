//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2006-2015 Joerg Henrichs
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

#ifndef HEADER_TRIANGLE_MESH_HPP
#define HEADER_TRIANGLE_MESH_HPP

#include <algorithm>
#include <vector>
#include "btBulletDynamicsCommon.h"

#include "physics/collision_file.hpp"
#include "physics/user_pointer.hpp"
#ifdef STK_LOW_MEMORY
#include "mini_glm.hpp"
#endif

class Material;

#ifdef STK_LOW_MEMORY
/** Append-only array for building meshes on memory-constrained targets.
 *  While it grows it lives in fixed-size chunks, so there is no doubling
 *  slack and no copy of the whole array on growth; compact() then moves it
 *  into one exactly-sized block. Indexing works in both states.
 *  \ingroup physics */
template<typename T> class ChunkedArray
{
    static const unsigned SHIFT = 10;
    static const size_t   CHUNK = size_t(1) << SHIFT;
    std::vector<T*> m_chunks;
    T*              m_flat;
    size_t          m_size;
    ChunkedArray(const ChunkedArray&) = delete;
    ChunkedArray& operator=(const ChunkedArray&) = delete;
    void freeChunks()
    {
        for (T* c : m_chunks)
            delete [] c;
        std::vector<T*>().swap(m_chunks);
    }
public:
    ChunkedArray() : m_flat(NULL), m_size(0) {}
    ~ChunkedArray() { clear(); }
    void clear()
    {
        freeChunks();
        delete [] m_flat;
        m_flat = NULL;
        m_size = 0;
    }
    size_t size() const { return m_size; }
    bool   empty() const { return m_size == 0; }
    void push_back(const T &v)
    {
        if (m_flat)
        {
            // Growing again after compact(): back to chunks
            T* flat = m_flat;
            const size_t n = m_size;
            m_flat = NULL;
            m_size = 0;
            for (size_t i = 0; i < n; i++)
                push_back(flat[i]);
            delete [] flat;
        }
        if ((m_size >> SHIFT) == m_chunks.size())
            m_chunks.push_back(new T[CHUNK]);
        m_chunks[m_size >> SHIFT][m_size & (CHUNK - 1)] = v;
        m_size++;
    }
    const T& operator[](size_t i) const
    {
        return m_flat ? m_flat[i] : m_chunks[i >> SHIFT][i & (CHUNK - 1)];
    }
    T& operator[](size_t i)
    {
        return m_flat ? m_flat[i] : m_chunks[i >> SHIFT][i & (CHUNK - 1)];
    }
    /** Moves the contents into a single exactly-sized block. */
    void compact()
    {
        if (m_flat || m_size == 0)
            return;
        T* flat = new T[m_size];
        for (size_t c = 0; c < m_chunks.size(); c++)
        {
            const size_t first = c << SHIFT;
            const size_t n = std::min(CHUNK, m_size - first);
            std::copy(m_chunks[c], m_chunks[c] + n, flat + first);
            delete [] m_chunks[c];
            m_chunks[c] = NULL;
        }
        std::vector<T*>().swap(m_chunks);
        m_flat = flat;
    }
    /** Contiguous data, only valid after compact(). */
    const T* data() const { return m_flat; }
};

/** Triangle storage for memory-constrained targets (the N64 port), used in
 *  place of btTriangleMesh. btTriangleMesh keeps three unshared vertices per
 *  triangle; here vertices that are bit-for-bit identical are shared, kept as
 *  3 floats, and indexed with 16-bit indices whenever they fit. Every
 *  triangle keeps exactly its original coordinates and its original index,
 *  so Bullet sees the same triangles in the same order.
 *  \ingroup physics */
class CompactTriangleMesh : public btTriangleIndexVertexArray
{
    ChunkedArray<float>    m_vertices;
    ChunkedArray<uint32_t> m_indices32;
    ChunkedArray<uint16_t> m_indices16;
    /** Vertex lookup, only alive while triangles are added: open addressing,
     *  each entry is a vertex index + 1 (0 means empty). */
    std::vector<uint32_t> m_lookup;
    unsigned int          m_num_triangles;

    uint32_t findOrAddVertex(const btVector3 &v);
    void     rebuildLookup(size_t capacity);
    uint32_t getIndex(unsigned int i) const
    {
        return m_indices16.empty() ? m_indices32[i] : m_indices16[i];
    }
public:
    CompactTriangleMesh() : m_num_triangles(0) {}
    void addTriangle(const btVector3 &t1, const btVector3 &t2,
                     const btVector3 &t3);
    /** Drops the build-time lookup, narrows the indices if possible and
     *  hands the arrays to Bullet. Must be called before Bullet uses the
     *  mesh, and again after adding more triangles. */
    void finalize();
    int  getNumTriangles() const { return (int)m_num_triangles; }
    void getTriangle(unsigned int indx, btVector3 *p1, btVector3 *p2,
                     btVector3 *p3) const
    {
        const ChunkedArray<float> &v = m_vertices;
        const size_t a = 3 * getIndex(3 * indx    );
        const size_t b = 3 * getIndex(3 * indx + 1);
        const size_t c = 3 * getIndex(3 * indx + 2);
        *p1 = btVector3(v[a], v[a + 1], v[a + 2]);
        *p2 = btVector3(v[b], v[b + 1], v[b + 2]);
        *p3 = btVector3(v[c], v[c + 1], v[c + 2]);
    }
};
#endif

/**
 * \brief A special class to store a triangle mesh with a separate material per triangle.
 * \ingroup physics
 */
class TriangleMesh
{
private:
    UserPointer                  m_user_pointer;
#ifdef STK_LOW_MEMORY
    ChunkedArray<const Material*> m_triangleIndex2Material;
#else
    std::vector<const Material*> m_triangleIndex2Material;
#endif
    btRigidBody                 *m_body;
    /** Keep track if the physical body was created here or not. */
    bool                         m_free_body;

    btCollisionObject           *m_collision_object;
#ifdef STK_LOW_MEMORY
    CompactTriangleMesh          m_mesh;
#else
    btTriangleMesh               m_mesh;
#endif
    btVector3 dummy1, dummy2;
    btDefaultMotionState        *m_motion_state;
    btCollisionShape            *m_collision_shape;

#ifdef STK_LOW_MEMORY
    /** The three normals for each triangle, packed 10:10:10:2 the way .spm
     *  files store normals (MiniGLM::compressVector3): 12 bytes per triangle
     *  instead of 48. */
    ChunkedArray<uint32_t>      m_normals;
#else
    /** The three normals for each triangle. */
    std::vector<btVector3>      m_normals;
#endif

#ifndef STK_LOW_MEMORY
    /** Pre-compute value used in smoothing. (Low-memory builds recompute it
     *  from the triangle instead, see getP1P2P3.) */
    std::vector<float>          m_p1p2p3;
#endif

#ifdef STK_ROM_COLLISION
    /** Hash of the triangle stream (see collision_file.hpp), and the
     *  materials in the order they first appear in it. */
    CollisionHash                 m_hash;
    std::vector<const Material*>  m_hash_materials;
    /** If set, all collision data is read from this file and addTriangle
     *  only hashes. */
    CollisionFile                *m_rom;
    unsigned int                  m_rom_triangles;
    unsigned int hashMaterial(const Material *m);
#endif

    /** If the rigid body can be transformed (which means that normalising
     *  the normals need to update the vertices and normals used according
     *  to the current transform of the body. */
    bool m_can_be_transformed;

public:
    class RigidBodyTriangleMesh : public btRigidBody
    {
    public:
        TriangleMesh *m_triangle_mesh;
        RigidBodyTriangleMesh(TriangleMesh *tm,
            const btRigidBody::btRigidBodyConstructionInfo &ci)
            : btRigidBody(ci),
            m_triangle_mesh(tm)
        {
        }   // RigidBodyTriangleMesh
    };

         TriangleMesh(bool can_be_transformed);
        ~TriangleMesh();
    void addTriangle(const btVector3 &t1, const btVector3 &t2,
                     const btVector3 &t3, const btVector3 &n1,
                     const btVector3 &n2, const btVector3 &n3,
                     const Material* m);
    void createCollisionShape(bool create_collision_object=true, const char* serialized_bhv=NULL);
    void createPhysicalBody(float friction,
                            btCollisionObject::CollisionFlags flags=
                               (btCollisionObject::CollisionFlags)0,
                            const char* serializedBhv = NULL);
    void removeAll();
    void removeCollisionObject();
    btVector3 getInterpolatedNormal(unsigned int index,
                                    const btVector3 &position) const;
    // ------------------------------------------------------------------------
    /** In case of physical objects of shape 'exact', the physical body is
     *  created outside of the mesh. Since raycasts need the body's world
     *  transform, the body can be set using this function. This will also
     *  cause the body not to be freed (since it will be freed as part of
     *  the physical object). */
    void setBody(btRigidBody *body)
    {
        assert(!m_body);
        // Mark that the body should not be deleted when this object is
        // deleted, since the body is managed elsewhere.
        m_free_body = false;
        m_body = body;
    }
    const btRigidBody *getBody() const { return m_body; }
    // ------------------------------------------------------------------------
    const Material* getMaterial(int n) const
    {
#ifdef STK_ROM_COLLISION
        if (m_rom)
            return m_hash_materials[m_rom->readMaterial(n)];
#endif
        return m_triangleIndex2Material[n];
    }
    // ------------------------------------------------------------------------
    int getNumTriangles() const
    {
#ifdef STK_ROM_COLLISION
        if (m_rom)
            return (int)m_rom->getHeader().m_num_triangles;
#endif
        return m_mesh.getNumTriangles();
    }
#ifdef STK_ROM_COLLISION
    // ------------------------------------------------------------------------
    /** Serve this mesh from a collision file if it exists and is valid; the
     *  triangles added afterwards are only hashed and must reproduce it. */
    bool useCollisionFile(const std::string &path);
    /** Write the collision built in RAM to a file (host builds). */
    bool exportCollisionFile(const std::string &path) const;
    bool verifyCollisionFile(const std::string &path,
                             const std::vector<uint32_t> &order) const;
    bool isFromCollisionFile() const { return m_rom != NULL; }
    const CollisionFile* getCollisionFile() const { return m_rom; }
#endif
    // ------------------------------------------------------------------------
    const btCollisionShape &getCollisionShape() const
                                          { return *m_collision_shape; }
    // ------------------------------------------------------------------------
    btCollisionShape &getCollisionShape() { return *m_collision_shape; }
    // ------------------------------------------------------------------------
    bool castRay(const btVector3 &from, const btVector3 &to,
                 btVector3 *xyz, const Material **material,
                 btVector3 *normal=NULL, bool interpolate_normal=false) const;
    // ------------------------------------------------------------------------
    /** Returns the points of the 'indx' triangle.
     *  \param indx Index of the triangle to get.
     *  \param p1,p2,p3 On return the three points of the triangle. */
    void getTriangle(unsigned int indx, btVector3 *p1, btVector3 *p2,
                     btVector3 *p3) const
    {
#ifdef STK_ROM_COLLISION
        if (m_rom)
        {
            m_rom->readTriangle(indx, p1, p2, p3);
            return;
        }
#endif
#ifdef STK_LOW_MEMORY
        m_mesh.getTriangle(indx, p1, p2, p3);
#else
        const IndexedMeshArray &m = m_mesh.getIndexedMeshArray();
        btVector3 *p = &(((btVector3*)(m[0].m_vertexBase))[3*indx]);
        *p1 = p[0];
        *p2 = p[1];
        *p3 = p[2];
#endif
    }   // getTriangle
    // ------------------------------------------------------------------------
    /** Returns the normals of the triangle with the given index.
     *  \param indx Index of the triangle to get the three normals of.
     *  \result n1,n2,n3 The three normals. */
    void getNormals(unsigned int indx, btVector3 *n1,
                    btVector3 *n2, btVector3 *n3) const
    {
#ifdef STK_ROM_COLLISION
        if (m_rom)
        {
            uint32_t packed[3];
            m_rom->readNormals(indx, packed);
            *n1 = unpackNormal(packed[0]);
            *n2 = unpackNormal(packed[1]);
            *n3 = unpackNormal(packed[2]);
            return;
        }
#endif
        assert(indx < m_triangleIndex2Material.size());
        unsigned int n = indx*3;
#ifdef STK_LOW_MEMORY
        *n1 = unpackNormal(m_normals[n  ]);
        *n2 = unpackNormal(m_normals[n+1]);
        *n3 = unpackNormal(m_normals[n+2]);
#else
        *n1 = m_normals[n  ];
        *n2 = m_normals[n+1];
        *n3 = m_normals[n+2];
#endif
    }   // getNormals
#ifdef STK_LOW_MEMORY
    // ------------------------------------------------------------------------
    static btVector3 unpackNormal(uint32_t packed)
    {
        const irr::core::vector3df v = MiniGLM::decompressVector3(packed);
        return btVector3(v.X, v.Y, v.Z);
    }
#endif
    // ------------------------------------------------------------------------
    /** Returns basically the area of the triangle, which is needed when
     *  smoothing the normals. */
    float getP1P2P3(unsigned int indx) const
    {
#ifdef STK_LOW_MEMORY
        // Same arithmetic as addTriangle used to store, on the same floats.
        btVector3 t1, t2, t3;
        getTriangle(indx, &t1, &t2, &t3);
        btVector3 edge1 = t2 - t1;
        btVector3 edge2 = t3 - t1;
        return edge1.cross(edge2).length2();
#else
        assert(indx < m_p1p2p3.size());
        return m_p1p2p3[indx];
#endif
    }
    // ------------------------------------------------------------------------
    void copyFrom(const TriangleMesh& tm)
    {
        for (int i = 0; i < tm.getNumTriangles(); i++)
        {
            btVector3 v[6];
            tm.getTriangle(i, v, v + 1, v + 2);
            tm.getNormals(i, v + 3, v + 4, v + 5);
            const Material* m = tm.getMaterial(i);
            addTriangle(v[0], v[1], v[2], v[3], v[4], v[5], m);
        }
    }
};
#endif
/* EOF */

