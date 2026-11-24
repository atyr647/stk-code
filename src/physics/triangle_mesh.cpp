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
//  but WITHOUT ANY WARRANTY; without even the implied warranty ofati
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "physics/triangle_mesh.hpp"

#include "config/stk_config.hpp"
#include "main_loop.hpp"
#include "physics/physics.hpp"
#include "utils/constants.hpp"
#include "utils/log.hpp"
#include "utils/time.hpp"

#include "btBulletDynamicsCommon.h"
#include "BulletCollision/NarrowPhaseCollision/btRaycastCallback.h"

#include <algorithm>
#include <cstring>
#include <fstream>

// -----------------------------------------------------------------------------
/** Constructor: Initialises all data structures with zero.
 */
#ifdef STK_LOW_MEMORY
// Memory-constrained targets (the N64 port) trade nothing in behaviour for
// size: vertices stored as 3 floats instead of 4, normals packed into 32 bits
// (see triangle_mesh.hpp), and a quantised BVH (16-byte nodes instead of 64 --
// it only culls, the triangle tests stay exact).
static const bool USE_QUANTIZED_BVH = true;
#else
static const bool USE_QUANTIZED_BVH = false;
#endif

#ifdef STK_LOW_MEMORY
// -----------------------------------------------------------------------------
static uint32_t hashVertex(const float *v)
{
    uint32_t b[3];
    memcpy(b, v, sizeof(b));
    uint32_t h = b[0] * 0x9E3779B1u ^ b[1] * 0x85EBCA77u ^ b[2] * 0xC2B2AE3Du;
    return h ^ (h >> 15);
}   // hashVertex

// -----------------------------------------------------------------------------
void CompactTriangleMesh::rebuildLookup(size_t capacity)
{
    std::vector<uint32_t>(capacity, 0).swap(m_lookup);
    const size_t mask = capacity - 1;
    const uint32_t count = (uint32_t)(m_vertices.size() / 3);
    for (uint32_t i = 0; i < count; i++)
    {
        const float v[3] = { m_vertices[3 * i], m_vertices[3 * i + 1],
                             m_vertices[3 * i + 2] };
        size_t slot = hashVertex(v) & mask;
        while (m_lookup[slot] != 0)
            slot = (slot + 1) & mask;
        m_lookup[slot] = i + 1;
    }
}   // rebuildLookup

// -----------------------------------------------------------------------------
uint32_t CompactTriangleMesh::findOrAddVertex(const btVector3 &vertex)
{
    const float v[3] = { (float)vertex.getX(), (float)vertex.getY(),
                         (float)vertex.getZ() };
    const uint32_t count = (uint32_t)(m_vertices.size() / 3);
    // Keep the table at most 3/4 full
    if (4 * (size_t)(count + 1) > 3 * m_lookup.size())
        rebuildLookup(std::max<size_t>(1024, 2 * m_lookup.size()));
    const size_t mask = m_lookup.size() - 1;
    size_t slot = hashVertex(v) & mask;
    while (m_lookup[slot] != 0)
    {
        const size_t o = 3 * (size_t)(m_lookup[slot] - 1);
        const float other[3] = { m_vertices[o], m_vertices[o + 1],
                                 m_vertices[o + 2] };
        if (memcmp(other, v, sizeof(v)) == 0)
            return m_lookup[slot] - 1;
        slot = (slot + 1) & mask;
    }
    m_vertices.push_back(v[0]);
    m_vertices.push_back(v[1]);
    m_vertices.push_back(v[2]);
    m_lookup[slot] = count + 1;
    return count;
}   // findOrAddVertex

// -----------------------------------------------------------------------------
void CompactTriangleMesh::addTriangle(const btVector3 &t1, const btVector3 &t2,
                                      const btVector3 &t3)
{
    if (!m_indices16.empty())
    {
        // Adding after finalize(): go back to the build-time layout
        for (size_t i = 0; i < m_indices16.size(); i++)
            m_indices32.push_back(m_indices16[i]);
        m_indices16.clear();
    }
    if (m_lookup.empty() && !m_vertices.empty())
    {
        size_t capacity = 1024;
        while (3 * capacity < 4 * (m_vertices.size() / 3 + 1))
            capacity *= 2;
        rebuildLookup(capacity);
    }
    m_indices32.push_back(findOrAddVertex(t1));
    m_indices32.push_back(findOrAddVertex(t2));
    m_indices32.push_back(findOrAddVertex(t3));
    m_num_triangles++;
}   // addTriangle

// -----------------------------------------------------------------------------
void CompactTriangleMesh::finalize()
{
    std::vector<uint32_t>().swap(m_lookup);
    const size_t vertex_count = m_vertices.size() / 3;
    if (m_indices16.empty() && vertex_count <= 65536)
    {
        for (size_t i = 0; i < m_indices32.size(); i++)
            m_indices16.push_back((uint16_t)m_indices32[i]);
        m_indices32.clear();
    }
    m_indices16.compact();
    m_indices32.compact();
    m_vertices.compact();

    const bool use16 = !m_indices16.empty();
    const PHY_ScalarType index_type = use16 ? PHY_SHORT : PHY_INTEGER;
    btIndexedMesh mesh;
    mesh.m_numTriangles        = (int)m_num_triangles;
    mesh.m_triangleIndexBase   = use16
                               ? (const unsigned char*)m_indices16.data()
                               : (const unsigned char*)m_indices32.data();
    mesh.m_triangleIndexStride = 3 * (use16 ? sizeof(uint16_t)
                                            : sizeof(uint32_t));
    mesh.m_numVertices         = (int)vertex_count;
    mesh.m_vertexBase          = (const unsigned char*)m_vertices.data();
    mesh.m_vertexStride        = 3 * sizeof(float);
    mesh.m_indexType           = index_type;
    mesh.m_vertexType          = PHY_FLOAT;
    if (m_indexedMeshes.size() == 0)
        addIndexedMesh(mesh, index_type);
    else
        m_indexedMeshes[0] = mesh;
}   // finalize

#endif

// -----------------------------------------------------------------------------
TriangleMesh::TriangleMesh(bool can_be_transformed)
            : m_mesh()
{
    m_body               = NULL;
    m_free_body          = true;
    m_motion_state       = NULL;
    m_can_be_transformed = can_be_transformed;
    // FIXME: on VS in release mode this statement actually overwrites
    // part of the data of m_mesh, causing a crash later. Debugging
    // shows that apparently m_collision_shape is at the same address
    // as m_mesh->m_use32bitIndices and m_use4componentVertices
    // (and m_mesh->m_weldingThreshold at m_normals
    m_collision_shape  = NULL;
    m_collision_object = NULL;
    m_user_pointer.set(this);
#ifdef STK_ROM_COLLISION
    m_rom              = NULL;
    m_rom_triangles    = 0;
#endif
}   // TriangleMesh

// -----------------------------------------------------------------------------
/** Destructor: delete all allocated data structures.
 */
TriangleMesh::~TriangleMesh()
{
    removeAll();
#ifdef STK_ROM_COLLISION
    if (m_rom)
    {
        const FilePageCache &c = m_rom->getCache();
        Log::info("TriangleMesh", "Collision cache (%u KB, %u B pages): "
                  "%u hits, %u misses (%.2f%%), %.1f MB read.",
                  c.getCacheBytes() / 1024, c.getPageSize(), c.getHits(),
                  c.getMisses(), 100.0 * c.getMisses() /
                  std::max(1u, c.getHits() + c.getMisses()),
                  double(c.getMisses()) * c.getPageSize() / 1048576.0);
    }
    delete m_rom;
#endif
}   // ~TriangleMesh

#ifdef STK_ROM_COLLISION
// -----------------------------------------------------------------------------
unsigned int TriangleMesh::hashMaterial(const Material *m)
{
    for (unsigned int i = 0; i < m_hash_materials.size(); i++)
    {
        if (m_hash_materials[i] == m)
            return i;
    }
    m_hash_materials.push_back(m);
    return (unsigned int)m_hash_materials.size() - 1;
}   // hashMaterial

// -----------------------------------------------------------------------------
bool TriangleMesh::useCollisionFile(const std::string &path)
{
    assert(m_rom == NULL && m_triangleIndex2Material.size() == 0);
    m_rom = CollisionFile::open(path);
    if (m_rom)
    {
        Log::info("TriangleMesh", "Collision from '%s': %u triangles.",
                  path.c_str(), m_rom->getHeader().m_num_triangles);
    }
    return m_rom != NULL;
}   // useCollisionFile

// -----------------------------------------------------------------------------
bool TriangleMesh::exportCollisionFile(const std::string &path,
                                       unsigned world_objects) const
{
    const btBvhTriangleMeshShape *shape =
        dynamic_cast<const btBvhTriangleMeshShape*>(m_collision_shape);
    if (m_rom || !shape || m_mesh.getIndexedMeshArray().size() != 1)
        return false;
    std::vector<const Material*> order;
    std::vector<uint16_t> materials;
    for (size_t i = 0; i < m_triangleIndex2Material.size(); i++)
    {
        const Material *m = m_triangleIndex2Material[i];
        size_t k = std::find(order.begin(), order.end(), m) - order.begin();
        if (k == order.size())
            order.push_back(m);
        materials.push_back((uint16_t)k);
    }
    auto record = [&](uint32_t i, CollisionTriangle *t)
    {
        btVector3 p[3];
        getTriangle(i, p, p + 1, p + 2);
        for (int k = 0; k < 3; k++)
        {
            t->m_vertices[3 * k    ] = (float)p[k].getX();
            t->m_vertices[3 * k + 1] = (float)p[k].getY();
            t->m_vertices[3 * k + 2] = (float)p[k].getZ();
            t->m_normals[k] = m_normals[3 * i + k];
        }
        t->m_material = materials[i];
    };
    std::vector<uint32_t> file_order;
    if (!CollisionFile::write(path, m_hash.get(), *shape,
                              (unsigned)m_triangleIndex2Material.size(),
                              (unsigned)order.size(), world_objects,
                              record, &file_order))
        return false;
    return verifyCollisionFile(path, file_order);
}   // exportCollisionFile

// -----------------------------------------------------------------------------
namespace
{
    struct RayHits : public btTriangleRaycastCallback
    {
        std::vector<float> m_out;
        RayHits(const btVector3 &from, const btVector3 &to)
            : btTriangleRaycastCallback(from, to) {}
        virtual btScalar reportHit(const btVector3 &n, btScalar fraction,
                                   int part, int triangle)
        {
            const float v[6] = { (float)part, (float)triangle,
                                 (float)fraction, (float)n.getX(),
                                 (float)n.getY(), (float)n.getZ() };
            m_out.insert(m_out.end(), v, v + 6);
            return fraction;
        }
    };
    struct BoxHits : public btTriangleCallback
    {
        std::vector<float> m_out;
        virtual void processTriangle(btVector3 *t, int part, int triangle)
        {
            m_out.push_back((float)part);
            m_out.push_back((float)triangle);
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 3; k++)
                    m_out.push_back((float)t[i][k]);
        }
    };
}   // anonymous namespace

// -----------------------------------------------------------------------------
/** Checks that a just-written collision file reproduces this mesh exactly:
 *  per-triangle data, and Bullet's answers to raycasts and AABB queries
 *  spread over the whole mesh. */
bool TriangleMesh::verifyCollisionFile(const std::string &path,
                                   const std::vector<uint32_t> &order) const
{
    CollisionFile *rom = CollisionFile::open(path);
    if (!rom)
        return false;
    btBvhTriangleMeshShape *ram =
        dynamic_cast<btBvhTriangleMeshShape*>(m_collision_shape);
    btBvhTriangleMeshShape file(rom, true, false);
    file.setOptimizedBvh(rom->getBvh());
    unsigned bad = 0;
    const unsigned n = (unsigned)m_triangleIndex2Material.size();
    if (order.size() != n)
        bad++;
    for (unsigned f = 0; f < n && order.size() == n; f++)
    {
        // file triangle f is mesh triangle order[f]
        const unsigned i = order[f];
        btVector3 a[3], b[3];
        getTriangle(i, a, a + 1, a + 2);
        rom->readTriangle(f, b, b + 1, b + 2);
        uint32_t normals[3];
        rom->readNormals(f, normals);
        for (int k = 0; k < 3; k++)
        {
            if (a[k] != b[k] || normals[k] != m_normals[3 * i + k])
                bad++;
        }
        if (m_hash_materials[rom->readMaterial(f)] !=
            m_triangleIndex2Material[i])
            bad++;
    }
    // Bullet reports file numbering for the file shape: map it back
    auto remap = [&order](std::vector<float> *v, size_t stride)
    {
        for (size_t k = 1; k < v->size(); k += stride)
            (*v)[k] = (float)order[(size_t)(*v)[k]];
    };
    if (ram->getLocalAabbMin() != file.getLocalAabbMin() ||
        ram->getLocalAabbMax() != file.getLocalAabbMax())
        bad++;
    const btVector3 lo = ram->getLocalAabbMin(), hi = ram->getLocalAabbMax();
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) * (1.0f / 16777216.0f);
    };
    auto point = [&]() {
        return btVector3(lo.getX() + rnd() * (hi.getX() - lo.getX()),
                         lo.getY() + rnd() * (hi.getY() - lo.getY()),
                         lo.getZ() + rnd() * (hi.getZ() - lo.getZ()));
    };
    unsigned hits = 0;
    for (int r = 0; r < 20000; r++)
    {
        // mostly short downward rays (wheels), some long arbitrary ones
        btVector3 from = point(), to;
        if (r % 4)
            to = from + btVector3(rnd() - 0.5f, -3.0f - 20.0f * rnd(),
                                  rnd() - 0.5f);
        else
            to = point();
        RayHits h1(from, to), h2(from, to);
        ram->performRaycast(&h1, from, to);
        file.performRaycast(&h2, from, to);
        remap(&h2.m_out, 6);
        hits += !h1.m_out.empty();
        if (h1.m_out != h2.m_out)
            bad++;
    }
    for (int q = 0; q < 5000; q++)
    {
        const btVector3 c = point();
        const btVector3 e(0.5f + 2 * rnd(), 0.5f + 2 * rnd(),
                          0.5f + 2 * rnd());
        BoxHits b1, b2;
        ram->processAllTriangles(&b1, c - e, c + e);
        file.processAllTriangles(&b2, c - e, c + e);
        remap(&b2.m_out, 11);
        if (b1.m_out != b2.m_out)
            bad++;
    }
    Log::info("TriangleMesh", "Verified '%s': %u triangles, 20000 rays "
              "(%u hit), 5000 boxes: %u mismatches.", path.c_str(), n, hits,
              bad);
    delete rom;
    return bad == 0;
}   // verifyCollisionFile
#endif

// -----------------------------------------------------------------------------
/** Adds a triangle to the bullet mesh. It also stores the material used for
 *  this triangle, and the three normals.
 *  \param t1,t2,t3 Points of the triangle.
 *  \param n1,n2,n3 Normals at the corresponding points.
 *  \param m Material used for this triangle
 */
void TriangleMesh::addTriangle(const btVector3 &t1, const btVector3 &t2,
                               const btVector3 &t3,
                               const btVector3 &n1, const btVector3 &n2,
                               const btVector3 &n3,
                               const Material* m)
{
    btVector3 normal = (t2-t1).cross(t3-t1);
#ifdef STK_LOW_MEMORY
    // A zero-area triangle has no face normal. Desktop code gets NaN from
    // normalize(), every angle comparison with it is false, and each
    // corner keeps its vertex normal; produce exactly that without the NaN
    // (the N64's FPU cannot compute with NaN: it raises an unmaskable
    // exception).
    const bool degenerate = normal.length2() == 0;
    if (!degenerate)
        normal.normalize();
    auto pack = [](const btVector3 &v)
    {
        btVector3 u = v.length2() > 0 ? v.normalized() : btVector3(0, 1, 0);
        return MiniGLM::compressVector3(irr::core::vector3df(u.getX(), u.getY(),
                                                             u.getZ()));
    };
    const float limit = stk_config->m_smooth_angle_limit;
    // (same for a zero vertex normal: angle() would be NaN)
    auto use_face = [&](const btVector3 &n)
    {
        return !degenerate && n.length2() != 0 && normal.angle(n) > limit;
    };
    const uint32_t packed[3] = { pack(use_face(n1) ? normal : n1),
                                 pack(use_face(n2) ? normal : n2),
                                 pack(use_face(n3) ? normal : n3) };
#ifdef STK_ROM_COLLISION
    const btVector3 *t[3] = { &t1, &t2, &t3 };
    for (int k = 0; k < 3; k++)
    {
        m_hash.add((float)t[k]->getX());
        m_hash.add((float)t[k]->getY());
        m_hash.add((float)t[k]->getZ());
    }
    for (int k = 0; k < 3; k++)
        m_hash.add(packed[k]);
    m_hash.add((uint32_t)hashMaterial(m));
    if (m_rom)
    {
        // Served from the collision file: only check it is the same mesh
        m_rom_triangles++;
        return;
    }
#endif
    m_triangleIndex2Material.push_back(m);
    m_normals.push_back(packed[0]);
    m_normals.push_back(packed[1]);
    m_normals.push_back(packed[2]);
#else
    normal.normalize();
    m_triangleIndex2Material.push_back(m);
    m_normals.push_back( normal.angle(n1)>stk_config->m_smooth_angle_limit
                         ? normal : n1                                     );
    m_normals.push_back( normal.angle(n2)>stk_config->m_smooth_angle_limit
                         ? normal : n2                                     );
    m_normals.push_back( normal.angle(n3)>stk_config->m_smooth_angle_limit
                         ? normal : n3                                     );
#endif
    m_mesh.addTriangle(t1, t2, t3);

#ifndef STK_LOW_MEMORY
    // Area of triangle ABC
    btVector3 edge1 = t2 - t1;
    btVector3 edge2 = t3 - t1;
    m_p1p2p3.push_back(edge1.cross(edge2).length2());
#endif
}   // addTriangle

// -----------------------------------------------------------------------------
/** Creates a collision body only, which can be used for raycasting, but
 *  has no physical properties.
 *  @param serialized_bhv if non-null, load the serialized bhv from file instead
 *                        of builing it on the fly
 */
void TriangleMesh::createCollisionShape(bool create_collision_object, const char* serialized_bhv)
{
#ifdef STK_ROM_COLLISION
    if (m_rom)
    {
        if (m_rom_triangles != m_rom->getHeader().m_num_triangles ||
            m_hash.get() != m_rom->getHash() ||
            m_hash_materials.size() != m_rom->getHeader().m_num_materials)
        {
            Log::fatal("TriangleMesh", "'%s' does not match this track "
                       "(%u triangles, file has %u): regenerate the data.",
                       m_rom->getPath().c_str(), m_rom_triangles,
                       m_rom->getHeader().m_num_triangles);
        }
        btBvhTriangleMeshShape *shape =
            new btBvhTriangleMeshShape(m_rom, /*quantized*/true,
                                       /*buildBvh*/false);
        shape->setOptimizedBvh(m_rom->getBvh());
        m_collision_shape = shape;
        m_collision_shape->setUserPointer(&m_user_pointer);
        if (create_collision_object)
        {
            m_collision_object = new btCollisionObject();
            btTransform bt;
            bt.setIdentity();
            m_collision_object->setWorldTransform(bt);
        }
        return;
    }
#endif
    if(m_triangleIndex2Material.size()==0)
    {
        m_collision_shape  = NULL;
        m_motion_state     = NULL;
        m_body             = NULL;
        m_collision_object = NULL;
        return;
    }
#ifdef STK_LOW_MEMORY
    // Release build-time slack before the BVH build, which is the peak
    m_mesh.finalize();
    m_triangleIndex2Material.compact();
    m_normals.compact();
#endif
    // Now convert the triangle mesh into a static rigid body
    btBvhTriangleMeshShape* bhv_triangle_mesh;

    if (serialized_bhv != NULL)
    {
        FILE *f = fopen(serialized_bhv, "rb");
        fseek(f, 0, SEEK_END);
        long pos = ftell(f);
        assert(pos != -1L);
        fseek(f, 0, SEEK_SET);

        void* bytes = btAlignedAlloc(pos, 16);
        fread(bytes, pos, 1, f);
        fclose(f);

        btOptimizedBvh* bhv = btOptimizedBvh::deSerializeInPlace(bytes, pos, !IS_LITTLE_ENDIAN);
        if (bhv == NULL)
        {
            Log::warn("TriangleMesh", "Failed to load serialized BHV");
            bhv_triangle_mesh = new btBvhTriangleMeshShape(&m_mesh, USE_QUANTIZED_BVH);
        }
        else
        {
            bhv_triangle_mesh = new btBvhTriangleMeshShape(&m_mesh, USE_QUANTIZED_BVH,
                                                           false /* buildBvh */);
            bhv_triangle_mesh->setOptimizedBvh( bhv );
        }
        // Do *NOT* free the bytes, 'deSerializeInPlace' makes the btOptimizedBvh object
        // directly at this memory location
        //free(bytes);

    }
    else
    {
        bhv_triangle_mesh = new btBvhTriangleMeshShape(&m_mesh, USE_QUANTIZED_BVH);

        /*
         // code to serialize triangle mesh
        btOptimizedBvh* bvh = bhv_triangle_mesh->getOptimizedBvh();
        unsigned int ssize = bvh->calculateSerializeBufferSize();
        char* buffer = (char*)btAlignedAlloc(ssize, 16);
        bool success = bvh->serialize(buffer, ssize, !IS_LITTLE_ENDIAN);
        printf("serialization success = %i\n", success);

        std::ofstream fileout("/tmp/btOptimizedBvh");
        fileout.write(buffer, ssize);
        fileout.close();

        btAlignedFree(buffer);
         */
    }

    m_collision_shape = bhv_triangle_mesh;
    m_collision_shape->setUserPointer(&m_user_pointer);
    if(create_collision_object)
    {
        m_collision_object = new btCollisionObject();
        btTransform bt;
        bt.setIdentity();
        m_collision_object->setWorldTransform(bt);
    }


}   // createCollisionShape

// -----------------------------------------------------------------------------
/** Creates the physics body for this triangle mesh. If the body already
 *  exists (because it was created by a previous call to createBody)
 *  it is first removed from the world. This is used by loading the track
 *  where a physics body is used to determine the height of terrain. To have
 *  an optimised rigid body including all static objects, the track is then
 *  removed and all objects together with the track is converted again into
 *  a single rigid body. This avoids using irrlicht (or the graphics engine)
 *  for height of terrain detection).
 *  \param friction Friction to be used for this TriangleMesh.
 *  \param flags Additional collision flags (default 0).
 *  \param serializedBhv if non-NULL, the bhv is deserialized instead of
 *                       being calculated on the fly
 */
void TriangleMesh::createPhysicalBody(float friction,
                                      btCollisionObject::CollisionFlags flags,
                                      const char* serializedBhv)
{
    // We need the collision shape, but not the collision object (since
    // this will be created when the dynamics body is anyway).
    createCollisionShape(/*create_collision_object*/false, serializedBhv);
    main_loop->renderGUI(5583);

    btTransform startTransform;
    startTransform.setIdentity();
    m_motion_state = new btDefaultMotionState(startTransform);
    btRigidBody::btRigidBodyConstructionInfo info(0.0f, m_motion_state,
                                                  m_collision_shape);
    info.m_restitution = 0.8f;
    info.m_friction    = friction;

    m_body=new RigidBodyTriangleMesh(this, info);
    Physics::get()->addBody(m_body);

    m_body->setUserPointer(&m_user_pointer);
    m_body->setCollisionFlags(m_body->getCollisionFlags()  |
                              flags                        |
                              btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
}   // createPhysicalBody

// ----------------------------------------------------------------------------
/** Removes the created body and/or collision object from the physics world.
 *  This is used when creating a temporary rigid body of the main track to get
 *  bullet raycasts. Then the main track is removed, and the track (main track
 *  including all additional objects which were loaded later) is converted
 *  again.
 */
void TriangleMesh::removeAll()
{
    // Don't free the physical body if it was created outside this object.
    if(m_body && m_free_body)
    {
        Physics::get()->removeBody(m_body);
        delete m_body;
        delete m_motion_state;
        m_body         = NULL;
        m_motion_state = NULL;
    }
    if(m_collision_object)
    {
        delete m_collision_object;
        m_collision_object = NULL;
    }
    delete m_collision_shape;
    m_collision_shape = NULL;
}   // removeAll

// -----------------------------------------------------------------------------
/** Interpolates the normal at the given position for the triangle with
 *  a given index. The position must be inside of the given triangle.
 *  \param index Index of the triangle to use.
 *  \param position The position for which to interpolate the normal.
 */
btVector3 TriangleMesh::getInterpolatedNormal(unsigned int index,
                                              const btVector3 &position) const
{
    btVector3 p1, p2, p3;
    btVector3 n1, n2, n3;
    if(m_can_be_transformed)
    {
        // If the object of this mesh can be transformed, we need to compute
        // the updated positions and normals before interpolating.
        btVector3 q1, q2, q3;
        getTriangle(index, &q1, &q2, &q3);
        const btTransform &tf = m_body->getWorldTransform();
        // The triangle verteces must be moved according to the transform of the body
        p1 = tf(q1);
        p2 = tf(q2);
        p3 = tf(q3);
        // The normals must be rotated according to the transform of the body
        btVector3 m1, m2, m3;
        getNormals(index, &m1, &m2, &m3);
        n1 = tf.getBasis() * m1;
        n2 = tf.getBasis() * m2;
        n3 = tf.getBasis() * m3;
    }
    else
    {
        getTriangle(index, &p1, &p2, &p3);
        getNormals(index, &n1, &n2, &n3);
    }

    // Compute the Barycentric coordinates of position inside  triangle
    // p1, p2, p3.

    float p1p2p3 = getP1P2P3(index);

    // Area of BCP
    btScalar p2p3p = (p3 - p2).cross(position - p2).length2();

    // Area of CAP
    btVector3 edge2 = p3 - p1;
    btScalar p3p1p = edge2.cross(position - p3).length2();
    btScalar s = btSqrt(p2p3p / p1p2p3);
    btScalar t = btSqrt(p3p1p / p1p2p3);
    btScalar w = 1.0f - s - t;

#ifdef NORMAL_DEBUGGING
    btVector3 regen_position = s * p1 + t * p2 + w * p3;

    if((regen_position - position).length2() >= 0.0001f)
    {
        printf("bary:\n");
        printf("new: %f %f %f\n", regen_position.getX(),regen_position.getY(),regen_position.getZ());
        printf("old: %f %f %f\n", position.getX(), position.getY(),position.getZ());
        printf("stw: %f %f %f\n", s, t, w);
        printf("p1:  %f %f %f\n", p1.getX(),p1.getY(),p1.getZ());
        printf("p2:  %f %f %f\n", p2.getX(),p2.getY(),p2.getZ());
        printf("p3:  %f %f %f\n", p3.getX(),p3.getY(),p3.getZ());
        printf("pos: %f %f %f\n", position.getX(),position.getY(),position.getZ());
    }
#endif

    return s*n1 + t*n2 + w*n3;
}   // getInterpolatedNormal

// ----------------------------------------------------------------------------
/** Casts a ray from 'from' to 'to'. If a triangle of this mesh was hit,
 *  xyz and material will be set.
 *  \param from/to The from and to position for the raycast.
 *  \param xyz The position in world where the ray hit.
 *  \param material The material of the mesh that was hit.
 *  \param normal The intrapolated normal at that position.
 *  \param interpolate_normal If true, the returned normal is the interpolated
 *         based on the three normals of the triangle and the location of the
 *         hit point (which is more compute intensive, but results in much
 *         smoother results).
 *  \return True if a triangle was hit, false otherwise (and no output
 *          variable will be set.
 */
bool TriangleMesh::castRay(const btVector3 &from, const btVector3 &to,
                           btVector3 *xyz, const Material **material,
                           btVector3 *normal, bool interpolate_normal) const
{
    if(!m_collision_shape)
    {
        *material=NULL;
        return false;
    }

    btTransform trans_from;
    trans_from.setIdentity();
    trans_from.setOrigin(from);

    btTransform trans_to;
    trans_to.setIdentity();
    trans_to.setOrigin(to);

    btTransform world_trans;
    // If there is a body, take the current transform from the body.
    if(m_body)
        world_trans = m_body->getWorldTransform();
    else
        world_trans.setIdentity();

    btCollisionWorld::ClosestRayResultCallback result(from, to);

    /** A special ray result class that stores the index of the triangle
     *  that was hit. */
    class MaterialRayResult : public btCollisionWorld::ClosestRayResultCallback
    {
    public:
        /** Stores the index of the triangle that was hit. */
        int m_index;
        // --------------------------------------------------------------------
        MaterialRayResult(const btVector3 &p1, const btVector3 &p2,
                          const TriangleMesh *me)
                        : btCollisionWorld::ClosestRayResultCallback(p1,p2)
        {
            m_index = -1;;
        }   // MaterialRayResult
        // --------------------------------------------------------------------
        virtual btScalar addSingleResult(btCollisionWorld::LocalRayResult& rayResult,
                                         bool normalInWorldSpace)
        {
            m_index = rayResult.m_localShapeInfo->m_triangleIndex;
            return btCollisionWorld::ClosestRayResultCallback
                    ::addSingleResult(rayResult, normalInWorldSpace);
        }   // AddSingleResult
        // --------------------------------------------------------------------
    };   // myCollision

    MaterialRayResult ray_callback(from, to, this);

    // If this is a rigid body, m_collision_object is NULL, and the
    // rigid body is the actual collision object.
    btCollisionWorld::rayTestSingle(trans_from, trans_to,
                                    m_collision_object ? m_collision_object : m_body,
                                    m_collision_shape, world_trans,
                                    ray_callback);
    // Get the index of the triangle hit
    int index = ray_callback.m_index;
    if(ray_callback.hasHit())
    {
        *xyz      = ray_callback.m_hitPointWorld;
        xyz->setW(0.0f);
        *material = getMaterial(index);

        if(normal)
        {
            // If requested interpolate the normal. I.e. instead of using
            // the normal of the triangle interpolate the normal at the
            // hit position based on the three normals of the triangle.
            if(interpolate_normal)
                *normal = getInterpolatedNormal(ray_callback.m_index,
                                                ray_callback.m_hitPointWorld);
            else
                *normal = ray_callback.m_hitNormalWorld;
            normal->normalize();
        }
    }
    else
    {
        *material = NULL;
        if(normal)
            normal->setValue(0, 1, 0);
    }
    return ray_callback.hasHit();

}   // castRay
