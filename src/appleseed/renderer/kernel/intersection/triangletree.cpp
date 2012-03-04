
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2010-2012 Francois Beaune, Jupiter Jazz
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Interface header.
#include "triangletree.h"

// appleseed.renderer headers.
#include "renderer/global/globallogger.h"
#include "renderer/kernel/tessellation/statictessellation.h"
#include "renderer/modeling/object/iregion.h"
#include "renderer/modeling/object/object.h"
#include "renderer/modeling/object/regionkit.h"
#include "renderer/modeling/object/triangle.h"
#include "renderer/modeling/scene/assembly.h"
#include "renderer/modeling/scene/containers.h"
#include "renderer/modeling/scene/objectinstance.h"

// appleseed.foundation headers.
#include "foundation/math/area.h"
#include "foundation/math/permutation.h"
#include "foundation/math/treeoptimizer.h"
#include "foundation/platform/system.h"
#include "foundation/platform/timer.h"
#include "foundation/utility/memory.h"

// Standard headers.
#include <algorithm>
#include <cassert>

using namespace foundation;
using namespace std;

namespace renderer
{

//
// TriangleTree class implementation.
//

namespace
{
    void collect_triangles(
        const TriangleTree::Arguments&  arguments,
        vector<GVector3>&               triangle_vertices,
        vector<TriangleKey>&            triangle_keys,
        vector<AABB3d>&                 triangle_bboxes)
    {
        const size_t region_count = arguments.m_regions.size();
        for (size_t region_index = 0; region_index < region_count; ++region_index)
        {
            // Fetch the region info.
            const RegionInfo& region_info = arguments.m_regions[region_index];

            // Retrieve the object instance and its transformation.
            const ObjectInstance* object_instance =
                arguments.m_assembly.object_instances().get_by_index(
                    region_info.get_object_instance_index());
            assert(object_instance);
            const Transformd& transform = object_instance->get_transform();

            // Retrieve the object.
            Object& object = object_instance->get_object();

            // Retrieve the region kit of the object.
            Access<RegionKit> region_kit(&object.get_region_kit());

            // Retrieve the region.
            const IRegion* region = (*region_kit)[region_info.get_region_index()];

            // Retrieve the tessellation of the region.
            Access<StaticTriangleTess> tess(&region->get_static_triangle_tess());

            // Collect all triangles of the region that intersect the bounding box of the tree.
            const size_t triangle_count = tess->m_primitives.size();
            for (size_t triangle_index = 0; triangle_index < triangle_count; ++triangle_index)
            {
                // Fetch the triangle.
                const Triangle& triangle = tess->m_primitives[triangle_index];

                // Retrieve object space vertices of the triangle.
                const GVector3& v0_os = tess->m_vertices[triangle.m_v0];
                const GVector3& v1_os = tess->m_vertices[triangle.m_v1];
                const GVector3& v2_os = tess->m_vertices[triangle.m_v2];

                // Transform triangle vertices to assembly space.
                const GVector3 v0 = transform.transform_point_to_parent(v0_os);
                const GVector3 v1 = transform.transform_point_to_parent(v1_os);
                const GVector3 v2 = transform.transform_point_to_parent(v2_os);

                // Calculate the (square of the) area of this triangle.
                const GScalar triangle_square_area = square_area(v0, v1, v2);

                // Ignore degenerate triangles.
                if (triangle_square_area == GScalar(0.0))
                    continue;

                // Insert this triangle into the tree if it intersects the tree's bounding box.
                if (intersect(arguments.m_bbox, v0, v1, v2))
                {
                    // Store the triangle.
                    triangle_vertices.push_back(v0);
                    triangle_vertices.push_back(v1);
                    triangle_vertices.push_back(v2);

                    // Store the triangle key.
                    triangle_keys.push_back(
                        TriangleKey(
                            region_info.get_object_instance_index(),
                            region_info.get_region_index(),
                            triangle_index));

                    // Compute and store the triangle bounding box.
                    AABB3d triangle_bbox;
                    triangle_bbox.invalidate();
                    triangle_bbox.insert(Vector3d(v0));
                    triangle_bbox.insert(Vector3d(v1));
                    triangle_bbox.insert(Vector3d(v2));
                    triangle_bbox.robust_grow(1.0e-15);
                    triangle_bboxes.push_back(triangle_bbox);
                }
            }
        }
    }

    struct TriangleItemHandler
    {
        const vector<GVector3>& m_triangle_vertices;

        explicit TriangleItemHandler(const vector<GVector3>& triangle_vertices)
          : m_triangle_vertices(triangle_vertices)
        {
        }

        double get_bbox_grow_eps() const
        {
            return 2.0e-9;
        }

        AABB3d clip(
            const size_t    item_index,
            const size_t    dimension,
            const double    bin_min,
            const double    bin_max) const
        {
            // todo: implement properly.

            AABB3d bbox;
            bbox.invalidate();

            bbox.insert(Vector3d(m_triangle_vertices[item_index * 3 + 0]));
            bbox.insert(Vector3d(m_triangle_vertices[item_index * 3 + 1]));
            bbox.insert(Vector3d(m_triangle_vertices[item_index * 3 + 2]));

            bbox.min[dimension] = max(bbox.min[dimension], bin_min);
            bbox.max[dimension] = min(bbox.max[dimension], bin_max);

            return bbox;
        }

        bool intersect(
            const size_t    item_index,
            const AABB3d&   bbox) const
        {
            return
                foundation::intersect(
                    bbox,
                    Vector3d(m_triangle_vertices[item_index * 3 + 0]),
                    Vector3d(m_triangle_vertices[item_index * 3 + 1]),
                    Vector3d(m_triangle_vertices[item_index * 3 + 2]));
        }
    };
}

TriangleTree::Arguments::Arguments(
    const UniqueID      triangle_tree_uid,
    const GAABB3&       bbox,
    const Assembly&     assembly,
    const RegionInfoVector& regions)
  : m_triangle_tree_uid(triangle_tree_uid)
  , m_bbox(bbox)
  , m_assembly(assembly)
  , m_regions(regions)
{
}

TriangleTree::TriangleTree(const Arguments& arguments)
  : TreeType(AlignedAllocator<void>(System::get_l1_data_cache_line_size()))
  , m_triangle_tree_uid(arguments.m_triangle_tree_uid)
{
    const string acceleration_structure =
        arguments.m_assembly.get_parameters().get_optional<string>("acceleration_structure", "sbvh");

    if (acceleration_structure == "bvh")
        build_bvh(arguments);
    else if (acceleration_structure == "sbvh")
        build_sbvh(arguments);
    else
    {
        RENDERER_LOG_DEBUG(
            "while building acceleration structure for assembly \"%s\": "
            "invalid acceleration structure \"%s\", using default value \"sbvh\".",
            arguments.m_assembly.get_name(),
            acceleration_structure.c_str());

        build_sbvh(arguments);
    }

    RENDERER_LOG_DEBUG(
        "triangle tree node array alignment: %s",
        pretty_size(alignment(&m_nodes[0])).c_str());
}

TriangleTree::~TriangleTree()
{
    RENDERER_LOG_INFO(
        "deleting triangle tree #" FMT_UNIQUE_ID "...",
        m_triangle_tree_uid);
}

size_t TriangleTree::get_memory_size() const
{
    return
          TreeType::get_memory_size()
        - sizeof(*static_cast<const TreeType*>(this))
        + sizeof(*this)
        + m_triangles.capacity() * sizeof(GTriangleType)
        + m_triangle_keys.capacity() * sizeof(TriangleKey);
}

void TriangleTree::build_bvh(const Arguments& arguments)
{
    // Collect triangles intersecting the bounding box of this tree.
    vector<GVector3> triangle_vertices;
    vector<TriangleKey> triangle_keys;
    vector<AABB3d> triangle_bboxes;
    collect_triangles(
        arguments,
        triangle_vertices,
        triangle_keys,
        triangle_bboxes);

    RENDERER_LOG_INFO(
        "building BVH triangle tree #" FMT_UNIQUE_ID " (%s %s)...",
        arguments.m_triangle_tree_uid,
        pretty_int(triangle_keys.size()).c_str(),
        plural(triangle_keys.size(), "triangle").c_str());

    // Build the tree.
    typedef bvh::SAHPartitioner<vector<AABB3d> > Partitioner;
    typedef bvh::Builder<TriangleTree, Partitioner> Builder;
    Partitioner partitioner(
        triangle_bboxes,
        TriangleTreeMaxLeafSize,
        TriangleTreeInteriorNodeTraversalCost,
        TriangleTreeTriangleIntersectionCost);
    Builder builder;
    builder.build<DefaultWallclockTimer>(*this, triangle_keys.size(), partitioner);

    // Bounding boxes are no longer needed.
    clear_release_memory(triangle_bboxes);

    // Store triangles and triangle keys into the tree.
    if (!partitioner.get_item_ordering().empty())
    {
        store_triangles(
            partitioner.get_item_ordering(),
            triangle_vertices,
            triangle_keys);
    }

    // Optimize the tree layout in memory.
    TreeOptimizer<NodeVectorType> tree_optimizer(m_nodes);
    tree_optimizer.optimize_node_layout(TriangleTreeSubtreeDepth);
    assert(m_nodes.size() == m_nodes.capacity());

    // Collect and print triangle tree statistics.
    bvh::TreeStatistics<TriangleTree, Builder> statistics(
        *this,
        AABB3d(arguments.m_bbox),
        builder);
    RENDERER_LOG_DEBUG(
        "triangle tree #" FMT_UNIQUE_ID " statistics:",
        arguments.m_triangle_tree_uid);
    statistics.print(global_logger());
}

void TriangleTree::build_sbvh(const Arguments& arguments)
{
    // Collect triangles intersecting the bounding box of this tree.
    vector<GVector3> triangle_vertices;
    vector<TriangleKey> triangle_keys;
    vector<AABB3d> triangle_bboxes;
    collect_triangles(
        arguments,
        triangle_vertices,
        triangle_keys,
        triangle_bboxes);

    RENDERER_LOG_INFO(
        "building SBVH triangle tree #" FMT_UNIQUE_ID " (%s %s)...",
        arguments.m_triangle_tree_uid,
        pretty_int(triangle_keys.size()).c_str(),
        plural(triangle_keys.size(), "triangle").c_str());

    // Build the tree.
    typedef bvh::SBVHPartitioner<TriangleItemHandler, vector<AABB3d> > Partitioner;
    typedef bvh::SpatialBuilder<TriangleTree, Partitioner> Builder;
    TriangleItemHandler triangle_handler(triangle_vertices);
    Partitioner partitioner(
        triangle_handler,
        triangle_bboxes,
        TriangleTreeMaxLeafSize,
        TriangleTreeBinCount,
        TriangleTreeInteriorNodeTraversalCost,
        TriangleTreeTriangleIntersectionCost);
    Builder builder;
    builder.build<DefaultWallclockTimer>(*this, triangle_keys.size(), partitioner);

    // Bounding boxes are no longer needed.
    clear_release_memory(triangle_bboxes);

    // Store triangles and triangle keys into the tree.
    if (!partitioner.get_item_ordering().empty())
    {
        store_triangles(
            partitioner.get_item_ordering(),
            triangle_vertices,
            triangle_keys);
    }

    // Optimize the tree layout in memory.
    TreeOptimizer<NodeVectorType> tree_optimizer(m_nodes);
    tree_optimizer.optimize_node_layout(TriangleTreeSubtreeDepth);
    assert(m_nodes.size() == m_nodes.capacity());

    // Collect and print triangle tree statistics.
    bvh::TreeStatistics<TriangleTree, Builder> statistics(
        *this,
        AABB3d(arguments.m_bbox),
        builder);
    RENDERER_LOG_DEBUG(
        "triangle tree #" FMT_UNIQUE_ID " statistics:",
        arguments.m_triangle_tree_uid);
    statistics.print(global_logger());
}

void TriangleTree::store_triangles(
    const vector<size_t>&   triangle_indices,
    vector<GVector3>&       triangle_vertices,
    vector<TriangleKey>&    triangle_keys)
{
    // Build triangles out of the vertices.
    const size_t triangle_count = triangle_keys.size();
    vector<GTriangleType> triangles;
    triangles.reserve(triangle_count);
    for (size_t i = 0; i < triangle_count; ++i)
    {
        triangles.push_back(
            GTriangleType(
                triangle_vertices[i * 3 + 0],
                triangle_vertices[i * 3 + 1],
                triangle_vertices[i * 3 + 2]));
    }

    // Triangle vertices are no longer needed.
    clear_release_memory(triangle_vertices);

    // Store triangle and triangle keys into the tree.
    store_triangles(triangle_indices, triangles, triangle_keys);
}

void TriangleTree::store_triangles(
    const vector<size_t>&   triangle_indices,
    vector<GTriangleType>&  triangles,
    vector<TriangleKey>&    triangle_keys)
{
    const size_t node_count = m_nodes.size();

    //
    // Step 1: Gather statistics.
    //

    size_t leaf_count = 0;
    size_t fat_leaf_count = 0;
    size_t external_triangle_count = 0;

    for (size_t i = 0; i < node_count; ++i)
    {
        const NodeType& node = m_nodes[i];

        if (node.is_leaf())
        {
            ++leaf_count;

            const size_t item_count = node.get_item_count();

            if (item_count <= NodeType::MaxUserDataSize / sizeof(GTriangleType))
                ++fat_leaf_count;
            else external_triangle_count += item_count;
        }
    }

    //
    // Step 2: Allocate new arrays.
    //

    m_triangle_keys.reserve(triangle_indices.size());
    m_triangles.reserve(external_triangle_count);

    //
    // Step 3: Handle slim leaves.
    //

    for (size_t i = 0; i < node_count; ++i)
    {
        NodeType& node = m_nodes[i];

        if (node.is_leaf())
        {
            const size_t item_begin = node.get_item_index();
            const size_t item_count = node.get_item_count();

            if (item_count > NodeType::MaxUserDataSize / sizeof(GTriangleType))
            {
                node.set_item_index(m_triangle_keys.size());

                for (size_t j = 0; j < item_count; ++j)
                {
                    const size_t triangle_index = triangle_indices[item_begin + j];
                    m_triangle_keys.push_back(triangle_keys[triangle_index]);
                    m_triangles.push_back(triangles[triangle_index]);
                }
            }
        }
    }

    assert(m_triangle_keys.size() == m_triangles.size());

    //
    // Step 4: Handle fat leaves.
    //

    for (size_t i = 0; i < node_count; ++i)
    {
        NodeType& node = m_nodes[i];

        if (node.is_leaf())
        {
            const size_t item_begin = node.get_item_index();
            const size_t item_count = node.get_item_count();

            if (item_count <= NodeType::MaxUserDataSize / sizeof(GTriangleType))
            {
                node.set_item_index(m_triangle_keys.size());

                GTriangleType* user_data = &node.get_user_data<GTriangleType>();

                for (size_t j = 0; j < item_count; ++j)
                {
                    const size_t triangle_index = triangle_indices[item_begin + j];
                    m_triangle_keys.push_back(triangle_keys[triangle_index]);
                    user_data[j] = triangles[triangle_index];
                }
            }
        }
    }

    RENDERER_LOG_DEBUG(
        "fat triangle tree leaves: %s",
        pretty_percent(fat_leaf_count, leaf_count).c_str());
}


//
// TriangleTreeFactory class implementation.
//

TriangleTreeFactory::TriangleTreeFactory(const TriangleTree::Arguments& arguments)
  : m_arguments(arguments)
{
}

auto_ptr<TriangleTree> TriangleTreeFactory::create()
{
    return auto_ptr<TriangleTree>(new TriangleTree(m_arguments));
}

}   // namespace renderer
