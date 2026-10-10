#ifndef HAMMURABI_ATTRIBUTES_H
#define HAMMURABI_ATTRIBUTES_H

// Marks a parameter that the constructed object (or return value) keeps referring to,
// so Clang can warn when it is bound to a temporary. Expands to nothing elsewhere.
#if __has_cpp_attribute(clang::lifetimebound)
#define HAMMURABI_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define HAMMURABI_LIFETIMEBOUND
#endif

#endif // HAMMURABI_ATTRIBUTES_H
