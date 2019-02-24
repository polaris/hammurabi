#ifndef HAMMURABI_RNG_H
#define HAMMURABI_RNG_H

#include <random>

namespace hammurabi {

namespace detail {

template<typename IntType>
class rng {
public:
    rng(IntType a, IntType b);

    IntType operator()();

private:
    std::random_device device_;
    std::mt19937 generator_;
    std::uniform_int_distribution<IntType> distribution_;
};

template<typename IntType>
rng<IntType>::rng(IntType a, IntType b)
        : device_{}
        , generator_{device_()}
        , distribution_{a, b} {
}

template<typename IntType>
IntType rng<IntType>::operator()() {
    return distribution_(generator_);
}

}

}

#endif //HAMMURABI_RNG_H
