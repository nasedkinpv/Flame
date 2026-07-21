//
// Created by DiaLight on 08.08.2024.
//

#ifndef FLAMETAL_CAST_H
#define FLAMETAL_CAST_H


template<class T, class V>
T *dyn_cast(V *cls) {
    if(cls->getVtbl() == T::vftable) return (T *) cls;
    return nullptr;
}

#endif //FLAMETAL_CAST_H
