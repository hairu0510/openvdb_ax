///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2015-2019 DNEG
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DNEG nor the names
// of its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////

#include "VolumeExecutable.h"

#include <openvdb_ax/Exceptions.h>

// @TODO refactor so we don't have to include VolumeComputeGenerator.h,
// but still have the functions defined in one place
#include <openvdb_ax/codegen/VolumeComputeGenerator.h>

#include <openvdb/Exceptions.h>
#include <openvdb/tree/LeafManager.h>
#include <openvdb/Types.h>
#include <openvdb/math/Coord.h>
#include <openvdb/math/Transform.h>
#include <openvdb/math/Vec3.h>
#include <openvdb/tree/ValueAccessor.h>
#include <openvdb/tree/LeafManager.h>

#include <tbb/parallel_for.h>

#include <memory>

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {

namespace ax {

namespace {

/// @brief Volume Kernel types
///
using KernelFunctionPtr = std::add_pointer<codegen::VolumeKernel::Signature>::type;
using FunctionTraitsT = codegen::VolumeKernel::FunctionTraitsT;
using ReturnT = FunctionTraitsT::ReturnType;


/// The arguments of the generated function
struct VolumeFunctionArguments
{
    struct Accessors
    {
        using UniquePtr = std::unique_ptr<Accessors>;
        virtual ~Accessors() = default;
    };

    template <typename TreeT>
    struct TypedAccessor final : public Accessors
    {
        using UniquePtr = std::unique_ptr<TypedAccessor<TreeT>>;

        ~TypedAccessor() override final = default;

        inline void*
        init(TreeT& tree) {
            mAccessor.reset(new tree::ValueAccessor<TreeT>(tree));
            return static_cast<void*>(mAccessor.get());
        }

        std::unique_ptr<tree::ValueAccessor<TreeT>> mAccessor;
    };


    ///////////////////////////////////////////////////////////////////////
    ///////////////////////////////////////////////////////////////////////


    VolumeFunctionArguments(const CustomData::ConstPtr customData)
        : mCustomData(customData)
        , mCoord()
        , mCoordWS()
        , mVoidAccessors()
        , mAccessors()
        , mVoidTransforms() {}

    /// @brief  Given a built version of the function signature, automatically
    ///         bind the current arguments and return a callable function
    ///         which takes no arguments
    ///
    /// @param  function  The fully generated function built from the
    ///                   VolumeComputeGenerator
    ///
    inline std::function<ReturnT()>
    bind(KernelFunctionPtr function)
    {
        return std::bind(function,
            static_cast<FunctionTraitsT::Arg<0>::Type>(mCustomData.get()),
            reinterpret_cast<FunctionTraitsT::Arg<1>::Type>(mCoord.data()),
            reinterpret_cast<FunctionTraitsT::Arg<2>::Type>(mCoordWS.asV()),
            static_cast<FunctionTraitsT::Arg<3>::Type>(mVoidAccessors.data()),
            static_cast<FunctionTraitsT::Arg<4>::Type>(mVoidTransforms.data()));
    }

    template <typename TreeT>
    inline void
    addAccessor(TreeT& tree)
    {
        typename TypedAccessor<TreeT>::UniquePtr accessor(new TypedAccessor<TreeT>());
        mVoidAccessors.emplace_back(accessor->init(tree));
        mAccessors.emplace_back(std::move(accessor));
    }

    template <typename TreeT>
    inline void
    addConstAccessor(const TreeT& tree)
    {
        typename TypedAccessor<const TreeT>::UniquePtr accessor(new TypedAccessor<const TreeT>());
        mVoidAccessors.emplace_back(accessor->init(tree));
        mAccessors.emplace_back(std::move(accessor));
    }

    inline void
    addTransform(math::Transform::Ptr transform)
    {
        mVoidTransforms.emplace_back(static_cast<void*>(transform.get()));
    }

    const CustomData::ConstPtr mCustomData;
    openvdb::Coord mCoord;
    openvdb::math::Vec3<float> mCoordWS;

private:
    std::vector<void*> mVoidAccessors;
    std::vector<Accessors::UniquePtr> mAccessors;
    std::vector<void*> mVoidTransforms;
};

template <typename ValueType>
inline void
retrieveAccessorTyped(VolumeFunctionArguments& args,
                      openvdb::GridBase::Ptr grid)
{
    using GridType = typename openvdb::BoolGrid::ValueConverter<ValueType>::Type;
    typename GridType::Ptr typed = openvdb::StaticPtrCast<GridType>(grid);
    args.addAccessor(typed->tree());
}

inline void
retrieveAccessor(VolumeFunctionArguments& args,
                 const openvdb::GridBase::Ptr grid,
                 const std::string& valueType)
{
    if (valueType == typeNameAsString<bool>())                      retrieveAccessorTyped<bool>(args, grid);
    else if (valueType == typeNameAsString<int16_t>())              retrieveAccessorTyped<int16_t>(args, grid);
    else if (valueType == typeNameAsString<int32_t>())              retrieveAccessorTyped<int32_t>(args, grid);
    else if (valueType == typeNameAsString<int64_t>())              retrieveAccessorTyped<int64_t>(args, grid);
    else if (valueType == typeNameAsString<float>())                retrieveAccessorTyped<float>(args, grid);
    else if (valueType == typeNameAsString<double>())               retrieveAccessorTyped<double>(args, grid);
    else if (valueType == typeNameAsString<math::Vec3<int32_t>>())  retrieveAccessorTyped<math::Vec3<int32_t>>(args, grid);
    else if (valueType == typeNameAsString<math::Vec3<float>>())    retrieveAccessorTyped<math::Vec3<float>>(args, grid);
    else if (valueType == typeNameAsString<math::Vec3<double>>())   retrieveAccessorTyped<math::Vec3<double>>(args, grid);
    else {
        OPENVDB_THROW(TypeError, "Could not retrieve attribute '" + grid->getName()
            + "' as it has an unknown value type '" + valueType + "'");
    }
}

template <typename TreeT>
struct VolumeExecuterOp
{
    using LeafManagerT = typename tree::LeafManager<TreeT>;

    VolumeExecuterOp(const VolumeRegistry& volumeRegistry,
                     const CustomData::ConstPtr& customData,
                     const math::Transform& assignedVolumeTransform,
                     KernelFunctionPtr computeFunction,
                     openvdb::GridPtrVec& grids)
        : mVolumeRegistry(volumeRegistry)
        , mCustomData(customData)
        , mComputeFunction(computeFunction)
        , mGrids(grids)
        , mTargetVolumeTransform(assignedVolumeTransform) {
            assert(!mGrids.empty());
        }

    void operator()(const typename LeafManagerT::LeafRange& range) const
    {
        VolumeFunctionArguments args(mCustomData);

        size_t location(0);
        for (const auto& iter : mVolumeRegistry.volumeData()) {
            retrieveAccessor(args, mGrids[location], iter.mType);
            args.addTransform(mGrids[location]->transformPtr());
            ++location;
        }

        for (auto leaf = range.begin(); leaf; ++leaf) {
            for (auto voxel = leaf->cbeginValueOn(); voxel; ++voxel) {
                args.mCoord = voxel.getCoord();
                args.mCoordWS = mTargetVolumeTransform.indexToWorld(args.mCoord);
                args.bind(mComputeFunction)();
            }
        }
    }

private:
    const VolumeRegistry&       mVolumeRegistry;
    const CustomData::ConstPtr  mCustomData;
    KernelFunctionPtr           mComputeFunction;
    const openvdb::GridPtrVec&  mGrids;
    const math::Transform&      mTargetVolumeTransform;
};

void registerVolumes(const GridPtrVec &grids, GridPtrVec &writeableGrids, GridPtrVec &usableGrids,
                     const VolumeRegistry::VolumeDataVec& volumeData)
{
    for (auto& iter : volumeData) {

        openvdb::GridBase::Ptr matchedGrid;
        bool matchedName(false);
        for (const auto grid : grids) {
            if (grid->getName() != iter.mName) continue;
            matchedName = true;
            if (grid->valueType() != iter.mType) continue;
            matchedGrid = grid;
            break;
        }

        if (!matchedName && !matchedGrid) {
            OPENVDB_THROW(LookupError, "Missing grid \"@" + iter.mName + "\".");
        }

        if (matchedName && !matchedGrid) {
            OPENVDB_THROW(TypeError, "Mismatching grid access type. \"@" + iter.mName +
                "\" exists but has been accessed with type \"" + iter.mType + "\".");
        }

        assert(matchedGrid);
        usableGrids.push_back(matchedGrid);

        if (iter.mWriteable) {
            writeableGrids.push_back(matchedGrid);
        }
    }
}

} // anonymous namespace

void VolumeExecutable::execute(const openvdb::GridPtrVec& grids) const
{
    openvdb::GridPtrVec usableGrids, writeableGrids;

    registerVolumes(grids, writeableGrids, usableGrids, mVolumeRegistry->volumeData());

    const int numBlocks = mBlockFunctionAddresses.size();

    for (int i = 0; i < numBlocks; i++) {

        const std::map<std::string, uint64_t>& blockFunctions = mBlockFunctionAddresses.at(i);

        const std::string funcName(codegen::VolumeKernel::getDefaultName() + std::to_string(i));
        auto iter = blockFunctions.find(funcName);

        KernelFunctionPtr compute = nullptr;
        if (iter != blockFunctions.cend() && (iter->second != uint64_t(0))) {
            compute = reinterpret_cast<KernelFunctionPtr>(iter->second);
        }

        if (!compute) {
            OPENVDB_THROW(AXCompilerError, "No code has been successfully compiled for execution.");
        }

        const std::string& currentVolumeAssigned = mAssignedVolumes[i];
        math::Transform::ConstPtr writeTransform = nullptr;

        // pointer to the grid which is being written to in the current block
        openvdb::GridBase::Ptr gridToModify = nullptr;

        for (const auto& grid : writeableGrids) {
            if (grid->getName() == currentVolumeAssigned) {
                writeTransform = grid->transformPtr();
                gridToModify = grid;
                break;
            }
        }

        // We execute over the topology of the grid currently being modified.  To do this, we need
        // a typed tree and leaf manager

        if (gridToModify->isType<BoolGrid>()) {
            BoolGrid::Ptr typed = StaticPtrCast<BoolGrid>(gridToModify);
            tree::LeafManager<BoolTree> leafManager(typed->tree());
            VolumeExecuterOp<BoolTree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<Int32Grid>()) {
            Int32Grid::Ptr typed = StaticPtrCast<Int32Grid>(gridToModify);
            tree::LeafManager<Int32Tree> leafManager(typed->tree());
            VolumeExecuterOp<Int32Tree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                 compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<Int64Grid>()) {
            Int64Grid::Ptr typed = StaticPtrCast<Int64Grid>(gridToModify);
            tree::LeafManager<Int64Tree> leafManager(typed->tree());
            VolumeExecuterOp<Int64Tree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<FloatGrid>()) {
            FloatGrid::Ptr typed = StaticPtrCast<FloatGrid>(gridToModify);
            tree::LeafManager<FloatTree> leafManager(typed->tree());
            VolumeExecuterOp<FloatTree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<DoubleGrid>()) {
            DoubleGrid::Ptr typed = StaticPtrCast<DoubleGrid>(gridToModify);
            tree::LeafManager<DoubleTree> leafManager(typed->tree());
            VolumeExecuterOp<DoubleTree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<Vec3IGrid>()) {
            Vec3IGrid::Ptr typed = StaticPtrCast<Vec3IGrid>(gridToModify);
            tree::LeafManager<Vec3ITree> leafManager(typed->tree());
            VolumeExecuterOp<Vec3ITree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<Vec3fGrid>()) {
            Vec3fGrid::Ptr typed = StaticPtrCast<Vec3fGrid>(gridToModify);
            tree::LeafManager<Vec3fTree> leafManager(typed->tree());
            VolumeExecuterOp<Vec3fTree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<Vec3dGrid>()) {
            Vec3dGrid::Ptr typed = StaticPtrCast<Vec3dGrid>(gridToModify);
            tree::LeafManager<Vec3dTree> leafManager(typed->tree());
            VolumeExecuterOp<Vec3dTree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else if (gridToModify->isType<MaskGrid>()) {
            MaskGrid::Ptr typed = StaticPtrCast<MaskGrid>(gridToModify);
            tree::LeafManager<MaskTree> leafManager(typed->tree());
            VolumeExecuterOp<MaskTree> executerOp(*mVolumeRegistry, mCustomData, *writeTransform,
                compute, usableGrids);
            tbb::parallel_for(leafManager.leafRange(), executerOp);
        }
        else {
            OPENVDB_THROW(TypeError, "Could not retrieve volume '" + gridToModify->getName()
                                     + "' as it has an unknown value type");
        }
    }
}

}
}
}

// Copyright (c) 2015-2019 DNEG
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
