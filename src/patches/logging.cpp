//
// Created by DiaLight on 19.01.2025.
//

#include "logging.h"
#include <cstdarg>
#include <cstdio>
#include <tools/flametal_config.h>

flametal_config::define_flame_option<bool> o_log_debug(
    "flametal:logging:debug", flametal_config::OG_Config,
    "Flametal debug logging",
    true
);
void patch::log::dbg(const char *format, ...) {
    if (!o_log_debug.get()) return;
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[d] %s\n", msg);
    fflush(stdout);
    va_end(args);
}

flametal_config::define_flame_option<bool> o_log_ReadSPMessage(
    "flametal:logging:readsp", flametal_config::OG_Config,
    "ReadSPMessage function logging",
    false
);
void patch::log::spmsg(const char *format, ...) {
    if (!o_log_ReadSPMessage.get()) return;
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[spmsg] %s\n", msg);
    va_end(args);
}

flametal_config::define_flame_option<bool> o_log_sock(
    "flametal:logging:sock", flametal_config::OG_Config,
    "Network packets logging",
    false
);

void patch::log::sock(const char *format, ...) {
    if (!o_log_sock.get()) return;
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[sock] %s\n", msg);
    va_end(args);
}

flametal_config::define_flame_option<bool> o_log_data(
    "flametal:logging:data", flametal_config::OG_Config,
    "Data packets logging",
    false
);
void patch::log::data(const char *format, ...) {
    if (!o_log_data.get()) return;
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[data] %s\n", msg);
    va_end(args);
}

flametal_config::define_flame_option<bool> o_log_gdata(
    "flametal:logging:gdata", flametal_config::OG_Config,
    "Guaranteed data packets logging",
    false
);
void patch::log::gdata(const char *format, ...) {
    if (!o_log_gdata.get()) return;
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[gdata] %s\n", msg);
    va_end(args);
}

flametal_config::define_flame_option<bool> o_log_weanetr_err(
    "flametal:logging:weanetr_err", flametal_config::OG_Config,
    "",
    true
);
void patch::log::err(const char *format, ...) {
    if (!o_log_weanetr_err.get()) return;
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[err] %s\n", msg);
    fflush(stdout);
    va_end(args);
}

flametal_config::define_flame_option<bool> o_log_weanetr(
    "flametal:logging:weanetr", flametal_config::OG_Config,
    "",
    false
);
void patch::log::v_weanetr(const char *format, va_list args) {
    if (!o_log_weanetr.get()) return;
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    printf("[weanetr] %s", msg);
}
