set(TARGET kwd_dump)

add_executable(${TARGET}
        main.cpp
)
target_link_libraries(${TARGET} PRIVATE kwd)
