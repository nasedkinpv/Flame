//
// Created by DiaLight on 9/23/2025.
//

#ifndef FLAMETAL_REPLACE_HEAP_H
#define FLAMETAL_REPLACE_HEAP_H


namespace patch::replace_heap {

    extern bool enabled;

    void *malloc(size_t size);
    void *realloc(void *ptr, size_t size);
    void free(void *ptr);
    size_t size(void *ptr);

}


#endif // FLAMETAL_REPLACE_HEAP_H
