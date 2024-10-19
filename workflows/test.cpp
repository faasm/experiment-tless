#include "tless.h"

#include <cstdint>
#include <iostream>
#include <string>

int main()
{
    if (tless::checkChain("word-count", "splitter", 1)) {
        std::cout << "Chain is valid!" << std::endl;
    } else {
        std::cout << "Chain is invalid :-(" << std::endl;
    }

    return 0;
}
