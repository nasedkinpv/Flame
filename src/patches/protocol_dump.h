//
// Created by DiaLight on 11.03.2025.
//

#ifndef FLAMETAL_PROTOCOL_DUMP_H
#define FLAMETAL_PROTOCOL_DUMP_H


namespace patch::protocol_dump {

    void tick();
    void onSend(size_t srcSlot, size_t dstSlot, void *data, size_t size, bool guaranteed);
    void onRecv(size_t srcSlot, size_t dstSlot, void *data, size_t size, const char *group);
    void onRecvGuaranteed(size_t srcSlot, size_t dstSlot, void *data, size_t size);

    void init();

};


#endif //FLAMETAL_PROTOCOL_DUMP_H
