/*
    nanogui/any.h -- A minimal C++14-friendly std::any replacement.

    macOS 10.12 (and other older platforms) ship a libc++ that does not
    expose <any> (or gates std::any behind a stricter deployment-target
    check). Rather than litter the codebase with platform ifdefs we
    provide a tiny header-only type-erased container with the subset of
    the std::any API that nanogui actually uses:

        any();
        any(const any&);            any(any&&) noexcept;
        any& operator=(const any&); any& operator=(any&&) noexcept;
        template<typename T> any(T&&);
        template<typename T> any& operator=(T&&);

        bool                  has_value() const noexcept;
        const std::type_info& type()      const noexcept;
        void                  reset()           noexcept;

        T  any_cast<T>(const any&);
        T  any_cast<T>(any&);
        T  any_cast<T>(any&&);
        T* any_cast<T>(any*)        noexcept;   // returns nullptr on type mismatch
        const T* any_cast<T>(const any*) noexcept;

    There is no Small-Buffer Optimization; every stored value is heap
    allocated via `new`. That's perfectly fine for the model values the
    DataGrid traffics in (strings, doubles, time_points, etc.) and keeps
    the implementation tiny and obviously correct.

    Implementation owes the usual "virtual holder" pattern that
    Boost.Any, std::any, and friends all use.
*/

#pragma once

#include <nanogui/common.h>
#include <type_traits>
#include <typeinfo>
#include <utility>

NAMESPACE_BEGIN(nanogui)

class NANOGUI_EXPORT bad_any_cast : public std::bad_cast {
public:
    const char* what() const noexcept override { return "nanogui::bad_any_cast"; }
};

class any {
public:
    any() noexcept : m_impl(nullptr) {}

    any(const any& other)
        : m_impl(other.m_impl ? other.m_impl->clone() : nullptr) {}

    any(any&& other) noexcept : m_impl(other.m_impl) {
        other.m_impl = nullptr;
    }

    template <typename T,
              typename = typename std::enable_if<
                  !std::is_same<typename std::decay<T>::type, any>::value
              >::type>
    any(T&& value)
        : m_impl(new holder<typename std::decay<T>::type>(std::forward<T>(value))) {}

    ~any() { delete m_impl; }

    any& operator=(const any& other) {
        if (this != &other) {
            base_holder* new_impl = other.m_impl ? other.m_impl->clone() : nullptr;
            delete m_impl;
            m_impl = new_impl;
        }
        return *this;
    }

    any& operator=(any&& other) noexcept {
        if (this != &other) {
            delete m_impl;
            m_impl = other.m_impl;
            other.m_impl = nullptr;
        }
        return *this;
    }

    template <typename T,
              typename = typename std::enable_if<
                  !std::is_same<typename std::decay<T>::type, any>::value
              >::type>
    any& operator=(T&& value) {
        base_holder* new_impl =
            new holder<typename std::decay<T>::type>(std::forward<T>(value));
        delete m_impl;
        m_impl = new_impl;
        return *this;
    }

    bool has_value() const noexcept { return m_impl != nullptr; }

    const std::type_info& type() const noexcept {
        return m_impl ? m_impl->type() : typeid(void);
    }

    void reset() noexcept {
        delete m_impl;
        m_impl = nullptr;
    }

    void swap(any& other) noexcept {
        base_holder* tmp = m_impl;
        m_impl = other.m_impl;
        other.m_impl = tmp;
    }

    // --- Internals (public so the any_cast<> free functions can see them) ---

    struct base_holder {
        virtual ~base_holder() = default;
        virtual const std::type_info& type() const noexcept = 0;
        virtual base_holder*           clone() const         = 0;
    };

    template <typename T>
    struct holder : base_holder {
        T value;
        template <typename U>
        explicit holder(U&& v) : value(std::forward<U>(v)) {}
        const std::type_info& type() const noexcept override { return typeid(T); }
        base_holder* clone() const override { return new holder(value); }
    };

    base_holder* m_impl;
};

// ---------------------------------------------------------------------------
// any_cast - by value, by reference, and by pointer
// ---------------------------------------------------------------------------

template <typename T>
T any_cast(const any& operand) {
    using U = typename std::remove_cv<
                  typename std::remove_reference<T>::type
              >::type;
    if (!operand.m_impl || operand.m_impl->type() != typeid(U))
        throw bad_any_cast();
    return static_cast<const any::holder<U>*>(operand.m_impl)->value;
}

template <typename T>
T any_cast(any& operand) {
    using U = typename std::remove_cv<
                  typename std::remove_reference<T>::type
              >::type;
    if (!operand.m_impl || operand.m_impl->type() != typeid(U))
        throw bad_any_cast();
    return static_cast<any::holder<U>*>(operand.m_impl)->value;
}

template <typename T>
T any_cast(any&& operand) {
    using U = typename std::remove_cv<
                  typename std::remove_reference<T>::type
              >::type;
    if (!operand.m_impl || operand.m_impl->type() != typeid(U))
        throw bad_any_cast();
    return std::move(static_cast<any::holder<U>*>(operand.m_impl)->value);
}

template <typename T>
const T* any_cast(const any* operand) noexcept {
    if (!operand || !operand->m_impl || operand->m_impl->type() != typeid(T))
        return nullptr;
    return &static_cast<const any::holder<T>*>(operand->m_impl)->value;
}

template <typename T>
T* any_cast(any* operand) noexcept {
    if (!operand || !operand->m_impl || operand->m_impl->type() != typeid(T))
        return nullptr;
    return &static_cast<any::holder<T>*>(operand->m_impl)->value;
}

NAMESPACE_END(nanogui)
