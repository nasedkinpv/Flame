//
// Created by DiaLight on 4/6/2025.
//

#ifndef FLAMETAL_CONFIG_H
#define FLAMETAL_CONFIG_H

#include <string>
#include <vector>
#include <type_traits>
#include <functional>


namespace flametal_config {
    enum value_type {
        VT_None,
        VT_String,
        VT_Boolean,
        VT_Int,
        VT_Float,
    };

    struct flametal_value {

        typedef std::string string_ty;
        typedef bool bool_ty;
        typedef int int_ty;
        typedef float float_ty;

        value_type ty;
        union {
            string_ty str_value;
            bool_ty bool_value;
            int_ty int_value;
            float_ty float_value;
        };

        explicit flametal_value() : ty(VT_None) { int_value = 0; }
        explicit flametal_value(const char *value) : ty(VT_String) { new (&str_value) string_ty(value); }
        explicit flametal_value(const std::string &value) : ty(VT_String) { new (&str_value) string_ty(value); }
        explicit flametal_value(bool value) : ty(VT_Boolean) { new (&bool_value) bool_ty(value); }
        explicit flametal_value(int value) : ty(VT_Int) { new (&int_value) int_ty(value); }
        explicit flametal_value(float value) : ty(VT_Float) { new (&float_value) float_ty(value); }
        flametal_value(const flametal_value &other) : flametal_value() { *this = other; }
        flametal_value(flametal_value &&other) noexcept : flametal_value() { *this = std::move(other); }

        flametal_value & operator=(const flametal_value &other) {
            if (this == &other) return *this;
            cleanup();
            ty = other.ty;
            switch (ty) {
            case VT_None: break;
            case VT_String: new (&str_value) string_ty(other.str_value); break;
            case VT_Boolean: new (&bool_value) bool_ty(other.bool_value); break;
            case VT_Int: new (&int_value) int_ty(other.int_value); break;
            case VT_Float: new (&float_value) float_ty(other.float_value); break;
            }
            return *this;
        }

        flametal_value & operator=(flametal_value &&other) noexcept {
            if (this == &other) return *this;
            cleanup();
            ty = other.ty;
            switch (ty) {
            case VT_None: break;
            case VT_String: new (&str_value) string_ty(std::move(other.str_value)); break;
            case VT_Boolean: new (&bool_value) bool_ty(std::move(other.bool_value)); break;
            case VT_Int: new (&int_value) int_ty(std::move(other.int_value)); break;
            case VT_Float: new (&float_value) float_ty(std::move(other.float_value)); break;
            }
            other.ty = VT_Int;
            new (&other.int_value) int_ty(0);
            return *this;
        }

        ~flametal_value() {
            cleanup();
        }

        std::string to_string() const {
            switch (ty) {
            case VT_None: break;
            case VT_String: return '"' + str_value + '"';
            case VT_Boolean: return bool_value ? "true" : "false";
            case VT_Int: return std::to_string(int_value);
            case VT_Float: return std::to_string(float_value);
            }
            return "";
        }

    private:
        void cleanup() {
            switch (ty) {
            case VT_None: break;
            case VT_String: str_value.~string_ty(); break;
            case VT_Boolean: bool_value.~bool_ty(); break;
            case VT_Int: int_value.~int_ty(); break;
            case VT_Float: float_value.~float_ty(); break;
            }
        }

    };

    bool operator==(const flametal_value& lhs, const flametal_value& rhs);

    flametal_value get_option(const std::string &path);
    void set_option(const std::string &path, flametal_value value);
    flametal_value get_cmdl_option(const std::string &path);
    void set_tmp_option(const std::string &path, flametal_value value);

    void help();
    void load(std::string &file);
    void save();
    bool changed();
    std::string shortDump();

    enum OptionGroup {
        OG_Config,
        OG_GameProgress,  // will be saved in separate file
        OG_HiddenState,  // will not be shown in gui
    };

    struct defined_flametal_option {
        const char *path;
        OptionGroup group;
        const char *help;

        flametal_value defaultValue;
        flametal_value &value;

        bool affected_by_command_line = false;

        defined_flametal_option(const char *path, OptionGroup group, const char *help, flametal_value &&defaultValue, flametal_value &value)
            : path(path), group(group), help(help), defaultValue(std::move(defaultValue)), value(value) {
        }
    };
    void iterateDefinedOptions(const std::function<void(defined_flametal_option&)> &cb);

    void _register_flametal_option(const char *path, OptionGroup group, const char *help, flametal_value &&defaultValue, flametal_value &value);

    template <typename T>
    struct define_flame_option {

        const char *path = NULL;
        flametal_value value;  // cache for faster access
        define_flame_option() = delete;
        define_flame_option(const char *path, OptionGroup group, const char *help, T defaultValue) : path(path) { _register_flametal_option(path, group, help, flametal_value(defaultValue), value); }

        // n template: failed requirement
        // 'std::is_same<bool, std::basic_string<char, std::char_traits<char>, std::allocator<char>>>::value'
        // ; 'enable_if' cannot be used to disable this declaration
        void set(T &value) requires std::is_same<T, std::string>::value {
            set_option(path, flametal_value(value));
        }
        void set(T value) requires (!std::is_same<T, std::string>::value) {
            set_option(path, flametal_value(value));
        }
        void set_tmp(T value) requires (!std::is_same<T, std::string>::value) {
            set_tmp_option(path, flametal_value(value));
        }

        T &get() requires std::is_same<T, std::string>::value {
            if (value.ty != VT_String) {
                printf("invalid option %s type. %d != %d\n", path, value.ty, VT_String);
                exit(-1);
            }
            return value.str_value;
        }
        _NODISCARD _CONSTEXPR23 T &operator*() noexcept requires std::is_same<T, std::string>::value {
            return get();
        }
        _NODISCARD _CONSTEXPR23 T *operator->() noexcept requires std::is_same<T, std::string>::value {
            return &get();
        }

        T get() const requires (!std::is_same<T, std::string>::value) {
            if constexpr (std::is_same_v<T, bool>) {
                if (value.ty != VT_Boolean) return false;
                return value.bool_value;
            }
            if constexpr (std::is_same_v<T, int>) {
                if (value.ty != VT_Int) {
                    printf("invalid option %s type. %d != %d\n", path, value.ty, VT_Int);
                    exit(-1);
                }
                return value.int_value;
            }
            if constexpr (std::is_same_v<T, float>) {
                if (value.ty != VT_Float) {
                    printf("invalid option %s type. %d != %d\n", path, value.ty, VT_Float);
                    exit(-1);
                }
                return value.float_value;
            }
            static_assert("invalid template type");
            return {};
        }
        _NODISCARD _CONSTEXPR23 T operator*() const noexcept requires (!std::is_same<T, std::string>::value) {
            return get();
        }

    };

}

#endif //FLAMETAL_CONFIG_H
