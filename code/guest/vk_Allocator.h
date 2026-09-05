#pragma once
#include <cstdlib>
#include <cstring>

class Allocator {
public:
    Allocator() = default;
    ~Allocator() = default;

    void* alloc(size_t wantedSize) {
        return malloc(wantedSize);
    }

    // Convenience function to allocate an array of objects of type T.
    template <class T>
    T* allocArray(size_t count) {
        size_t bytes = sizeof(T) * count;
        void* res = alloc(bytes);
        return static_cast<T*>(res);
    }

    char* strDup(const char* toCopy) {
        size_t bytes = strlen(toCopy) + 1;
        char* res = static_cast<char*>(alloc(bytes));
        if (res) {
            memcpy(res, toCopy, bytes);
        }
        return res;
    }

    char** strDupArray(const char* const* arrayToCopy, size_t count) {
        char** res = allocArray<char*>(count);
        if (res) {
            for (size_t i = 0; i < count; i++) {
                res[i] = strDup(arrayToCopy[i]);
            }
        }
        return res;
    }

    void* dupArray(const void* buf, size_t bytes) {
        void* res = alloc(bytes);
        if (res) {
            memcpy(res, buf, bytes);
        }
        return res;
    }

    void freeMem(void* ptr) {
        free(ptr);
    }
};
