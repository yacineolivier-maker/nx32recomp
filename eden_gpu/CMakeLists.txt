# eden_gpu.dll : émulation GPU d'Eden (video_core, traducteur de shaders, périphériques nvdrv) derrière une API C, pour
# le pilote NVN d'origine du jeu. Sources d'Eden et dépendances dans NX_TIERS (non versionnées, cf.
# docs/DEPENDANCES.md et CORRECTIFS_EDEN.md). Les en-têtes de shim/ remplacent le noyau et le système d'Eden (mémoire identité, événements
# du runtime) ; src/patch/ contient les copies modifiées de fichiers d'Eden.

set(REF ${NX_TIERS})
set(EDEN ${REF}/eden)
set(ESRC ${EDEN}/src)

# --- video_core (moteur Vulkan seul)
file(GLOB_RECURSE SRC_VIDEO CONFIGURE_DEPENDS ${ESRC}/video_core/*.cpp)
list(FILTER SRC_VIDEO EXCLUDE REGEX "/video_core/(renderer_opengl|frame_gen|post_processing)/")
list(FILTER SRC_VIDEO EXCLUDE REGEX "/renderer_vulkan/present/(frame_gen|lsfg_|post_process)")
list(FILTER SRC_VIDEO EXCLUDE REGEX "/host1x/(nvdec|vic|ffmpeg|codecs/|control|host1x\\.cpp|sync_manager)")
list(FILTER SRC_VIDEO EXCLUDE REGEX "/video_core/cdma_pusher\\.cpp")
list(FILTER SRC_VIDEO EXCLUDE REGEX "/video_core/macro\\.cpp$") # copies modifiées dans src/patch/
list(FILTER SRC_VIDEO EXCLUDE REGEX "/(vulkan_instance|vk_swapchain|vk_present_manager)\\.cpp$")

# --- shaders de la sortie HDR (src/hdr.cpp) : SPIR-V en en-têtes générés
find_program(GLSLANG glslangValidator REQUIRED)
set(SHADERS_GEN ${CMAKE_BINARY_DIR}/eden_gpu_shaders)
set(HDR_SPV)
foreach(s hdr.vert hdr.frag)
    string(REPLACE "." "_" nom ${s})
    string(TOUPPER "${nom}_SPV" var)
    add_custom_command(OUTPUT ${SHADERS_GEN}/${nom}_spv.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADERS_GEN}
        COMMAND ${GLSLANG} -V --quiet --target-env vulkan1.1 --vn ${var} -o ${SHADERS_GEN}/${nom}_spv.h ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s} VERBATIM)
    list(APPEND HDR_SPV ${SHADERS_GEN}/${nom}_spv.h)
endforeach()

# --- traducteur de shaders (SPIR-V seul) et sirit
file(GLOB_RECURSE SRC_SHADER CONFIGURE_DEPENDS ${ESRC}/shader_recompiler/*.cpp)
list(FILTER SRC_SHADER EXCLUDE REGEX "/backend/(glasm|glsl)/")
file(GLOB_RECURSE SRC_SIRIT CONFIGURE_DEPENDS ${REF}/sirit-1.0.5/src/*.cpp)

# --- common : sous-ensemble utilisé par video_core
set(SRC_COMMON)
foreach(f settings settings_common settings_input time_zone logging assert string_util thread adpf cpu_features steady_clock
          dynamic_library error cityhash address_space multi_level_page_table sparse_large_vector memory_detect param_package
          uuid random stb hex_util)
    list(APPEND SRC_COMMON ${ESRC}/common/${f}.cpp)
endforeach()
list(APPEND SRC_COMMON ${ESRC}/common/fs/file.cpp ${ESRC}/common/fs/fs.cpp ${ESRC}/common/fs/fs_util.cpp ${ESRC}/common/fs/path_util.cpp
     ${ESRC}/common/x64/rdtsc.cpp ${ESRC}/common/windows/timer_resolution.cpp)

# --- core : fenêtre, nvdrv (périphériques et cœur)
set(NV ${ESRC}/core/hle/service/nvdrv)
set(SRC_CORE
    ${ESRC}/core/frontend/emu_window.cpp ${ESRC}/core/frontend/framebuffer_layout.cpp
    ${NV}/core/nvmap.cpp ${NV}/core/heap_mapper.cpp ${NV}/core/syncpoint_manager.cpp
    ${NV}/devices/nvhost_as_gpu.cpp ${NV}/devices/nvhost_gpu.cpp ${NV}/devices/nvhost_ctrl.cpp ${NV}/devices/nvhost_ctrl_gpu.cpp
    ${NV}/devices/nvmap.cpp ${NV}/devices/nvdisp_disp0.cpp)
# --- core : files de tampons Android et compositeur (vi)
set(NF ${ESRC}/core/hle/service/nvnflinger)
list(APPEND SRC_CORE
    ${NF}/buffer_queue_core.cpp ${NF}/buffer_queue_producer.cpp ${NF}/buffer_queue_consumer.cpp ${NF}/buffer_item_consumer.cpp
    ${NF}/consumer_base.cpp ${NF}/hardware_composer.cpp ${NF}/graphic_buffer_producer.cpp ${NF}/ui/graphic_buffer.cpp)
# --- core : Mii (tables du firmware, construction et vérification des CharInfo)
set(MII ${ESRC}/core/hle/service/mii/types)
list(APPEND SRC_CORE ${MII}/char_info.cpp ${MII}/core_data.cpp ${MII}/store_data.cpp ${MII}/raw_data.cpp)

# --- externals compilés
set(SRC_EXT ${EDEN}/externals/bc_decoder/bc_decoder.cpp ${EDEN}/externals/stb/stb_dxt.cpp)

add_library(eden_gpu SHARED
    src/api.cpp src/systeme.cpp src/nvdrv_module.cpp src/scm_rev.cpp src/vma.cpp src/patch/container.cpp src/patch/host1x.cpp src/patch/macro.cpp src/affichage.cpp
    src/patch/vulkan_instance.cpp src/patch/vk_swapchain.cpp src/patch/vk_present_manager.cpp src/hdr.cpp ${HDR_SPV} src/mii.cpp
    ${SRC_VIDEO} ${SRC_SHADER} ${SRC_SIRIT} ${SRC_COMMON} ${SRC_CORE} ${SRC_EXT})

target_include_directories(eden_gpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim ${CMAKE_CURRENT_SOURCE_DIR}/src ${SHADERS_GEN}
    ${ESRC} ${REF}/eden_gen/include
    ${EDEN}/externals/stb ${EDEN}/externals/bc_decoder ${EDEN}/externals/FidelityFX-FSR/ffx-fsr ${EDEN}/externals
    ${REF}/sirit-1.0.5/include ${REF}/sirit-1.0.5/src ${REF}/SPIRV-Headers-main/include
    ${REF}/fmt-12.1.0/include ${REF}/boost_1_86_0
    ${REF}/dl_vma/VulkanMemoryAllocator-3.3.0/include ${REF}/dl_xbyak/xbyak-7.40.1
    ${REF}/dl_vkutil/Vulkan-Utility-Libraries-1.4.345/include ${REF}/Vulkan-Headers-1.4.350/include)
target_include_directories(eden_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_definitions(eden_gpu PRIVATE EDEN_GPU_EXPORTS FMT_HEADER_ONLY NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE BOOST_ALL_NO_LIB ARCHITECTURE_x86_64=1 _WIN32_WINNT=0x0A00
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING BUILD_ID="nx32recomp"
    _USE_STD_VECTOR_ALGORITHMS=0) # find vectorisé de la STL MSVC : refuse les structures de 16 octets comparées bit à bit
set_target_properties(eden_gpu PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_compile_options(eden_gpu PRIVATE -O2 -Wno-everything -fsized-deallocation)
# clang_rt.builtins : divisions 128 bits (__udivti3) émises par clang pour la cible MSVC
get_filename_component(LLVM_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
file(GLOB CLANG_RT_BUILTINS ${LLVM_BIN}/../lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib)
target_link_libraries(eden_gpu PRIVATE ntdll user32 shell32 ole32 dbghelp ${CLANG_RT_BUILTINS})
target_link_options(eden_gpu PRIVATE -Wl,/MAP:${CMAKE_BINARY_DIR}/eden_gpu.map) # adresses des fonctions (diagnostic des fautes)
# LAMBDA_FORCEINLINE vaut [[msvc::forceinline]] si _MSC_VER et NDEBUG : clang refuse cet attribut sur les lambdas
set_source_files_properties(${ESRC}/video_core/renderer_vulkan/vk_graphics_pipeline.cpp PROPERTIES COMPILE_OPTIONS "-UNDEBUG")
