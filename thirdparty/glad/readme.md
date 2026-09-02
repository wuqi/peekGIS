A repository that contains glad files for opengl 3.3 core.

Adds a cmake library with the name of glad

add this to your cmake file 

```
...

include_directories(external_libraries/glad_opengl_3.3_core/include)
add_subdirectory(external_libraries/glad_opengl_3.3_core)

... 

target_link_libraries(your_project_name ... glad)
```

