#include "dag.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace tless::dag {
static DagChains parseChains(const std::vector<DagNode>& funcs)
{
    std::unordered_map<std::string, std::vector<std::string>> chains;

    for (const auto& func : funcs) {
        if (!func.chainsTo.empty()) {
            // Check if the chainsTo is a valid function name
            bool validChain = false;
            for (const auto& f : funcs) {
                if (f.name == func.chainsTo) {
                    validChain = true;
                    break;
                }
            }
            if (!validChain) {
                std::cerr << "tless(dag): invalid chainsTo reference: "
                          << func.chainsTo
                          << std::endl;
                throw std::runtime_error("tless(dag): invalid chainsTo reference: " + func.chainsTo);
            }

            chains[func.name].push_back(func.chainsTo);
        }
    }

    return chains;
}

// Implements the de-serialization protocol complementary to the serialization
// one that we implement in tlessctl/src/tasks/dag.rs
Dag deserialize(const std::vector<uint8_t>& data) {
    Dag dag;
    std::istringstream stream(std::string(data.begin(), data.end()));
    std::string line;
    DagNode currentNode;
    int fieldCount = 0;

    while (std::getline(stream, line)) {
        if (line.empty()) {
            if (fieldCount >= 2) {
                dag.funcs.push_back(currentNode);
                currentNode = DagNode();
                fieldCount = 0;
            }

            continue;
        }

        if (fieldCount == 0) {
            currentNode.name = line;
        } else if (fieldCount == 1) {
            currentNode.scale = line;
        } else if (fieldCount == 2) {
            currentNode.chainsTo = line;
        }

        fieldCount++;
    }

    // Last function without trailing newline
    if (fieldCount >= 2) {
        dag.funcs.push_back(currentNode);
    }

    dag.chains = parseChains(dag.funcs);

    return dag;
}

static void dfs(const Dag& dag, const std::string& func, std::vector<std::string>& result)
{
    for (const auto& [from, toList] : dag.chains) {
        auto itr = std::find(toList.begin(), toList.end(), func);
        if (itr != toList.end()) {
            dfs(dag, from, result);
            break;
        }
    }

    result.push_back(func);

    return;
}

std::vector<std::string> getCallChain(const Dag& dag, const std::string& func)
{
    std::vector<std::string> result;
    dfs(dag, func, result);

    return result;
}

std::vector<std::string> getFuncChainFromCertChain(const std::vector<uint8_t>& certChain)
{
    std::string certChainStr((char*) certChain.data(), certChain.size());
    return getFuncChainFromCertChain(certChainStr);
}

std::vector<std::string> getFuncChainFromCertChain(const std::string& certChain)
{
    std::vector<std::string> funcChain;
    std::string delimiter = ",";
    std::string stringCopy = certChain;

    size_t pos = 0;
    std::string token;
    while ((pos = certChain.find(delimiter)) != std::string::npos) {
        funcChain.push_back(stringCopy.substr(0, pos));
        stringCopy.erase(0, pos + delimiter.length());
    }
    funcChain.push_back(stringCopy);

    return funcChain;
}
}
