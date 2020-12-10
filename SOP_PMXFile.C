
#include "SOP_PMXFile.h"
#include "SOP_PMXFile.proto.h"

#include <GU/GU_Detail.h>
#include <GEO/GEO_PrimPoly.h>
#include <OP/OP_Operator.h>
#include <OP/OP_OperatorTable.h>
#include <PRM/PRM_Include.h>
#include <PRM/PRM_TemplateBuilder.h>
#include <UT/UT_DSOVersion.h>
#include <UT/UT_Interrupt.h>
#include <UT/UT_StringHolder.h>
#include <UT/UT_Vector.h>
#include <UT/UT_Vector2.h>
#include <UT/UT_Vector4.h>
#include <UT/UT_Matrix4.h>

#include <SYS/SYS_Math.h>
#include <limits.h>

#include <FS/FS_Reader.h>
#include <GA/GA_PolyCounts.h>
#include <GA/GA_AIFNumericArray.h>
#include <GA/GA_AIFJSON.h>

using namespace PMX;

struct FileHeader
{
    char signature[4];
    fpreal32 version;
};

enum BoneFlag : uint16_t
{
    ConnectBoneDisplay = 0x0001, // 接続先(PMD子ボーン指定)表示方法 -> 0:座標オフセットで指定 1 : ボーンで指定

    Rotate = 0x0002, // 回転可能
    Translate = 0x0004, // 移動可能
    Display = 0x0008, // 表示
    Operationable = 0x0010, // 操作可

    IK = 0x0020, // IK

    AddLocal = 0x0080, // ローカル付与 | 付与対象 0:ユーザー変形値／IKリンク／多重付与 1:親のローカル変形量
    AddRotate = 0x0100, // 回転付与
    AddTranslate = 0x0200, // 移動付与

    FixedAxis = 0x0400, // 軸固定
    LocalAxis = 0x0800, // ローカル軸

    DeformAfterPhysics = 0x1000, // 物理後変形
    DeformForeignParent = 0x2000, // 外部親変形
};

struct BoneWeightInfo
{
    uint8_t     deform_type;
    exint       bone_indices[4];
    fpreal32    weights[4];

    // for SDEF
    UT_Vector3 c;
    UT_Vector3 r0;
    UT_Vector3 r1;
};

struct Material
{
    UT_String name;
    UT_String name_en;

    UT_Vector4 diffuse;
    UT_Vector4 specular;
    UT_Vector3 ambient;
    uint8_t flag;

    UT_Vector4 edge_color;
    fpreal32 edge_size;

    exint texture_index;
    exint sphere_texture_index;
    uint8_t sphere_mode;
    uint8_t shared_toon_flag;
    exint toon_texture_index;

    UT_String memo;
};


const void* memoryOffset(const void* momory, int offset)
{
    return static_cast<const void*>(static_cast<const char*>(momory) + offset);
}

template<typename T>
T read(const void*& momory, bool advance = true)
{
    constexpr size_t size = sizeof(T);
    T value;
    memcpy(&value, momory, size);
    if (advance)
    {
        momory = static_cast<const void*>(static_cast<const char*>(momory) + size);
    }
    return value;
}

UT_String  readText(const void*& momory, uint8_t encode)
{
    exint byte_size = read<int32_t>(momory);
    const void* begin = momory;
    momory = memoryOffset(momory, byte_size);
    const void* end = momory;

    if (encode == 0) // UTF16
    {
        UT_Array<utf16> utf16_buffer;
        utf16_buffer.setCapacity((byte_size-1) / 2 + 2);
        memset(utf16_buffer.array(), 0, sizeof(utf16) * utf16_buffer.capacity());
        memcpy(utf16_buffer.array(), begin, byte_size);
        UT_WorkBuffer work_buffer = UT_WorkBuffer::narrow(utf16_buffer.array());
        UT_String str;
        work_buffer.copyIntoString(str);
        return str;
    }
    else // UTF8
    {
        UT_String str(static_cast<const char*>(begin), static_cast<const char*>(end));
        return str;
    }
};

exint readIndex(const void*& momory, uint8_t stride)
{
    switch (stride)
    {
    case 1: return read<uint8_t>(momory);
    case 2: return read<uint16_t>(momory);
    case 4:	return read<uint32_t>(momory);
    default:
        return -1;
    }
};



/// This is the internal name of the SOP type.
/// It isn't allowed to be the same as any other SOP's type name.
const UT_StringHolder SOP_PMXFile::theSOPTypeName("pmx_file"_sh);

/// newSopOperator is the hook that Houdini grabs from this dll
/// and invokes to register the SOP.  In this case, we add ourselves
/// to the specified operator table.
void newSopOperator(OP_OperatorTable *table)
{
    table->addOperator(new OP_Operator(
        SOP_PMXFile::theSOPTypeName,   // Internal name
        "PMX File",                    // UI name
        SOP_PMXFile::myConstructor,    // How to build the SOP
        SOP_PMXFile::buildTemplates(), // My parameters
        0,                          // Min # of sources
        0,                          // Max # of sources
        nullptr,                    // Custom local variables (none)
        OP_FLAG_GENERATOR));        // Flag it as generator
}

/// This is a multi-line raw string specifying the parameter interface
/// for this SOP.
static const char *theDsFile = R"THEDSFILE(
{
    name        parameters
    parm {
        name    "file"
        cppname "FilePath"
        label   "File"
        type    file
        parmtag { "filechooser_pattern" "*.pmx" }
        parmtag { "filechooser_mode" "read" }
    }
    parm {
        name    "scale"
        label   "Uniform Scale"
        type    float
        size    1
        default { "0.1" }
    }
}
)THEDSFILE";

PRM_Template* SOP_PMXFile::buildTemplates()
{
    static PRM_TemplateBuilder templ("SOP_PMXFile.C"_sh, theDsFile);
    return templ.templates();
}

class SOP_PMXFileVerb : public SOP_NodeVerb
{
public:
    SOP_PMXFileVerb() {}
    virtual ~SOP_PMXFileVerb() {}

    virtual SOP_PMXFileParms *allocParms() const { return new SOP_PMXFileParms(); }
    virtual UT_StringHolder name() const { return SOP_PMXFile::theSOPTypeName; }

    virtual CookMode cookMode(const SOP_NodeParms *parms) const { return COOK_GENERIC; }

    virtual void cook(const CookParms &cookparms) const;
    
    /// This static data member automatically registers
    /// this verb class at library load time.
    static const SOP_NodeVerb::Register<SOP_PMXFileVerb> theVerb;
};

// The static member variable definition has to be outside the class definition.
// The declaration is inside the class.
const SOP_NodeVerb::Register<SOP_PMXFileVerb> SOP_PMXFileVerb::theVerb;

const SOP_NodeVerb* SOP_PMXFile::cookVerb() const
{ 
    return SOP_PMXFileVerb::theVerb.get();
}

/// This is the function that does the actual work.
void SOP_PMXFileVerb::cook(const SOP_NodeVerb::CookParms &cookparms) const
{
    auto &&sopparms = cookparms.parms<SOP_PMXFileParms>();
    GU_Detail *detail = cookparms.gdh().gdpNC();

    detail->clearAndDestroy();

    // Start the interrupt scope
    UT_AutoInterrupt boss("Load PMX File");
    if (boss.wasInterrupted())
        return;

    auto& filepath = sopparms.getFilePath();
    auto scale = sopparms.getScale();

    FS_Reader reader(filepath);
    if (!reader.isGood())
    {
        return;
    }

    UT_IStream* stream = reader.getStream();
    UT_WorkBuffer work_buffer;
    stream->getAll(work_buffer);

    const void* current = work_buffer.begin();
    const void* end = work_buffer.end();

    // PMXヘッダ 
    FileHeader header = read<FileHeader>(current);

    const bool check_signature = (
        header.signature[0] == 'P' &&
        header.signature[1] == 'M' &&
        header.signature[2] == 'X' &&
        header.signature[3] == ' ');

    if (!check_signature)
    {
        return;
    }

    if (header.version != 2.0f && header.version != 2.1f)
    {
        return;
    }

    uint8_t info_count = read<uint8_t>(current);

    if (info_count < 8)
    {
        return;
    }

    if ((header.version == 2.f) && (info_count != 8))
    {
        return;
    }

    uint8_t text_encode             = *(static_cast<const uint8_t*>(current) + 0);
    uint8_t additional_uv_count     = *(static_cast<const uint8_t*>(current) + 1);
    uint8_t vertex_index_stride     = *(static_cast<const uint8_t*>(current) + 2);
    uint8_t texture_index_stride    = *(static_cast<const uint8_t*>(current) + 3);
    uint8_t material_index_stride   = *(static_cast<const uint8_t*>(current) + 4);
    uint8_t bone_index_stride       = *(static_cast<const uint8_t*>(current) + 5);
    uint8_t morph_index_stride      = *(static_cast<const uint8_t*>(current) + 6);
    uint8_t rigid_index_stride      = *(static_cast<const uint8_t*>(current) + 7);

    current = memoryOffset(current, info_count);

    // モデル情報
    UT_String model_name    = readText(current, text_encode);
    UT_String model_name_en = readText(current, text_encode);
    UT_String comment       = readText(current, text_encode);
    UT_String comment_en    = readText(current, text_encode);

    // 頂点
    exint num_vertices = read<int32_t>(current);

    UT_Array<UT_Vector3> position_array;
    UT_Array<UT_Vector3> normal_array;
    UT_Array<UT_Vector2> uv_array;
    UT_Array<BoneWeightInfo> bone_weight_array;
    UT_Array<UT_String> texture_array;
    UT_Array<Material> mat_array;
    UT_Array<exint> mat_index_count_array;

    position_array.setCapacity(num_vertices);
    normal_array.setCapacity(num_vertices);
    uv_array.setCapacity(num_vertices);
    bone_weight_array.setCapacity(num_vertices);

    for (exint i = 0; i < num_vertices; i++)
    {
        position_array.append( read<UT_Vector3>(current) );
        normal_array.append( read<UT_Vector3>(current) );
        uv_array.append( read<UT_Vector2>(current) );

        for (int32_t j = 0; j < additional_uv_count; j++)
        {
            UT_Vector4 additional_uv = read<UT_Vector4>(current);
        }

        uint8_t deform_type = read<uint8_t>(current);

        BoneWeightInfo bone_weight_info;
        bone_weight_info.deform_type = deform_type;

        switch (deform_type)
        {
        case 0:	// BDEF1
        {
            bone_weight_info.bone_indices[0] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[1] = -1;
            bone_weight_info.bone_indices[2] = -1;
            bone_weight_info.bone_indices[3] = -1;
            bone_weight_info.weights[0] = 1.f;
            bone_weight_info.weights[1] = 0.f;
            bone_weight_info.weights[2] = 0.f;
            bone_weight_info.weights[3] = 0.f;
            break;
        }
        case 1:	// BDEF2
        {
            bone_weight_info.bone_indices[0] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[1] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[2] = -1;
            bone_weight_info.bone_indices[3] = -1;
            bone_weight_info.weights[0] = read<fpreal32>(current);
            bone_weight_info.weights[1] = 1.f - bone_weight_info.weights[0];
            bone_weight_info.weights[2] = 0.f;
            bone_weight_info.weights[3] = 0.f;
            break;
        }
        case 2:	// BDEF4
        {
            bone_weight_info.bone_indices[0] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[1] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[2] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[3] = readIndex(current, bone_index_stride);
            bone_weight_info.weights[0] = read<fpreal32>(current);
            bone_weight_info.weights[1] = read<fpreal32>(current);
            bone_weight_info.weights[2] = read<fpreal32>(current);
            bone_weight_info.weights[3] = read<fpreal32>(current);
            break;
        }
        case 3:	// SDEF
        {
            bone_weight_info.bone_indices[0] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[1] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[2] = -1;
            bone_weight_info.bone_indices[3] = -1;
            bone_weight_info.weights[0] = read<fpreal32>(current);
            bone_weight_info.weights[1] = 1.f - bone_weight_info.weights[0];
            bone_weight_info.weights[2] = 0.f;
            bone_weight_info.weights[3] = 0.f;

            bone_weight_info.c = read<UT_Vector3>(current);
            bone_weight_info.r0 = read<UT_Vector3>(current);
            bone_weight_info.r1 = read<UT_Vector3>(current);
            break;
        }
        case 4: // QDEF
        {
            bone_weight_info.bone_indices[0] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[1] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[2] = readIndex(current, bone_index_stride);
            bone_weight_info.bone_indices[3] = readIndex(current, bone_index_stride);
            bone_weight_info.weights[0] = read<fpreal32>(current);
            bone_weight_info.weights[1] = read<fpreal32>(current);
            bone_weight_info.weights[2] = read<fpreal32>(current);
            bone_weight_info.weights[3] = read<fpreal32>(current);
            break;
        }
        default:
            break;
        }
        bone_weight_array.append(bone_weight_info);

        fpreal32 edge_rate = read<fpreal32>(current);
    }

    // インデックス 
    UT_Array<int> indices;
    exint face_index_count = read<int32_t>(current);
    if (face_index_count % 3 != 0)
    {
        return;
    }
    indices.setCapacity(face_index_count);

    for (int32_t i = 0; i < face_index_count; i++)
    {
        indices.append( readIndex(current, vertex_index_stride) );
    }

    // テクスチャ
    exint texture_count = read<int32_t>(current);
    texture_array.setCapacity(texture_count);
    for (int32_t i = 0; i < texture_count; i++)
    {
        texture_array.append(readText(current, text_encode));
    }

    // 材質
    int32_t material_count = read<int32_t>(current);
    mat_array.setCapacity(material_count);
    mat_index_count_array.setCapacity(material_count);
    for (int32_t i = 0; i < material_count; i++)
    {
        Material material;

        material.name = readText(current, text_encode);
        material.name_en = readText(current, text_encode);

        material.diffuse = read<UT_Vector4>(current);
        material.specular = read<UT_Vector4>(current);
        material.ambient = read<UT_Vector3>(current);
        material.flag = read<uint8_t>(current);

        material.edge_color = read<UT_Vector4>(current);
        material.edge_size = read<fpreal32>(current);

        material.texture_index = readIndex(current, texture_index_stride);
        material.sphere_texture_index = readIndex(current, texture_index_stride);
        material.sphere_mode = read<uint8_t>(current);
        material.shared_toon_flag = read<uint8_t>(current);
        if (material.shared_toon_flag == 0)
        {
            material.toon_texture_index = readIndex(current, texture_index_stride);
        }
        else // if (shared_toon_flag == 1)
        {
            material.toon_texture_index = static_cast<exint>(read<uint8_t>(current));
        }

        material.memo = readText(current, text_encode);

        mat_array.append(std::move(material));

        exint material_index_count = read<int32_t>(current);
        mat_index_count_array.append(material_index_count);
    }


    reader.close();

    UT_Matrix4 axis_mat(1.f);
    // axis_mat.rotate(UT_Axis3::YAXIS, SYSdegToRad(180.f));
    axis_mat.scale(UT_Vector3(1.f, 1.f, -1.f));
    
    UT_Matrix4 pos_conv_mat(axis_mat);
    pos_conv_mat.scale(scale);

    // set points 
    const exint num_points = num_vertices;
    GA_Offset startptoff = detail->appendPointBlock(num_points);
    for (exint point_idx = 0; point_idx < num_vertices; ++point_idx)
    {
        GA_Offset ptoff = startptoff + point_idx;
        auto pos = position_array[point_idx] * pos_conv_mat;
        detail->setPos3(ptoff, pos);
    }

    // set normal 
    if (auto normal_attrib = detail->addNormalAttribute(GA_ATTRIB_POINT))
    {
        GA_RWHandleV3 handle(normal_attrib);
        for (exint point_idx = 0; point_idx < num_vertices; ++point_idx)
        {
            auto normal = normal_array[point_idx] * axis_mat;
            handle.set(point_idx, normal);
        }
    }
    // set uv
    if (auto uv_attrib = detail->addTextureAttribute(GA_ATTRIB_POINT))
    {
        GA_RWHandleV3 handle(uv_attrib);
        for (exint point_idx = 0; point_idx < num_vertices; ++point_idx)
        {
            UT_Vector3 uv(uv_array[point_idx].x(), uv_array[point_idx].y(), 1.f);
            handle.set(point_idx, uv);
        }
    }

    // check index range
    for(auto index : indices)
    {
        if (index<0 || index >= num_points)
        {
            return;
        }
    }

    // set polygon 
    GA_PolyCounts poly_counts;
    poly_counts.append(3, face_index_count/3);
    GEO_PrimPoly::buildBlock(detail, startptoff, num_points, poly_counts, indices.data(), true);
    
    // set mat_index
    if(auto mat_index_attrib = detail->addIntTuple(GA_ATTRIB_PRIMITIVE, "mat_index", 1))
    {
        GA_RWHandleI handle(mat_index_attrib);
        exint prim_index = 0;
        for (exint mat_index=0; mat_index<mat_index_count_array.size(); mat_index++)
        {
            exint mat_index_count = mat_index_count_array[mat_index];
            for (exint i = 0; i < mat_index_count; i += 3)
            {
                handle.set(prim_index++, mat_index);
            }
        }
    }


    { // filepath
        auto filepath_attrib = detail->addStringTuple(GA_ATTRIB_DETAIL, "filepath", 1);
        GA_RWHandleS handle(filepath_attrib);
        handle.set(GA_Offset(0), filepath.c_str());
    }

    {// texture name 
        auto tex_names_attrib = detail->addStringArray(GA_ATTRIB_DETAIL, "tex_names");
        if (const GA_AIFSharedStringArray* aif = tex_names_attrib->getAIFSharedStringArray())
        {
            UT_StringArray names_for_set;
            names_for_set.setCapacity(texture_array.size());
            for (auto& name : texture_array)
            {
                names_for_set.append(name);
            }
            aif->set(tex_names_attrib, GA_Offset(0), names_for_set);
        }
    }

    {// mat_parms
        auto mat_parms_attrib = detail->addDictArray(GA_ATTRIB_DETAIL, "mat_parms");
        if (const GA_AIFSharedDictArray* aif = mat_parms_attrib->getAIFSharedDictArray())
        {
            UT_Array<UT_OptionsHolder> json_values;
            json_values.setCapacity(mat_array.size());
            for (const Material& mat : mat_array)
            {
                UT_Options options(
                    "string name", mat.name.c_str(),
                    "string name_en", mat.name_en.c_str(),
                    "vector4 diffuse", mat.diffuse.x(), mat.diffuse.y(), mat.diffuse.z(), mat.diffuse.w(),
                    "vector3 specular", mat.specular.x(), mat.specular.y(), mat.specular.z(),
                    "vector3 ambient", mat.ambient.x(), mat.ambient.y(), mat.ambient.z(),
                    "int flag", int(mat.flag),
                    "vector4 edge_color", mat.edge_color.x(), mat.edge_color.y(), mat.edge_color.z(), mat.edge_color.w(),
                    "float edge_size", mat.edge_size,
                    "int tex_index", int(mat.texture_index),
                    "int sphere_tex_index", int(mat.sphere_texture_index),
                    "int sphere_mode", int(mat.sphere_mode),
                    "bool toon_flag", mat.shared_toon_flag != 0,
                    "int toon_tex_index", int(mat.toon_texture_index),
                    nullptr
                );
                json_values.append(UT_OptionsHolder(&options));
            }
            aif->set(mat_parms_attrib, GA_Offset(0), json_values);
        }
    }
}
