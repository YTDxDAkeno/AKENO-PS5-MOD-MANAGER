# SPDX-License-Identifier: GPL-3.0-or-later
# cmake -DDIRECTORY=<dir> -DFILES=a,b -P Checksums.cmake
# Writes <dir>/SHA256SUMS in the format of sha256sum(1), so users can verify downloads.
string(REPLACE "," ";" file_list "${FILES}")
set(content "")
foreach(name IN LISTS file_list)
    file(SHA256 "${DIRECTORY}/${name}" hash)
    string(APPEND content "${hash}  ${name}\n")
endforeach()
file(WRITE "${DIRECTORY}/SHA256SUMS" "${content}")
