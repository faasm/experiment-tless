#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#define TLESS_CHAIN_GENESIS "G3N0SY5"

namespace tless::dag {

struct DagNode {
    // We assume function names in the DAG are unique
    std::string name;
    std::string scale;
    std::string chainsTo;
};

typedef std::unordered_map<std::string, std::vector<std::string>> DagChains;

struct Dag {
    std::vector<DagNode> funcs;
    DagChains chains;
};

Dag deserialize(const std::vector<uint8_t>& data);

// Given a function name, return the expected call chain according to the DAG
std::vector<std::string> getCallChain(const Dag& dag, const std::string& func);

std::vector<std::string> getFuncChainFromCertChain(const std::vector<uint8_t>& certChain);
std::vector<std::string> getFuncChainFromCertChain(const std::string& certChain);
}
