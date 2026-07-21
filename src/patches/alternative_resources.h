//
// Created by DiaLight on 11/27/2025.
//

#ifndef FLAMETAL_ALTERNATIVE_RESOURCES_H
#define FLAMETAL_ALTERNATIVE_RESOURCES_H


namespace patch::alternative_resources {
    extern bool enabled;

    void init();
    bool tryUse(char *outPath, int outSize, const char *fileName);

}


#endif // FLAMETAL_ALTERNATIVE_RESOURCES_H
