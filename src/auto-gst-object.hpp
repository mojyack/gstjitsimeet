#pragma once
#include <memory>

#include <gst/gstobject.h>

template <class T>
struct AutoGstObject {
    struct Deleter {
        auto operator()(T* const ptr) -> void {
            if(ptr != NULL) {
                gst_object_unref(ptr);
            }
        }
    };

    using Ptr = std::unique_ptr<T, Deleter>;

    Ptr ptr;

    auto get() -> T* {
        return ptr.get();
    }

    auto get() const -> T* {
        return ptr.get();
    }

    auto release() -> T* {
        return ptr.release();
    }

    operator bool() const {
        return ptr.get() != nullptr;
    }

    AutoGstObject() {}

    AutoGstObject(T* const ptr) : ptr(ptr) {
        if(ptr != NULL) {
            gst_object_ref_sink(ptr);
        }
    }
};

