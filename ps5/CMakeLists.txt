# Oogie's Revenge PS5 â€” the title's build.
#
# Separate CMake project, configured with the PS5 SDK toolchain
# (toolchain/prospero.cmake); tools/build.sh drives it.
#
# M3: the xboxrecomp runtime and the 10,538 generated game functions are
# compiled for x86_64-sie-ps5 together with the port's hand-written overrides
# (recomp_manual.c) and a PS5 platform layer. title_main.c boots the guest.
#
# The runtime is built from upstream's *patched* sources, exactly as the PC
# build builds it: port/patches/*.old -> *.new add the Oogie-specific
# behaviour (guest CPU scheduling, push-buffer fixes, APU mix routing) and
# define symbols recomp_manual.c links against. oogie_patch_source below is
# upstream's own function, unchanged.
#
# Layout and startup come from the user's The-Darkness-Recomp-PS5 app/ps5
# (app_crt.cpp, ps5-pie.ld, the link flags); ps5_settings.c replaces upstream's
# pc_settings.c, and fp_ieee.cpp comes from fefw-ps5.
cmake_minimum_required(VERSION 3.20)
project(oogie_ps5 CXX C)

set(OOGIE_PS5_LAYOUT "${CMAKE_CURRENT_SOURCE_DIR}/platform/ps5-pie.ld")
set(OOGIE_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../upstream/port")

# The generated game code and the xboxrecomp checkout live outside the tree
# (they are produced from the player's own disc), so both are cache paths.
set(OOGIE_GEN_DIR "$ENV{HOME}/oogie-work/gen" CACHE PATH "generated recomp chunks")
set(XBOXRECOMP_DIR "$ENV{HOME}/oogie-work/xboxrecomp" CACHE PATH "xboxrecomp checkout")

if(NOT EXISTS "${OOGIE_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR
        "No generated game code in ${OOGIE_GEN_DIR}.\n"
        "Generate it first: bash tools/lift.sh   (needs your own disc image)")
endif()
if(NOT EXISTS "${XBOXRECOMP_DIR}/src/kernel/kernel.h")
    message(FATAL_ERROR "No xboxrecomp checkout in ${XBOXRECOMP_DIR} (set XBOXRECOMP_DIR)")
endif()

# â”€â”€ upstream's patch machinery, verbatim â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
# Build-time patches to the runtime (see port/patches/README.md). The
# xboxrecomp checkout stays unmodified: for each listed source file, CMake
# copies it into the build tree, applies the exact text replacements
# patches/<name>.old -> <name>.new, and compiles the copy instead of the
# original. A replacement that does not match exactly once stops
# configuration, so a patch can never silently stop applying.
function(oogie_patch_source target srcdir file)
    set(_src "${srcdir}/${file}")
    file(READ "${_src}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    foreach(_name IN LISTS ARGN)
        # Upstream's patches first; then this port's own (patches/ here:
        # LP64 fixes a Windows-only declaration needs on the console).
        if(EXISTS "${OOGIE_PORT_DIR}/patches/${_name}.old")
            set(_pdir "${OOGIE_PORT_DIR}/patches")
        else()
            set(_pdir "${CMAKE_CURRENT_SOURCE_DIR}/patches")
        endif()
        file(READ "${_pdir}/${_name}.old" _old)
        file(READ "${_pdir}/${_name}.new" _new)
        string(REPLACE "\r\n" "\n" _old "${_old}")
        string(REPLACE "\r\n" "\n" _new "${_new}")
        string(FIND "${_text}" "${_old}" _first)
        string(FIND "${_text}" "${_old}" _last REVERSE)
        if(_first EQUAL -1 OR NOT _first EQUAL _last)
            message(FATAL_ERROR "Oogie patch '${_name}' does not match exactly once in ${_src}")
        endif()
        string(REPLACE "${_old}" "${_new}" _text "${_text}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${_pdir}/${_name}.old" "${_pdir}/${_name}.new")
    endforeach()
    set(_out "${CMAKE_BINARY_DIR}/oogie_patched/${file}")
    file(WRITE "${_out}.tmp" "${_text}")
    configure_file("${_out}.tmp" "${_out}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    get_target_property(_srcs ${target} SOURCES)
    list(FIND _srcs "${_src}" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "Oogie patch: ${file} is not a source of ${target}")
    endif()
    list(REMOVE_AT _srcs ${_idx})
    list(APPEND _srcs "${_out}")
    set_property(TARGET ${target} PROPERTY SOURCES ${_srcs})
    target_include_directories(${target} PRIVATE "${srcdir}")
    message(STATUS "Oogie: patched ${file} (${ARGN})")
endfunction()

# â”€â”€ the generated game (13 chunks + dispatch + stubs) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
file(GLOB OOGIE_GENERATED CONFIGURE_DEPENDS "${OOGIE_GEN_DIR}/recomp_*.c")

# â”€â”€ the runtime, as static library â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
add_library(oogie_runtime STATIC
    # platform: Win32/NT vocabulary + POSIX primitives (threads, events,
    # virtual memory, the file API, the clock). The layer M3 asked for.
    ${XBOXRECOMP_DIR}/src/platform/win32_compat.c
    ${XBOXRECOMP_DIR}/src/platform/recomp_exit_hook.c

    # kernel: the Xbox kernel HLE the guest's imports land in.
    ${XBOXRECOMP_DIR}/src/kernel/kernel_path.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_pool.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_rtl.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_memory.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_file.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_missing.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_thread.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_sync.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_hal.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_xbox.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_ob.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_io.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_crypto.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_thunks.c
    ${XBOXRECOMP_DIR}/src/kernel/kernel_bridge.c
    ${XBOXRECOMP_DIR}/src/kernel/xbox_memory_layout.c
    ${XBOXRECOMP_DIR}/src/kernel/guest_vmem.c
    ${XBOXRECOMP_DIR}/src/kernel/icall_feedback.c
    ${XBOXRECOMP_DIR}/src/kernel/recomp_trace.c
    # the NV2A push-buffer executor and its vertex/combiner interpreters
    ${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_scan.c
    ${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c
    ${XBOXRECOMP_DIR}/src/kernel/nv2a_vsh_interp.c
    ${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c

    # NV2A device model (registers, MMIO traps, push-buffer replay)
    ${XBOXRECOMP_DIR}/src/nv2a/nv2a_core.c
    ${XBOXRECOMP_DIR}/src/nv2a/nv2a_mmio_hook.c
    ${XBOXRECOMP_DIR}/src/nv2a/nv2a_pb_replay.c
    ${XBOXRECOMP_DIR}/src/nv2a/nv2a_pgraph_d3d11.c

    # USB OHCI: a pad only exists if the controller's registers do
    ${XBOXRECOMP_DIR}/src/usb/ohci.c
    ${XBOXRECOMP_DIR}/src/usb/usb_gamepad.c

    # APU: the MCPX audio processor the guest's DirectSound drives
    ${XBOXRECOMP_DIR}/src/apu/apu_core.c
    ${XBOXRECOMP_DIR}/src/apu/apu_vp.c
    ${XBOXRECOMP_DIR}/src/apu/apu_dsp.c
    ${XBOXRECOMP_DIR}/src/apu/apu_mmio_hook.c
    ${XBOXRECOMP_DIR}/src/apu/apu_xaudio2.c
    ${XBOXRECOMP_DIR}/src/apu/apu_wav.c

    # DirectSound compatibility layer
    ${XBOXRECOMP_DIR}/src/audio/dsound_device.c
    ${XBOXRECOMP_DIR}/src/audio/wma_decoder.c

    # Keyboard input
    ${XBOXRECOMP_DIR}/src/input/keyboard.c

    # The guest framebuffer the CPU rasteriser fills
    ${XBOXRECOMP_DIR}/src/video/fb_present.c
    ${XBOXRECOMP_DIR}/src/video/video_player.c
)

# â”€â”€ upstream's patches, same list and order as port/CMakeLists.txt â”€â”€â”€â”€â”€â”€â”€
oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" xbox_memory_layout.c
                   thread_stacks quiet_gpu_trace pb_segment_wrap
                   contig_quarantine_a contig_quarantine_b contig_quarantine_c
                   contig_quarantine_d contig_quarantine_e
                   pb_ring_reset pb_ring_reset_label pb_history_a pb_history_b
                   klog_tiled mirrors_fewer_ps5 span_smaller_ps5 tiled_degrade_ps5 mirror_last_half_ps5 pb_scan_timing pb_idle_counter pb_idle_reset pb_idle_backoff pb_affinity)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" nv2a_combiner.c
                   nv2a_unpack_tls)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" nv2a_pb_exec.c
                   clear_channels_32 clear_channels_16 vertex_current_value
                   swizzled_surfaces_a swizzled_surfaces_b swizzled_surfaces_c
                   quiet_draw_progress parallel_vsh cache_verbose_getenv
                   backend_extra_stages decode_linear_texture
                   backend_vertex_extra_a backend_vertex_extra_b backend_vertex_extra_c
                   backend_gpu_vsh gpu_attr_decode gpu_attr_pack_helpers gpu_attr_pack_batch vsh_input_layout_cache gpu_vertex_compact gpu_vertex_remap gpu_vertex_count diag_skip_batches diag_trace_d0 pb_method_ring
                   sem_drop_test clear_rect palette_decode
                   sem_offset_decl sem_offset flip_done pb_geometry_burst pb_vsh_constant_burst pb_draw_array_indices pb_draw_array_burst pb_draw_array_scalar
                   pb_position_survey pb_texture_survey
                   strip_winding_a strip_winding_b backend_reg_read klog_gpu_report klog_vsh_decl klog_vsh_words klog_sxf klog_sxf_incl klog_xf_d0 xf_w_guard raster_pool_ps5 dxt_cache_heap xf_bpp_backend vsh_sample pool_diag_c pool_diag_a pool_diag_d pool_diag_b raster_stack xfp_stack lazy_zbuf)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" kernel_bridge.c
                   kernel_note_file kernel_summary_quiet kernel_read_quiet
                   ke_wait_d3d_gpu guest_cpu_held usb_dpc_lock_a usb_dpc_lock_b
                   guest_cpu_fair
                   thread_suspended_b thread_suspended_a thread_suspended_c
                   wait_log_bounded wait_multi_log_bounded
                   guest_priority_table guest_cpu_priority guest_cpu_safepoint
                   klog_helper klog_cdrom klog_filefail klog_exit
                   klog_wait_entry klog_wait_ge klog_wait_done
                   klog_hb_impl klog_read klog_bufok klog_contig klog_nobridge klog_heartbeat
                   klog_spawn klog_spawn_nf klog_spawn_ns resume_fake_handle gcpu_fast gcpu_enter_stats gcpu_dispatch_tls gcpu_uncontended kernel_summary_sample gcpu_fast_d3d_cs)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" kernel_path.c
                   mods_folder_a mods_folder_b path_ci_incl path_ci)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/usb" ohci.c
                   ohci_ed_head_a ohci_ed_head_b pad_rumble
                   ohci_td_validate ohci_head_writeback_check ohci_head_repair
                   usb_frame_read usb_frame_after_a usb_frame_after_b
                   ohci_ps5_a ohci_ps5_b ohci_ps5_c ohci_ps5_d)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/apu" apu_dsp.c
                   apu_bin_trace apu_mixdown_stereo apu_doorbell_latch)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/apu" apu_core.c
                   apu_irq_poll_decl apu_irq_poll_open)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/apu" apu_vp.c
                   apu_route_trace apu_headroom_trace apu_headroom_trace_b apu_voice_gain
                   apu_fecv_trace_c apu_fecv_trace_a apu_fecv_trace_b
                   apu_idle_fair_a apu_idle_fair_b apu_idle_fair_c
                   # must follow apu_fecv_trace_c, which inserts the line it rewrites
                   apu_tid_lp64)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/apu" apu_xaudio2.c
                   audio_volume)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" nv2a_vsh_interp.c
                   vsh_access klog_vsh_run klog_vsh_out vsh_trace_decl vsh_trace
                   vsh_constant_generation vsh_constant_generation_word vsh_constant_generation_vector vsh_constant_generation_component vsh_constant_generation_output
                   vsh_program_generation vsh_program_generation_word vsh_program_generation_instruction vsh_constant_burst)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" kernel_rtl.c
                   cs_wait_report klog_cs
                   # Guest locks: a title that sets D3D state per draw takes the
                   # same few locks millions of times a second, so the shadow
                   # lookup gets a lock-free cache and the diagnostic counters
                   # leave the hot path (patches/cs_cache_*.old, cs_stats_*.old).
                   cs_cache_decl cs_cache_lookup cs_cache_publish
                   cs_stats_enter cs_stats_leave cs_d3d_try)

oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/platform" win32_compat.c
                   mapview_fixed_ps5 exit_quick prof_register)
oogie_patch_source(oogie_runtime "${XBOXRECOMP_DIR}/src/kernel" nv2a_pb_scan.c
                   pb_segment_bounds_a pb_segment_bounds_b pb_segment_ack
                   pb_segment_carry pb_note_off pb_survey_flag pb_survey_init pb_survey_gate
                   pb_survey_dma pb_survey_carry pb_survey_segment pb_geometry_burst_consume pb_draw_array_dma pb_vsh_constant_consume pb_where_decl pb_where
                   pb_call_log pb_carry_log
                   pb_ring_target_a pb_ring_target_b pb_reread)

# Upstream's x86-64 trapped-MMIO decoder is Windows-guarded; ps5_mmio.c
# feeds it a CONTEXT mirrored from the console's ucontext. Open the guard on
# a build-tree copy rather than editing upstream.
file(READ "${XBOXRECOMP_DIR}/src/platform/mmio_decode.h" _mmio_dec)
string(REPLACE "\r\n" "\n" _mmio_dec "${_mmio_dec}")
string(REPLACE "#if defined(_WIN32)\n#include <windows.h>" "#if 1" _mmio_dec "${_mmio_dec}")
string(REPLACE "#endif /* _WIN32 */" "#endif" _mmio_dec "${_mmio_dec}")
file(WRITE "${CMAKE_BINARY_DIR}/oogie_patched/mmio_decode_ps5.h.tmp" "${_mmio_dec}")
configure_file("${CMAKE_BINARY_DIR}/oogie_patched/mmio_decode_ps5.h.tmp"
               "${CMAKE_BINARY_DIR}/oogie_patched/mmio_decode_ps5.h" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${XBOXRECOMP_DIR}/src/platform/mmio_decode.h")

# â”€â”€ the title â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
add_executable(oogie_ps5
    # startup + link layout, from The-Darkness-Recomp-PS5
    platform/app_crt.cpp
    platform/fp_ieee.cpp          # IEEE FP env, from fefw-ps5
    platform/posix_compat.cpp

    # the port's own layer, on the console
    platform/ps5_settings.c       # replaces upstream port/src/pc_settings.c
    platform/ps5_hle.c            # host versions of the hottest guest setters
    platform/ps5_libc.c           # explicit_bzero, memfd_create (glibc-only)
    platform/ps5_video.c          # M4: the VideoOut presentation layer
    platform/ps5_mmio.c           # x86-64 trapped MMIO: emulated APU, AC'97
    platform/ps5_prof.c           # sampling profiler to the kernel log
    platform/ps5_emutls.c         # per-thread cache for emulated TLS: the
                                  # guest reads TLS once per function call
    platform/ps5_vk.c             # M9: Vulkan (RADV) presentation
    platform/ps5_backend_stub.c
    platform/ps5_audio.c

    # the guest: recompiled game + the hand-written overrides
    ${OOGIE_GENERATED}
    ${OOGIE_PORT_DIR}/src/recomp_manual.c

    # the title
    title/title_main.c
    title/game_setup.cpp
    title/iso_import.cpp

    # the runtime (patched)
    $<TARGET_OBJECTS:oogie_runtime>
)


# The PS5 target lowers each TLS symbol to a call into compiler-rt. Give the
# translated guest and its bridges one register block so the compiler can
# reuse its address within each function. The VFS overlay keeps the upstream
# checkout and the generated files shared with other builds intact.
option(OOGIE_GROUP_GUEST_REGS "Group the PS5 guest registers in one TLS block" ON)
option(OOGIE_PASS_CONTEXT "Pass the PS5 register block on lifted direct calls" OFF)
if(OOGIE_GROUP_GUEST_REGS)
    set(_guest_overlay_options)
    if(OOGIE_PASS_CONTEXT)
        list(APPEND _guest_overlay_options --pass-context)
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_guest_overlay_script "${CMAKE_CURRENT_SOURCE_DIR}/tools/guest_register_overlay.py")
    set(_guest_overlay_inputs
        "${XBOXRECOMP_DIR}/src"
        "${XBOXRECOMP_DIR}/include"
        "${OOGIE_GEN_DIR}"
        "${OOGIE_PORT_DIR}/src/recomp_manual.c"
        "${CMAKE_BINARY_DIR}/oogie_patched"
        "${CMAKE_CURRENT_SOURCE_DIR}/title/title_main.c")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_guest_overlay_script}"
                ${_guest_overlay_options} "${CMAKE_BINARY_DIR}/guest_registers" ${_guest_overlay_inputs}
        OUTPUT_VARIABLE _guest_overlay
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    file(GLOB_RECURSE _guest_runtime_sources CONFIGURE_DEPENDS
        "${XBOXRECOMP_DIR}/src/*.c" "${XBOXRECOMP_DIR}/src/*.h"
        "${XBOXRECOMP_DIR}/include/*.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_guest_overlay_script}" "${OOGIE_GEN_DIR}/recomp_types.h" ${OOGIE_GENERATED}
        "${OOGIE_PORT_DIR}/src/recomp_manual.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/title/title_main.c"
        ${_guest_runtime_sources})
    foreach(tgt oogie_ps5 oogie_runtime)
        target_compile_options(${tgt} PRIVATE
            -ivfsoverlay "${_guest_overlay}"
            "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/platform/ps5_guest_regs.h\"")
    endforeach()
endif()


target_include_directories(oogie_ps5 PRIVATE
    ${CMAKE_BINARY_DIR}/oogie_patched
    ${CMAKE_CURRENT_SOURCE_DIR}/win32
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
    ${OOGIE_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
    ${XBOXRECOMP_DIR}/src/platform
    ${XBOXRECOMP_DIR}/src/kernel
    ${XBOXRECOMP_DIR}/src/nv2a
    ${XBOXRECOMP_DIR}/src/apu
    ${XBOXRECOMP_DIR}/src/usb
    ${XBOXRECOMP_DIR}/src/input
    ${XBOXRECOMP_DIR}/src/audio
    ${XBOXRECOMP_DIR}/src/video
    ${OOGIE_PORT_DIR}/src
)

set_target_properties(oogie_ps5 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    LINK_DEPENDS "${OOGIE_PS5_LAYOUT}"
)

# The generated chunks are ~1.4 M lines: no per-file warnings, and the
# interpreter's own diagnostics stay readable.
set_source_files_properties(${OOGIE_GENERATED} PROPERTIES COMPILE_OPTIONS "-w")
option(OOGIE_TUNE_GUEST "Build lifted game functions with Zen 2 tuning" OFF)
if(OOGIE_TUNE_GUEST)
    # Preserve scalar guest memory accesses for the MMIO fault decoder.
    set_property(SOURCE ${OOGIE_GENERATED} APPEND PROPERTY COMPILE_OPTIONS
        -O3 -mtune=znver2 -fno-vectorize -fno-slp-vectorize)
endif()

# The runtime's patched sources use a few Win32 calls without including
# <windows.h> (they rely on the Windows build's forced headers), so the PS5
# extras are force-included for the runtime target.
target_compile_options(oogie_runtime PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/win32/win32_ps5_extra.h)

foreach(tgt oogie_ps5 oogie_runtime)
    target_compile_options(${tgt} PRIVATE
        -O2 -g1 -ffunction-sections -fdata-sections -fno-omit-frame-pointer
        -fno-strict-aliasing -w
        # The PS5's CPU is Zen 2. Without this the generated code is built for
        # baseline x86-64, which leaves AVX2 and the whole Zen scheduling model
        # on the table for the vector-heavy guest translation.
        -march=znver2)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/win32
        ${CMAKE_CURRENT_SOURCE_DIR}/shims
        ${OOGIE_GEN_DIR}
        ${XBOXRECOMP_DIR}/include
        ${XBOXRECOMP_DIR}/src
        ${XBOXRECOMP_DIR}/src/platform
        ${OOGIE_PORT_DIR}/src)
endforeach()

# RECOMP_CALL_PROFILE: every direct guest call reports to the guest CPU
# lock's safepoint (recomp_manual.c) â€” upstream's own definition.
# reporting hook on every direct call; off for release
## reporting hook on every direct call; collection is done, off for release
#target_compile_definitions(oogie_ps5 PRIVATE RECOMP_CALL_PROFILE=1)

target_link_options(oogie_ps5 PRIVATE
    -lkernel
    -Wl,--as-needed
    # Resolve intra-binary calls directly instead of through the GOT.
    -Wl,-Bsymbolic-functions
    -nostartfiles
    -Wl,--wrap=mtx_init -Wl,--wrap=mtx_lock -Wl,--wrap=mtx_timedlock
    -Wl,--wrap=mtx_trylock -Wl,--wrap=mtx_unlock
    -Wl,--wrap=cnd_init -Wl,--wrap=cnd_wait -Wl,--wrap=cnd_timedwait
    -Wl,--wrap=cnd_signal -Wl,--wrap=cnd_broadcast
    -Wl,--wrap=thrd_create -Wl,--wrap=thrd_join -Wl,--wrap=thrd_detach
    -Wl,--wrap=sceVideoOutOpen
    -Wl,--wrap=realpath
    -Wl,--wrap=printf -Wl,--wrap=vprintf -Wl,--wrap=puts -Wl,--wrap=putchar
    -Wl,--wrap=__emutls_get_address
    -Wl,--exclude-libs=ALL
    -Wl,--eh-frame-hdr
    -Wl,--build-id=none
    "-Wl,-T,${OOGIE_PS5_LAYOUT}"
)

target_link_libraries(oogie_ps5 PRIVATE SceUserService SceVideoOut SceSystemService ScePad SceAudioOut)

# M9: RADV, linked whole with PS5_Vulkan's recipe (tools/build.sh writes the
# list). platform/ps5_vk.c then presents through Vulkan; without the list it
# compiles to "no Vulkan" and VideoOut presents as before.
if(OOGIE_RADV_FLAGS_FILE AND EXISTS "${OOGIE_RADV_FLAGS_FILE}")
    file(STRINGS "${OOGIE_RADV_FLAGS_FILE}" _radv_flags)
    target_link_options(oogie_ps5 PRIVATE ${_radv_flags})
    target_compile_definitions(oogie_ps5 PRIVATE OOGIE_HAVE_VULKAN=1)
    
    target_include_directories(oogie_ps5 PRIVATE "${OOGIE_VULKAN_INCLUDE}" "${CMAKE_BINARY_DIR}/gen")
    message(STATUS "Oogie: Vulkan via RADV (${OOGIE_RADV_FLAGS_FILE})")
endif()
