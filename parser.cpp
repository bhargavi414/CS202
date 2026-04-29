#include <clang/AST/AST.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/Basic/LangOptions.h>

#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <vector>
#include <queue>
#include <set>
#include <map>
#include <fstream>
#include <sstream>
#include <cctype>
#include <functional>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory ToolCategory("cfg-tool options");

// -------- Expr → string --------
std::string exprToString(clang::Expr *expr) {
    std::string str;
    llvm::raw_string_ostream os(str);
    expr->printPretty(os, nullptr,
        clang::PrintingPolicy(clang::LangOptions()));
    return os.str();
}

// -------- CFG Block --------
struct CFGBlock {
    int id;
    std::vector<std::string> instructions;
    std::vector<int> successors;

    std::set<std::string> use;
    std::set<std::string> def;

    std::set<std::string> live_in;
    std::set<std::string> live_out;
};

// -------- CFG Visitor --------
class CFGVisitor : public RecursiveASTVisitor<CFGVisitor> {
public:
    std::vector<CFGBlock> blocks;
    int currentBlock = -1;
    int blockId = 0;

    int newBlock() {
        CFGBlock b;
        b.id = blockId++;
        blocks.push_back(b);
        return b.id;
    }

    void addEdge(int from, int to) {
        if (from >= 0)
            blocks[from].successors.push_back(to);
    }

    int startNewBlock() {
        int b = newBlock();
        addEdge(currentBlock, b);
        currentBlock = b;
        return b;
    }

    // -------- Extract variables --------
    void extractVars(clang::Expr *expr, std::set<std::string> &vars) {
        if (!expr) return;
        expr = expr->IgnoreImpCasts();

        if (auto *d = dyn_cast<DeclRefExpr>(expr)) {
            vars.insert(d->getNameInfo().getAsString());
        }

        for (auto *child : expr->children()) {
            if (auto *e = dyn_cast_or_null<Expr>(child)) {
                extractVars(e, vars);
            }
        }
    }

    // -------- Function --------
    bool VisitFunctionDecl(FunctionDecl *f) {
        if (!f->hasBody()) return true;

        currentBlock = newBlock();
        blocks[currentBlock].instructions.push_back("ENTRY");
        return true;
    }

    // -------- Statements --------
    bool VisitStmt(Stmt *s) {

        // --- DECL ---
        if (auto *declStmt = dyn_cast<DeclStmt>(s)) {
            for (auto *decl : declStmt->decls()) {
                if (auto *varDecl = dyn_cast<VarDecl>(decl)) {
                    if (varDecl->hasInit()) {
                        std::string var = varDecl->getNameAsString();
                        std::string rhs = exprToString(varDecl->getInit());

                        blocks[currentBlock].def.insert(var);
                        extractVars(varDecl->getInit(),
                                    blocks[currentBlock].use);

                        blocks[currentBlock].instructions.push_back(
                            var + " = " + rhs
                        );
                    }
                }
            }
        }

        // --- ASSIGN ---
        if (auto *bin = dyn_cast<BinaryOperator>(s)) {
            if (bin->isAssignmentOp()) {

                auto *lhs = dyn_cast<DeclRefExpr>(bin->getLHS());
                auto *rhsExpr = bin->getRHS();

                if (lhs) {
                    std::string var = lhs->getNameInfo().getAsString();
                    std::string rhs = exprToString(rhsExpr);

                    blocks[currentBlock].def.insert(var);
                    extractVars(rhsExpr, blocks[currentBlock].use);

                    blocks[currentBlock].instructions.push_back(
                        var + " = " + rhs
                    );
                }
            }
        }

        // --- RETURN ---
        if (auto *ret = dyn_cast<ReturnStmt>(s)) {
            extractVars(ret->getRetValue(), blocks[currentBlock].use);
            blocks[currentBlock].instructions.push_back("RETURN");

            currentBlock = newBlock(); // terminate
            return true;
        }

        // --- IF ---
        if (auto *ifStmt = dyn_cast<IfStmt>(s)) {

            int condBlock = currentBlock;
            blocks[condBlock].instructions.push_back("IF");

            int thenBlock = newBlock();
            addEdge(condBlock, thenBlock);

            int elseBlock = -1;
            if (ifStmt->getElse()) {
                elseBlock = newBlock();
                addEdge(condBlock, elseBlock);
            }

            int mergeBlock = newBlock();

            // THEN
            currentBlock = thenBlock;
            TraverseStmt(ifStmt->getThen());
            addEdge(currentBlock, mergeBlock);

            // ELSE
            if (elseBlock != -1) {
                currentBlock = elseBlock;
                TraverseStmt(ifStmt->getElse());
                addEdge(currentBlock, mergeBlock);
            } else {
                addEdge(condBlock, mergeBlock);
            }

            currentBlock = mergeBlock;
            return false;
        }

        // --- WHILE ---
        if (auto *whileStmt = dyn_cast<WhileStmt>(s)) {

            int condBlock = currentBlock;
            blocks[condBlock].instructions.push_back("WHILE");

            int bodyBlock = newBlock();
            int afterBlock = newBlock();

            addEdge(condBlock, bodyBlock);
            addEdge(condBlock, afterBlock);

            currentBlock = bodyBlock;
            TraverseStmt(whileStmt->getBody());

            addEdge(currentBlock, condBlock); // loop back

            currentBlock = afterBlock;
            return false;
        }

        return true;
    }

    // -------- Liveness --------
    void runLiveness() {
        bool changed = true;

        while (changed) {
            changed = false;

            for (int i = blocks.size() - 1; i >= 0; i--) {
                auto &b = blocks[i];

                auto old_in = b.live_in;
                auto old_out = b.live_out;

                b.live_out.clear();
                for (int s : b.successors) {
                    b.live_out.insert(blocks[s].live_in.begin(),
                                      blocks[s].live_in.end());
                }

                b.live_in = b.use;

                for (auto &v : b.live_out) {
                    if (b.def.find(v) == b.def.end()) {
                        b.live_in.insert(v);
                    }
                }

                if (b.live_in != old_in || b.live_out != old_out) {
                    changed = true;
                }
            }
        }
    }

    // -------- DCE --------
    void deadCodeElim() {
        for (auto &b : blocks) {
            std::vector<std::string> newInst;

            for (auto &inst : b.instructions) {
                if (inst.find("=") != std::string::npos) {
                    std::string var = inst.substr(0, inst.find("=") - 1);

                    if (b.live_out.find(var) == b.live_out.end()) {
                        continue;
                    }
                }
                newInst.push_back(inst);
            }

            b.instructions = newInst;
        }
    }

    // -------- Constant Folding --------
    void constantFolding() {
        for (auto &b : blocks) {
            for (auto &inst : b.instructions) {

                if (inst.find("=") != std::string::npos &&
                    inst.find("+") != std::string::npos) {

                    std::string lhs, eq, a, op, bval;
                    std::stringstream ss(inst);
                    ss >> lhs >> eq >> a >> op >> bval;

                    if (!a.empty() && !bval.empty() &&
                        isdigit(a[0]) && isdigit(bval[0])) {

                        int val = std::stoi(a) + std::stoi(bval);
                        inst = lhs + " = " + std::to_string(val);
                    }
                }
            }
        }
    }

    // -------- Constant Propagation --------
    void constantPropagation() {
        for (auto &b : blocks) {

            std::map<std::string, std::string> constMap;

            for (auto &inst : b.instructions) {

                if (inst.find("=") != std::string::npos) {
                    std::string lhs, eq, rhs;
                    std::stringstream ss(inst);
                    ss >> lhs >> eq >> rhs;

                    if (!rhs.empty() && isdigit(rhs[0])) {
                        constMap[lhs] = rhs;
                    } else if (constMap.count(rhs)) {
                        inst = lhs + " = " + constMap[rhs];
                    } else {
                        constMap.erase(lhs);
                    }
                }
            }
        }
    }

   // -------- Unreachable Code Removal --------
    void removeUnreachableBlocks() {
        std::set<int> visited;
        std::queue<int> q;

        if (blocks.empty()) return;
        q.push(0);

        while (!q.empty()) {
            int id = q.front(); q.pop();
            if (visited.count(id)) continue;
            visited.insert(id);
            for (int s : blocks[id].successors)
                q.push(s);
        }

        blocks.erase(
            std::remove_if(blocks.begin(), blocks.end(),
                [&](const CFGBlock &b) {
                    return visited.find(b.id) == visited.end();
                }),
            blocks.end()
        );
    }

    // -------- DOT --------
    void exportDOT(const std::string &file) {
        std::ofstream out(file);
        out << "digraph CFG {\n";

        for (auto &b : blocks) {
            out << "B" << b.id << " [label=\"B" << b.id << "\\n";

            for (auto &i : b.instructions)
                out << i << "\\n";

            out << "\"];\n";

            for (auto s : b.successors)
                out << "B" << b.id << " -> B" << s << ";\n";
        }

        out << "}\n";
    }
};

// -------- Consumer --------
class CFGConsumer : public ASTConsumer {
public:
    CFGVisitor visitor;

    void HandleTranslationUnit(ASTContext &context) override {
        visitor.TraverseDecl(context.getTranslationUnitDecl());

        visitor.exportDOT("before.dot");

        visitor.constantFolding();
        visitor.constantPropagation();
        visitor.removeUnreachableBlocks();
        visitor.runLiveness();
        visitor.deadCodeElim();

        visitor.exportDOT("after.dot");
    }
};

// -------- Frontend --------
class CFGAction : public ASTFrontendAction {
public:
    std::unique_ptr<ASTConsumer>
    CreateASTConsumer(CompilerInstance &, StringRef) override {
        return std::make_unique<CFGConsumer>();
    }
};

// -------- MAIN --------
int main(int argc, const char **argv) {
    auto ExpectedParser =
        CommonOptionsParser::create(argc, argv, ToolCategory);

    if (!ExpectedParser) return 1;

    auto &OptionsParser = ExpectedParser.get();

    ClangTool Tool(
        OptionsParser.getCompilations(),
        OptionsParser.getSourcePathList()
    );

    return Tool.run(
        newFrontendActionFactory<CFGAction>().get()
    );
}
