#pragma once


#include "cfg.h"

#include <set>
#include <map>
#include <tuple>
#include <vector>
#include <algorithm>


using Definition = std::tuple<std::string, int, int>;



class DataFlowAnalysis {
public:
    explicit DataFlowAnalysis(const CFG &cfg) : cfg(cfg) {}

    struct ReachingResult {
        std::map<int, std::set<Definition>> IN;
        std::map<int, std::set<Definition>> OUT;
    };

    ReachingResult reachingDefinitions() const {
        std::map<int, std::set<Definition>> gen;
        std::set<Definition> allDefs;

        for (auto &[bbId, bb] : cfg.blocks) {
            std::set<Definition> bbGen;
            for (int idx = 0; idx < (int)bb.instructions.size(); idx++) {
                auto defs = bb.instructions[idx].getDefs();
                for (auto &d : defs) {
                    Definition def{d, bbId, idx};
                    bbGen.insert(def);
                    allDefs.insert(def);
                }
            }
            gen[bbId] = bbGen;
        }

        std::map<int, std::set<Definition>> kill;
        for (auto &[bbId, bb] : cfg.blocks) {
            std::set<Definition> bbKill;
            for (auto &[var, bId, idx] : allDefs) {
                if (bId != bbId) {
                    bool blockDefinesVar = false;
                    for (auto &[gv, gb, gi] : gen[bbId]) {
                        if (gv == var) { blockDefinesVar = true; break; }
                    }
                    if (blockDefinesVar)
                        bbKill.insert({var, bId, idx});
                }
            }
            kill[bbId] = bbKill;
        }

        ReachingResult result;
        for (auto &[bbId, _] : cfg.blocks) {
            result.IN[bbId]  = {};
            result.OUT[bbId] = gen[bbId];
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (auto &[bbId, bb] : cfg.blocks) {
                auto oldOut = result.OUT[bbId];


                std::set<Definition> newIn;
                for (int pred : bb.predecessors)
                    newIn.insert(result.OUT[pred].begin(),
                                 result.OUT[pred].end());
                result.IN[bbId] = newIn;

                std::set<Definition> diff;
                std::set_difference(newIn.begin(), newIn.end(),
                                    kill[bbId].begin(), kill[bbId].end(),
                                    std::inserter(diff, diff.begin()));
                result.OUT[bbId] = gen[bbId];
                result.OUT[bbId].insert(diff.begin(), diff.end());

                if (result.OUT[bbId] != oldOut)
                    changed = true;
            }
        }
        return result;
    }


    struct LiveResult {
        std::map<int, std::set<std::string>> IN;
        std::map<int, std::set<std::string>> OUT;
    };

    LiveResult liveVariables() const {

        std::map<int, std::set<std::string>> USE, DEF;

        for (auto &[bbId, bb] : cfg.blocks) {
            std::set<std::string> useSet, defSet;
            for (auto &inst : bb.instructions) {
                auto uses = inst.getUses();
                auto defs = inst.getDefs();

                for (auto &u : uses)
                    if (defSet.find(u) == defSet.end())
                        useSet.insert(u);
                for (auto &d : defs)
                    if (useSet.find(d) == useSet.end())
                        defSet.insert(d);
            }
            USE[bbId] = useSet;
            DEF[bbId] = defSet;
        }


        LiveResult result;
        for (auto &[bbId, _] : cfg.blocks) {
            result.IN[bbId]  = {};
            result.OUT[bbId] = {};
        }

        bool changed = true;
        while (changed) {
            changed = false;

            for (auto it = cfg.blocks.rbegin(); it != cfg.blocks.rend(); ++it) {
                int bbId = it->first;
                const auto &bb = it->second;
                auto oldIn = result.IN[bbId];


                std::set<std::string> newOut;
                for (int succ : bb.successors)
                    newOut.insert(result.IN[succ].begin(),
                                 result.IN[succ].end());
                result.OUT[bbId] = newOut;


                std::set<std::string> diff;
                std::set_difference(newOut.begin(), newOut.end(),
                                    DEF[bbId].begin(), DEF[bbId].end(),
                                    std::inserter(diff, diff.begin()));
                result.IN[bbId] = USE[bbId];
                result.IN[bbId].insert(diff.begin(), diff.end());

                if (result.IN[bbId] != oldIn)
                    changed = true;
            }
        }
        return result;
    }


    struct UninitWarning {
        int blockId;
        int stmtIndex;
        std::string varName;
        std::string context;    
    };

    std::vector<UninitWarning> detectUninitializedVars() const {
        auto rdResult = reachingDefinitions();
        std::vector<UninitWarning> warnings;

        for (auto &[bbId, bb] : cfg.blocks) {

            auto reachingIn = rdResult.IN.count(bbId)
                ? rdResult.IN.at(bbId)
                : std::set<Definition>{};


            std::set<std::string> definedSoFar;
            for (auto &[var, blk, idx] : reachingIn)
                definedSoFar.insert(var);

            for (int i = 0; i < (int)bb.instructions.size(); i++) {
                auto &inst = bb.instructions[i];


                auto uses = inst.getUses();
                for (auto &u : uses) {
                    if (definedSoFar.find(u) == definedSoFar.end()) {
                        warnings.push_back({
                            bbId, i, u, inst.toString()
                        });
                    }
                }

                auto defs = inst.getDefs();
                for (auto &d : defs)
                    definedSoFar.insert(d);
            }
        }
        return warnings;
    }

private:
    const CFG &cfg;
};
