# SPDX-License-Identifier: GPL-3.0-or-later
# akeno_embed_file(<target> <input file> <function name> <bytes|text>)
# Adds a generated source to <target> that defines akeno::embedded::<function name>().

function(akeno_embed_file target input function kind)
    if(NOT EXISTS "${input}")
        message(FATAL_ERROR "akeno_embed_file: ${input} does not exist")
    endif()
    set(output "${CMAKE_BINARY_DIR}/generated/embedded_${function}.cpp")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${input} -DOUTPUT=${output} -DFUNCTION=${function} -DKIND=${kind}
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
        DEPENDS "${input}" "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
        COMMENT "Embedding ${input}"
        VERBATIM)
    target_sources(${target} PRIVATE "${output}")
endfunction()
