#ifndef HAMMURABI_OVERLOADED_H
#define HAMMURABI_OVERLOADED_H

namespace hammurabi {

// One visitor out of several lambdas: std::visit(overloaded{[](const a&) {...}, [](const b&) {...}}, v).
// A variant alternative without a matching lambda breaks the build instead of being ignored silently.
template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

} // namespace hammurabi

#endif // HAMMURABI_OVERLOADED_H
