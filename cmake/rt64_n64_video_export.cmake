# Writes the video core's RT64 patch as two file trees, for
# scripts/export_n64_video_rt64_patch.py to diff into
# src/renderer/n64_video/rt64_n64_video.patch. Run in script mode:
#
#   cmake -DRT64_ROOT=<pinned RT64> -DBEFORE_DIR=<dir> -DAFTER_DIR=<dir>
#         -P cmake/rt64_n64_video_export.cmake
#
# BEFORE_DIR gets each pinned RT64 file the patch changes, AFTER_DIR the same
# file after rt64_n64_video.cmake's patch sites, plus the files the patch adds.
# Both are LF-normalised. Each pinned input is hash-checked as in the build.
# Unlike the build, the patch is applied to the pristine pinned file, not on
# top of this project's other RT64 patches, so the result holds the video
# core's changes only.

cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS RT64_ROOT BEFORE_DIR AFTER_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "rt64_n64_video_export.cmake needs -D${required}=...")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/rt64_n64_video.cmake")

foreach(input IN LISTS jfg_n64_video_source_view_inputs jfg_n64_video_target_unit_inputs)
    jfg_rt64_patch_split_pair("${input}" relative expected)
    jfg_rt64_patch_check_input("video core" "${RT64_ROOT}" "${relative}" "${expected}")
    file(READ "${RT64_ROOT}/${relative}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    file(WRITE "${BEFORE_DIR}/${relative}" "${contents}")
    jfg_n64_video_patch_contents("${relative}")
    file(WRITE "${AFTER_DIR}/${relative}" "${contents}")
endforeach()

foreach(new_file IN LISTS jfg_n64_video_new_files)
    jfg_rt64_patch_split_pair("${new_file}" name relative)
    file(READ "${jfg_n64_video_core_dir}/${name}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    file(WRITE "${AFTER_DIR}/${relative}" "${contents}")
endforeach()
