# Helpers shared by the fork's RT64 patch scripts that patch files site by
# site (rt64_n64_video.cmake, rt64_vi_coverage.cmake). Each script keeps its
# own patch sites and pinned hashes; these only do the mechanics.

include_guard(GLOBAL)

# Replaces one patch site in the caller's `contents`; a missing site fails
# the build.
function(jfg_rt64_patch_replace before after)
    string(FIND "${contents}" "${before}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Missing RT64 patch site: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${contents}")
    set(contents "${contents}" PARENT_SCOPE)
endfunction()

# Splits an "a|b" pair from an input list into two variables.
function(jfg_rt64_patch_split_pair pair first_variable second_variable)
    string(REPLACE "|" ";" fields "${pair}")
    list(GET fields 0 first)
    list(GET fields 1 second)
    set(${first_variable} "${first}" PARENT_SCOPE)
    set(${second_variable} "${second}" PARENT_SCOPE)
endfunction()

# Fails the build unless `relative` in the pinned RT64 checkout still has the
# audited content. The hash is of the LF-normalised file, so an LF and a CRLF
# checkout both match it (cmake/rt64_vi_presentation.cmake hashes raw bytes and
# so lists the CRLF hash of rt64_vi_renderer.cpp separately). `patch_name`
# names the patch in the error.
function(jfg_rt64_patch_check_input patch_name original_root relative expected)
    file(READ "${original_root}/${relative}" pinned)
    string(REPLACE "\r\n" "\n" pinned "${pinned}")
    string(SHA256 digest "${pinned}")
    if(NOT digest STREQUAL expected)
        message(FATAL_ERROR "Pinned ${patch_name} RT64 input changed: ${relative}")
    endif()
endfunction()

# Writes `contents` unless the file already holds it, so an unchanged patch
# doesn't trigger a rebuild.
function(jfg_rt64_patch_write destination contents)
    set(existing "")
    if(EXISTS "${destination}")
        file(READ "${destination}" existing)
    endif()
    if(NOT existing STREQUAL contents)
        file(WRITE "${destination}" "${contents}")
    endif()
endfunction()

# Patches RT64 translation units of the rt64 target, on top of whatever
# version of each unit the target compiles now (other patches may already
# have replaced it), and points the target at the patched copy under
# `generated_dir`. `inputs` lists "relative|sha256" pairs of the pinned
# checkout; `patch_function` is called with each relative path and edits
# `contents`.
function(jfg_rt64_patch_target_units patch_name original_root patch_function generated_dir)
    foreach(input IN LISTS ARGN)
        jfg_rt64_patch_split_pair("${input}" relative expected)
        jfg_rt64_patch_check_input("${patch_name}" "${original_root}" "${relative}" "${expected}")
        get_filename_component(basename "${relative}" NAME)
        get_target_property(sources rt64 SOURCES)
        set(patched_sources)
        set(compiled_unit "")
        foreach(unit IN LISTS sources)
            get_filename_component(unit_name "${unit}" NAME)
            if(unit_name STREQUAL basename)
                set(compiled_unit "${unit}")
            else()
                list(APPEND patched_sources "${unit}")
            endif()
        endforeach()
        if(compiled_unit STREQUAL "")
            message(FATAL_ERROR "RT64 target is missing ${relative}")
        endif()
        if(NOT IS_ABSOLUTE "${compiled_unit}")
            get_target_property(rt64_source_dir rt64 SOURCE_DIR)
            set(compiled_unit "${rt64_source_dir}/${compiled_unit}")
        endif()
        file(READ "${compiled_unit}" contents)
        string(REPLACE "\r\n" "\n" contents "${contents}")
        cmake_language(CALL "${patch_function}" "${relative}")
        set(generated "${generated_dir}/${basename}")
        jfg_rt64_patch_write("${generated}" "${contents}")
        list(APPEND patched_sources "${generated}")
        set_property(TARGET rt64 PROPERTY SOURCES "${patched_sources}")
    endforeach()
endfunction()
