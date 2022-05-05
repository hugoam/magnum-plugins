/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022 Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "GltfSceneConverter.h"

#include <unordered_map>
#include <Corrade/Containers/ArrayTuple.h>
#include <Corrade/Containers/ArrayViewStl.h> /** @todo drop once Configuration is STL-free */
#include <Corrade/Containers/GrowableArray.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/Containers/Triple.h>
#include <Corrade/Containers/ScopeGuard.h>
#include <Corrade/Utility/Algorithms.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/Format.h>
#include <Corrade/Utility/JsonWriter.h>
#include <Corrade/Utility/Path.h>
#include <Corrade/Utility/String.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/Math/PackingBatch.h>
#include <Magnum/Math/Quaternion.h>
#include <Magnum/Trade/ArrayAllocator.h>
#include <Magnum/Trade/MeshData.h>
#include <Magnum/Trade/SceneData.h>

#include "MagnumPlugins/GltfImporter/Gltf.h"

/* We'd have to endian-flip everything that goes into buffers, plus the binary
   glTF headers, etc. Too much work, hard to automatically test because the
   HW is hard to get. */
#ifdef CORRADE_TARGET_BIG_ENDIAN
#error this code will not work on Big Endian, sorry
#endif

namespace Magnum { namespace Trade {

struct GltfSceneConverter::State {
    /* Empty if saving to data. Storing the full filename and not just the path
       in order to know how to name the external buffer file. */
    Containers::Optional<Containers::StringView> filename;
    /* Custom mesh attribute names */
    Containers::Array<Containers::Pair<UnsignedShort, Containers::String>> customMeshAttributes;
    /* Object names */
    Containers::Array<Containers::String> objectNames;
    /* Scene field names */
    std::unordered_map<UnsignedInt, Containers::String> sceneFieldNames;

    /* Output format. Defaults for a binary output. */
    bool binary = true;
    Utility::JsonWriter::Options jsonOptions;
    UnsignedInt jsonIndentation = 0;

    /* Extensions required based on data added */
    bool requireKhrMeshQuantization = false;

    Utility::JsonWriter gltfBuffers;
    Utility::JsonWriter gltfBufferViews;
    Utility::JsonWriter gltfAccessors;
    Utility::JsonWriter gltfMeshes;
    Utility::JsonWriter gltfNodes;
    Utility::JsonWriter gltfScenes;

    Containers::Array<char> buffer;
};

using namespace Containers::Literals;

GltfSceneConverter::GltfSceneConverter(PluginManager::AbstractManager& manager, const Containers::StringView& plugin): AbstractSceneConverter{manager, plugin} {}

GltfSceneConverter::~GltfSceneConverter() = default;

SceneConverterFeatures GltfSceneConverter::doFeatures() const {
    return SceneConverterFeature::ConvertMultipleToData|
           SceneConverterFeature::AddMeshes|
           SceneConverterFeature::AddScenes;
}

void GltfSceneConverter::doBeginFile(const Containers::StringView filename) {
    CORRADE_INTERNAL_ASSERT(!_state);
    _state.emplace();
    _state->filename = filename;

    /* Decide if we're writing a text or a binary file */
    if(!configuration().value<Containers::StringView>("binary")) {
        _state->binary = Utility::String::lowercase(Utility::Path::splitExtension(filename).second()) != ".gltf"_s;
    } else _state->binary = configuration().value<bool>("binary");

    AbstractSceneConverter::doBeginFile(filename);
}

void GltfSceneConverter::doBeginData() {
    /* If the state is already there, it's from doBeginFile(). Otherwise create
       a new one. */
    if(!_state) {
        _state.emplace();

        /* Binary is the default for data output because we can't write
           external files. Override if the configuration is non-empty. */
        if(!configuration().value<Containers::StringView>("binary"))
            _state->binary = true;
        else
            _state->binary = configuration().value<bool>("binary");
    }

    /* Text file is pretty-printed according to options. For a binary file the
       defaults are already alright.  */
    if(!_state->binary) {
        _state->jsonOptions = Utility::JsonWriter::Option::Wrap|Utility::JsonWriter::Option::TypographicalSpace;
        _state->jsonIndentation = 2;

        /* Update the JSON writers with desired options. These will be inside
           the top-level object, so need one level of initial indentation. */
        for(Utility::JsonWriter* const writer: {
            &_state->gltfBuffers,
            &_state->gltfBufferViews,
            &_state->gltfAccessors,
            &_state->gltfMeshes,
            &_state->gltfNodes,
            &_state->gltfScenes
        })
            *writer = Utility::JsonWriter{_state->jsonOptions, _state->jsonIndentation, _state->jsonIndentation*1};
    }
}

Containers::Optional<Containers::Array<char>> GltfSceneConverter::doEndData() {
    Utility::JsonWriter json{_state->jsonOptions, _state->jsonIndentation};
    json.beginObject();

    /* Asset object, always present */
    {
        json.writeKey("asset"_s);
        Containers::ScopeGuard gltfAsset = json.beginObjectScope();

        json.writeKey("version"_s).write("2.0"_s);

        if(const Containers::StringView copyright = configuration().value<Containers::StringView>("copyright"_s))
            json.writeKey("copyright"_s).write(copyright);
        if(const Containers::StringView generator = configuration().value<Containers::StringView>("generator"_s))
            json.writeKey("generator"_s).write(generator);
    }

    /* Used and required extensions */
    {
        /** @todo FFS what the stone age types here */
        std::vector<Containers::StringView> extensionsUsed = configuration().values<Containers::StringView>("extensionUsed");
        std::vector<Containers::StringView> extensionsRequired = configuration().values<Containers::StringView>("extensionRequired");

        const auto contains = [](Containers::ArrayView<const Containers::StringView> extensions, Containers::StringView extension) {
            for(const Containers::StringView i: extensions)
                if(i == extension) return true;
            return false;
        };
        if(_state->requireKhrMeshQuantization) {
            if(!contains(extensionsUsed, "KHR_mesh_quantization"_s))
                extensionsUsed.push_back("KHR_mesh_quantization"_s);
            if(!contains(extensionsRequired, "KHR_mesh_quantization"_s))
                extensionsRequired.push_back("KHR_mesh_quantization"_s);
        }

        if(!extensionsUsed.empty()) {
            json.writeKey("extensionsUsed"_s);
            Containers::ScopeGuard gltfExtensionsUsed = json.beginArrayScope();
            for(const Containers::StringView i: extensionsUsed) json.write(i);
        }
        if(!extensionsRequired.empty()) {
            json.writeKey("extensionsRequired"_s);
            Containers::ScopeGuard gltfExtensionsRequired = json.beginArrayScope();
            for(const Containers::StringView i: extensionsRequired) json.write(i);
        }
    }

    /* Wrap up the buffer if it's non-empty or if there are any (empty) buffer
       views referencing it */
    if(!_state->buffer.isEmpty() || !_state->gltfBufferViews.isEmpty()) {
        json.writeKey("buffers");
        Containers::ScopeGuard gltfBuffers = json.beginArrayScope();
        Containers::ScopeGuard gltfBuffer = json.beginObjectScope();

        /* If not writing a binary glTF and the buffer is non-empty, save the
           buffer to an external file and reference it. In a binary glTF the
           buffer is just one with an implicit location. */
        if(!_state->binary && !_state->buffer.isEmpty()) {
            if(!_state->filename) {
                Error{} << "Trade::GltfSceneConverter::endData(): can only write a glTF with external buffers if converting to a file";
                return {};
            }

            Containers::String bufferFilename = Utility::Path::splitExtension(*_state->filename).first() + ".bin"_s;
            Utility::Path::write(bufferFilename, _state->buffer);
            /** @todo configurable buffer name? or a path prefix if ending with /?
                or an extension alone if .. what, exactly? */

            /* Writing just the filename as the two files are expected to be
               next to each other */
            json.writeKey("uri").write(Utility::Path::split(bufferFilename).second());
        }

        json.writeKey("byteLength").write(_state->buffer.size());
    }

    /* Accessors, buffer views, meshes. If there are any, the array is left
       open -- close it and put the whole JSON into the file */
    if(!_state->gltfBufferViews.isEmpty())
        json.writeKey("bufferViews").writeJson(_state->gltfBufferViews.endArray().toString());
    if(!_state->gltfAccessors.isEmpty())
        json.writeKey("accessors").writeJson(_state->gltfAccessors.endArray().toString());
    if(!_state->gltfMeshes.isEmpty())
        json.writeKey("meshes").writeJson(_state->gltfMeshes.endArray().toString());

    /* Nodes and scenes, those got written all at once in
       doAdd(const SceneData&) so no need to close anything */
    if(!_state->gltfNodes.isEmpty())
        json.writeKey("nodes").writeJson(_state->gltfNodes.toString());
    if(!_state->gltfScenes.isEmpty()) {
        json.writeKey("scenes").writeJson(_state->gltfScenes.toString());
        /* Currently there's at most one scene, so make it a default */
        json.writeKey("scene").write(0);
    }

    /* Done! */
    json.endObject();

    union CharCaster {
        UnsignedInt value;
        const char data[4];
    };

    /* Reserve the output array and write headers for a binary glTF */
    Containers::Array<char> out;
    if(_state->binary) {
        const std::size_t totalSize = 12 + /* file header */
            8 + json.size() + /* JSON chunk + header */
            (_state->buffer.isEmpty() ? 0 :
                8 + _state->buffer.size()); /* BIN chunk + header */
        Containers::arrayReserve<ArrayAllocator>(out, totalSize);
        /** @todo check for JSON and buffer sizes >4GB and fail, suggest using
            external buffers / "text" glTF */
        // TODO option to not store the buffer inside a glb for this very
        //  reason?

        /* glTF header */
        Containers::arrayAppend<ArrayAllocator>(out,
            Containers::ArrayView<const char>{"glTF\x02\x00\x00\x00"_s});
        /** @todo WTF the casts here */
        Containers::arrayAppend<ArrayAllocator>(out,
            Containers::arrayView(CharCaster{UnsignedInt(totalSize)}.data));

        /* JSON chunk header */
        /** @todo WTF the cast here */
        Containers::arrayAppend<ArrayAllocator>(out,
            Containers::arrayView(CharCaster{UnsignedInt(json.size())}.data));
        Containers::arrayAppend<ArrayAllocator>(out, {'J', 'S', 'O', 'N'});

    /* Otherwise reserve just for the JSON */
    } else Containers::arrayReserve<ArrayAllocator>(out, json.size());

    /* Copy the JSON data to the output. In case of a text glTF we would
       ideally just pass the memory from the JsonWriter but the class uses an
       arbitrary growable deleter internally and custom deleters are forbidden
       in plugins. */
    /** @todo make it possible to specify an external allocator in JsonWriter
        once allocators-as-arguments are a thing */
    /** @todo WTF the casts here */
    Containers::arrayAppend<ArrayAllocator>(out, Containers::ArrayView<const char>(json.toString()));

    /* Add the buffer as a second BIN chunk for a binary glTF */
    if(_state->binary && !_state->buffer.isEmpty()) {
        /** @todo WTF the cast here */
        Containers::arrayAppend<ArrayAllocator>(out,
            Containers::arrayView(CharCaster{UnsignedInt(_state->buffer.size())}.data));
        Containers::arrayAppend<ArrayAllocator>(out, {'B', 'I', 'N', '\0'});
        /** @todo WTF the casts here */
        Containers::arrayAppend<ArrayAllocator>(out, Containers::ArrayView<const char>(_state->buffer));
    }

    /* GCC 4.8 and Clang 3.8 need extra help here */
    return Containers::optional(std::move(out));
}

void GltfSceneConverter::doAbort() {
    _state = {};
}

void GltfSceneConverter::doSetObjectName(const UnsignedLong object, const Containers::StringView name) {
    if(_state->objectNames.size() <= object)
        arrayResize(_state->objectNames, object + 1);
    _state->objectNames[object] = Containers::String::nullTerminatedGlobalView(name);
}

void GltfSceneConverter::doSetSceneFieldName(const UnsignedInt field, const Containers::StringView name) {
    _state->sceneFieldNames[field] = Containers::String::nullTerminatedGlobalView(name);
}

bool GltfSceneConverter::doAdd(const UnsignedInt id, const SceneData& scene, const Containers::StringView name) {
    if(!scene.is3D()) {
        Error{} << "Trade::GltfSceneConverter::add(): expected a 3D scene";
        return {};
    }

    /* Calculate count of field assignments for each object. Initially shifted
       by two values, `objectFieldOffsets[i + 2]` is the count of fields for
       object `i`. */
    Containers::Array<std::size_t> objectFieldOffsets{ValueInit, std::size_t(scene.mappingBound() + 2)};
    Containers::Array<UnsignedInt> mappingStorage{NoInit, std::size_t(scene.mappingBound())};
    for(UnsignedInt i = 0, iMax = scene.fieldCount(); i != iMax; ++i) {
        const SceneField name = scene.fieldName(i);

        /* Skip fields that are treated differently */
        if(
            /* Parents are converted to a child list instead -- a presence of
               a parent field doesn't say anything about given object having
               any children */
            name == SceneField::Parent ||
            /* Materials are tied to the Mesh field -- if Mesh exists,
               Materials have the exact same mapping, thus there's no point in
               counting them separately */
            name == SceneField::MeshMaterial
        ) continue;

        /* Custom fields */
        if(isSceneFieldCustom(name)) {
            /* Skip ones for which we don't have a name */
            const auto found = _state->sceneFieldNames.find(sceneFieldCustom(name));
            if(found == _state->sceneFieldNames.end()) {
                Warning{} << "Trade::GltfSceneConverter::add(): custom scene field" << sceneFieldCustom(name) << "has no name assigned, skipping";
                continue;
            }

            /* Allow only scalar numbers for now */
            // TODO for vectors we'd have to use += size instead of
            //  ++objectFieldOffsets below, have different output arrays for
            //  numbers, bools, enums and strings, ... Too much trouble ATM.
            const SceneFieldType type = scene.fieldType(i);
            if(type != SceneFieldType::UnsignedInt &&
               type != SceneFieldType::Int &&
               type != SceneFieldType::Float) {
                Warning{} << "Trade::GltfSceneConverter::add(): custom field" << found->second << "has unsupported type" << type << Debug::nospace << ", skipping";
                continue;
            }
        }

        const std::size_t fieldSize = scene.fieldSize(i);
        const Containers::ArrayView<UnsignedInt> mapping = mappingStorage.prefix(fieldSize);
        scene.mappingInto(i, mapping);
        for(std::size_t j = 0, jMax = fieldSize; j != jMax; ++j) {
            if(mapping[j] >= scene.mappingBound()) {
                Error{} << "Trade::GltfSceneConverter::add(): scene field" << i << "mapping" << mapping[j] << "out of bounds for" << scene.mappingBound() << "objects";
                return {};
            }

            ++objectFieldOffsets[mapping[j] + 2];
        }
    }

    /* Turn that into an offset array. This makes it shifted by just one value,
       so `objectFieldOffsets[i + 2] - objectFieldOffsets[i + 1]` is the count
       of fields for object `i`. */
    std::size_t totalFieldCount = 0;
    for(std::size_t& i: objectFieldOffsets) {
        const std::size_t count = i;
        i += totalFieldCount;
        totalFieldCount += count;
    }

    /* Arrays containing field IDs and offsets. This makes `objectFieldOffsets`
       finally unshifted, so `fieldIds[objectFieldOffsets[i]]` to
       `fieldIds[objectFieldOffsets[i + 1]]` contains field IDs for object `i`,
       same with `offsets`. */
    Containers::Array<UnsignedInt> fieldIds{NoInit, totalFieldCount};
    Containers::Array<std::size_t> fieldOffsets{NoInit, totalFieldCount};
    std::size_t customFieldOffset = 0;
    for(UnsignedInt i = 0, iMax = scene.fieldCount(); i != iMax; ++i) {
        /* Same as in the previous loop */
        // TODO use a bitfield ffs, too error prone
        if(scene.fieldName(i) == SceneField::Parent ||
           scene.fieldName(i) == SceneField::MeshMaterial) continue;
        const SceneField name = scene.fieldName(i);
        if(isSceneFieldCustom(name)) {
            const SceneFieldType type = scene.fieldType(i);
            if(_state->sceneFieldNames.find(sceneFieldCustom(name)) == _state->sceneFieldNames.end() ||
               (type != SceneFieldType::UnsignedInt &&
                type != SceneFieldType::Int &&
                type != SceneFieldType::Float)) continue;
        }

        /* As we put all custom fields into a single float/int array, the
           offset needs to also include sizes of all previous custom fields
           already written */
        // TODO when we support more than just numeric fields, this probably
        //  needs to branch on field type
        const std::size_t baseOffset = isSceneFieldCustom(name) ? customFieldOffset : 0;

        const std::size_t fieldSize = scene.fieldSize(i);
        const Containers::ArrayView<UnsignedInt> mapping = mappingStorage.prefix(fieldSize);
        scene.mappingInto(i, mapping); // TODO do this just once?
        for(std::size_t j = 0; j != fieldSize; ++j) {
            std::size_t& objectFieldOffset = objectFieldOffsets[mapping[j] + 1];
            fieldIds[objectFieldOffset] = i;
            fieldOffsets[objectFieldOffset] = baseOffset + j;
            ++objectFieldOffset;
        }

        if(isSceneFieldCustom(name))
            customFieldOffset += fieldSize;
    }
    CORRADE_INTERNAL_ASSERT(
        objectFieldOffsets[0] == 0 &&
        objectFieldOffsets[objectFieldOffsets.size() - 1] == totalFieldCount &&
        objectFieldOffsets[objectFieldOffsets.size() - 2] == totalFieldCount);

    /* Retrieve sizes of builtin fields in known types */
    std::size_t parentCount = 0;
    // TODO how to represent parent-less nodes? or just add them w/o
    //  referencing from a scene? well that wouldn't be *a scene*, would it
    // TODO should ignore parent-less nodes (needs a bitfield, again) ... tho
    //  those are what is used for an "instance-less" scene, sigh, what to do?
    //  WHAT TO DO? (or just export them but then during import put them into a
    //  dedicated "leftover" scene that won't have a parent field?)
    std::size_t transformationCount = 0;
    std::size_t trsCount = 0;
    bool hasTranslation = false;
    bool hasRotation = false;
    bool hasScaling = false;
    std::size_t meshMaterialCount = 0;
    bool hasMaterial = false;
    std::size_t lightCount = 0;
    std::size_t cameraCount = 0;
    std::size_t skinCount = 0;
    for(UnsignedInt i = 0; i != scene.fieldCount(); ++i) {
        const std::size_t size = scene.fieldSize(i);
        switch(scene.fieldName(i)) {
            case SceneField::Parent:
                parentCount = size;
                continue;
            case SceneField::Transformation:
                transformationCount = size;
                continue;
            case SceneField::Translation:
                hasTranslation = true;
                trsCount = size;
                continue;
            case SceneField::Rotation:
                hasRotation = true;
                trsCount = size;
                continue;
            case SceneField::Scaling:
                hasScaling = true;
                trsCount = size;
                continue;
            case SceneField::Mesh:
                meshMaterialCount = size;
                continue;
            case SceneField::MeshMaterial:
                /* Used only in combination with a mesh, alone it doesn't
                   contribute to meshMaterialCount */
                hasMaterial = true;
                continue;
            case SceneField::Light:
                lightCount = size;
                continue;
            case SceneField::Camera:
                cameraCount = size;
                continue;
            case SceneField::Skin:
                skinCount = size;
                continue;

            /* ImporterState is ignored, it makes no sense to save a pointer
               value */
            case SceneField::ImporterState:
                continue;
        }
    }

    /* Allocate and populate them */
    Containers::ArrayView<Int> parents;
    Containers::ArrayView<UnsignedInt> childOffsets;
    Containers::ArrayView<UnsignedInt> children;
    Containers::ArrayView<Matrix4> transformations;
    Containers::ArrayView<Vector3> translations;
    Containers::ArrayView<Quaternion> rotations;
    Containers::ArrayView<Vector3> scalings;
    Containers::ArrayView<UnsignedInt> meshes;
    Containers::ArrayView<Int> meshMaterials;
    Containers::ArrayView<UnsignedInt> lights;
    Containers::ArrayView<UnsignedInt> cameras;
    Containers::ArrayView<UnsignedInt> skins;
    Containers::ArrayView<Float> customFields;
    Containers::ArrayTuple fieldStorage{
        {NoInit, parentCount, parents},
        /* The first element is 0, the second is root object count */
        {ValueInit, scene.mappingBound() + 2, childOffsets},
        {NoInit, parentCount, children},
        {NoInit, transformationCount, transformations},
        {NoInit, hasTranslation ? trsCount : 0, translations},
        {NoInit, hasRotation ? trsCount : 0, rotations},
        {NoInit, hasScaling ? trsCount : 0, scalings},
        {NoInit, meshMaterialCount, meshes},
        {NoInit, hasMaterial ? meshMaterialCount : 0, meshMaterials},
        {NoInit, lightCount, lights},
        {NoInit, cameraCount, cameras},
        {NoInit, skinCount, skins},
        {NoInit, customFieldOffset, customFields}
    };

    /* Parent pointers, convert that to a child list */
    if(parentCount) {
        const Containers::ArrayView<UnsignedInt> parentMapping = mappingStorage.prefix(parentCount);
        scene.parentsInto(parentMapping, parents);

        /* Calculate count of children for every object. Initially shifted
           by two values, `childOffsets[i + 2]` is the count of children for
           object `i`, `childOffsets[1]` is the count of root objects,
           `childOffsets[0]` is 0. */
        for(std::size_t i = 0; i != parents.size(); ++i) {
            Int parent = parents[i];
            if(parent != -1 && UnsignedInt(parent) >= scene.mappingBound()) {
                Error{} << "Trade::GltfSceneConverter::add(): scene parent reference" << parent << "out of bounds for" << scene.mappingBound() << "objects";
                return {};
            }

            ++childOffsets[parent + 2];
        }

        // TODO cycle detection (the same object multiple times in the mapping)
        //  needs a bitfield

        /* Turn that into an offset array. This makes it shifted by just one
           value, so `childOffsets[i + 2] - childOffsets[i + 1]` is the count
           of children for object `i`; `childOffsets[1]` is the count of
           root objects, `childOffsets[0]` is 0. */
        std::size_t offset = 0;
        for(UnsignedInt& i: childOffsets) {
            const std::size_t count = i;
            i += offset;
            offset += count;
        }
        CORRADE_INTERNAL_ASSERT(offset == parents.size());

        /* Populate the child array. This makes `childOffsets` finally
           unshifted, so `children[childOffsets[i]]` to
           `children[childOffsets[i + 1]]` contains children of object `i`;
           `children[0]` until `childOffsets[i]` contains root objects. */
        for(std::size_t i = 0; i != parents.size(); ++i) {
            const UnsignedInt object = parentMapping[i];
            if(object >= scene.mappingBound()) {
                Error{} << "Trade::GltfSceneConverter::add(): scene parent mapping" << object << "out of bounds for" << scene.mappingBound() << "objects";
                return {};
            }

            children[childOffsets[parents[i] + 1]++] = object;
        }
        CORRADE_INTERNAL_ASSERT(
            childOffsets[childOffsets.size() - 1] == parentCount &&
            childOffsets[childOffsets.size() - 2] == parentCount);
    }

    /* Other builtin fields */
    if(transformationCount)
        scene.transformations3DInto(nullptr, transformations);
    if(trsCount)
        scene.translationsRotationsScalings3DInto(nullptr,
            hasTranslation ? translations : nullptr,
            hasRotation ? rotations : nullptr,
            hasScaling ? scalings : nullptr);
    if(meshMaterialCount)
        scene.meshesMaterialsInto(nullptr, meshes,
            hasMaterial ? meshMaterials : nullptr);
    if(lightCount)
        scene.lightsInto(nullptr, lights);
    if(cameraCount)
        scene.camerasInto(nullptr, cameras);
    if(skinCount)
        scene.skinsInto(nullptr, skins);

    /* Custom fields */
    {
        std::size_t offset = 0;
        for(std::size_t i = 0; i != scene.fieldCount(); ++i) {
            // TODO USE A BITFIELD FFS, third time copying this crap
            const SceneField name = scene.fieldName(i);
            if(!isSceneFieldCustom(name) ||
            _state->sceneFieldNames.find(sceneFieldCustom(name)) == _state->sceneFieldNames.end()) continue;
            const SceneFieldType type = scene.fieldType(i);
            if(_state->sceneFieldNames.find(sceneFieldCustom(name)) == _state->sceneFieldNames.end() ||
               (type != SceneFieldType::UnsignedInt &&
                type != SceneFieldType::Int &&
                type != SceneFieldType::Float)) continue;

            const std::size_t size = scene.fieldSize(i);
            const Containers::ArrayView<Float> dst = customFields.slice(offset, offset + size);
            // TODO this won't work for more than 23 bits of precision, use
            //  ints directly FFS
            if(type == SceneFieldType::UnsignedInt) Math::castInto(
                Containers::arrayCast<2, const UnsignedInt>(scene.field<UnsignedInt>(i)),
                Containers::arrayCast<2, Float>(dst));
            else if(type == SceneFieldType::Int) Math::castInto(
                Containers::arrayCast<2, const Int>(scene.field<Int>(i)),
                Containers::arrayCast<2, Float>(dst));
            else if(type == SceneFieldType::Float)
                Utility::copy(scene.field<Float>(i), dst);
            else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */

            offset += size;
        }

        CORRADE_INTERNAL_ASSERT(offset == customFieldOffset);
    }

    /* Go object by object and consume the fields, populating the glTF node
       array */
    // TODO gracefully fail if adding more than one scene
    CORRADE_INTERNAL_ASSERT(_state->gltfNodes.isEmpty());
    Containers::ScopeGuard gltfNodes = _state->gltfNodes.beginArrayScope();
    for(UnsignedLong j = 0; j != scene.mappingBound(); ++j) {
        Containers::ScopeGuard gltfNode = _state->gltfNodes.beginObjectScope();

        /* Write the children array, if there's any */
        if(childOffsets[j + 1] - childOffsets[j]) {
            _state->gltfNodes.writeKey("children").writeArray(children.slice(childOffsets[j], childOffsets[j + 1]));
        }

        // TODO ensure that extra fields are always last so this works
        bool extrasOpen = false;

        SceneField previous{};
        for(std::size_t i = objectFieldOffsets[j], iMax = objectFieldOffsets[j + 1]; i != iMax; ++i) {
            const UnsignedInt id = fieldIds[i];
            const std::size_t offset = fieldOffsets[i];
            const SceneField name = scene.fieldName(id);
            if(name == previous) {
                // TODO for custom fields it could make sense to put them into arrays, but a multi-field should be detected in advance
                // TODO needs a bitfield
                Warning{} << "Trade::GltfSceneConverter::add(): ignoring duplicate field" << previous << "for node" << j;
                continue;
            }

            previous = name;

            if(name == SceneField::Transformation) {
                // TODO ignore if TRS is present, assume these are sufficient?
                // TODO needs a bitfield to know which objects have transform,
                //  which have TRS, and then writing transform for only those
                //  that are left in transform&~trs
                // TODO When a node is targeted for animation (referenced by an animation.channel.target), matrix MUST NOT be present.
                _state->gltfNodes.writeKey("matrix").writeArray(transformations[offset].data(), 4);
            } else if(name == SceneField::Translation) {
                if(translations[offset] != Vector3{})
                    _state->gltfNodes.writeKey("translation").writeArray(translations[offset].data());
            } else if(name == SceneField::Rotation) {
                if(rotations[offset] != Quaternion{})
                    /* glTF also uses the XYZW order */
                    _state->gltfNodes.writeKey("rotation").writeArray(rotations[offset].data());
            } else if(name == SceneField::Scaling) {
                if(translations[offset] != Vector3{1.0f})
                    _state->gltfNodes.writeKey("scale").writeArray(scalings[offset].data());
            } else if(name == SceneField::Mesh) {
                _state->gltfNodes.writeKey("mesh").write(meshes[offset]);
                // TODO material
                // TODO ffs!!! how dafuq, delay mesh adding altogether because F
                //  array of Mesh, which is: prim, attrib key/value list, indices
                // TODO ffs! what if same mesh different material?! F
            } else if(name == SceneField::Light) {
                _state->gltfNodes.writeKey("light").write(lights[offset]);
            } else if(name == SceneField::Camera) {
                _state->gltfNodes.writeKey("camera").write(cameras[offset]);
            } else if(name == SceneField::Skin) {
                _state->gltfNodes.writeKey("skin").write(skins[offset]);
            } else if(name == SceneField::Parent ||
                      name == SceneField::MeshMaterial) {
                /* Skipped when counting the fields, thus shouldn't appear
                   here */
                CORRADE_INTERNAL_ASSERT_UNREACHABLE();
            } else {
                CORRADE_INTERNAL_ASSERT(isSceneFieldCustom(name));

                if(!extrasOpen) {
                    _state->gltfNodes.writeKey("extras"_s).beginObject();
                    extrasOpen = true;
                }

                // TODO yes ofc it goes to extras; LATER
                const auto found = _state->sceneFieldNames.find(sceneFieldCustom(name));
                CORRADE_INTERNAL_ASSERT(found != _state->sceneFieldNames.end());
                _state->gltfNodes.writeKey(found->second).write(Double(customFields[offset]));
                // TODO ugh the double is a BAD workaround, fix properly
            }
        }

        if(extrasOpen) _state->gltfNodes.endObject();

        if(_state->objectNames.size() > j && _state->objectNames[j])
            _state->gltfNodes.writeKey("name").write(_state->objectNames[j]);
    }

    /* Scene object referencing the root children */
    CORRADE_INTERNAL_ASSERT(_state->gltfScenes.isEmpty());
    Containers::ScopeGuard gltfScenes = _state->gltfScenes.beginArrayScope();
    CORRADE_INTERNAL_ASSERT(_state->gltfScenes.currentArraySize() == id);
    Containers::ScopeGuard gltfScene = _state->gltfScenes.beginObjectScope();
    if(childOffsets[0]) {
        // TODO what if there are no root objects? happens when a SceneData has
        //  no parent info, e.g.
        _state->gltfScenes.writeKey("nodes").writeArray(children.prefix(childOffsets[0]));
    }

    // TODO _s for all strings!!
    if(name)
        _state->gltfScenes.writeKey("name"_s).write(name);

    return true;
}

void GltfSceneConverter::doSetMeshAttributeName(const UnsignedShort attribute, Containers::StringView name) {
    /* Replace the previous entry if already set */
    for(Containers::Pair<UnsignedShort, Containers::String>& i: _state->customMeshAttributes) {
        if(i.first() == attribute) {
            i.second() = Containers::String::nullTerminatedGlobalView(name);
            return;
        }
    }

    arrayAppend(_state->customMeshAttributes, InPlaceInit, attribute, Containers::String::nullTerminatedGlobalView(name));
}

bool GltfSceneConverter::doAdd(const UnsignedInt id, const MeshData& mesh, const Containers::StringView name) {
    /* Check and convert mesh primitive */
    /** @todo check primitive count according to the spec */
    Int gltfMode;
    switch(mesh.primitive()) {
        case MeshPrimitive::Points:
            gltfMode = Implementation::GltfModePoints;
            break;
        case MeshPrimitive::Lines:
            gltfMode = Implementation::GltfModeLines;
            break;
        case MeshPrimitive::LineLoop:
            gltfMode = Implementation::GltfModeLineLoop;
            break;
        case MeshPrimitive::LineStrip:
            gltfMode = Implementation::GltfModeLineStrip;
            break;
        case MeshPrimitive::Triangles:
            gltfMode = Implementation::GltfModeTriangles;
            break;
        case MeshPrimitive::TriangleStrip:
            gltfMode = Implementation::GltfModeTriangleStrip;
            break;
        case MeshPrimitive::TriangleFan:
            gltfMode = Implementation::GltfModeTriangleFan;
            break;
        default:
            Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh primitive" << mesh.primitive();
            return {};
    }

    /* Check and convert mesh index type */
    Int gltfIndexType;
    if(mesh.isIndexed()) {
        if(!mesh.indices().isContiguous()) {
            Error{} << "Trade::GltfSceneConverter::add(): non-contiguous mesh index arrays are not supported";
            return {};
        }
        switch(mesh.indexType()) {
            case MeshIndexType::UnsignedByte:
                gltfIndexType = Implementation::GltfTypeUnsignedByte;
                break;
            case MeshIndexType::UnsignedShort:
                gltfIndexType = Implementation::GltfTypeUnsignedShort;
                break;
            case MeshIndexType::UnsignedInt:
                gltfIndexType = Implementation::GltfTypeUnsignedInt;
                break;
            default:
                Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh index type" << mesh.indexType();
                return {};
        }
    }

    /* 3.7.2.1 (Geometry § Meshes § Overview) says "Primitives specify one or
       more attributes"; we allow this in non-strict mode */
    if(!mesh.attributeCount()) {
        /* The count is specified only in the accessors, if we have none we
           can't preserve that information. */
        if(mesh.vertexCount()) {
            Error{} << "Trade::GltfSceneConverter::add(): attribute-less mesh with a non-zero vertex count is unrepresentable in glTF";
            return {};
        }

        if(configuration().value<bool>("strict")) {
            Error{} << "Trade::GltfSceneConverter::add(): attribute-less meshes are not valid glTF, set strict=false to allow them";
            return {};
        } else Warning{} << "Trade::GltfSceneConverter::add(): strict mode disabled, allowing an attribute-less mesh";

    /* 3.7.2.1 (Geometry § Meshes § Overview) says "[count] MUST be non-zero";
       we allow this in non-strict mode. Attribute-less meshes in glTF
       implicitly have zero vertices, so don't warn twice in that case. */
    } else if(!mesh.vertexCount()) {
        if(configuration().value<bool>("strict")) {
            Error{} << "Trade::GltfSceneConverter::add(): meshes with zero vertices are not valid glTF, set strict=false to allow them";
            return {};
        } else Warning{} << "Trade::GltfSceneConverter::add(): strict mode disabled, allowing a mesh with zero vertices";
    }

    /* Check and convert attributes */
    /** @todo detect and merge interleaved attributes into common buffer views */
    Containers::Array<Containers::Triple<Containers::String, Containers::StringView, Int>> gltfAttributeNamesTypes;
    for(UnsignedInt i = 0; i != mesh.attributeCount(); ++i) {
        arrayAppend(gltfAttributeNamesTypes, InPlaceInit);

        /** @todo option to skip unrepresentable attributes instead of failing
            the whole mesh */

        const VertexFormat format = mesh.attributeFormat(i);
        if(isVertexFormatImplementationSpecific(format)) {
            Error{} << "Trade::GltfSceneConverter::add(): implementation-specific vertex format" << reinterpret_cast<void*>(vertexFormatUnwrap(format)) << "can't be exported";
            return {};
        }

        const UnsignedInt componentCount = vertexFormatComponentCount(format);
        const UnsignedInt vectorCount = vertexFormatVectorCount(format);
        const MeshAttribute name = mesh.attributeName(i);

        /* Positions are always three-component, two-component positions would
           fail */
        Containers::String gltfAttributeName;
        if(name == MeshAttribute::Position) {
            gltfAttributeName = Containers::String::nullTerminatedGlobalView("POSITION"_s);

            /* Half-float types and cross-byte-packed types not supported by
               glTF */
            if(format == VertexFormat::Vector3b ||
               format == VertexFormat::Vector3bNormalized ||
               format == VertexFormat::Vector3ub ||
               format == VertexFormat::Vector3ubNormalized ||
               format == VertexFormat::Vector3s ||
               format == VertexFormat::Vector3sNormalized ||
               format == VertexFormat::Vector3us ||
               format == VertexFormat::Vector3usNormalized) {
                _state->requireKhrMeshQuantization = true;
            } else if(format != VertexFormat::Vector3) {
                Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh position attribute format" << format;
                return {};
            }

        /* Normals are always three-component, Magnum doesn't have
           two-component normal packing at the moment */
        } else if(name == MeshAttribute::Normal) {
            gltfAttributeName = Containers::String::nullTerminatedGlobalView("NORMAL"_s);

            /* Half-float types and cross-byte-packed types not supported by
               glTF */
            if(format == VertexFormat::Vector3bNormalized ||
               format == VertexFormat::Vector3sNormalized) {
                _state->requireKhrMeshQuantization = true;
            } else if(format != VertexFormat::Vector3) {
                Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh normal attribute format" << format;
                return {};
            }

        /* Tangents are always four-component. Because three-component
           tangents are also common, these will be exported as a custom
           attribute with a warning. */
        } else if(name == MeshAttribute::Tangent && componentCount == 4) {
            gltfAttributeName = Containers::String::nullTerminatedGlobalView("TANGENT"_s);

            if(format == VertexFormat::Vector4bNormalized ||
               format == VertexFormat::Vector4sNormalized) {
                _state->requireKhrMeshQuantization = true;
            } else if(format != VertexFormat::Vector4) {
                Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh tangent attribute format" << format;
                return {};
            }

        /* Texture coordinates are always two-component, Magnum doesn't have
           three-compoent / layered texture coordinates at the moment */
        } else if(name == MeshAttribute::TextureCoordinates) {
            gltfAttributeName = Containers::String::nullTerminatedGlobalView("TEXCOORD"_s);

            if(format == VertexFormat::Vector2b ||
               format == VertexFormat::Vector2bNormalized ||
               format == VertexFormat::Vector2ub ||
               format == VertexFormat::Vector2s ||
               format == VertexFormat::Vector2sNormalized ||
               format == VertexFormat::Vector2us) {
                _state->requireKhrMeshQuantization = true;
            } else if(format != VertexFormat::Vector2 &&
                      format != VertexFormat::Vector2ubNormalized &&
                      format != VertexFormat::Vector2usNormalized) {
                Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh texture coordinate attribute format" << format;
                return {};
            }

        /* Colors are either three- or four-component */
        } else if(name == MeshAttribute::Color) {
            gltfAttributeName = Containers::String::nullTerminatedGlobalView("COLOR"_s);

            if(format != VertexFormat::Vector3 &&
               format != VertexFormat::Vector4 &&
               format != VertexFormat::Vector3ubNormalized &&
               format != VertexFormat::Vector4ubNormalized &&
               format != VertexFormat::Vector3usNormalized &&
               format != VertexFormat::Vector4usNormalized) {
                Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh color attribute format" << format;
                return {};
            }

        /* Otherwise it's a custom attribute where anything representable by
           glTF is allowed */
        } else {
            switch(name) {
                /* LCOV_EXCL_START */
                case MeshAttribute::Position:
                case MeshAttribute::Normal:
                case MeshAttribute::TextureCoordinates:
                case MeshAttribute::Color:
                    CORRADE_INTERNAL_ASSERT_UNREACHABLE();
                /* LCOV_EXCL_STOP */

                case MeshAttribute::Tangent:
                    CORRADE_INTERNAL_ASSERT(componentCount == 3);
                    gltfAttributeName = Containers::String::nullTerminatedGlobalView("_TANGENT3"_s);
                    Warning{} << "Trade::GltfSceneConverter::add(): exporting three-component mesh tangents as a custom" << gltfAttributeName << "attribute";
                    break;

                case MeshAttribute::Bitangent:
                    gltfAttributeName = Containers::String::nullTerminatedGlobalView("_BITANGENT"_s);
                    Warning{} << "Trade::GltfSceneConverter::add(): exporting separate mesh bitangents as a custom" << gltfAttributeName << "attribute";
                    break;

                case MeshAttribute::ObjectId:
                    /* The returned view isn't global, but will stay in scope
                       until the configuration gets modified. Which won't
                       happen inside this function so we're fine. */
                    gltfAttributeName = Containers::String::nullTerminatedView(configuration().value<Containers::StringView>("objectIdAttribute"));
                    break;
            }

            /* For custom attributes pick an externally supplied name or
               generate one from the numeric value if not supplied */
            if(!gltfAttributeName) {
                CORRADE_INTERNAL_ASSERT(isMeshAttributeCustom(name));
                const UnsignedInt id = meshAttributeCustom(name);
                for(const Containers::Pair<UnsignedShort, Containers::String>& i: _state->customMeshAttributes) {
                    if(i.first() == id) {
                        /* Make a non-owning reference to avoid a copy */
                        gltfAttributeName = Containers::String::nullTerminatedView(i.second());
                        break;
                    }
                }
                if(!gltfAttributeName) {
                    gltfAttributeName = Utility::format("_{}", meshAttributeCustom(name));
                    Warning{} << "Trade::GltfSceneConverter::add(): no name set for" << name << Debug::nospace << ", exporting as" << gltfAttributeName;
                }
            }
        }

        /** @todo spec says that POSITION accessor MUST have its min and max
            properties defined, I don't care at the moment */

        /* If a builtin glTF numbered attribute, append an ID to the name */
        if(gltfAttributeName == "TEXCOORD"_s ||
           gltfAttributeName == "COLOR"_s ||
           /* Not a builtin MeshAttribute yet, but expected to be used by
              people until builtin support is added */
           gltfAttributeName == "JOINTS"_s ||
           gltfAttributeName == "WEIGHTS"_s)
        {
            gltfAttributeName = Utility::format("{}_{}", gltfAttributeName, mesh.attributeId(i));

        /* Otherwise, if it's a second or further duplicate attribute,
           underscore it if not already and append an ID as well -- e.g. second
           and third POSITION attribute becomes _POSITION_1 and _POSITION_2,
           secondary _OBJECT_ID becomes _OBJECT_ID_1 */
        } else if(const UnsignedInt id = mesh.attributeId(i)) {
            gltfAttributeName = Utility::format(
                gltfAttributeName.hasPrefix('_') ? "{}_{}" : "_{}_{}",
                gltfAttributeName, id);
        }

        Containers::StringView gltfAccessorType;
        if(vectorCount == 1) {
            if(componentCount == 1)
                gltfAccessorType = "SCALAR"_s;
            else if(componentCount == 2)
                gltfAccessorType = "VEC2"_s;
            else if(componentCount == 3)
                gltfAccessorType = "VEC3"_s;
            else if(componentCount == 4)
                gltfAccessorType = "VEC4"_s;
            else CORRADE_INTERNAL_ASSERT_UNREACHABLE(); /* LCOV_EXCL_LINE */
        } else if(vectorCount == 2 && componentCount == 2) {
            gltfAccessorType = "MAT2"_s;
        } else if(vectorCount == 3 && componentCount == 3) {
            gltfAccessorType = "MAT3"_s;
        } else if(vectorCount == 4 && componentCount == 4) {
            gltfAccessorType = "MAT4"_s;
        } else {
            Error{} << "Trade::GltfSceneConverter::add(): unrepresentable mesh vertex format" << format;
            return {};
        }

        /* glTF requires matrices to be aligned to four bytes -- i.e., using
           the Matrix2x2bNormalizedAligned, Matrix3x3bNormalizedAligned or Matrix3x3sNormalizedAligned formats instead of the formats missing
           the Aligned suffix. Fortunately we don't need to check each
           individually as we have a neat tool instead. */
        if(vectorCount != 1 && vertexFormatVectorStride(format) % 4 != 0) {
            Error{} << "Trade::GltfSceneConverter::add(): mesh matrix attributes are required to be four-byte-aligned but got" << format;
            return {};
        }

        Int gltfAccessorComponentType;
        const VertexFormat componentFormat = vertexFormatComponentFormat(format);
        if(componentFormat == VertexFormat::Byte)
            gltfAccessorComponentType = Implementation::GltfTypeByte;
        else if(componentFormat == VertexFormat::UnsignedByte)
            gltfAccessorComponentType = Implementation::GltfTypeUnsignedByte;
        else if(componentFormat == VertexFormat::Short)
            gltfAccessorComponentType = Implementation::GltfTypeShort;
        else if(componentFormat == VertexFormat::UnsignedShort)
            gltfAccessorComponentType = Implementation::GltfTypeUnsignedShort;
        else if(componentFormat == VertexFormat::UnsignedInt) {
            /* UnsignedInt is supported only for indices, not attributes; we
               allow this in non-strict mode  */
            if(configuration().value<bool>("strict")) {
                Error{} << "Trade::GltfSceneConverter::add(): mesh attributes with" << format << "are not valid glTF, set strict=false to allow them";
                return {};
            } else Warning{} << "Trade::GltfSceneConverter::add(): strict mode disabled, allowing a 32-bit integer attribute" << gltfAttributeName;

            gltfAccessorComponentType = Implementation::GltfTypeUnsignedInt;
        } else if(componentFormat == VertexFormat::Float)
            gltfAccessorComponentType = Implementation::GltfTypeFloat;
        else {
            Error{} << "Trade::GltfSceneConverter::add(): unrepresentable mesh vertex format" << format;
            return {};
        }

        /* Final checks on attribute weirdness */
        if(mesh.attributeStride(i) <= 0) {
            Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh attribute with stride" << mesh.attributeStride(i);
            return {};
        }
        if(mesh.attributeArraySize(i) != 0) {
            Error{} << "Trade::GltfSceneConverter::add(): unsupported mesh attribute with array size" << mesh.attributeArraySize(i);
            return {};
        }

        gltfAttributeNamesTypes.back() = {std::move(gltfAttributeName), gltfAccessorType, gltfAccessorComponentType};
    }

    /* At this point we're sure nothing will fail so we can start writing the
       JSON. Otherwise we'd end up with a partly-written JSON in case of an
       unsupported mesh, corruputing the output. */

    /* If this is a first mesh, open the meshes array. Do the same for buffer
       views and accessors if we have an index buffer or at least one
       attribute. */
    if(_state->gltfMeshes.isEmpty())
        _state->gltfMeshes.beginArray();
    if(mesh.isIndexed() || mesh.attributeCount()) {
        if(_state->gltfBufferViews.isEmpty())
            _state->gltfBufferViews.beginArray();
        if(_state->gltfAccessors.isEmpty())
            _state->gltfAccessors.beginArray();
    }

    CORRADE_INTERNAL_ASSERT(_state->gltfMeshes.currentArraySize() == id);
    Containers::ScopeGuard gltfMesh = _state->gltfMeshes.beginObjectScope();
    _state->gltfMeshes.writeKey("primitives");
    {
        Containers::ScopeGuard gltfPrimitives = _state->gltfMeshes.beginArrayScope();
        Containers::ScopeGuard gltfPrimitive = _state->gltfMeshes.beginObjectScope();

        /* Index view and accessor if the mesh is indexed */
        if(mesh.isIndexed()) {
            /* Using indices() instead of indexData() to discard arbitrary
               padding before and after */
            /** @todo or put the whole thing there, consistently with
                vertexData()? */
            Containers::ArrayView<char> indexData = arrayAppend(_state->buffer, mesh.indices().asContiguous());

            const std::size_t gltfBufferViewIndex = _state->gltfBufferViews.currentArraySize();
            Containers::ScopeGuard gltfBufferView = _state->gltfBufferViews.beginObjectScope();
            _state->gltfBufferViews
                .writeKey("buffer"_s).write(0)
                /** @todo could be omitted if zero, is that useful for anything? */
                .writeKey("byteOffset"_s).write(indexData - _state->buffer)
                .writeKey("byteLength"_s).write(indexData.size());
            /** @todo target, once we don't have one view per accessor */
            if(configuration().value<bool>("accessorNames"))
                _state->gltfBufferViews.writeKey("name"_s).write(Utility::format(
                    name ? "mesh {0} ({1}) indices" : "mesh {0} indices",
                    id, name));

            const std::size_t gltfAccessorIndex = _state->gltfAccessors.currentArraySize();
            Containers::ScopeGuard gltfAccessor = _state->gltfAccessors.beginObjectScope();
            _state->gltfAccessors
                .writeKey("bufferView"_s).write(gltfBufferViewIndex)
                /* bufferOffset is implicitly 0 */
                .writeKey("componentType"_s).write(gltfIndexType)
                .writeKey("count"_s).write(mesh.indexCount())
                .writeKey("type"_s).write("SCALAR"_s);
            if(configuration().value<bool>("accessorNames"))
                _state->gltfAccessors.writeKey("name"_s).write(Utility::format(
                    name ? "mesh {0} ({1}) indices" : "mesh {0} indices",
                    id, name));

            _state->gltfMeshes.writeKey("indices").write(gltfAccessorIndex);
        }

        /* Vertex data */
        Containers::ArrayView<char> vertexData = arrayAppend(_state->buffer, mesh.vertexData());

        /* Attribute views and accessors. If we have no attributes, the glTF is
           not strictly valid anyway, so omiting the attributes key should be
           fine. */
        if(mesh.attributeCount()) {
            _state->gltfMeshes.writeKey("attributes");
            Containers::ScopeGuard gltfAttributes = _state->gltfMeshes.beginObjectScope();

            for(UnsignedInt i = 0; i != mesh.attributeCount(); ++i) {
                const VertexFormat format = mesh.attributeFormat(i);
                const std::size_t formatSize = vertexFormatSize(format);
                const std::size_t attributeStride = mesh.attributeStride(i);
                const std::size_t gltfBufferViewIndex = _state->gltfBufferViews.currentArraySize();
                Containers::ScopeGuard gltfBufferView = _state->gltfBufferViews.beginObjectScope();
                _state->gltfBufferViews
                    .writeKey("buffer"_s).write(0)
                    /* Byte offset could be omitted if zero but since that
                       happens only for the very first view in a buffer and we
                       have always at most one buffer, the minimal savings are
                       not worth the inconsistency */
                    .writeKey("byteOffset"_s).write(vertexData - _state->buffer + mesh.attributeOffset(i));

                /* Byte length, make sure to not count padding into it as
                   that'd fail bound checks. If there are no vertices, the
                   length is zero. */
                /** @todo spec says it can't be smaller than stride (for
                    single-vertex meshes), fix alongside merging buffer views
                    for interleaved attributes */
                const std::size_t gltfByteLength = mesh.vertexCount() ?
                    /** @todo this needs to include array size once we use that
                        for builtin attributes (skinning?) */
                    attributeStride*(mesh.vertexCount() - 1) + formatSize : 0;
                _state->gltfBufferViews.writeKey("byteLength"_s).write(gltfByteLength);

                /* If byteStride is omitted, it's implicitly treated as tightly
                   packed, same as in GL. If/once views get shared, this needs
                   to also check that the view isn't shared among multiple
                   accessors. */
                if(attributeStride != formatSize)
                    _state->gltfBufferViews.writeKey("byteStride"_s).write(attributeStride);

                /** @todo target, once we don't have one view per accessor */

                if(configuration().value<bool>("accessorNames"))
                    _state->gltfBufferViews.writeKey("name"_s).write(Utility::format(
                        name ? "mesh {0} ({1}) {2}" : "mesh {0} {2}",
                        id, name, gltfAttributeNamesTypes[i].first()));

                const std::size_t gltfAccessorIndex = _state->gltfAccessors.currentArraySize();
                Containers::ScopeGuard gltfAccessor = _state->gltfAccessors.beginObjectScope();
                _state->gltfAccessors
                    .writeKey("bufferView"_s).write(gltfBufferViewIndex)
                    /* We don't share views among accessors yet, so
                       bufferOffset is implicitly 0 */
                    .writeKey("componentType"_s).write(gltfAttributeNamesTypes[i].third());
                if(isVertexFormatNormalized(format))
                    _state->gltfAccessors.writeKey("normalized"_s).write(true);
                _state->gltfAccessors
                    .writeKey("count"_s).write(mesh.vertexCount())
                    .writeKey("type"_s).write(gltfAttributeNamesTypes[i].second());
                if(configuration().value<bool>("accessorNames"))
                    _state->gltfAccessors.writeKey("name"_s).write(Utility::format(
                        name ? "mesh {0} ({1}) {2}" : "mesh {0} {2}",
                        id, name, gltfAttributeNamesTypes[i].first()));

                _state->gltfMeshes.writeKey(gltfAttributeNamesTypes[i].first()).write(gltfAccessorIndex);
            }
        }

        /* Triangles are a default */
        if(gltfMode != 4) _state->gltfMeshes.writeKey("mode").write(gltfMode);
    }

    if(name)
        _state->gltfMeshes.writeKey("name").write(name);

    return true;
}

}}

CORRADE_PLUGIN_REGISTER(GltfSceneConverter, Magnum::Trade::GltfSceneConverter,
    "cz.mosra.magnum.Trade.AbstractSceneConverter/0.2")
