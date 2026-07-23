set(TARGET dkii_res)

set(VER_PRODUCT_NUMBER 1)
set(VER_PRODUCT_VERSION 7)
set(VER_BUILD_NUMBER 0)
set(VER_REVISION_NUMBER 0)
set(VER_PROJECT_URL "https://github.com/DiaLight/Flame")

set(DKII_ICON "${CMAKE_CURRENT_LIST_DIR}/packed_icon.ico")
set(VER_COMMENTS "url: ${VER_PROJECT_URL}")
set(VER_FILE_SUFFIX " Flametal build")
if(NOT VER_BUILD_NUMBER EQUAL 0)
    string(APPEND VER_FILE_SUFFIX ".${VER_BUILD_NUMBER}")
endif ()
if(CONFIGURE_DATE)
    string(APPEND VER_COMMENTS ", date: ${CONFIGURE_DATE}")
    string(APPEND VER_FILE_SUFFIX " ${CONFIGURE_DATE}")
endif ()
if(GIT_SHA1)
    # A raw commit subject can contain a '"', which breaks the .rc string
    # literal it's embedded into below (rc.exe then fails on whatever
    # follows as a stray keyword). Strip quotes before embedding.
    string(REPLACE "\"" "'" GIT_COMMIT_SUBJECT_SAFE "${GIT_COMMIT_SUBJECT}")
    string(APPEND VER_COMMENTS ", commit: ${GIT_SHA1} ${GIT_DATE} ${GIT_COMMIT_SUBJECT_SAFE}")
    string(APPEND VER_FILE_SUFFIX " ${GIT_SHORT_SHA1}")
endif ()

string(LENGTH "${VER_COMMENTS}" VER_COMMENTS_LEN)
if(VER_COMMENTS_LEN GREATER_EQUAL 255)
    string(SUBSTRING "${VER_COMMENTS}" 0 252 VER_COMMENTS)
    string(APPEND VER_COMMENTS "...")
endif ()


install(FILES "${CMAKE_CURRENT_LIST_DIR}/readme.txt" DESTINATION ".")
