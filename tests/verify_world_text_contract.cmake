if(NOT DEFINED MCVR_SOURCE_DIR)
    message(FATAL_ERROR "MCVR_SOURCE_DIR is required")
endif()

set(VANILLA_ROOT "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/vanilla-pt")
set(ADVANCED_ROOT "${MCVR_SOURCE_DIR}/src/shader/world/ray_tracing/internal/advanced")
set(VANILLA_CONFIG "${VANILLA_ROOT}/configs.json")
set(ADVANCED_CONFIG "${ADVANCED_ROOT}/configs.json")
set(VERTEX_GLSL "${MCVR_SOURCE_DIR}/src/shader/util/vertex.glsl")

set(REQUIRED_TEXT_FILES
    "${VANILLA_CONFIG}"
    "${ADVANCED_CONFIG}"
    "${VANILLA_ROOT}/world/text.rahit"
    "${ADVANCED_ROOT}/common/text.rahit"
    "${VANILLA_ROOT}/world/default.rahit"
    "${ADVANCED_ROOT}/common/default.rahit"
    "${VANILLA_ROOT}/world/shadow.rahit"
    "${ADVANCED_ROOT}/common/shadow.rahit"
    "${VERTEX_GLSL}"
    "${VANILLA_ROOT}/priority/background.rgen"
    "${VANILLA_ROOT}/priority/priority.rgen"
    "${VANILLA_ROOT}/priority/priority.rmiss"
    "${VANILLA_ROOT}/priority/text.rahit"
    "${VANILLA_ROOT}/priority/text.rchit"
    "${VANILLA_ROOT}/priority/name_tag_text.rchit"
    "${VANILLA_ROOT}/priority/composite.comp"
    "${MCVR_SOURCE_DIR}/src/shader/util/priority_payload.glsl"
    "${MCVR_SOURCE_DIR}/src/core/render/modules/world/post_render/post_render_module.cpp"
    "${MCVR_SOURCE_DIR}/src/core/render/modules/world/post_render/post_render_module.hpp"
    "${MCVR_SOURCE_DIR}/src/core/render/modules/world/shader_pack/shader_pack.hpp")

foreach(required_file IN LISTS REQUIRED_TEXT_FILES)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Missing upstream text-routing source: ${required_file}")
    endif()
endforeach()

file(READ "${VANILLA_CONFIG}" vanilla_config_source)
file(READ "${ADVANCED_CONFIG}" advanced_config_source)
set(config_source "${vanilla_config_source}\n${advanced_config_source}")

foreach(required_marker IN ITEMS
        "\"name\": \"priority_background\""
        "\"name\": \"priority_geometry\""
        "\"name\": \"priority_composite\""
        "priority/priority.rgen"
        "priority/background.rgen"
        "priority/name_tag_text.rchit"
        "priority/text.rahit"
        "priority/text.rchit"
        "priority/composite.comp"
        "\"priority_outline\":"
        "\"text\":"
        "\"text_intensity\":"
        "\"text_background\":"
        text_background_see_through
        text_see_through
        text_intensity_see_through
        text_polygon_offset
        text_intensity_polygon_offset)
    string(FIND "${config_source}" "${required_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Missing upstream text marker: ${required_marker}")
    endif()
endforeach()

foreach(forbidden_marker IN ITEMS
        post_render_text
        post_render_name_tag_see_through
        post_render/render_text.vert
        post_render/render_text.frag
        "\"content\": \"name_tag_see_through\""
        "\"name\": \"post_render_name_tag\""
        "\"content\": \"name_tag_normal\""
        text_non_occluding.rahit
        "\"name\": \"post_render_text\"")
    string(FIND "${config_source}" "${forbidden_marker}" marker_index)
    if(NOT marker_index EQUAL -1)
        message(FATAL_ERROR "Local world-text hit-group override remains: ${forbidden_marker}")
    endif()
endforeach()

foreach(text_any_hit IN ITEMS
        "${VANILLA_ROOT}/world/text.rahit"
        "${ADVANCED_ROOT}/common/text.rahit")
    file(READ "${text_any_hit}" text_any_hit_source)
    foreach(required_marker IN ITEMS lodWithCone resolveTextCoverage "alpha <= 0.0" "rand(mainRay.seed)" ignoreIntersectionEXT)
        string(FIND "${text_any_hit_source}" "${required_marker}" marker_index)
        if(marker_index EQUAL -1)
            message(FATAL_ERROR "${text_any_hit} is missing upstream marker: ${required_marker}")
        endif()
    endforeach()
    foreach(forbidden_marker IN ITEMS "uv, 0.0, false" "alpha < 0.1")
        string(FIND "${text_any_hit_source}" "${forbidden_marker}" marker_index)
        if(NOT marker_index EQUAL -1)
            message(FATAL_ERROR "${text_any_hit} still contains a local glyph sampling override: ${forbidden_marker}")
        endif()
    endforeach()
endforeach()

foreach(default_any_hit IN ITEMS
        "${VANILLA_ROOT}/world/default.rahit"
        "${ADVANCED_ROOT}/common/default.rahit")
    file(READ "${default_any_hit}" default_any_hit_source)
    string(FIND "${default_any_hit_source}" "alphaLod = isCutoutAlphaMode" fixed_cutout_lod_index)
    if(NOT fixed_cutout_lod_index EQUAL -1)
        message(FATAL_ERROR "${default_any_hit} still forces base-level sampling for all cutout surfaces")
    endif()
endforeach()

file(READ "${VERTEX_GLSL}" vertex_source)
string(FIND "${vertex_source}" "TEXT_SURFACE_BIT" text_surface_bit_index)
if(NOT text_surface_bit_index EQUAL -1)
    message(FATAL_ERROR "Local TEXT_SURFACE_BIT remains in packed vertex data")
endif()

foreach(shadow_any_hit IN ITEMS
        "${VANILLA_ROOT}/world/shadow.rahit"
        "${ADVANCED_ROOT}/common/shadow.rahit")
    file(READ "${shadow_any_hit}" shadow_source)
    foreach(forbidden_marker IN ITEMS isTextSurface resolveTextCoverage)
        string(FIND "${shadow_source}" "${forbidden_marker}" marker_index)
        if(NOT marker_index EQUAL -1)
            message(FATAL_ERROR "${shadow_any_hit} still contains a local text-shadow branch: ${forbidden_marker}")
        endif()
    endforeach()
endforeach()

foreach(local_shader IN ITEMS
        "${VANILLA_ROOT}/world/text.rchit"
        "${ADVANCED_ROOT}/common/text.rchit"
        "${ADVANCED_ROOT}/common/text_non_occluding.rahit")
    if(EXISTS "${local_shader}")
        message(FATAL_ERROR "Local text shader must not exist when following upstream: ${local_shader}")
    endif()
endforeach()

foreach(world_surface IN ITEMS
        "${VANILLA_ROOT}/world/default.rchit"
        "${VANILLA_ROOT}/world/no_height.rchit"
        "${ADVANCED_ROOT}/world/default.rchit"
        "${ADVANCED_ROOT}/world/no_height.rchit"
        "${ADVANCED_ROOT}/common/surface_eval.glsl")
    file(READ "${world_surface}" world_surface_source)
    foreach(required_marker IN ITEMS
            "isTextMode(alphaMode)"
            "resolveTextTextureColor"
            "bool usePbr = !textSurface"
            "specularValue = textureMap.specular >= 0 && usePbr ?"
            "normalValue = textureMap.normal >= 0 && usePbr ?")
        string(FIND "${world_surface_source}" "${required_marker}" marker_index)
        if(marker_index EQUAL -1)
            message(FATAL_ERROR
                "World text does not default to a flat material in ${world_surface}: ${required_marker}")
        endif()
    endforeach()
endforeach()

file(READ
    "${MCVR_SOURCE_DIR}/src/core/render/modules/world/post_render/post_render_module.cpp"
    post_render_source)
file(READ
    "${MCVR_SOURCE_DIR}/src/core/render/modules/world/post_render/post_render_module.hpp"
    post_render_header)
file(READ
    "${MCVR_SOURCE_DIR}/src/core/render/modules/world/shader_pack/shader_pack.hpp"
    shader_pack_header)
set(native_source "${post_render_source}\n${post_render_header}\n${shader_pack_header}")

foreach(required_marker IN ITEMS
        "Target::Text"
        textPostFlag)
    string(FIND "${native_source}" "${required_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Native post-render is missing upstream text target: ${required_marker}")
    endif()
endforeach()

foreach(forbidden_native_marker IN ITEMS
        "Target::NameTagSeeThrough"
        nameTagSeeThroughPostFlag
        "Target::NameTagNormal"
        "Target::NameTag,"
        nameTagNormalPostFlag)
    string(FIND "${native_source}" "${forbidden_native_marker}" marker_index)
    if(NOT marker_index EQUAL -1)
        message(FATAL_ERROR "Unnecessary normal/name-tag post target remains: ${forbidden_native_marker}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/shader/util/text_mode.glsl" text_mode_source)
foreach(text_mode_marker IN ITEMS
        "POST_TEXT_MODE_BACKGROUND = 12u"
        "POST_TEXT_MODE_RGBA_POLYGON_OFFSET = 19u")
    string(FIND "${text_mode_source}" "${text_mode_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Text mode namespace is not isolated from material alpha modes: ${text_mode_marker}")
    endif()
endforeach()

foreach(packed_marker IN ITEMS
        "ALPHA_MODE_MASK = 0x1Fu"
        "COORDINATE_SHIFT = 13u"
        "NO_HEIGHT_SURFACE_BIT = 1u << 17u")
    string(FIND "${vertex_source}" "${packed_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR "Packed material field layout is missing widened alpha contract: ${packed_marker}")
    endif()
endforeach()

file(READ "${VANILLA_ROOT}/priority/priority.rgen" priority_raygen_source)
foreach(priority_visibility_marker IN ITEMS
        "uint priorityMask = PRIORITY_MASK"
        "worldUBO.isFirstPerson == 0"
        "priorityMask |= PLAYER_MASK"
        "gl_RayFlagsCullBackFacingTrianglesEXT, priorityMask"
        "origin, 0.000001"
        "origin = hitPosition + direction * 0.00001")
    string(FIND "${priority_raygen_source}" "${priority_visibility_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR
            "Priority geometry is missing camera-entity visibility contract: ${priority_visibility_marker}")
    endif()
endforeach()

foreach(forbidden_priority_marker IN ITEMS "imageLoad(firstHitDepthImage" hiddenByWorld worldDepth)
    string(FIND "${priority_raygen_source}" "${forbidden_priority_marker}" marker_index)
    if(NOT marker_index EQUAL -1)
        message(FATAL_ERROR
            "Priority geometry still depends on world occlusion: ${forbidden_priority_marker}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/common/mapping.hpp" mapping_source)
foreach(priority_mask_marker IN ITEMS
        "#    define PRIORITY_MASK 4")
    string(FIND "${mapping_source}" "${priority_mask_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR
            "Priority-only geometry is missing its isolated visibility mask: ${priority_mask_marker}")
    endif()
endforeach()

foreach(world_ray_source IN ITEMS
        "${VANILLA_ROOT}/world/world.rgen"
        "${ADVANCED_ROOT}/world/world.rgen")
    file(READ "${world_ray_source}" world_ray_source_text)
    foreach(required_marker IN ITEMS
            "void preparePrimary"
            "void prepareSecondary"
            "WORLD_MASK | PRIORITY_MASK"
            "WORLD_MASK | PLAYER_MASK | PRIORITY_MASK")
        string(FIND "${world_ray_source_text}" "${required_marker}" marker_index)
        if(marker_index EQUAL -1)
            message(FATAL_ERROR
                "Physical priority geometry is missing from secondary rays: ${world_ray_source} ${required_marker}")
        endif()
    endforeach()
endforeach()

file(READ "${ADVANCED_ROOT}/common/primary_trace.glsl" advanced_primary_source)
string(FIND "${advanced_primary_source}" "PRIORITY_MASK" primary_priority_index)
if(NOT primary_priority_index EQUAL -1)
    message(FATAL_ERROR "Primary camera tracing must not include physical priority geometry")
endif()

foreach(config_text IN ITEMS vanilla_config_source advanced_config_source)
    foreach(priority_input_marker IN ITEMS
            "\"name\": \"priority_background\""
            "\"name\": \"priority_geometry\""
            "priority/background.rgen"
            "priority/name_tag_text.rchit"
            "\"priority_geometry\","
            "\"priority_background\"")
        string(FIND "${${config_text}}" "${priority_input_marker}" marker_index)
        if(marker_index EQUAL -1)
            message(FATAL_ERROR
                "Priority geometry lacks ordered foreground/background composition: ${priority_input_marker}")
        endif()
    endforeach()

    string(REGEX MATCH
        "\"text_background\"[ \t\r\n]*:[ \t\r\n]*\\{[ \t\r\n]*\"type\"[ \t\r\n]*:[ \t\r\n]*\"triangle\"[ \t\r\n]*,[ \t\r\n]*\"shaders\"[ \t\r\n]*:[ \t\r\n]*\\{[ \t\r\n]*\"rchit\"[ \t\r\n]*:[ \t\r\n]*\"priority/text.rchit\""
        priority_normal_background_route
        "${${config_text}}")
    if(priority_normal_background_route STREQUAL "")
        message(FATAL_ERROR
            "Priority background pass must accept normal name-tag backgrounds")
    endif()
endforeach()

file(READ "${VANILLA_ROOT}/priority/composite.comp" priority_composite_source)
foreach(required_marker IN ITEMS
        priorityBackgroundImage
        "base.rgb * (1.0 - clamp(background.a"
        "composed * (1.0 - clamp(foreground.a")
    string(FIND "${priority_composite_source}" "${required_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR
            "Priority compositor is missing premultiplied ordered blending: ${required_marker}")
    endif()
endforeach()

file(READ "${MCVR_SOURCE_DIR}/src/core/render/entities.cpp" entities_source)
foreach(name_tag_material_marker IN ITEMS
        "geometryContentNames.back().rfind(\"/name_tag/\", 0) == 0"
        "geometryGroupName.find(\"text_background\")"
        "(nameTagContent && !nameTagBackground)")
    string(FIND "${entities_source}" "${name_tag_material_marker}" marker_index)
    if(marker_index EQUAL -1)
        message(FATAL_ERROR
            "Name-tag glyph/background material split is missing: ${name_tag_material_marker}")
    endif()
endforeach()
