/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file AstTranslator.cpp
 *
 * Translator from AST to RAM structures.
 *
 ***********************************************************************/

#include "AstTranslator.h"
#include "AstArgument.h"
#include "AstAttribute.h"
#include "AstClause.h"
#include "AstFunctorDeclaration.h"
#include "AstIO.h"
#include "AstLiteral.h"
#include "AstNode.h"
#include "AstProgram.h"
#include "AstRelation.h"
#include "AstTranslationUnit.h"
#include "AstTypeEnvironmentAnalysis.h"
#include "AstUtils.h"
#include "AstVisitor.h"
#include "BinaryConstraintOps.h"
#include "DebugReport.h"
#include "Global.h"
#include "IODirectives.h"
#include "LogStatement.h"
#include "PrecedenceGraph.h"
#include "RamCondition.h"
#include "RamExpression.h"
#include "RamNode.h"
#include "RamOperation.h"
#include "RamProgram.h"
#include "RamRelation.h"
#include "RamStatement.h"
#include "RamTranslationUnit.h"
#include "SrcLocation.h"
#include "TypeSystem.h"
#include "Util.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <typeinfo>
#include <utility>
#include <vector>

namespace souffle {

class ErrorReport;
class SymbolTable;

std::unique_ptr<RamTupleElement> AstTranslator::makeRamTupleElement(const Location& loc) {
    return std::make_unique<RamTupleElement>(loc.identifier, loc.element);
}

void AstTranslator::makeIODirective(IODirectives& ioDirective, const AstRelation* rel,
        const std::string& filePath, const std::string& fileExt, const bool isIntermediate) {
    // set relation name correctly
    ioDirective.setRelationName(getRelationName(rel->getName()));

    // set a default IO type of file and a default filename if not supplied
    if (!ioDirective.has("IO")) {
        ioDirective.setIOType("file");
    }

    // load intermediate relations from correct files
    if (ioDirective.getIOType() == "file") {
        // all intermediate relations are given the default delimiter and have no headers
        if (isIntermediate) {
            ioDirective.set("intermediate", "true");
            ioDirective.set("delimiter", "\t");
            ioDirective.set("headers", "false");
        }

        // set filename by relation if not given, or if relation is intermediate
        if (!ioDirective.has("filename") || isIntermediate) {
            ioDirective.setFileName(ioDirective.getRelationName() + fileExt);
        }

        // if filename is not an absolute path, concat with cmd line facts directory
        if (ioDirective.getIOType() == "file" && ioDirective.getFileName().front() != '/') {
            ioDirective.setFileName(filePath + "/" + ioDirective.getFileName());
        }
    }
}

std::vector<IODirectives> AstTranslator::getInputIODirectives(
        const AstRelation* rel, std::string filePath, const std::string& fileExt) {
    std::vector<IODirectives> inputDirectives;

    for (const auto& current : rel->getLoads()) {
        IODirectives ioDirectives;
        for (const auto& currentPair : current->getIODirectiveMap()) {
            ioDirectives.set(currentPair.first, currentPair.second);
        }
        inputDirectives.push_back(ioDirectives);
    }

    if (inputDirectives.empty()) {
        inputDirectives.emplace_back();
    }

    const std::string inputFilePath = (filePath.empty()) ? Global::config().get("fact-dir") : filePath;
    const std::string inputFileExt = (fileExt.empty()) ? ".facts" : fileExt;

    const bool isIntermediate =
            (Global::config().has("engine") && inputFilePath == Global::config().get("output-dir") &&
                    inputFileExt == ".facts");

    for (auto& ioDirective : inputDirectives) {
        makeIODirective(ioDirective, rel, inputFilePath, inputFileExt, isIntermediate);
    }

    return inputDirectives;
}

std::vector<IODirectives> AstTranslator::getOutputIODirectives(
        const AstRelation* rel, std::string filePath, const std::string& fileExt) {
    std::vector<IODirectives> outputDirectives;

    // If stdout is requested then remove all directives from the datalog file.
    if (Global::config().get("output-dir") == "-") {
        bool hasOutput = false;
        for (const auto* current : rel->getStores()) {
            IODirectives ioDirectives;
            if (dynamic_cast<const AstPrintSize*>(current) != nullptr) {
                ioDirectives.setIOType("stdoutprintsize");
                outputDirectives.push_back(ioDirectives);
            } else if (!hasOutput) {
                hasOutput = true;
                ioDirectives.setIOType("stdout");
                ioDirectives.set("headers", "true");
                outputDirectives.push_back(ioDirectives);
            }
        }
    } else {
        for (const auto* current : rel->getStores()) {
            IODirectives ioDirectives;
            for (const auto& currentPair : current->getIODirectiveMap()) {
                ioDirectives.set(currentPair.first, currentPair.second);
            }
            outputDirectives.push_back(ioDirectives);
        }
    }

    if (outputDirectives.empty()) {
        outputDirectives.emplace_back();
    }

    const std::string outputFilePath = (filePath.empty()) ? Global::config().get("output-dir") : filePath;
    const std::string outputFileExt = (fileExt.empty()) ? ".csv" : fileExt;

    const bool isIntermediate =
            (Global::config().has("engine") && outputFilePath == Global::config().get("output-dir") &&
                    outputFileExt == ".facts");

    for (auto& ioDirective : outputDirectives) {
        makeIODirective(ioDirective, rel, outputFilePath, outputFileExt, isIntermediate);

        if (!ioDirective.has("attributeNames")) {
            std::string delimiter("\t");
            if (ioDirective.has("delimiter")) {
                delimiter = ioDirective.get("delimiter");
            }
            std::vector<std::string> attributeNames;
            for (unsigned int i = 0; i < rel->getArity(); i++) {
                attributeNames.push_back(rel->getAttribute(i)->getAttributeName());
            }

            if (Global::config().has("provenance")) {
                std::vector<std::string> originalAttributeNames(
                        attributeNames.begin(), attributeNames.end() - 1 - rel->numberOfHeightParameters());
                ioDirective.set("attributeNames", toString(join(originalAttributeNames, delimiter)));
            } else {
                ioDirective.set("attributeNames", toString(join(attributeNames, delimiter)));
            }
        }
    }

    return outputDirectives;
}

std::unique_ptr<RamRelationReference> AstTranslator::createRelationReference(const std::string name,
        const size_t arity, const size_t numberOfHeights, const std::vector<std::string> attributeNames,
        const std::vector<std::string> attributeTypeQualifiers, const RelationRepresentation representation) {
    const RamRelation* ramRel = ramProg->getRelation(name);
    if (ramRel == nullptr) {
        ramProg->addRelation(std::make_unique<RamRelation>(
                name, arity, numberOfHeights, attributeNames, attributeTypeQualifiers, representation));
        ramRel = ramProg->getRelation(name);
        assert(ramRel != nullptr && "cannot find relation");
    }
    return std::make_unique<RamRelationReference>(ramRel);
}

std::unique_ptr<RamRelationReference> AstTranslator::createRelationReference(
        const std::string name, const size_t arity, const size_t numberOfHeights) {
    return createRelationReference(name, arity, numberOfHeights, {}, {}, {});
}

std::unique_ptr<RamRelationReference> AstTranslator::translateRelation(const AstAtom* atom) {
    if (const auto rel = getAtomRelation(atom, program)) {
        return translateRelation(rel);
    } else {
        return createRelationReference(
                getRelationName(atom->getName()), atom->getArity(), getNumberOfHeights(atom, program));
    }
}

std::unique_ptr<RamRelationReference> AstTranslator::translateRelation(
        const AstRelation* rel, const std::string relationNamePrefix) {
    std::vector<std::string> attributeNames;
    std::vector<std::string> attributeTypeQualifiers;
    for (size_t i = 0; i < rel->getArity(); ++i) {
        attributeNames.push_back(rel->getAttribute(i)->getAttributeName());
        if (typeEnv) {
            attributeTypeQualifiers.push_back(
                    getTypeQualifier(typeEnv->getType(rel->getAttribute(i)->getTypeName())));
        }
    }

    return createRelationReference(relationNamePrefix + getRelationName(rel->getName()), rel->getArity(),
            rel->numberOfHeightParameters(), attributeNames, attributeTypeQualifiers,
            rel->getRepresentation());
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDeltaRelation(const AstRelation* rel) {
    return translateRelation(rel, "@delta_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateNewRelation(const AstRelation* rel) {
    return translateRelation(rel, "@new_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translatePreviousIndexedRelation(const AstRelation* rel) {
    return translateRelation(rel, "@indexed_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffMinusRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_minus@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffPlusRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_plus@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateNewDiffMinusRelation(const AstRelation* rel) {
    return translateRelation(rel, "@new_diff_minus@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateNewDiffPlusRelation(const AstRelation* rel) {
    return translateRelation(rel, "@new_diff_plus@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffMinusAppliedRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_minus_applied@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDeltaDiffMinusAppliedRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "@delta_diff_minus_applied@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffPlusAppliedRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_plus_applied@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffMinusCountRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_minus_count@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffPlusCountRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_plus_count@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDeltaDiffMinusCountRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "@delta_diff_minus_count@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDeltaDiffPlusCountRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "@delta_diff_plus_count@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDiffAppliedRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "diff_applied@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateTemporaryDeltaDiffAppliedRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "@temp_delta_diff_applied@_");
}

std::unique_ptr<RamRelationReference> AstTranslator::translateDeltaDiffAppliedRelation(
        const AstRelation* rel) {
    return translateRelation(rel, "@delta_diff_applied@_");
}

std::unique_ptr<RamExpression> AstTranslator::translateValue(
        const AstArgument* arg, const ValueIndex& index) {
    if (arg == nullptr) {
        return nullptr;
    }

    class ValueTranslator : public AstVisitor<std::unique_ptr<RamExpression>> {
        AstTranslator& translator;
        const ValueIndex& index;

    public:
        ValueTranslator(AstTranslator& translator, const ValueIndex& index)
                : translator(translator), index(index) {}

        std::unique_ptr<RamExpression> visitVariable(const AstVariable& var) override {
            assert(index.isDefined(var) && "variable not grounded");
            return makeRamTupleElement(index.getDefinitionPoint(var));
        }

        std::unique_ptr<RamExpression> visitUnnamedVariable(const AstUnnamedVariable& var) override {
            return std::make_unique<RamUndefValue>();
        }

        std::unique_ptr<RamExpression> visitConstant(const AstConstant& c) override {
            return std::make_unique<RamNumber>(c.getIndex());
        }

        std::unique_ptr<RamExpression> visitIntrinsicFunctor(const AstIntrinsicFunctor& inf) override {
            std::vector<std::unique_ptr<RamExpression>> values;
            for (const auto& cur : inf.getArguments()) {
                values.push_back(translator.translateValue(cur, index));
            }
            return std::make_unique<RamIntrinsicOperator>(inf.getFunction(), std::move(values));
        }

        std::unique_ptr<RamExpression> visitUserDefinedFunctor(const AstUserDefinedFunctor& udf) override {
            std::vector<std::unique_ptr<RamExpression>> values;
            for (const auto& cur : udf.getArguments()) {
                values.push_back(translator.translateValue(cur, index));
            }
            const AstFunctorDeclaration* decl = translator.program->getFunctorDeclaration(udf.getName());
            std::string type = decl->getType();
            return std::make_unique<RamUserDefinedOperator>(udf.getName(), type, std::move(values));
        }

        std::unique_ptr<RamExpression> visitCounter(const AstCounter&) override {
            return std::make_unique<RamAutoIncrement>();
        }

        std::unique_ptr<RamExpression> visitIterationNumber(const AstIterationNumber&) override {
            return std::make_unique<RamIterationNumber>();
        }

        std::unique_ptr<RamExpression> visitRecordInit(const AstRecordInit& init) override {
            std::vector<std::unique_ptr<RamExpression>> values;
            for (const auto& cur : init.getArguments()) {
                values.push_back(translator.translateValue(cur, index));
            }
            return std::make_unique<RamPackRecord>(std::move(values));
        }

        std::unique_ptr<RamExpression> visitAggregator(const AstAggregator& agg) override {
            // here we look up the location the aggregation result gets bound
            return translator.makeRamTupleElement(index.getAggregatorLocation(agg));
        }

        std::unique_ptr<RamExpression> visitSubroutineArgument(const AstSubroutineArgument& subArg) override {
            return std::make_unique<RamSubroutineArgument>(subArg.getNumber());
        }
    };

    return ValueTranslator(*this, index)(*arg);
}

std::unique_ptr<RamCondition> AstTranslator::translateConstraint(
        const AstLiteral* lit, const ValueIndex& index) {
    class ConstraintTranslator : public AstVisitor<std::unique_ptr<RamCondition>> {
        AstTranslator& translator;
        const ValueIndex& index;

    public:
        ConstraintTranslator(AstTranslator& translator, const ValueIndex& index)
                : translator(translator), index(index) {}

        /** for atoms */
        std::unique_ptr<RamCondition> visitAtom(const AstAtom&) override {
            return nullptr;  // covered already within the scan/lookup generation step
        }

        /** for binary relations */
        std::unique_ptr<RamCondition> visitBinaryConstraint(const AstBinaryConstraint& binRel) override {
            std::unique_ptr<RamExpression> valLHS = translator.translateValue(binRel.getLHS(), index);
            std::unique_ptr<RamExpression> valRHS = translator.translateValue(binRel.getRHS(), index);
            return std::make_unique<RamConstraint>(binRel.getOperator(),
                    translator.translateValue(binRel.getLHS(), index),
                    translator.translateValue(binRel.getRHS(), index));
        }

        /** for conjunctions */
        std::unique_ptr<RamCondition> visitConjunctionConstraint(const AstConjunctionConstraint& conjunctionConstraint) override {
            std::unique_ptr<RamCondition> valLHS = translator.translateConstraint(conjunctionConstraint.getLHS(), index);
            std::unique_ptr<RamCondition> valRHS = translator.translateConstraint(conjunctionConstraint.getRHS(), index);
            return std::make_unique<RamConjunction>(std::move(valLHS), std::move(valRHS));
        }

        /** for disjunctions */
        std::unique_ptr<RamCondition> visitDisjunctionConstraint(const AstDisjunctionConstraint& disjunctionConstraint) override {
            std::unique_ptr<RamCondition> valLHS = translator.translateConstraint(disjunctionConstraint.getLHS(), index);
            std::unique_ptr<RamCondition> valRHS = translator.translateConstraint(disjunctionConstraint.getRHS(), index);
            return std::make_unique<RamDisjunction>(std::move(valLHS), std::move(valRHS));
        }

        /** for provenance negation */
        std::unique_ptr<RamCondition> visitExistenceCheck(const AstExistenceCheck& exists) override {
            // get contained atom
            const AstAtom* atom = exists.getAtom();
            auto arity = atom->getArity();

            std::vector<std::unique_ptr<RamExpression>> values;

            for (size_t i = 0; i < arity; i++) {
                const auto& arg = atom->getArgument(i);
                values.push_back(translator.translateValue(arg, index));
            }

            // add constraint
            return std::make_unique<RamPositiveExistenceCheck>(
                    translator.translateRelation(atom), std::move(values));
        }

        /** for negations */
        std::unique_ptr<RamCondition> visitNegation(const AstNegation& neg) override {
            // get contained atom
            const auto* atom = neg.getAtom();
            auto arity = atom->getArity();
            auto numberOfHeightParameters = getNumberOfHeights(atom, translator.program);

            // account for extra provenance columns
            if (Global::config().has("provenance")) {
                // rule number
                arity -= 1;
                // height parameters
                arity -= numberOfHeightParameters;
            }

            std::vector<std::unique_ptr<RamExpression>> values;

            for (size_t i = 0; i < arity; i++) {
                values.push_back(translator.translateValue(atom->getArgument(i), index));
            }

            // we don't care about the provenance columns when doing the existence check
            if (Global::config().has("provenance")) {
                values.push_back(std::make_unique<RamUndefValue>());
                for (size_t h = 0; h < numberOfHeightParameters; h++) {
                    values.push_back(std::make_unique<RamUndefValue>());
                }
            }

            // add constraint
            if (arity > 0) {
                return std::make_unique<RamNegation>(std::make_unique<RamExistenceCheck>(
                        translator.translateRelation(atom), std::move(values)));
            } else {
                return std::make_unique<RamEmptinessCheck>(translator.translateRelation(atom));
            }
        }

        /** for provenance negation */
        std::unique_ptr<RamCondition> visitPositiveNegation(const AstPositiveNegation& neg) override {
            // get contained atom
            const AstAtom* atom = neg.getAtom();
            auto arity = atom->getArity();

            std::vector<std::unique_ptr<RamExpression>> values;

            for (size_t i = 0; i < arity; i++) {
                const auto& arg = atom->getArgument(i);
                values.push_back(translator.translateValue(arg, index));
            }

            // add constraint
            return std::make_unique<RamNegation>(std::make_unique<RamPositiveExistenceCheck>(
                    translator.translateRelation(atom), std::move(values)));
        }

        /** for provenance negation */
        std::unique_ptr<RamCondition> visitSubsumptionNegation(const AstSubsumptionNegation& neg) override {
            // get contained atom
            const AstAtom* atom = neg.getAtom();
            auto arity = atom->getArity();
            auto subsumptionArity = arity - neg.getNumSubsumptionFields();

            /*
            auto numberOfHeightParameters = getNumberOfHeights(atom, translator.program);

            // account for extra provenance columns
            if (Global::config().has("provenance")) {
                // rule number
                arity -= 1;
                // level numbers
                arity -= numberOfHeightParameters;
            }
            */

            std::vector<std::unique_ptr<RamExpression>> values;

            // for (size_t i = 0; i < subsumptionArity; i++) {
            for (size_t i = 0; i < arity; i++) {
                const auto& arg = atom->getArgument(i);
                // for (const auto& arg : atom->getArguments()) {
                values.push_back(translator.translateValue(arg, index));
            }

            /*
            // we don't care about the provenance columns when doing the existence check
            if (Global::config().has("provenance")) {
                values.push_back(std::make_unique<RamUndefValue>());
                // add the height annotation for provenanceNotExists
                for (size_t h = 0; h < numberOfHeightParameters; h++)
                    values.push_back(translator.translateValue(atom->getArgument(arity + h + 1), index));
            }
            */

            // add constraint
            return std::make_unique<RamNegation>(std::make_unique<RamSubsumptionExistenceCheck>(
                    translator.translateRelation(atom), std::move(values)));
        }
    };

    return ConstraintTranslator(*this, index)(*lit);
}

std::unique_ptr<AstClause> AstTranslator::ClauseTranslator::getReorderedClause(
        const AstClause& clause, const int version) const {
    const auto plan = clause.getExecutionPlan();

    // check whether there is an imposed order constraint
    if (plan != nullptr && plan->hasOrderFor(version)) {
        // get the imposed order
        const auto& order = plan->getOrderFor(version);

        // create a copy and fix order
        std::unique_ptr<AstClause> reorderedClause(clause.clone());

        // Change order to start at zero
        std::vector<unsigned int> newOrder(order.size());
        std::transform(order.begin(), order.end(), newOrder.begin(),
                [](unsigned int i) -> unsigned int { return i - 1; });

        // re-order atoms
        reorderedClause->reorderAtoms(newOrder);

        // clear other order and fix plan
        reorderedClause->clearExecutionPlan();
        reorderedClause->setFixedExecutionPlan();

        return reorderedClause;
    }

    return nullptr;
}

AstTranslator::ClauseTranslator::arg_list* AstTranslator::ClauseTranslator::getArgList(
        const AstNode* curNode, std::map<const AstNode*, std::unique_ptr<arg_list>>& nodeArgs) const {
    if (!nodeArgs.count(curNode)) {
        if (auto rec = dynamic_cast<const AstRecordInit*>(curNode)) {
            nodeArgs[curNode] = std::make_unique<arg_list>(rec->getArguments());
        } else if (auto atom = dynamic_cast<const AstAtom*>(curNode)) {
            nodeArgs[curNode] = std::make_unique<arg_list>(atom->getArguments());
        } else {
            assert(false && "node type doesn't have arguments!");
        }
    }
    return nodeArgs[curNode].get();
}

void AstTranslator::ClauseTranslator::indexValues(const AstNode* curNode,
        std::map<const AstNode*, std::unique_ptr<arg_list>>& nodeArgs,
        std::map<const arg_list*, int>& arg_level, RamRelationReference* relation) {
    arg_list* cur = getArgList(curNode, nodeArgs);
    for (size_t pos = 0; pos < cur->size(); ++pos) {
        // get argument
        auto& arg = (*cur)[pos];

        // check for variable references
        if (auto var = dynamic_cast<const AstVariable*>(arg)) {
            if (pos < relation->get()->getArity()) {
                valueIndex.addVarReference(
                        *var, arg_level[cur], pos, std::unique_ptr<RamRelationReference>(relation->clone()));
            } else {
                valueIndex.addVarReference(*var, arg_level[cur], pos);
            }
        }

        // check for nested records
        if (auto rec = dynamic_cast<const AstRecordInit*>(arg)) {
            // introduce new nesting level for unpack
            op_nesting.push_back(rec);
            arg_level[getArgList(rec, nodeArgs)] = level++;

            // register location of record
            valueIndex.setRecordDefinition(*rec, arg_level[cur], pos);

            // resolve nested components
            indexValues(rec, nodeArgs, arg_level, relation);
        }
    }
}

/** index values in rule */
void AstTranslator::ClauseTranslator::createValueIndex(const AstClause& clause) {
    for (const AstAtom* atom : clause.getAtoms()) {
        // std::map<const arg_list*, int> arg_level;
        std::map<const AstNode*, std::unique_ptr<arg_list>> nodeArgs;

        std::map<const arg_list*, int> arg_level;
        nodeArgs[atom] = std::make_unique<arg_list>(atom->getArguments());
        // the atom is obtained at the current level
        // increment nesting level for the atom
        arg_level[nodeArgs[atom].get()] = level++;
        op_nesting.push_back(atom);

        indexValues(atom, nodeArgs, arg_level, translator.translateRelation(atom).get());
    }

    // add aggregation functions
    visitDepthFirstPostOrder(clause, [&](const AstAggregator& cur) {
        // add each aggregator expression only once
        if (any_of(aggregators, [&](const AstAggregator* agg) { return *agg == cur; })) {
            return;
        }

        int aggLoc = level++;
        valueIndex.setAggregatorLocation(cur, Location({aggLoc, 0}));

        // bind aggregator variables to locations
        assert(nullptr != dynamic_cast<const AstAtom*>(cur.getBodyLiterals()[0]));
        const AstAtom& atom = static_cast<const AstAtom&>(*cur.getBodyLiterals()[0]);
        for (size_t pos = 0; pos < atom.getArguments().size(); ++pos) {
            if (const auto* var = dynamic_cast<const AstVariable*>(atom.getArgument(pos))) {
                valueIndex.addVarReference(*var, aggLoc, (int)pos, translator.translateRelation(&atom));
            }
        };

        // and remember aggregator
        aggregators.push_back(&cur);
    });
}

std::unique_ptr<RamOperation> AstTranslator::ClauseTranslator::createOperation(const AstClause& clause) {
    const auto head = clause.getHead();

    std::vector<std::unique_ptr<RamExpression>> values;
    for (AstArgument* arg : head->getArguments()) {
        values.push_back(translator.translateValue(arg, valueIndex));
    }

    std::unique_ptr<RamOperation> project =
            std::make_unique<RamProject>(translator.translateRelation(head), std::move(values));

    if (head->getArity() == 0) {
        project = std::make_unique<RamFilter>(
                std::make_unique<RamEmptinessCheck>(translator.translateRelation(head)), std::move(project));
    }

    // check existence for original tuple if we have provenance
    // only if we don't compile
    if (Global::config().has("provenance") &&
            ((!Global::config().has("compile") && !Global::config().has("dl-program") &&
                    !Global::config().has("generate")))) {
        size_t numberOfHeights = getNumberOfHeights(head, translator.program);

        auto arity = head->getArity() - 1 - numberOfHeights;

        std::vector<std::unique_ptr<RamExpression>> values;

        bool isVolatile = true;
        // add args for original tuple
        for (size_t i = 0; i < arity; i++) {
            auto arg = head->getArgument(i);

            // don't add counters
            visitDepthFirst(*arg, [&](const AstCounter& cur) { isVolatile = false; });
            values.push_back(translator.translateValue(arg, valueIndex));
        }

        // add unnamed args for provenance columns
        values.push_back(std::make_unique<RamUndefValue>());
        for (size_t h = 0; h < numberOfHeights; h++) {
            values.push_back(std::make_unique<RamUndefValue>());
        }

        if (isVolatile) {
            return std::make_unique<RamFilter>(
                    std::make_unique<RamNegation>(std::make_unique<RamExistenceCheck>(
                            translator.translateRelation(head), std::move(values))),
                    std::move(project));
        }
    }

    // build up insertion call
    return project;  // start with innermost
}

std::unique_ptr<RamOperation> AstTranslator::ProvenanceClauseTranslator::createOperation(
        const AstClause& clause) {
    std::vector<std::unique_ptr<RamExpression>> values;

    // get all values in the body
    for (AstLiteral* lit : clause.getBodyLiterals()) {
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            for (AstArgument* arg : atom->getArguments()) {
                values.push_back(translator.translateValue(arg, valueIndex));
            }
        } else if (auto neg = dynamic_cast<AstNegation*>(lit)) {
            for (AstArgument* arg : neg->getAtom()->getArguments()) {
                values.push_back(translator.translateValue(arg, valueIndex));
            }
        } else if (auto con = dynamic_cast<AstBinaryConstraint*>(lit)) {
            values.push_back(translator.translateValue(con->getLHS(), valueIndex));
            values.push_back(translator.translateValue(con->getRHS(), valueIndex));
        } else if (auto neg = dynamic_cast<AstSubsumptionNegation*>(lit)) {
            size_t numberOfHeights = getNumberOfHeights(neg->getAtom(), translator.program);

            // non provenance arguments
            for (size_t i = 0; i < neg->getAtom()->getArguments().size() - 1 - numberOfHeights; ++i) {
                auto arg = neg->getAtom()->getArguments()[i];
                values.push_back(translator.translateValue(arg, valueIndex));
            }

            // provenance annotation arguments
            for (size_t i = 0; i < numberOfHeights + 1; ++i)
                values.push_back(std::make_unique<RamNumber>(-1));
        }
    }

    return std::make_unique<RamSubroutineReturnValue>(std::move(values));
}

std::unique_ptr<RamCondition> AstTranslator::ClauseTranslator::createCondition(
        const AstClause& originalClause) {
    const auto head = originalClause.getHead();

    // add stopping criteria for nullary relations
    // (if it contains already the null tuple, don't re-compute)
    if (head->getArity() == 0) {
        return std::make_unique<RamEmptinessCheck>(translator.translateRelation(head));
    }
    return nullptr;
}

std::unique_ptr<RamCondition> AstTranslator::ProvenanceClauseTranslator::createCondition(
        const AstClause& originalClause) {
    return nullptr;
}

/** generate RAM code for a clause */
std::unique_ptr<RamStatement> AstTranslator::ClauseTranslator::translateClause(
        const AstClause& clause, const AstClause& originalClause, const int version) {
    if (auto reorderedClause = getReorderedClause(clause, version)) {
        // translate reordered clause
        return translateClause(*reorderedClause, originalClause, version);
    }

    // get extract some details
    const AstAtom* head = clause.getHead();

    // handle facts
    if (clause.isFact()) {
        // translate arguments
        std::vector<std::unique_ptr<RamExpression>> values;
        for (auto& arg : head->getArguments()) {
            values.push_back(translator.translateValue(arg, ValueIndex()));
        }

        // create a fact statement
        return std::make_unique<RamFact>(translator.translateRelation(head), std::move(values));
    }

    // the rest should be rules
    assert(clause.isRule());

    createValueIndex(clause);

    // -- create RAM statement --

    std::unique_ptr<RamOperation> op = createOperation(clause);

    /* add equivalence constraints imposed by variable binding */
    for (const auto& cur : valueIndex.getVariableReferences()) {
        // the first appearance
        const Location& first = *cur.second.begin();
        // all other appearances
        for (const Location& loc : cur.second) {
            if (first != loc && !valueIndex.isAggregator(loc.identifier)) {
                op = std::make_unique<RamFilter>(
                        std::make_unique<RamConstraint>(
                                BinaryConstraintOp::EQ, makeRamTupleElement(first), makeRamTupleElement(loc)),
                        std::move(op));
            }
        }
    }

    /* add conditions caused by atoms, negations, and binary relations */
    for (const auto& lit : clause.getBodyLiterals()) {
        if (auto condition = translator.translateConstraint(lit, valueIndex)) {
            op = std::make_unique<RamFilter>(std::move(condition), std::move(op));
        }
    }

    // add aggregator conditions
    size_t curLevel = op_nesting.size() - 1;
    for (auto it = op_nesting.rbegin(); it != op_nesting.rend(); ++it, --curLevel) {
        const AstNode* cur = *it;

        if (const auto* atom = dynamic_cast<const AstAtom*>(cur)) {
            // add constraints
            for (size_t pos = 0; pos < atom->argSize(); ++pos) {
                if (auto* agg = dynamic_cast<AstAggregator*>(atom->getArgument(pos))) {
                    auto loc = valueIndex.getAggregatorLocation(*agg);
                    op = std::make_unique<RamFilter>(std::make_unique<RamConstraint>(BinaryConstraintOp::EQ,
                                                             std::make_unique<RamTupleElement>(curLevel, pos),
                                                             makeRamTupleElement(loc)),
                            std::move(op));
                }
            }
        }
    }

    // add aggregator levels
    --level;
    for (auto it = aggregators.rbegin(); it != aggregators.rend(); ++it, --level) {
        const AstAggregator* cur = *it;

        // translate aggregation function
        AggregateFunction fun = souffle::MIN;
        switch (cur->getOperator()) {
            case AstAggregator::min:
                fun = souffle::MIN;
                break;
            case AstAggregator::max:
                fun = souffle::MAX;
                break;
            case AstAggregator::count:
                fun = souffle::COUNT;
                break;
            case AstAggregator::sum:
                fun = souffle::SUM;
                break;
        }

        // condition for aggregate and helper function to add terms
        std::unique_ptr<RamCondition> aggCondition;
        auto addAggCondition = [&](std::unique_ptr<RamCondition>& arg) {
            if (aggCondition == nullptr) {
                aggCondition = std::move(arg);
            } else {
                aggCondition = std::make_unique<RamConjunction>(std::move(aggCondition), std::move(arg));
            }
        };

        // translate constraints of sub-clause
        for (const auto& lit : cur->getBodyLiterals()) {
            if (auto newCondition = translator.translateConstraint(lit, valueIndex)) {
                addAggCondition(newCondition);
            }
        }

        // get the first predicate of the sub-clause
        // NB: at most one atom is permitted in a sub-clause
        const AstAtom* atom = nullptr;
        for (const auto& lit : cur->getBodyLiterals()) {
            if (atom == nullptr) {
                atom = dynamic_cast<const AstAtom*>(lit);
            } else {
                assert(dynamic_cast<const AstAtom*>(lit) != nullptr &&
                        "Unsupported complex aggregation body encountered!");
            }
        }

        // translate arguments's of atom (if exists) to conditions
        if (atom != nullptr) {
            for (size_t pos = 0; pos < atom->argSize(); ++pos) {
                // variable bindings are issued differently since we don't want self
                // referential variable bindings
                if (const auto* var = dynamic_cast<const AstVariable*>(atom->getArgument(pos))) {
                    for (const Location& loc :
                            valueIndex.getVariableReferences().find(var->getName())->second) {
                        if (level != loc.identifier || (int)pos != loc.element) {
                            std::unique_ptr<RamCondition> newCondition = std::make_unique<RamConstraint>(
                                    BinaryConstraintOp::EQ, makeRamTupleElement(loc),
                                    std::make_unique<RamTupleElement>(level, pos));
                            addAggCondition(newCondition);
                            break;
                        }
                    }
                } else if (atom->getArgument(pos) != nullptr) {
                    std::unique_ptr<RamExpression> value =
                            translator.translateValue(atom->getArgument(pos), valueIndex);
                    if (value != nullptr && !isRamUndefValue(value.get())) {
                        std::unique_ptr<RamCondition> newCondition =
                                std::make_unique<RamConstraint>(BinaryConstraintOp::EQ,
                                        std::make_unique<RamTupleElement>(level, pos), std::move(value));
                        addAggCondition(newCondition);
                    }
                }
            }
        }

        // translate aggregate expression
        std::unique_ptr<RamExpression> expr =
                translator.translateValue(cur->getTargetExpression(), valueIndex);
        if (expr == nullptr) {
            expr = std::make_unique<RamUndefValue>();
        }

        if (aggCondition == nullptr) {
            aggCondition = std::make_unique<RamTrue>();
        }

        // add Ram-Aggregation layer
        std::unique_ptr<RamAggregate> aggregate = std::make_unique<RamAggregate>(std::move(op), fun,
                translator.translateRelation(atom), std::move(expr), std::move(aggCondition), level);
        op = std::move(aggregate);
    }

    // build operation bottom-up
    while (!op_nesting.empty()) {
        // get next operator
        const AstNode* cur = op_nesting.back();
        op_nesting.pop_back();

        // get current nesting level
        auto level = op_nesting.size();

        if (const auto* atom = dynamic_cast<const AstAtom*>(cur)) {
            // add constraints
            for (size_t pos = 0; pos < atom->argSize(); ++pos) {
                if (auto* c = dynamic_cast<AstConstant*>(atom->getArgument(pos))) {
                    op = std::make_unique<RamFilter>(std::make_unique<RamConstraint>(BinaryConstraintOp::EQ,
                                                             std::make_unique<RamTupleElement>(level, pos),
                                                             std::make_unique<RamNumber>(c->getIndex())),
                            std::move(op));
                }
            }

            // check whether all arguments are unnamed variables
            bool isAllArgsUnnamed = true;
            for (auto* argument : atom->getArguments()) {
                if (dynamic_cast<AstUnnamedVariable*>(argument) == nullptr) {
                    isAllArgsUnnamed = false;
                }
            }

            // add check for emptiness for an atom
            op = std::make_unique<RamFilter>(
                    std::make_unique<RamNegation>(
                            std::make_unique<RamEmptinessCheck>(translator.translateRelation(atom))),
                    std::move(op));

            // add a scan level
            if (atom->getArity() != 0 && !isAllArgsUnnamed) {
                if (head->getArity() == 0) {
                    op = std::make_unique<RamBreak>(
                            std::make_unique<RamNegation>(
                                    std::make_unique<RamEmptinessCheck>(translator.translateRelation(head))),
                            std::move(op));
                }
                if (Global::config().has("profile")) {
                    std::stringstream ss;
                    ss << head->getName();
                    ss.str("");
                    ss << "@frequency-atom" << ';';
                    ss << originalClause.getHead()->getName() << ';';
                    ss << version << ';';
                    ss << stringify(toString(clause)) << ';';
                    ss << stringify(toString(*atom)) << ';';
                    ss << stringify(toString(originalClause)) << ';';
                    ss << level << ';';
                    op = std::make_unique<RamScan>(
                            translator.translateRelation(atom), level, std::move(op), ss.str());
                } else {
                    op = std::make_unique<RamScan>(translator.translateRelation(atom), level, std::move(op));
                }
            }

            // TODO: support constants in nested records!
        } else if (const auto* rec = dynamic_cast<const AstRecordInit*>(cur)) {
            // add constant constraints
            for (size_t pos = 0; pos < rec->getArguments().size(); ++pos) {
                if (AstConstant* c = dynamic_cast<AstConstant*>(rec->getArguments()[pos])) {
                    op = std::make_unique<RamFilter>(std::make_unique<RamConstraint>(BinaryConstraintOp::EQ,
                                                             std::make_unique<RamTupleElement>(level, pos),
                                                             std::make_unique<RamNumber>(c->getIndex())),
                            std::move(op));
                } else if (AstFunctor* func = dynamic_cast<AstFunctor*>(rec->getArguments()[pos])) {
                    op = std::make_unique<RamFilter>(std::make_unique<RamConstraint>(BinaryConstraintOp::EQ,
                                                             std::make_unique<RamTupleElement>(level, pos),
                                                             translator.translateValue(func, valueIndex)),
                            std::move(op));
                }
            }

            // add an unpack level
            const Location& loc = valueIndex.getDefinitionPoint(*rec);
            op = std::make_unique<RamUnpackRecord>(
                    std::move(op), level, makeRamTupleElement(loc), rec->getArguments().size());
        } else {
            assert(false && "Unsupported AST node for creation of scan-level!");
        }
    }

    /* generate the final RAM Insert statement */
    std::unique_ptr<RamCondition> cond = createCondition(originalClause);
    if (cond != nullptr) {
        return std::make_unique<RamQuery>(std::make_unique<RamFilter>(std::move(cond), std::move(op)));
    } else {
        return std::make_unique<RamQuery>(std::move(op));
    }
}

/* utility for appending statements */
void AstTranslator::appendStmt(std::unique_ptr<RamStatement>& stmtList, std::unique_ptr<RamStatement> stmt) {
    if (stmt) {
        if (stmtList) {
            RamSequence* stmtSeq;
            if ((stmtSeq = dynamic_cast<RamSequence*>(stmtList.get()))) {
                stmtSeq->add(std::move(stmt));
            } else {
                stmtList = std::make_unique<RamSequence>(std::move(stmtList), std::move(stmt));
            }
        } else {
            stmtList = std::move(stmt);
        }
    }
}

/** generate RAM code for a non-recursive relation */
std::unique_ptr<RamStatement> AstTranslator::translateNonRecursiveRelation(
        const AstRelation& rel, const RecursiveClauses* recursiveClauses) {
    /* start with an empty sequence */
    std::unique_ptr<RamStatement> res;

    // the ram table reference
    std::unique_ptr<RamRelationReference> rrel = translateRelation(&rel);

    // utility to convert a list of AstConstraints to a disjunction
    auto toAstDisjunction = [&](std::vector<AstConstraint*> constraints) -> std::unique_ptr<AstConstraint> {
        std::unique_ptr<AstConstraint> result;
        for (const auto& cur : constraints) {
            if (result == nullptr) {
                result = std::unique_ptr<AstConstraint>(cur->clone());
            } else {
                result = std::make_unique<AstDisjunctionConstraint>(std::move(result), std::unique_ptr<AstConstraint>(cur->clone()));
            }
            std::cout << "cur: " << *cur << " res: " << *result << std::endl;
        }
        return result;
    };

    /* iterate over all clauses that belong to the relation */
    for (AstClause* clause : rel.getClauses()) {
        // skip recursive rules
        if (recursiveClauses->recursive(clause)) {
            continue;
        }

        if (Global::config().has("incremental")) {
            // store previous count and current count to determine if the rule is insertion or deletion
            auto prevCount = clause->getHead()->getArgument(rel.getArity() - 2);
            auto curCount = clause->getHead()->getArgument(rel.getArity() - 1);

            // these should not be nullptrs
            auto prevCountNum = dynamic_cast<AstNumberConstant*>(prevCount);
            auto curCountNum = dynamic_cast<AstNumberConstant*>(curCount);

            if (prevCountNum == nullptr || curCountNum == nullptr) {
                std::cerr << "count annotations are not intialized!" << std::endl;
            }

            nameUnnamedVariables(clause);

            // check if this clause is re-inserting hidden tuples
            bool isReinsertionRule = (*prevCountNum == AstNumberConstant(1) && *curCountNum == AstNumberConstant(1));
            bool isInsertionRule = (*curCountNum == AstNumberConstant(1)) && !isReinsertionRule;
            bool isDeletionRule = (*curCountNum == AstNumberConstant(-1));

            const auto& atoms = clause->getAtoms();
            const auto& negations = clause->getNegations();

            std::unique_ptr<RamStatement> rule;

            if (isReinsertionRule) {
                /*
                std::unique_ptr<AstClause> cl(clause->clone());
                cl->getHead()->setName(translateDiffPlusRelation(&rel)->get()->getName());

                const auto& atoms = cl->getAtoms();
                for (size_t i = 0; i < atoms.size(); i++) {
                    // cl->getAtoms()[i]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                    std::unique_ptr<AstClause> r1(cl->clone());
                    r1->getAtoms()[i]->setName(translateDiffPlusRelation(getAtomRelation(atoms[i], program))->get()->getName());

                    // ensure that the previous count is positive so we are actually re-inserting
                    // r1->getAtoms()[i]->setArgument(r1->getAtoms()[i]->getArity() - 2, std::make_unique<AstNumberConstant>(1));
                    r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ, std::unique_ptr<AstArgument>(r1->getAtoms()[i]->getArgument(r1->getAtoms()[i]->getArity() - 2)->clone()), std::make_unique<AstNumberConstant>(1)));

                    for (size_t j = i + 1; j < atoms.size(); j++) {
                        r1->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                    }

                    // we don't want to consider tuples which are to be deleted
                    for (size_t j = 0; j < i; j++) {
                        auto& atomJ = atoms[j];
                        r1->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atomJ, program))->get()->getName());

                        / *
                        // add a negation to the rule stating that the tuple shouldn't be inserted
                        // this prevents double counting, e.g., if we have:
                        // A(x, y) :- B(x, y), diff+C(x, y).
                        // A(x, y) :- diff+B(x, y), diff+appliedC(x, y).
                        // then we would double insert if we insert B(1, 2) and also C(1, 2)
                        auto noAdditionNegation = atomJ->clone();
                        noAdditionNegation->setName(translateDiffPlusRelation(getAtomRelation(atomJ, program))->get()->getName());
                        noAdditionNegation->setArgument(noAdditionNegation->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        noAdditionNegation->setArgument(noAdditionNegation->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                        noAdditionNegation->setArgument(noAdditionNegation->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        r1->addToBody(std::make_unique<AstNegation>(std::unique_ptr<AstAtom>(noAdditionNegation)));
                        * /
                    }

                    // a tuple should only be reinserted if that tuple is deleted
                    auto deletedTuple = clause->getHead()->clone();
                    deletedTuple->setName(translateDiffMinusRelation(&rel)->get()->getName());
                    deletedTuple->setArgument(deletedTuple->getArity() - 1, std::make_unique<AstVariable>("@deleted_count"));
                    deletedTuple->setArgument(deletedTuple->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                    deletedTuple->setArgument(deletedTuple->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                    r1->addToBody(std::unique_ptr<AstAtom>(deletedTuple));
                    // cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE, std::make_unique<AstVariable>("@deleted_count"), std::make_unique<AstNumberConstant>(0)));

                    // reorder cl so that the deletedTuple atom is evaluated first
                    std::vector<unsigned int> reordering;
                    reordering.push_back(atoms.size());
                    for (unsigned int j = 0; j < atoms.size(); j++) {
                        reordering.push_back(j);
                    }
                    std::cout << "non-recursive re-insertion" << *cl << " reorder: " << reordering << std::endl;
                    r1->reorderAtoms(reordering);

                    std::cout << *r1 << std::endl;

                    // translate clause
                    std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*r1, *clause);

                    // add logging
                    if (Global::config().has("profile")) {
                        const std::string& relationName = toString(rel.getName());
                        const SrcLocation& srcLocation = r1->getSrcLoc();
                        const std::string clauseText = stringify(toString(*r1));
                        const std::string logTimerStatement =
                                LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
                        const std::string logSizeStatement =
                                LogStatement::nNonrecursiveRule(relationName, srcLocation, clauseText);
                        rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
                    }

                    // add debug info
                    std::ostringstream ds;
                    ds << toString(*r1) << "\nin file ";
                    ds << r1->getSrcLoc();
                    rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                    appendStmt(res, std::move(rule));
                }
                    */
            } else {
                if (isInsertionRule) {
                    for (size_t i = 0; i < atoms.size(); i++) {
                        // an insertion rule should look as follows:
                        // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                        auto cl = clause->clone();

                        // set the head of the rule to be the diff relation
                        cl->getHead()->setName(translateDiffPlusRelation(&rel)->get()->getName());

                        // ensure i-th tuple did not exist previously, this prevents double insertions
                        auto noPrevious = atoms[i]->clone();
                        noPrevious->setName(translateRelation(getAtomRelation(atoms[i], program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                        // the current version of the rule should have diff_plus_count in the i-th position
                        cl->getAtoms()[i]->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));

                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));

                        // atoms before the i-th position should not fulfill the conditions for incremental insertion, otherwise we will have double insertions
                        for (size_t j = 0; j < i; j++) {
                            cl->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());

                            // ensure tuple is not actually inserted
                            auto curAtom = atoms[j]->clone();
                            curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[j], program))->get()->getName());
                            curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                            curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                            // also ensure tuple existed previously
                            auto noPrevious = atoms[j]->clone();
                            noPrevious->setName(translateRelation(getAtomRelation(atoms[j], program))->get()->getName());
                            noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                            noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                            // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                            cl->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                        }

                        for (size_t j = i + 1; j < atoms.size(); j++) {
                            cl->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                        }

                        // process negations
                        for (size_t j = 0; j < negations.size(); j++) {
                            // each negation needs to have either not existed, or be deleted
                            // for the non-existence case, we use a positive negation instead
                            auto negatedAtom = negations[j]->getAtom()->clone();
                            negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                            cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                        }

                        cl->clearNegations();

                        std::cout << "non-recursive: " << *cl << std::endl;

                        // translate cl
                        std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*cl, *cl);

                        // add logging
                        if (Global::config().has("profile")) {
                            const std::string& relationName = toString(rel.getName());
                            const SrcLocation& srcLocation = cl->getSrcLoc();
                            const std::string clText = stringify(toString(*cl));
                            const std::string logTimerStatement =
                                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clText);
                            const std::string logSizeStatement =
                                    LogStatement::nNonrecursiveRule(relationName, srcLocation, clText);
                            rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                    logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
                        }

                        // add debug info
                        std::ostringstream ds;
                        ds << toString(*cl) << "\nin file ";
                        ds << cl->getSrcLoc();
                        rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                        // add rule to result
                        appendStmt(res, std::move(rule));
                    }

                    // TODO: if there is a negation, then we need to add a version of the rule which applies when only the negations apply
                    for (size_t i = 0; i < negations.size(); i++) {
                        // an insertion rule should look as follows:
                        // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                        auto cl = clause->clone();

                        // set the head of the rule to be the diff relation
                        cl->getHead()->setName(translateDiffPlusRelation(&rel)->get()->getName());

                        // clone the i-th negation's atom
                        // each negation needs to have either not existed, or be deleted
                        // for the non-existence case, we use a positive negation instead
                        auto negatedAtom = negations[i]->getAtom()->clone();
                        negatedAtom->setName(translateDiffMinusCountRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                        negatedAtom->setArgument(negatedAtom->getArity() - 1, std::make_unique<AstNumberConstant>(0));
                        negatedAtom->setArgument(negatedAtom->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                        cl->addToBody(std::unique_ptr<AstAtom>(negatedAtom));

                        /*
                        // add constraints saying that the i-th negation atom should be deleted
                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));

                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));
                                    */

                        // prevent double insertions across epochs
                        auto noPrevious = negations[i]->getAtom()->clone();
                        noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(noPrevious, program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                        // atoms before the i-th position should not fulfill the conditions for incremental deletion, otherwise we will have double insertions
                        for (size_t j = 0; j < i; j++) {
                            // cl->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                            // ensure tuple is not actually inserted
                            auto curAtom = negations[j]->getAtom()->clone();
                            curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(curAtom, program))->get()->getName());

                            curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                            curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                            // also ensure tuple existed previously
                            auto noPrevious = negations[j]->getAtom()->clone();
                            noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(noPrevious, program))->get()->getName());
                            noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                            noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                            // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                            cl->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                        }

                        // process negations
                        for (size_t j = 0; j < negations.size(); j++) {
                            // each negation needs to have either not existed, or be deleted
                            // for the non-existence case, we use a positive negation instead
                            auto negatedAtom = negations[j]->getAtom()->clone();
                            negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                            cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                        }

                        // the base relation for addition should be diff_applied, so translate each positive atom to the diff applied version
                        for (size_t j = 0; j < atoms.size(); j++) {
                            cl->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                        }


                        cl->clearNegations();

                        std::cout << "non-recursive: " << *cl << std::endl;

                        // translate cl
                        std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*cl, *cl);

                        // add logging
                        if (Global::config().has("profile")) {
                            const std::string& relationName = toString(rel.getName());
                            const SrcLocation& srcLocation = cl->getSrcLoc();
                            const std::string clText = stringify(toString(*cl));
                            const std::string logTimerStatement =
                                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clText);
                            const std::string logSizeStatement =
                                    LogStatement::nNonrecursiveRule(relationName, srcLocation, clText);
                            rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                    logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
                        }

                        // add debug info
                        std::ostringstream ds;
                        ds << toString(*cl) << "\nin file ";
                        ds << cl->getSrcLoc();
                        rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                        // add rule to result
                        appendStmt(res, std::move(rule));
                    }



                    /*

                    // add a constraint saying that at least one body tuple should be in the diff_plus version of the relation
                    std::vector<AstConstraint*> existsDiffPlus;
                    for (size_t i = 0; i < atoms.size(); i++) {
                        // ensure tuple is actually inserted
                        auto curAtom = atoms[i]->clone();
                        curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                        curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                        // prevent double insertions across epochs
                        auto noPrevious = atoms[i]->clone();
                        noPrevious->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        existsDiffPlus.push_back(new AstConjunctionConstraint(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious))));

                        // the base relation for insertion should be diff_applied
                        clause->getAtoms()[i]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                    }

                    // process negations
                    for (size_t i = 0; i < negations.size(); i++) {
                        // each negation needs to have either not existed, or be deleted
                        // for the non-existence case, we use a positive negation instead
                        auto negatedAtom = negations[i]->getAtom()->clone();
                        negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                        clause->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));

                        // for the deletion case, we check if the atom is in diff minus
                        auto curAtom = negations[i]->getAtom()->clone();
                        curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(curAtom, program))->get()->getName());

                        curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                        // prevent double insertions across epochs
                        auto noPrevious = negations[i]->getAtom()->clone();
                        noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(noPrevious, program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        existsDiffPlus.push_back(new AstConjunctionConstraint(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious))));
                    }

                    // clear negations as they are now adapted for the incremental case
                    clause->clearNegations();

                    if (existsDiffPlus.size() > 0) {
                        clause->addToBody(toAstDisjunction(existsDiffPlus));
                    }
                    */



                    /*
                    // set atom i to use the diff relation
                    r1->getAtoms()[i]->setName(translateDiffPlusCountRelation(getAtomRelation(atom, program))->get()->getName());

                    // we don't want to consider tuples which are to be deleted
                    for (size_t j = 0; j < i; j++) {
                        auto& atomJ = atoms[j];
                        r1->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atomJ, program))->get()->getName());
                        / *

                        // add a negation to the rule stating that the tuple shouldn't be inserted
                        // this prevents double counting, e.g., if we have:
                        // A(x, y) :- B(x, y), diff+C(x, y).
                        // A(x, y) :- diff+B(x, y), diff+appliedC(x, y).
                        // then we would double insert if we insert B(1, 2) and also C(1, 2)
                        auto noAdditionNegation = atomJ->clone();
                        noAdditionNegation->setName(translateDiffPlusCountRelation(getAtomRelation(atomJ, program))->get()->getName());
                        noAdditionNegation->setArgument(noAdditionNegation->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        noAdditionNegation->setArgument(noAdditionNegation->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noAdditionNegation->setArgument(noAdditionNegation->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        r1->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noAdditionNegation)));
                        * /
                    }

                    for (size_t j = i + 1; j < atoms.size(); j++) {
                        auto& atomJ = atoms[j];
                        r1->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atomJ, program))->get()->getName());
                    }

                    auto& atomK = atoms[i];

                    // add a negation to the rule stating that the tuple shouldn't be inserted
                    // this prevents double counting, e.g., if we have:
                    // A(x, y) :- B(x, y), diff+C(x, y).
                    // A(x, y) :- diff+B(x, y), diff+appliedC(x, y).
                    // then we would double insert if we insert B(1, 2) and also C(1, 2)
                    auto noPrevious = atomK->clone();
                    noPrevious->setName(translateDiffMinusAppliedRelation(getAtomRelation(atomK, program))->get()->getName());
                    noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                    noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                    // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                    r1->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));
                    */
                } else if (isDeletionRule) {
                    for (size_t i = 0; i < atoms.size(); i++) {
                        // an insertion rule should look as follows:
                        // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                        auto cl = clause->clone();

                        // set the head of the rule to be the diff relation
                        cl->getHead()->setName(translateDiffMinusRelation(&rel)->get()->getName());

                        // ensure i-th tuple did not exist previously, this prevents double insertions
                        auto noPrevious = atoms[i]->clone();
                        noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                        // the current version of the rule should have diff_minus_count in the i-th position
                        cl->getAtoms()[i]->setName(translateDiffMinusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));

                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));

                        // atoms before the i-th position should not fulfill the conditions for incremental insertion, otherwise we will have double insertions
                        for (size_t j = 0; j < i; j++) {
                            // cl->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                            // ensure tuple is not actually inserted
                            auto curAtom = atoms[j]->clone();
                            curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(atoms[j], program))->get()->getName());

                            curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                            curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                            // also ensure tuple existed previously
                            auto noPrevious = atoms[j]->clone();
                            noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                            noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                            noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                            // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                            cl->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                        }

                        for (size_t j = i + 1; j < atoms.size(); j++) {
                            cl->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                        }


                        // process negations
                        for (size_t j = 0; j < negations.size(); j++) {
                            // each negation needs to have either not existed, or be deleted
                            // for the non-existence case, we use a positive negation instead
                            auto negatedAtom = negations[j]->getAtom()->clone();
                            // negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                            cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                        }

                        cl->clearNegations();

                        std::cout << "non-recursive: " << *cl << std::endl;

                        // translate cl
                        std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*cl, *cl);

                        // add logging
                        if (Global::config().has("profile")) {
                            const std::string& relationName = toString(rel.getName());
                            const SrcLocation& srcLocation = cl->getSrcLoc();
                            const std::string clText = stringify(toString(*cl));
                            const std::string logTimerStatement =
                                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clText);
                            const std::string logSizeStatement =
                                    LogStatement::nNonrecursiveRule(relationName, srcLocation, clText);
                            rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                    logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
                        }

                        // add debug info
                        std::ostringstream ds;
                        ds << toString(*cl) << "\nin file ";
                        ds << cl->getSrcLoc();
                        rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                        // add rule to result
                        appendStmt(res, std::move(rule));
                    }

                    // TODO: if there is a negation, then we need to add a version of the rule which applies when only the negations apply
                    for (size_t i = 0; i < negations.size(); i++) {
                        // an insertion rule should look as follows:
                        // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                        auto cl = clause->clone();

                        // set the head of the rule to be the diff relation
                        cl->getHead()->setName(translateDiffMinusRelation(&rel)->get()->getName());

                        // clone the i-th negation's atom
                        // each negation needs to have either not existed, or be deleted
                        // for the non-existence case, we use a positive negation instead
                        auto negatedAtom = negations[i]->getAtom()->clone();
                        negatedAtom->setName(translateDiffPlusCountRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                        negatedAtom->setArgument(negatedAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        negatedAtom->setArgument(negatedAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        negatedAtom->setArgument(negatedAtom->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                        cl->addToBody(std::unique_ptr<AstAtom>(negatedAtom));

                        /*
                        // add constraints saying that the i-th negation atom should be deleted
                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                    std::make_unique<AstVariable>("@negated_current_count"),
                                    std::make_unique<AstNumberConstant>(0)));
                                    */

                        /*
                        // add constraints saying that the i-th negation atom should be deleted
                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));

                        cl->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                    std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                    std::make_unique<AstNumberConstant>(0)));
                                    */

                        // prevent double insertions across epochs
                        auto noPrevious = negations[i]->getAtom()->clone();
                        noPrevious->setName(translateRelation(getAtomRelation(noPrevious, program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                        // atoms before the i-th position should not fulfill the conditions for incremental deletion, otherwise we will have double insertions
                        for (size_t j = 0; j < i; j++) {
                            // cl->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                            // ensure tuple is actually inserted
                            auto curAtom = negations[j]->getAtom()->clone();
                            curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(curAtom, program))->get()->getName());

                            curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                            curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                            // also ensure tuple existed previously
                            auto noPrevious = negations[j]->getAtom()->clone();
                            noPrevious->setName(translateRelation(getAtomRelation(noPrevious, program))->get()->getName());
                            noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                            noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                            // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                            cl->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                        }

                        // process negations
                        for (size_t j = 0; j < negations.size(); j++) {
                            // each negation needs to have either not existed, or be deleted
                            // for the non-existence case, we use a positive negation instead
                            auto negatedAtom = negations[j]->getAtom()->clone();
                            // negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                            cl->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                        }


                        cl->clearNegations();

                        std::cout << "non-recursive: " << *cl << std::endl;

                        // translate cl
                        std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*cl, *cl);

                        // add logging
                        if (Global::config().has("profile")) {
                            const std::string& relationName = toString(rel.getName());
                            const SrcLocation& srcLocation = cl->getSrcLoc();
                            const std::string clText = stringify(toString(*cl));
                            const std::string logTimerStatement =
                                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clText);
                            const std::string logSizeStatement =
                                    LogStatement::nNonrecursiveRule(relationName, srcLocation, clText);
                            rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                    logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
                        }

                        // add debug info
                        std::ostringstream ds;
                        ds << toString(*cl) << "\nin file ";
                        ds << cl->getSrcLoc();
                        rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                        // add rule to result
                        appendStmt(res, std::move(rule));
                    }

                    /*
                    // set the head of the rule to be the diff relation
                    clause->getHead()->setName(translateDiffMinusRelation(&rel)->get()->getName());

                    // add a constraint saying that at least one body tuple should be in the diff_plus version of the relation
                    std::vector<AstConstraint*> existsDiffMinus;
                    for (size_t i = 0; i < atoms.size(); i++) {
                        // ensure tuple is actually deleted
                        auto curAtom = atoms[i]->clone();
                        curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                        curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                        // prevent double insertions across epochs
                        auto noInsertion = atoms[i]->clone();
                        noInsertion->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                        noInsertion->setArgument(noInsertion->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noInsertion->setArgument(noInsertion->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noInsertion->setArgument(noInsertion->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        existsDiffMinus.push_back(new AstConjunctionConstraint(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noInsertion))));
                    }

                    // process negations
                    for (size_t i = 0; i < negations.size(); i++) {
                        // each negation needs to have either not existed, or be deleted
                        // for the non-existence case, we use a positive negation instead
                        auto negatedAtom = negations[i]->getAtom()->clone();
                        // negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                        clause->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));

                        // for the deletion case, we check if the atom is in diff minus
                        auto curAtom = negations[i]->getAtom()->clone();
                        curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(curAtom, program))->get()->getName());

                        curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                        // prevent double insertions across epochs
                        auto noPrevious = negations[i]->getAtom()->clone();
                        noPrevious->setName(translateDiffMinusAppliedRelation(getAtomRelation(noPrevious, program))->get()->getName());
                        noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                        noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                        // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        existsDiffMinus.push_back(new AstConjunctionConstraint(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious))));
                    }

                    // clear negations as they are now adapted for the incremental case
                    clause->clearNegations();


                    if (existsDiffMinus.size() > 0) {
                        clause->addToBody(toAstDisjunction(existsDiffMinus));
                    }
                    */

                    /*
                    // set atom i to use the diff relation
                    r1->getAtoms()[i]->setName(translateDiffMinusCountRelation(getAtomRelation(atom, program))->get()->getName());

                    for (size_t j = 0; j < i; j++) {
                        auto& atomJ = atoms[j];

                        / *
                        // add a negation to the rule stating that the tuple shouldn't be deleted
                        // this prevents double counting, e.g., if we have:
                        // A(x, y) :- B(x, y), diff-C(x, y).
                        // A(x, y) :- diff-B(x, y), diff-appliedC(x, y).
                        // then we would double delete if we delete B(1, 2) and also C(1, 2)
                        auto noDeletionNegation = atomJ->clone();
                        noDeletionNegation->setName(translateDiffMinusRelation(getAtomRelation(atomJ, program))->get()->getName());
                        noDeletionNegation->setArgument(noDeletionNegation->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                        noDeletionNegation->setArgument(noDeletionNegation->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                        noDeletionNegation->setArgument(noDeletionNegation->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                        r1->addToBody(std::make_unique<AstNegation>(std::unique_ptr<AstAtom>(noDeletionNegation)));
                        * /
                    }

                    for (size_t j = i + 1; j < atoms.size(); j++) {
                        auto& atomJ = atoms[j];
                        r1->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atomJ, program))->get()->getName());
                    }
                    */
                }

                /*
                // reorder cl so that the deletedTuple atom is evaluated first
                std::vector<unsigned int> reordering;
                reordering.push_back(i);
                for (unsigned int j = 0; j < atoms.size(); j++) {
                    if (j != i) {
                        reordering.push_back(j);
                    }
                }
                std::cout << "non-recursive re-insertion" << *r1 << " reorder: " << reordering << std::endl;
                r1->reorderAtoms(reordering);
                */

                /*
                std::cout << "non-recursive: " << *clause << std::endl;

                // translate clause
                std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*clause, *clause);

                // add logging
                if (Global::config().has("profile")) {
                    const std::string& relationName = toString(rel.getName());
                    const SrcLocation& srcLocation = clause->getSrcLoc();
                    const std::string clauseText = stringify(toString(*clause));
                    const std::string logTimerStatement =
                            LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
                    const std::string logSizeStatement =
                            LogStatement::nNonrecursiveRule(relationName, srcLocation, clauseText);
                    rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                            logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
                }

                // add debug info
                std::ostringstream ds;
                ds << toString(*clause) << "\nin file ";
                ds << clause->getSrcLoc();
                rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                // add rule to result
                appendStmt(res, std::move(rule));
                */
            }
        } else {
            std::unique_ptr<RamStatement> rule;
            if (Global::config().has("incremental")) {
            } else {
                // translate clause
                rule = ClauseTranslator(*this).translateClause(*clause, *clause);
            }

            // add logging
            if (Global::config().has("profile")) {
                const std::string& relationName = toString(rel.getName());
                const SrcLocation& srcLocation = clause->getSrcLoc();
                const std::string clauseText = stringify(toString(*clause));
                const std::string logTimerStatement =
                        LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
                const std::string logSizeStatement =
                        LogStatement::nNonrecursiveRule(relationName, srcLocation, clauseText);
                rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                        logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone())));
            }

            // add debug info
            std::ostringstream ds;
            ds << toString(*clause) << "\nin file ";
            ds << clause->getSrcLoc();
            rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

            // add rule to result
            appendStmt(res, std::move(rule));
        }
    }

    // add logging for entire relation
    if (Global::config().has("profile")) {
        const std::string& relationName = toString(rel.getName());
        const SrcLocation& srcLocation = rel.getSrcLoc();
        const std::string logSizeStatement = LogStatement::nNonrecursiveRelation(relationName, srcLocation);

        // add timer if we did any work
        if (res) {
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRelation(relationName, srcLocation);
            res = std::make_unique<RamLogRelationTimer>(
                    std::move(res), logTimerStatement, std::unique_ptr<RamRelationReference>(rrel->clone()));
        } else {
            // add table size printer
            appendStmt(res, std::make_unique<RamLogSize>(
                                    std::unique_ptr<RamRelationReference>(rrel->clone()), logSizeStatement));
        }
    }

    // done
    return res;
}

/**
 * A utility function assigning names to unnamed variables such that enclosing
 * constructs may be cloned without losing the variable-identity.
 */
void AstTranslator::nameUnnamedVariables(AstClause* clause) {
    // the node mapper conducting the actual renaming
    struct Instantiator : public AstNodeMapper {
        mutable int counter = 0;

        Instantiator() = default;

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            // apply recursive
            node->apply(*this);

            // replace unknown variables
            if (dynamic_cast<AstUnnamedVariable*>(node.get())) {
                auto name = " _unnamed_var" + toString(++counter);
                return std::make_unique<AstVariable>(name);
            }

            // otherwise nothing
            return node;
        }
    };

    // name all variables in the atoms
    Instantiator init;
    for (auto& atom : clause->getAtoms()) {
        atom->apply(init);
    }
}

/** generate RAM code for recursive relations in a strongly-connected component */
std::unique_ptr<RamStatement> AstTranslator::translateRecursiveRelation(
        const std::set<const AstRelation*>& scc, const RecursiveClauses* recursiveClauses, int indexOfScc) {
    // initialize sections
    std::unique_ptr<RamStatement> preamble;
    std::unique_ptr<RamSequence> clearTable(new RamSequence());
    std::unique_ptr<RamSequence> updateTable(new RamSequence());
    std::unique_ptr<RamStatement> postamble;

    // --- create preamble ---

    // mappings for temporary relations
    std::map<const AstRelation*, std::unique_ptr<RamRelationReference>> rrel;
    std::map<const AstRelation*, std::unique_ptr<RamRelationReference>> relDelta;
    std::map<const AstRelation*, std::unique_ptr<RamRelationReference>> relNew;

    // utility to convert a list of AstConstraints to a disjunction
    auto toAstDisjunction = [&](std::vector<AstConstraint*> constraints) -> std::unique_ptr<AstConstraint> {
        std::unique_ptr<AstConstraint> result;
        for (const auto& cur : constraints) {
            if (result == nullptr) {
                result = std::unique_ptr<AstConstraint>(cur->clone());
            } else {
                result = std::make_unique<AstDisjunctionConstraint>(std::move(result), std::unique_ptr<AstConstraint>(cur->clone()));
            }
            std::cout << "cur: " << *cur << " res: " << *result << std::endl;
        }
        return result;
    };

    /* Compute non-recursive clauses for relations in scc and push
       the results in their delta tables. */
    for (const AstRelation* rel : scc) {

        std::unique_ptr<RamStatement> updateRelTable;
        std::unique_ptr<RamStatement> clearRelTable;

        /* create two temporary tables for relaxed semi-naive evaluation */
        rrel[rel] = translateRelation(rel);
        relDelta[rel] = translateDeltaRelation(rel);
        relNew[rel] = translateNewRelation(rel);

        /* create update statements for fixpoint (even iteration) */
        appendStmt(updateRelTable,
                std::make_unique<RamSequence>(
                        std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(rrel[rel]->clone()),
                                std::unique_ptr<RamRelationReference>(relNew[rel]->clone())),
                        std::make_unique<RamSwap>(
                                std::unique_ptr<RamRelationReference>(relDelta[rel]->clone()),
                                std::unique_ptr<RamRelationReference>(relNew[rel]->clone())),
                        std::make_unique<RamClear>(
                                std::unique_ptr<RamRelationReference>(relNew[rel]->clone()))));

        if (Global::config().has("incremental")) {
            appendStmt(clearRelTable,
                    std::make_unique<RamSequence>(
                            // all the deltas should be cleared
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone())),
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone())),
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone())),
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusAppliedRelation(rel)->clone())),
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusCountRelation(rel)->clone())),
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffPlusCountRelation(rel)->clone()))));

            appendStmt(updateRelTable,
                    std::make_unique<RamSequence>(
                            // populate the delta relation
                            std::make_unique<RamPositiveMerge>(std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translatePreviousIndexedRelation(rel)->clone())),

                            // populate the diff minus relations
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            /*
                            std::make_unique<RamPositiveMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateRelation(rel)->clone())),
                                    */
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),

                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            /*
                            std::make_unique<RamPositiveMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateRelation(rel)->clone())),
                                    */
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),

                            /*
                            std::make_unique<RamPositiveMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateRelation(rel)->clone())),
                                    */
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                            std::make_unique<RamSemiMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),

                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamSemiMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),

                            // populate the applied relations
                            /*
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),

                            std::make_unique<RamExistingMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone())),
                            std::make_unique<RamExistingMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                                    */

                            /*
                            std::make_unique<RamPositiveMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone())),
                                    */

                            /*
                            std::make_unique<RamSemiMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                                    */
                            std::make_unique<RamSemiMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone())),
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),



                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone())),
                            std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusAppliedRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),


                            // populate the delta diff count relations
                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffPlusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                            std::make_unique<RamSemiMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffPlusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(rel)->clone())),

                            std::make_unique<RamMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamSemiMerge>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusCountRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone())),

                            /*
                            std::make_unique<RamSwap>(
                                    std::unique_ptr<RamRelationReference>(translateDeltaDiffRelation(rel)->clone()),
                                    std::unique_ptr<RamRelationReference>(translateNewDiffRelation(rel)->clone())),
                                    */

                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())),
                            std::make_unique<RamClear>(
                                    std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone()))));
        }

        /* measure update time for each relation */
        if (Global::config().has("profile")) {
            updateRelTable = std::make_unique<RamLogRelationTimer>(std::move(updateRelTable),
                    LogStatement::cRecursiveRelation(toString(rel->getName()), rel->getSrcLoc()),
                    std::unique_ptr<RamRelationReference>(relNew[rel]->clone()));
        }

        /* drop temporary tables after recursion */
        appendStmt(postamble, std::make_unique<RamSequence>(
                                      std::make_unique<RamDrop>(
                                              std::unique_ptr<RamRelationReference>(relDelta[rel]->clone())),
                                      std::make_unique<RamDrop>(
                                              std::unique_ptr<RamRelationReference>(relNew[rel]->clone()))));

        if (Global::config().has("incremental")) {
            appendStmt(postamble, std::make_unique<RamSequence>(
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translatePreviousIndexedRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusAppliedRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateDeltaDiffPlusCountRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusCountRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())),
                                          std::make_unique<RamDrop>(
                                                  std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone()))));
        }


        /*
        if (Global::config().has("incremental")) {
            appendStmt(postamble, std::make_unique<RamMerge>(
                        std::unique_ptr<RamRelationReference>(rrel[rel]->clone()),
                        std::unique_ptr<RamRelationReference>(translateDiffRelation(rel)->clone())));

            appendStmt(postamble, std::make_unique<RamMerge>(
                        std::unique_ptr<RamRelationReference>(rrel[rel]->clone()),
                        std::unique_ptr<RamRelationReference>(translateNewDiffRelation(rel)->clone())));
        }
        */

        /* Generate code for non-recursive part of relation */
        appendStmt(preamble, translateNonRecursiveRelation(*rel, recursiveClauses));

        // for incremental, create a temporary table storing the previous epoch's tuples in a fully indexable relation
        // we want the relation to be a copy of the full relation
        if (Global::config().has("incremental")) {
            for (const AstRelation* rel : scc) {
                auto previousIndexedRelation = translatePreviousIndexedRelation(rel)->clone();

                // appendStmt(preamble, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(translatePreviousIndexedRelation(rel)->clone())));
                appendStmt(preamble, std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translatePreviousIndexedRelation(rel)->clone()),
                        std::unique_ptr<RamRelationReference>(translateRelation(rel)->clone())));
            }
        }

        if (Global::config().has("incremental")) {
            // populate the delta relation
            appendStmt(preamble,
                    std::make_unique<RamPositiveMerge>(std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translatePreviousIndexedRelation(rel)->clone())));

            /*
            // populate the applied relations
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone())));
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone())));
                            */

            // populate the applied relations
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDeltaRelation(rel)->clone())));
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone())));
            /*
            appendStmt(preamble,
                    std::make_unique<RamPositiveMerge>(std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone()), std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));
                            */
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));
            /*
            appendStmt(preamble,
                    std::make_unique<RamPositiveMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone()), std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));
                            */
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone())));
                            // std::unique_ptr<RamRelationReference>(translateTemporaryDeltaDiffAppliedRelation(rel)->clone())));

            /*
            appendStmt(preamble,
                    std::make_unique<RamPositiveMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone()), std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));
                            */
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone())));
            appendStmt(preamble,
                    std::make_unique<RamSemiMerge>(std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone())));
            appendStmt(preamble,
                    std::make_unique<RamSemiMerge>(std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone())));

            // populate the delta diff count relations
            appendStmt(preamble,
                    std::make_unique<RamMerge>(
                            std::unique_ptr<RamRelationReference>(translateDeltaDiffPlusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(rel)->clone())));
            appendStmt(preamble,
                    std::make_unique<RamSemiMerge>(
                            std::unique_ptr<RamRelationReference>(translateDeltaDiffPlusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(
                            std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(rel)->clone())));
            appendStmt(preamble,
                    std::make_unique<RamSemiMerge>(
                            std::unique_ptr<RamRelationReference>(translateDeltaDiffMinusCountRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(rel)->clone())));

        }

        /* Generate merge operation for temp tables */
        appendStmt(preamble,
                std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(relDelta[rel]->clone()),
                        std::unique_ptr<RamRelationReference>(rrel[rel]->clone())));

        /*
        if (Global::config().has("incremental")) {
            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffRelation(rel)->clone())));

            appendStmt(preamble,
                    std::make_unique<RamMerge>(std::unique_ptr<RamRelationReference>(translateDeltaDiffAppliedRelation(rel)->clone()),
                            std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(rel)->clone())));
        }
        */

        /* Add update operations of relations to parallel statements */
        updateTable->add(std::move(updateRelTable));
        clearTable->add(std::move(clearRelTable));
    }

    // for incremental, create a temporary table storing the max iteration number in the current SCC
    // we want the relation to have a single fact, like
    // @max_iter_scc_i(
    //   max(
    //     max @iter : rel_1(_, _, @iter, _, _), 
    //     max @iter : rel_2(_, _, @iter, _, _),
    //     ...)).
    auto maxIterRelation = new RamRelation("scc_" + std::to_string(indexOfScc) + "_@max_iter", 1, 1, std::vector<std::string>({"max_iter"}), std::vector<std::string>({"s"}), RelationRepresentation::DEFAULT);
    auto maxIterRelationRef = std::make_unique<RamRelationReference>(maxIterRelation);

    if (Global::config().has("incremental")) {
        appendStmt(preamble, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(maxIterRelationRef->clone())));

        // we make the final project first
        std::vector<std::unique_ptr<RamExpression>> maxIterNumbers;
        int ident = 0;
        for (const AstRelation* rel : scc) {
            maxIterNumbers.push_back(std::make_unique<RamTupleElement>(ident, 0));
            ident++;
        }
        auto maxIterNumber = std::make_unique<RamIntrinsicOperator>(FunctorOp::MAX, std::move(maxIterNumbers));

        // create a set of aggregates over the relations in the scc
        // a RamAggregate is a nested structure
        std::vector<std::unique_ptr<RamExpression>> maxIterNumFunctor;
        maxIterNumFunctor.push_back(std::move(maxIterNumber));
        std::unique_ptr<RamOperation> outerMaxIterAggregate = std::make_unique<RamProject>(std::unique_ptr<RamRelationReference>(maxIterRelationRef->clone()), std::move(maxIterNumFunctor));

        ident = 0;
        for (const AstRelation* rel : scc) {
            outerMaxIterAggregate = std::make_unique<RamAggregate>(std::move(outerMaxIterAggregate), AggregateFunction::MAX, std::unique_ptr<RamRelationReference>(rrel[rel]->clone()), std::make_unique<RamTupleElement>(ident, rrel[rel]->get()->getArity() - 3), std::make_unique<RamTrue>(), ident);
            ident++;
        }

        appendStmt(preamble, std::make_unique<RamQuery>(std::move(outerMaxIterAggregate)));
    }

    // --- build main loop ---

    std::unique_ptr<RamParallel> loopSeq(new RamParallel());

    // create a utility to check SCC membership
    auto isInSameSCC = [&](const AstRelation* rel) {
        return std::find(scc.begin(), scc.end(), rel) != scc.end();
    };

    /* Compute temp for the current tables */
    for (const AstRelation* rel : scc) {
        std::unique_ptr<RamStatement> loopRelSeq;

        /* Find clauses for relation rel */
        for (size_t i = 0; i < rel->clauseSize(); i++) {
            AstClause* cl = rel->getClause(i);

            // skip non-recursive clauses
            if (!recursiveClauses->recursive(cl)) {
                continue;
            }

            // each recursive rule results in several operations
            int version = 0;
            const auto& atoms = cl->getAtoms();
            const auto& negations = cl->getNegations();

            if (Global::config().has("incremental")) {
                // store previous count and current count to determine if the rule is insertion or deletion
                auto prevCount = cl->getHead()->getArgument(rel->getArity() - 2);
                auto curCount = cl->getHead()->getArgument(rel->getArity() - 1);

                // these should not be nullptrs
                auto prevCountNum = dynamic_cast<AstNumberConstant*>(prevCount);
                auto curCountNum = dynamic_cast<AstNumberConstant*>(curCount);

                if (prevCountNum == nullptr || curCountNum == nullptr) {
                    std::cerr << "count annotations are not intialized!" << std::endl;
                }

                nameUnnamedVariables(cl);

                // check if this clause is re-inserting hidden tuples
                bool isReinsertionRule = (*prevCountNum == AstNumberConstant(1) && *curCountNum == AstNumberConstant(1));
                bool isInsertionRule = (*curCountNum == AstNumberConstant(1)) && !isReinsertionRule;
                bool isDeletionRule = (*curCountNum == AstNumberConstant(-1));

                if (isReinsertionRule) {
                    // translate each atom into the delta version
                    // for (size_t j = 0; j < atoms.size(); j++) {
                    /*
                        AstAtom* atom = atoms[j];
                        const AstRelation* atomRelation = getAtomRelation(atom, program);

                        if (!isInSameSCC(atomRelation)) {
                            continue;
                        }
                        */

                        std::unique_ptr<AstClause> rdiff(cl->clone());

                        rdiff->getHead()->setName(translateNewDiffPlusRelation(rel)->get()->getName());

                        for (size_t k = 0; k < atoms.size(); k++) {
                            rdiff->getAtoms()[k]->setName(translateDiffAppliedRelation(getAtomRelation(rdiff->getAtoms()[k], program))->get()->getName());
                        }

                        // for incremental, we use iteration counts to simulate delta relation rather than explicitly having a separate relation
                        auto diffAppliedHeadAtom = cl->getHead()->clone();
                        diffAppliedHeadAtom->setName(translateDiffAppliedRelation(getAtomRelation(diffAppliedHeadAtom, program))->get()->getName());

                        // add constraints saying that each body tuple must have existed in the previous epoch
                        std::vector<AstConstraint*> existsReinsertion;
                        for (size_t i = 0; i < atoms.size(); i++) {
                            // ensure tuple actually existed
                            auto curAtom = atoms[i]->clone();
                            curAtom->setName(translateRelation(getAtomRelation(atoms[i], program))->get()->getName());

                            curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                            curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstUnnamedVariable>());

                            rdiff->addToBody(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)));

                            /*
                            // also ensure tuple isn't newly inserted in current epoch
                            auto notNewInsertion = atoms[i]->clone();
                            notNewInsertion->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                            notNewInsertion->setArgument(notNewInsertion->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                            notNewInsertion->setArgument(notNewInsertion->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                            // unless it was an update to a smaller iteration
                            auto notIterationUpdate = atoms[i]->clone();
                            notIterationUpdate->setName(translateRelation(getAtomRelation(atoms[i], program))->get()->getName());

                            notIterationUpdate->setArgument(notNewInsertion->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                            notIterationUpdate->setArgument(notNewInsertion->getArity() - 2, std::make_unique<AstNumberConstant>(1));

                            rdiff->addToBody(std::make_unique<AstConjunctionConstraint>(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstConjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(notNewInsertion)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(notNewInsertion)))));
                            */
                        }

                        // if we have incremental evaluation, we use iteration counts to simulate delta relations
                        // rather than explicitly having a separate relation
                        rdiff->addToBody(std::make_unique<AstSubsumptionNegation>(
                                std::unique_ptr<AstAtom>(diffAppliedHeadAtom), 1));

                        /*
                        // simulate the delta relation with a constraint on the iteration number
                        rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GE,
                                    std::unique_ptr<AstArgument>(atom->getArgument(atom->getArity() - 3)->clone()),
                                    std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                    */

                        // replace wildcards with variables (reduces indices when wildcards are used in recursive
                        // atoms)

                        /*
                        // reduce R to P ...
                        for (size_t k = j + 1; k < atoms.size(); k++) {
                            if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                                auto atomK = rdiff->getAtoms()[k];
                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                            std::unique_ptr<AstArgument>(atomK->getArgument(atomK->getArity() - 3)->clone()),
                                            std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                            }
                        }
                        */

                        // a tuple should only be reinserted if that tuple is deleted
                        auto deletedTuple = cl->getHead()->clone();
                        deletedTuple->setName(translateDiffMinusCountRelation(rel)->get()->getName());
                        deletedTuple->setArgument(deletedTuple->getArity() - 1, std::make_unique<AstVariable>("@deleted_count"));
                        deletedTuple->setArgument(deletedTuple->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                        deletedTuple->setArgument(deletedTuple->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                        rdiff->addToBody(std::unique_ptr<AstAtom>(deletedTuple));
                        rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE, std::make_unique<AstVariable>("@deleted_count"), std::make_unique<AstNumberConstant>(0)));

                        /*
                        // duplicate the rule to express the version which re-derives tuples based on previous re-derived tuples
                        for (size_t k = 0; k < atoms.size(); k++) {
                            auto r2 = rdiff->clone();

                            r2->getAtoms()[k]->setName(translateDiffPlusRelation(getAtomRelation(atoms[k], program))->get()->getName());

                            r2->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ, std::unique_ptr<AstArgument>(r2->getAtoms()[k]->getArgument(rdiff->getAtoms()[k]->getArity() - 2)->clone()), std::make_unique<AstNumberConstant>(1)));

                            for (size_t l = 0; l < k; l++) {
                                r2->getAtoms()[l]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[l], program))->get()->getName());

                                // add a negation to the rule stating that the tuple shouldn't be inserted
                                // this prevents double counting, e.g., if we have:
                                // A(x, y) :- B(x, y), diff+C(x, y).
                                // A(x, y) :- diff+B(x, y), diff+appliedC(x, y).
                                // then we would double insert if we insert B(1, 2) and also C(1, 2)
                                auto noAdditionNegation = atoms[l]->clone();
                                noAdditionNegation->setName(translateDiffPlusRelation(getAtomRelation(atoms[l], program))->get()->getName());
                                noAdditionNegation->setArgument(noAdditionNegation->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                noAdditionNegation->setArgument(noAdditionNegation->getArity() - 2, std::make_unique<AstNumberConstant>(1));
                                noAdditionNegation->setArgument(noAdditionNegation->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                r2->addToBody(std::make_unique<AstNegation>(std::unique_ptr<AstAtom>(noAdditionNegation)));
                            }

                            for (size_t l = k + 1; l < atoms.size(); l++) {
                                r2->getAtoms()[l]->setName(translateDiffPlusAppliedRelation(getAtomRelation(atoms[l], program))->get()->getName());
                            }

                            // reorder cl so that the deletedTuple atom is evaluated first
                            std::vector<unsigned int> reordering;
                            reordering.push_back(atoms.size());
                            for (unsigned int l = 0; l < atoms.size(); l++) {
                                reordering.push_back(l);
                            }
                            std::cout << "recursive re-insertion: " << *r2 << " reorder: " << reordering << std::endl;
                            r2->reorderAtoms(reordering);

                            std::unique_ptr<RamStatement> rule =
                                    ClauseTranslator(*this).translateClause(*r2, *cl, version);

                            // add logging 
                            if (Global::config().has("profile")) {
                                const std::string& relationName = toString(rel->getName());
                                const SrcLocation& srcLocation = r2->getSrcLoc();
                                const std::string clauseText = stringify(toString(*r2));
                                const std::string logTimerStatement =
                                        LogStatement::tRecursiveRule(relationName, version, srcLocation, clauseText);
                                const std::string logSizeStatement =
                                        LogStatement::nRecursiveRule(relationName, version, srcLocation, clauseText);
                                rule = std::make_unique<RamSequence>(
                                        std::make_unique<RamLogRelationTimer>(std::move(rule), logTimerStatement,
                                                std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                            }

                            // add debug info
                            std::ostringstream ds;
                            ds << toString(*r2) << "\nin file ";
                            ds << r2->getSrcLoc();
                            rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                            // add to loop body
                            appendStmt(loopRelSeq, std::move(rule));
                        }
                    */

                        std::vector<std::unique_ptr<AstLiteral>> notDeletedChecks;

                        // process negations
                        for (size_t j = 0; j < negations.size(); j++) {
                            // each negation needs to have either not existed, or be deleted
                            // for the non-existence case, we use a positive negation instead
                            auto negatedAtom = negations[j]->getAtom()->clone();
                            negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                            rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));

                            // negations shouldn't exist in diff_minus_count, otherwise they will be processed by insertion rules
                            auto notDeleted = negations[j]->getAtom()->clone();
                            notDeleted->setName(translateDiffMinusCountRelation(getAtomRelation(notDeleted, program))->get()->getName());
                            notDeleted->setArgument(notDeleted->getArity() - 1, std::make_unique<AstNumberConstant>(0));
                            notDeleted->setArgument(notDeleted->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                            notDeleted->setArgument(notDeleted->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                            notDeletedChecks.push_back(std::make_unique<AstNegation>(std::unique_ptr<AstAtom>(notDeleted)));
                        }

                        rdiff->clearNegations();

                        for (auto& notDeleted : notDeletedChecks) {
                            rdiff->addToBody(std::move(notDeleted));
                        }

                        // use delta versions of relations for semi-naive evaluation
                        for (size_t j = 0; j < atoms.size(); j++) {
                            if (!isInSameSCC(getAtomRelation(atoms[j], program))) {
                                continue;
                            }

                            // create clone
                            std::unique_ptr<AstClause> r1(rdiff->clone());

                            // set the j-th atom to use DeltaDiffApplied
                            r1->getAtoms()[j]->setName(translateDeltaDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());

                            // any atoms after atom j should not be in delta, check this by a constraint on the iteration number
                            for (size_t k = j + 1; k < atoms.size(); k++) {
                                if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                                    r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                                std::unique_ptr<AstArgument>(r1->getAtoms()[k]->getArgument(r1->getAtoms()[k]->getArity() - 3)->clone()),
                                                std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                }
                            }

                            // reorder cl so that the deletedTuple atom is evaluated first
                            std::vector<unsigned int> reordering;
                            reordering.push_back(atoms.size());
                            for (unsigned int k = 0; k < atoms.size(); k++) {
                                reordering.push_back(k);
                            }
                            std::cout << "recursive re-insertion: " << *r1 << " reorder: " << reordering << std::endl;
                            r1->reorderAtoms(reordering);


                            std::cout << "recursive: " << *r1 << std::endl;

                            // translate rdiff
                            std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*r1, *r1);

                            // add logging
                            if (Global::config().has("profile")) {
                                const std::string& relationName = toString(rel->getName());
                                const SrcLocation& srcLocation = r1->getSrcLoc();
                                const std::string clText = stringify(toString(*r1));
                                const std::string logTimerStatement =
                                        LogStatement::tRecursiveRule(relationName, version, srcLocation, clText);
                                const std::string logSizeStatement =
                                        LogStatement::nRecursiveRule(relationName, version, srcLocation, clText);
                                rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                        logTimerStatement, std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                            }

                            // add debug info
                            std::ostringstream ds;
                            ds << toString(*r1) << "\nin file ";
                            ds << r1->getSrcLoc();
                            rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                            // add rule to result
                            appendStmt(loopRelSeq, std::move(rule));
                        }

                        /*
                        std::unique_ptr<RamStatement> rule =
                                ClauseTranslator(*this).translateClause(*r1, *cl, version);

                        // add logging
                        if (Global::config().has("profile")) {
                            const std::string& relationName = toString(rel->getName());
                            const SrcLocation& srcLocation = r1->getSrcLoc();
                            const std::string clauseText = stringify(toString(*r1));
                            const std::string logTimerStatement =
                                    LogStatement::tRecursiveRule(relationName, version, srcLocation, clauseText);
                            const std::string logSizeStatement =
                                    LogStatement::nRecursiveRule(relationName, version, srcLocation, clauseText);
                            rule = std::make_unique<RamSequence>(
                                    std::make_unique<RamLogRelationTimer>(std::move(rule), logTimerStatement,
                                            std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                        }

                        // add debug info
                        std::ostringstream ds;
                        ds << toString(*r1) << "\nin file ";
                        ds << r1->getSrcLoc();
                        rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                        // add to loop body
                        appendStmt(loopRelSeq, std::move(rule));
                        */

                        // increment version counter
                        version++;
                    // }
                } else {
                    // for (size_t j = 0; j < atoms.size(); j++) {
                        // AstAtom* atom = atoms[j];
                        // const AstRelation* atomRelation = getAtomRelation(atom, program);

                        // clone the clause so we can use diff and diff_applied auxiliary relations
                        // std::unique_ptr<AstClause> rdiff(cl->clone());

                        if (isInsertionRule) {
                            for (size_t i = 0; i < atoms.size(); i++) {
                                // an insertion rule should look as follows:
                                // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                                std::unique_ptr<AstClause> rdiff(cl->clone());

                                // set the head of the rule to be the diff relation
                                rdiff->getHead()->setName(translateDiffPlusRelation(rel)->get()->getName());

                                // ensure i-th tuple did not exist previously, this prevents double insertions
                                auto noPrevious = atoms[i]->clone();
                                noPrevious->setName(translateRelation(getAtomRelation(atoms[i], program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                                // the current version of the rule should have diff_plus_count in the i-th position
                                rdiff->getAtoms()[i]->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));

                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));

                                // atoms before the i-th position should not fulfill the conditions for incremental insertion, otherwise we will have double insertions
                                for (size_t j = 0; j < i; j++) {
                                    rdiff->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());

                                    // ensure tuple is not actually inserted
                                    auto curAtom = atoms[j]->clone();
                                    curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                    curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                                    // also ensure tuple existed previously
                                    auto noPrevious = atoms[j]->clone();
                                    noPrevious->setName(translateRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                    noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                    // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                    rdiff->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                                }

                                for (size_t j = i + 1; j < atoms.size(); j++) {
                                    rdiff->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                }


                                // process negations
                                for (size_t j = 0; j < negations.size(); j++) {
                                    // each negation needs to have either not existed, or be deleted
                                    // for the non-existence case, we use a positive negation instead
                                    auto negatedAtom = negations[j]->getAtom()->clone();
                                    negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                                    rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                                }

                                rdiff->clearNegations();

                                // create a subsumption negation so we don't re-insert previously discovered tuples
                                auto diffAppliedHeadAtom = cl->getHead()->clone();
                                diffAppliedHeadAtom->setName(translateDiffAppliedRelation(getAtomRelation(diffAppliedHeadAtom, program))->get()->getName());

                                // write into new_diff_plus
                                rdiff->getHead()->setName(translateNewDiffPlusRelation(rel)->get()->getName());

                                // if we have incremental evaluation, we use iteration counts to simulate delta relations
                                // rather than explicitly having a separate relation
                                rdiff->addToBody(std::make_unique<AstSubsumptionNegation>(
                                        std::unique_ptr<AstAtom>(diffAppliedHeadAtom), 1));

                                // use delta versions of relations for semi-naive evaluation
                                for (size_t j = 0; j < atoms.size(); j++) {
                                    if (!isInSameSCC(getAtomRelation(atoms[j], program))) {
                                        continue;
                                    }

                                    // create clone
                                    std::unique_ptr<AstClause> r1(rdiff->clone());

                                    // translate to correct delta version of the relation
                                    if (j < i) {
                                        r1->getAtoms()[j]->setName(translateDeltaDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    } else if (j == i) {
                                        r1->getAtoms()[j]->setName(translateDeltaDiffPlusCountRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    } else if (j > i) {
                                        r1->getAtoms()[j]->setName(translateDeltaDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    }

                                    // any atoms after atom j should not be in delta, check this by a constraint on the iteration number
                                    for (size_t k = j + 1; k < atoms.size(); k++) {
                                        if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                                            r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                                        std::unique_ptr<AstArgument>(r1->getAtoms()[k]->getArgument(r1->getAtoms()[k]->getArity() - 3)->clone()),
                                                        std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                        }
                                    }

                                    std::cout << "recursive: " << *r1 << std::endl;

                                    // reorder cl so that the deletedTuple atom is evaluated first
                                    std::vector<unsigned int> reordering;
                                    reordering.push_back(j);
                                    for (unsigned int k = 0; k < r1->getAtoms().size(); k++) {
                                        if (k != j) {
                                            reordering.push_back(k);
                                        }
                                    }

                                    // r1->reorderAtoms(reordering);

                                    // translate rdiff
                                    std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*r1, *r1);

                                    // add logging
                                    if (Global::config().has("profile")) {
                                        const std::string& relationName = toString(rel->getName());
                                        const SrcLocation& srcLocation = r1->getSrcLoc();
                                        const std::string clText = stringify(toString(*r1));
                                        const std::string logTimerStatement =
                                                LogStatement::tRecursiveRule(relationName, version, srcLocation, clText);
                                        const std::string logSizeStatement =
                                                LogStatement::nRecursiveRule(relationName, version, srcLocation, clText);
                                        rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                                logTimerStatement, std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                                    }

                                    // add debug info
                                    std::ostringstream ds;
                                    ds << toString(*r1) << "\nin file ";
                                    ds << r1->getSrcLoc();
                                    rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                                    // add rule to result
                                    appendStmt(loopRelSeq, std::move(rule));
                                }
                            }

                            // TODO: if there is a negation, then we need to add a version of the rule which applies when only the negations apply
                            for (size_t i = 0; i < negations.size(); i++) {
                                // an insertion rule should look as follows:
                                // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                                auto rdiff = cl->clone();

                                // set the head of the rule to be the diff relation
                                rdiff->getHead()->setName(translateDiffPlusRelation(rel)->get()->getName());

                                // clone the i-th negation's atom
                                // each negation needs to have either not existed, or be deleted
                                // for the non-existence case, we use a positive negation instead
                                auto negatedAtom = negations[i]->getAtom()->clone();
                                negatedAtom->setName(translateDiffMinusCountRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                                negatedAtom->setArgument(negatedAtom->getArity() - 1, std::make_unique<AstNumberConstant>(0));
                                negatedAtom->setArgument(negatedAtom->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                                rdiff->addToBody(std::unique_ptr<AstAtom>(negatedAtom));

                                /*
                                // add constraints saying that the i-th negation atom should be deleted
                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));

                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));
                                            */

                                // prevent double insertions across epochs
                                auto noPrevious = negations[i]->getAtom()->clone();
                                noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(noPrevious, program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                                // atoms before the i-th position should not fulfill the conditions for incremental deletion, otherwise we will have double insertions
                                for (size_t j = 0; j < i; j++) {
                                    // rdiff->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                    // ensure tuple is not actually inserted
                                    auto curAtom = negations[j]->getAtom()->clone();
                                    curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(curAtom, program))->get()->getName());

                                    curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                    curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                                    // also ensure tuple existed previously
                                    auto noPrevious = negations[j]->getAtom()->clone();
                                    noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(noPrevious, program))->get()->getName());
                                    noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                    noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                    // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                    rdiff->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                                }

                                // process negations
                                for (size_t j = 0; j < negations.size(); j++) {
                                    // each negation needs to have either not existed, or be deleted
                                    // for the non-existence case, we use a positive negation instead
                                    auto negatedAtom = negations[j]->getAtom()->clone();
                                    negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                                    rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                                }

                                // the base relation for addition should be diff_applied, so translate each positive atom to the diff applied version
                                for (size_t j = 0; j < atoms.size(); j++) {
                                    rdiff->getAtoms()[j]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                }

                                rdiff->clearNegations();

                                // create a subsumption negation so we don't re-insert previously discovered tuples
                                auto diffAppliedHeadAtom = cl->getHead()->clone();
                                diffAppliedHeadAtom->setName(translateDiffAppliedRelation(getAtomRelation(diffAppliedHeadAtom, program))->get()->getName());

                                // write into new_diff_plus
                                rdiff->getHead()->setName(translateNewDiffPlusRelation(rel)->get()->getName());

                                // if we have incremental evaluation, we use iteration counts to simulate delta relations
                                // rather than explicitly having a separate relation
                                rdiff->addToBody(std::make_unique<AstSubsumptionNegation>(
                                        std::unique_ptr<AstAtom>(diffAppliedHeadAtom), 1));


                                std::cout << "recursive: " << *rdiff << std::endl;

                                // use delta versions of relations for semi-naive evaluation
                                for (size_t j = 0; j < atoms.size(); j++) {
                                    if (!isInSameSCC(getAtomRelation(atoms[j], program))) {
                                        continue;
                                    }

                                    // create clone
                                    std::unique_ptr<AstClause> r1(rdiff->clone());

                                    r1->getAtoms()[j]->setName(translateDeltaDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());

                                    // any atoms after atom j should not be in delta, check this by a constraint on the iteration number
                                    for (size_t k = j + 1; k < atoms.size(); k++) {
                                        if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                                            r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                                        std::unique_ptr<AstArgument>(r1->getAtoms()[k]->getArgument(r1->getAtoms()[k]->getArity() - 3)->clone()),
                                                        std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                        }
                                    }

                                    std::cout << "recursive: " << *r1 << std::endl;

                                    // reorder cl so that the deletedTuple atom is evaluated first
                                    std::vector<unsigned int> reordering;
                                    reordering.push_back(j);
                                    for (unsigned int k = 0; k < r1->getAtoms().size(); k++) {
                                        if (k != j) {
                                            reordering.push_back(k);
                                        }
                                    }

                                    // r1->reorderAtoms(reordering);

                                    // translate rdiff
                                    std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*r1, *r1);

                                    // add logging
                                    if (Global::config().has("profile")) {
                                        const std::string& relationName = toString(rel->getName());
                                        const SrcLocation& srcLocation = r1->getSrcLoc();
                                        const std::string clText = stringify(toString(*r1));
                                        const std::string logTimerStatement =
                                                LogStatement::tRecursiveRule(relationName, version, srcLocation, clText);
                                        const std::string logSizeStatement =
                                                LogStatement::nRecursiveRule(relationName, version, srcLocation, clText);
                                        rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                                logTimerStatement, std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                                    }

                                    // add debug info
                                    std::ostringstream ds;
                                    ds << toString(*r1) << "\nin file ";
                                    ds << r1->getSrcLoc();
                                    rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                                    // add rule to result
                                    appendStmt(loopRelSeq, std::move(rule));
                                }
                            }
                            /*
                            // add a constraint saying that at least one body tuple should be in the diff_plus version of the relation
                            std::vector<AstConstraint*> existsDiffPlus;
                            for (size_t i = 0; i < atoms.size(); i++) {
                                // ensure tuple is actually inserted
                                auto curAtom = atoms[i]->clone();
                                curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                                // prevent double insertions across epochs
                                auto noPrevious = atoms[i]->clone();
                                noPrevious->setName(translateRelation(getAtomRelation(atoms[i], program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                existsDiffPlus.push_back(new AstConjunctionConstraint(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious))));

                                // the base relation for insertion should be diff_applied
                                rdiff->getAtoms()[i]->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                            }

                            if (existsDiffPlus.size() > 0) {
                                rdiff->addToBody(toAstDisjunction(existsDiffPlus));
                            }

                            // set atom i to use the diff relation
                            // rdiff->getAtoms()[j]->setName(translateDiffPlusCountRelation(getAtomRelation(atom, program))->get()->getName());
                            */

                            /*
                            for (size_t k = 0; k < j; k++) {
                                auto& atomK = atoms[k];
                                rdiff->getAtoms()[k]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atomK, program))->get()->getName());

                                // add a negation to the rule stating that the tuple shouldn't be deleted
                                // this prevents double counting, e.g., if we have:
                                // A(x, y) :- B(x, y), diff-C(x, y).
                                // A(x, y) :- diff-B(x, y), diff-appliedC(x, y).
                                // then we would double delete if we delete B(1, 2) and also C(1, 2)
                                / *
                                auto noAdditionNegation = atomK->clone();
                                noAdditionNegation->setName(translateDiffPlusCountRelation(getAtomRelation(atomK, program))->get()->getName());
                                noAdditionNegation->setArgument(noAdditionNegation->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                noAdditionNegation->setArgument(noAdditionNegation->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noAdditionNegation->setArgument(noAdditionNegation->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noAdditionNegation)));
                                * /
                            }

                            for (size_t k = j + 1; k < atoms.size(); k++) {
                                auto& atomK = atoms[k];
                                rdiff->getAtoms()[k]->setName(translateDiffAppliedRelation(getAtomRelation(atomK, program))->get()->getName());
                            }


                            // for (size_t k = j; k < atoms.size(); k++) {
                                auto& atomK = atoms[j];

                                / *
                                if (!isInSameSCC(getAtomRelation(atomK, program))) {
                                    continue;
                                }
                                * /

                                // add a negation to the rule stating that the tuple shouldn't be inserted
                                // this prevents double counting, e.g., if we have:
                                // A(x, y) :- B(x, y), diff+C(x, y).
                                // A(x, y) :- diff+B(x, y), diff+appliedC(x, y).
                                // then we would double insert if we insert B(1, 2) and also C(1, 2)
                                auto noPrevious = atomK->clone();
                                noPrevious->setName(translateDiffMinusAppliedRelation(getAtomRelation(atomK, program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));
                                */
                            // }
                        } else if (isDeletionRule) {
                            for (size_t i = 0; i < atoms.size(); i++) {
                                // an insertion rule should look as follows:
                                // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                                std::unique_ptr<AstClause> rdiff(cl->clone());

                                // set the head of the rule to be the diff relation
                                rdiff->getHead()->setName(translateDiffMinusRelation(rel)->get()->getName());

                                // ensure i-th tuple did not exist previously, this prevents double insertions
                                auto noPrevious = atoms[i]->clone();
                                noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                                // the current version of the rule should have diff_minus_count in the i-th position
                                rdiff->getAtoms()[i]->setName(translateDiffMinusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));

                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));

                                // atoms before the i-th position should not fulfill the conditions for incremental insertion, otherwise we will have double insertions
                                for (size_t j = 0; j < i; j++) {
                                    // rdiff->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                    // ensure tuple is not actually inserted
                                    auto curAtom = atoms[j]->clone();
                                    curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(atoms[j], program))->get()->getName());

                                    curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                    curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                                    // also ensure tuple existed previously
                                    auto noPrevious = atoms[j]->clone();
                                    noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                    noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                    // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                    rdiff->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                                }

                                for (size_t j = i + 1; j < atoms.size(); j++) {
                                    rdiff->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                }


                                // process negations
                                for (size_t j = 0; j < negations.size(); j++) {
                                    // each negation needs to have either not existed, or be deleted
                                    // for the non-existence case, we use a positive negation instead
                                    auto negatedAtom = negations[j]->getAtom()->clone();
                                    // negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                                    rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                                }

                                rdiff->clearNegations();

                                std::cout << "recursive: " << *rdiff << std::endl;

                                // create a subsumption negation so we don't re-insert previously discovered tuples
                                auto diffAppliedHeadAtom = cl->getHead()->clone();
                                diffAppliedHeadAtom->setName(translateDiffAppliedRelation(getAtomRelation(diffAppliedHeadAtom, program))->get()->getName());

                                // write into new_diff_minus
                                rdiff->getHead()->setName(translateNewDiffMinusRelation(rel)->get()->getName());

                                // if we have incremental evaluation, we use iteration counts to simulate delta relations
                                // rather than explicitly having a separate relation
                                rdiff->addToBody(std::make_unique<AstSubsumptionNegation>(
                                        std::unique_ptr<AstAtom>(diffAppliedHeadAtom), 1));

                                // use delta versions of relations for semi-naive evaluation
                                for (size_t j = 0; j < atoms.size(); j++) {
                                    if (!isInSameSCC(getAtomRelation(atoms[j], program))) {
                                        continue;
                                    }

                                    // create clone
                                    std::unique_ptr<AstClause> r1(rdiff->clone());

                                    // translate to correct delta version of the relation
                                    if (j < i) {
                                        r1->getAtoms()[j]->setName(translateDeltaRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    } else if (j == i) {
                                        r1->getAtoms()[j]->setName(translateDeltaDiffMinusCountRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    } else if (j > i) {
                                        r1->getAtoms()[j]->setName(translateDeltaDiffMinusAppliedRelation(getAtomRelation(atoms[j], program))->get()->getName());
                                    }

                                    // any atoms after atom j should not be in delta, check this by a constraint on the iteration number
                                    for (size_t k = j + 1; k < atoms.size(); k++) {
                                        if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                                            r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                                        std::unique_ptr<AstArgument>(r1->getAtoms()[k]->getArgument(r1->getAtoms()[k]->getArity() - 3)->clone()),
                                                        std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                        }
                                    }

                                    std::cout << "recursive: " << *r1 << std::endl;

                                    // reorder cl so that the deletedTuple atom is evaluated first
                                    std::vector<unsigned int> reordering;
                                    reordering.push_back(j);
                                    for (unsigned int k = 0; k < r1->getAtoms().size(); k++) {
                                        if (k != j) {
                                            reordering.push_back(k);
                                        }
                                    }

                                    // r1->reorderAtoms(reordering);

                                    // translate rdiff
                                    std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*r1, *r1);

                                    // add logging
                                    if (Global::config().has("profile")) {
                                        const std::string& relationName = toString(rel->getName());
                                        const SrcLocation& srcLocation = r1->getSrcLoc();
                                        const std::string clText = stringify(toString(*r1));
                                        const std::string logTimerStatement =
                                                LogStatement::tRecursiveRule(relationName, version, srcLocation, clText);
                                        const std::string logSizeStatement =
                                                LogStatement::nRecursiveRule(relationName, version, srcLocation, clText);
                                        rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                                logTimerStatement, std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                                    }

                                    // add debug info
                                    std::ostringstream ds;
                                    ds << toString(*r1) << "\nin file ";
                                    ds << r1->getSrcLoc();
                                    rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                                    // add rule to result
                                    appendStmt(loopRelSeq, std::move(rule));
                                }

                                /*
                                // translate rdiff
                                std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*rdiff, *rdiff);

                                // add logging
                                if (Global::config().has("profile")) {
                                    const std::string& relationName = toString(rel->getName());
                                    const SrcLocation& srcLocation = rdiff->getSrcLoc();
                                    const std::string clText = stringify(toString(*rdiff));
                                    const std::string logTimerStatement =
                                            LogStatement::tNonrecursiveRule(relationName, srcLocation, clText);
                                    const std::string logSizeStatement =
                                            LogStatement::nNonrecursiveRule(relationName, srcLocation, clText);
                                    rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                            logTimerStatement, std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                                }

                                // add debug info
                                std::ostringstream ds;
                                ds << toString(*rdiff) << "\nin file ";
                                ds << rdiff->getSrcLoc();
                                rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                                // add rule to result
                                appendStmt(loopRelSeq, std::move(rule));
                                */
                            }

                            // TODO: if there is a negation, then we need to add a version of the rule which applies when only the negations apply
                            for (size_t i = 0; i < negations.size(); i++) {
                                // an insertion rule should look as follows:
                                // R :- R_1, R_2, ..., diff_plus_count_R_i, diff_applied_R_i+1, ..., diff_applied_R_n

                                auto rdiff = cl->clone();

                                // set the head of the rule to be the diff relation
                                rdiff->getHead()->setName(translateDiffMinusRelation(rel)->get()->getName());

                                // clone the i-th negation's atom
                                // each negation needs to have either not existed, or be deleted
                                // for the non-existence case, we use a positive negation instead
                                auto negatedAtom = negations[i]->getAtom()->clone();
                                negatedAtom->setName(translateDiffPlusCountRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                                negatedAtom->setArgument(negatedAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                negatedAtom->setArgument(negatedAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                negatedAtom->setArgument(negatedAtom->getArity() - 3, std::make_unique<AstUnnamedVariable>());
                                rdiff->addToBody(std::unique_ptr<AstAtom>(negatedAtom));

                                /*
                                // add constraints saying that the i-th negation atom should be deleted
                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 2)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));

                                rdiff->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                                            std::unique_ptr<AstArgument>(atoms[i]->getArgument(atoms[i]->getArity() - 1)->clone()),
                                            std::make_unique<AstNumberConstant>(0)));
                                            */

                                // prevent double insertions across epochs
                                auto noPrevious = negations[i]->getAtom()->clone();
                                noPrevious->setName(translateRelation(getAtomRelation(noPrevious, program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));

                                // atoms before the i-th position should not fulfill the conditions for incremental deletion, otherwise we will have double insertions
                                for (size_t j = 0; j < i; j++) {
                                    // rdiff->getAtoms()[j]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                    // ensure tuple is not actually inserted
                                    auto curAtom = negations[j]->getAtom()->clone();
                                    curAtom->setName(translateDiffPlusCountRelation(getAtomRelation(curAtom, program))->get()->getName());

                                    curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                    curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

                                    // also ensure tuple existed previously
                                    auto noPrevious = negations[j]->getAtom()->clone();
                                    noPrevious->setName(translateRelation(getAtomRelation(noPrevious, program))->get()->getName());
                                    noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                    noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                    // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                    rdiff->addToBody(std::make_unique<AstDisjunctionConstraint>(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(noPrevious))));
                                }

                                // process negations
                                for (size_t j = 0; j < negations.size(); j++) {
                                    // each negation needs to have either not existed, or be deleted
                                    // for the non-existence case, we use a positive negation instead
                                    auto negatedAtom = negations[j]->getAtom()->clone();
                                    // negatedAtom->setName(translateDiffAppliedRelation(getAtomRelation(negatedAtom, program))->get()->getName());
                                    rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(negatedAtom)));
                                }

                                rdiff->clearNegations();

                                // create a subsumption negation so we don't re-insert previously discovered tuples
                                auto diffAppliedHeadAtom = cl->getHead()->clone();
                                diffAppliedHeadAtom->setName(translateDiffAppliedRelation(getAtomRelation(diffAppliedHeadAtom, program))->get()->getName());

                                // write into new_diff_plus
                                rdiff->getHead()->setName(translateNewDiffMinusRelation(rel)->get()->getName());

                                // if we have incremental evaluation, we use iteration counts to simulate delta relations
                                // rather than explicitly having a separate relation
                                rdiff->addToBody(std::make_unique<AstSubsumptionNegation>(
                                        std::unique_ptr<AstAtom>(diffAppliedHeadAtom), 1));

                                std::cout << "recursive: " << *rdiff << std::endl;

                                // use delta versions of relations for semi-naive evaluation
                                for (size_t j = 0; j < atoms.size(); j++) {
                                    if (!isInSameSCC(getAtomRelation(atoms[j], program))) {
                                        continue;
                                    }

                                    // create clone
                                    std::unique_ptr<AstClause> r1(rdiff->clone());

                                    r1->getAtoms()[j]->setName(translateDeltaRelation(getAtomRelation(atoms[j], program))->get()->getName());

                                    // any atoms after atom j should not be in delta, check this by a constraint on the iteration number
                                    for (size_t k = j + 1; k < atoms.size(); k++) {
                                        if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                                            r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                                        std::unique_ptr<AstArgument>(r1->getAtoms()[k]->getArgument(r1->getAtoms()[k]->getArity() - 3)->clone()),
                                                        std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                        }
                                    }

                                    std::cout << "recursive: " << *r1 << std::endl;

                                    // reorder cl so that the deletedTuple atom is evaluated first
                                    std::vector<unsigned int> reordering;
                                    reordering.push_back(j);
                                    for (unsigned int k = 0; k < r1->getAtoms().size(); k++) {
                                        if (k != j) {
                                            reordering.push_back(k);
                                        }
                                    }

                                    // r1->reorderAtoms(reordering);

                                    // translate rdiff
                                    std::unique_ptr<RamStatement> rule = ClauseTranslator(*this).translateClause(*r1, *r1);

                                    // add logging
                                    if (Global::config().has("profile")) {
                                        const std::string& relationName = toString(rel->getName());
                                        const SrcLocation& srcLocation = r1->getSrcLoc();
                                        const std::string clText = stringify(toString(*r1));
                                        const std::string logTimerStatement =
                                                LogStatement::tRecursiveRule(relationName, version, srcLocation, clText);
                                        const std::string logSizeStatement =
                                                LogStatement::nRecursiveRule(relationName, version, srcLocation, clText);
                                        rule = std::make_unique<RamSequence>(std::make_unique<RamLogRelationTimer>(std::move(rule),
                                                logTimerStatement, std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                                    }

                                    // add debug info
                                    std::ostringstream ds;
                                    ds << toString(*r1) << "\nin file ";
                                    ds << r1->getSrcLoc();
                                    rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                                    // add rule to result
                                    appendStmt(loopRelSeq, std::move(rule));
                                }
                            }

                            /*
                            // add a constraint saying that at least one body tuple should be in the diff_plus version of the relation
                            std::vector<AstConstraint*> existsDiffMinus;
                            for (size_t i = 0; i < atoms.size(); i++) {
                                // ensure tuple is actually deleted
                                auto curAtom = atoms[i]->clone();
                                curAtom->setName(translateDiffMinusCountRelation(getAtomRelation(atoms[i], program))->get()->getName());

                                curAtom->setArgument(curAtom->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                curAtom->setArgument(curAtom->getArity() - 2, std::make_unique<AstNumberConstant>(-1));

                                // prevent double insertions across epochs
                                auto noPrevious = atoms[i]->clone();
                                noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(atoms[i], program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstNumberConstant>(1));
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstNumberConstant>(0));
                                // noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                existsDiffMinus.push_back(new AstConjunctionConstraint(std::make_unique<AstExistenceCheck>(std::unique_ptr<AstAtom>(curAtom)), std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious))));
                            }

                            if (existsDiffMinus.size() > 0) {
                                rdiff->addToBody(toAstDisjunction(existsDiffMinus));
                            }
                            */
                            /*
                            // set atom i to use the diff relation
                            rdiff->getAtoms()[j]->setName(translateDiffMinusCountRelation(getAtomRelation(atom, program))->get()->getName());

                            for (size_t k = 0; k < j; k++) {
                                auto& atomK = atoms[k];

                                // add a negation to the rule stating that the tuple shouldn't be deleted
                                // this prevents double counting, e.g., if we have:
                                // A(x, y) :- B(x, y), diff-C(x, y).
                                // A(x, y) :- diff-B(x, y), diff-appliedC(x, y).
                                // then we would double delete if we delete B(1, 2) and also C(1, 2)
                                auto noDeletionNegation = atomK->clone();
                                noDeletionNegation->setName(translateDiffMinusRelation(getAtomRelation(atomK, program))->get()->getName());
                                noDeletionNegation->setArgument(noDeletionNegation->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                noDeletionNegation->setArgument(noDeletionNegation->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                                noDeletionNegation->setArgument(noDeletionNegation->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstNegation>(std::unique_ptr<AstAtom>(noDeletionNegation)));
                            }

                            for (size_t k = j + 1; k < atoms.size(); k++) {
                                auto& atomK = atoms[k];
                                rdiff->getAtoms()[k]->setName(translateDiffMinusAppliedRelation(getAtomRelation(atomK, program))->get()->getName());
                            }

                            / *
                            for (size_t k = j; k < atoms.size(); k++) {
                                auto& atomK = atoms[k];

                                // add a negation to the rule stating that the tuple shouldn't be inserted
                                // this prevents double counting, e.g., if we have:
                                // A(x, y) :- B(x, y), diff+C(x, y).
                                // A(x, y) :- diff+B(x, y), diff+appliedC(x, y).
                                // then we would double insert if we insert B(1, 2) and also C(1, 2)
                                auto noPrevious = atomK->clone();
                                noPrevious->setName(translateDiffAppliedRelation(getAtomRelation(atomK, program))->get()->getName());
                                noPrevious->setArgument(noPrevious->getArity() - 1, std::make_unique<AstUnnamedVariable>());
                                noPrevious->setArgument(noPrevious->getArity() - 2, std::make_unique<AstUnnamedVariable>());
                                noPrevious->setArgument(noPrevious->getArity() - 3, std::make_unique<AstUnnamedVariable>());

                                rdiff->addToBody(std::make_unique<AstPositiveNegation>(std::unique_ptr<AstAtom>(noPrevious)));
                            }
                            */
                        }

                        /*
                        // reorder cl so that the deletedTuple atom is evaluated first
                        std::vector<unsigned int> reordering;
                        reordering.push_back(j);
                        for (unsigned int k = 0; k < atoms.size(); k++) {
                            if (k != j) {
                                reordering.push_back(k);
                            }
                        }

                        std::cout << "recursive: " << *rdiff << " reorder: " << reordering << std::endl;
                        */

                        // std::cout << "recursive: " << *rdiff << std::endl;

                        // for (size_t k = 0; k < atoms.size(); ++k) {
                            /*
                            AstAtom* atom = atoms[k];
                            const AstRelation* atomRelation = getAtomRelation(atom, program);

                            // only interested in atoms within the same SCC
                            if (!isInSameSCC(atomRelation)) {
                                continue;
                            }
                            */

                        /*
                            auto diffAppliedHeadAtom = cl->getHead()->clone();
                            diffAppliedHeadAtom->setName(translateDiffAppliedRelation(getAtomRelation(diffAppliedHeadAtom, program))->get()->getName());

                            // modify the processed rule to use relDelta and write to relNew
                            std::unique_ptr<AstClause> r1(rdiff->clone());

                            if (isInsertionRule) {
                                r1->getHead()->setName(translateNewDiffPlusRelation(rel)->get()->getName());
                            } else {
                                r1->getHead()->setName(translateNewDiffMinusRelation(rel)->get()->getName());
                            }

                            // if we have incremental evaluation, we use iteration counts to simulate delta relations
                            // rather than explicitly having a separate relation
                            r1->addToBody(std::make_unique<AstSubsumptionNegation>(
                                    std::unique_ptr<AstAtom>(diffAppliedHeadAtom), 1));
                                    */

                            /*
                            // simulate the delta relation with a constraint on the iteration number
                            r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GE,
                                        std::unique_ptr<AstArgument>(atom->getArgument(atom->getArity() - 3)->clone()),
                                        std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                        */

                            // replace wildcards with variables (reduces indices when wildcards are used in recursive
                            // atoms)
                            // nameUnnamedVariables(r1.get());

                            /*
                            // reduce R to P ...
                            for (size_t l = k + 1; l < atoms.size(); l++) {
                                if (isInSameSCC(getAtomRelation(atoms[l], program))) {
                                    r1->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                                                std::unique_ptr<AstArgument>(r1->getAtoms()[l]->getArgument(r1->getAtoms()[l]->getArity() - 3)->clone()),
                                                std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
                                }
                            }
                            */

                            /*
                            std::cout << "TRANSLATING RULE: " << *r1 << std::endl;

                            // r1->reorderAtoms(reordering);

                            std::unique_ptr<RamStatement> rule =
                                    ClauseTranslator(*this).translateClause(*r1, *cl, version);

                            // add logging
                            if (Global::config().has("profile")) {
                                const std::string& relationName = toString(rel->getName());
                                const SrcLocation& srcLocation = cl->getSrcLoc();
                                const std::string clauseText = stringify(toString(*cl));
                                const std::string logTimerStatement =
                                        LogStatement::tRecursiveRule(relationName, version, srcLocation, clauseText);
                                const std::string logSizeStatement =
                                        LogStatement::nRecursiveRule(relationName, version, srcLocation, clauseText);
                                rule = std::make_unique<RamSequence>(
                                        std::make_unique<RamLogRelationTimer>(std::move(rule), logTimerStatement,
                                                std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                            }

                            // add debug info
                            std::ostringstream ds;
                            ds << toString(*r1) << "\nin file ";
                            ds << r1->getSrcLoc();
                            rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                            // add to loop body
                            appendStmt(loopRelSeq, std::move(rule));
                            */

                            // increment version counter
                            version++;
                        // }

                    // }
                }
            } else {
                for (size_t j = 0; j < atoms.size(); ++j) {
                    AstAtom* atom = atoms[j];
                    const AstRelation* atomRelation = getAtomRelation(atom, program);

                    // only interested in atoms within the same SCC
                    if (!isInSameSCC(atomRelation)) {
                        continue;
                    }

                    // modify the processed rule to use relDelta and write to relNew
                    std::unique_ptr<AstClause> r1(cl->clone());
                    r1->getHead()->setName(relNew[rel]->get()->getName());
                    // if we have incremental evaluation, we use iteration counts to simulate delta relations
                    // rather than explicitly having a separate relation
                    if (!Global::config().has("incremental")) {
                        r1->getAtoms()[j]->setName(relDelta[atomRelation]->get()->getName());
                    }
                    if (Global::config().has("provenance")) {
                        size_t numberOfHeights = rel->numberOfHeightParameters();
                        r1->addToBody(std::make_unique<AstSubsumptionNegation>(
                                std::unique_ptr<AstAtom>(cl->getHead()->clone()), 1 + numberOfHeights));
                    } else {
                        if (r1->getHead()->getArity() > 0)
                            r1->addToBody(std::make_unique<AstNegation>(
                                    std::unique_ptr<AstAtom>(cl->getHead()->clone())));
                    }

                    // replace wildcards with variables (reduces indices when wildcards are used in recursive
                    // atoms)
                    nameUnnamedVariables(r1.get());

                    // reduce R to P ...
                    for (size_t k = j + 1; k < atoms.size(); k++) {
                        if (isInSameSCC(getAtomRelation(atoms[k], program))) {
                            AstAtom* cur = r1->getAtoms()[k]->clone();
                            cur->setName(relDelta[getAtomRelation(atoms[k], program)]->get()->getName());
                            r1->addToBody(std::make_unique<AstNegation>(std::unique_ptr<AstAtom>(cur)));
                        }
                    }

                    std::unique_ptr<RamStatement> rule =
                            ClauseTranslator(*this).translateClause(*r1, *cl, version);

                    /* add logging */
                    if (Global::config().has("profile")) {
                        const std::string& relationName = toString(rel->getName());
                        const SrcLocation& srcLocation = cl->getSrcLoc();
                        const std::string clauseText = stringify(toString(*cl));
                        const std::string logTimerStatement =
                                LogStatement::tRecursiveRule(relationName, version, srcLocation, clauseText);
                        const std::string logSizeStatement =
                                LogStatement::nRecursiveRule(relationName, version, srcLocation, clauseText);
                        rule = std::make_unique<RamSequence>(
                                std::make_unique<RamLogRelationTimer>(std::move(rule), logTimerStatement,
                                        std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
                    }

                    // add debug info
                    std::ostringstream ds;
                    ds << toString(*cl) << "\nin file ";
                    ds << cl->getSrcLoc();
                    rule = std::make_unique<RamDebugInfo>(std::move(rule), ds.str());

                    // add to loop body
                    appendStmt(loopRelSeq, std::move(rule));

                    // increment version counter
                    version++;
                }
            }
            assert(cl->getExecutionPlan() == nullptr || version > cl->getExecutionPlan()->getMaxVersion());
        }

        // if there was no rule, continue
        if (!loopRelSeq) {
            continue;
        }

        // label all versions
        if (Global::config().has("profile")) {
            const std::string& relationName = toString(rel->getName());
            const SrcLocation& srcLocation = rel->getSrcLoc();
            const std::string logTimerStatement = LogStatement::tRecursiveRelation(relationName, srcLocation);
            const std::string logSizeStatement = LogStatement::nRecursiveRelation(relationName, srcLocation);
            loopRelSeq = std::make_unique<RamLogRelationTimer>(std::move(loopRelSeq), logTimerStatement,
                    std::unique_ptr<RamRelationReference>(relNew[rel]->clone()));
        }

        /* add rule computations of a relation to parallel statement */
        loopSeq->add(std::move(loopRelSeq));
    }

    /* construct exit conditions for odd and even iteration */
    auto addCondition = [](std::unique_ptr<RamCondition>& cond, std::unique_ptr<RamCondition> clause) {
        cond = ((cond) ? std::make_unique<RamConjunction>(std::move(cond), std::move(clause))
                       : std::move(clause));
    };

    std::unique_ptr<RamCondition> exitCond;
    for (const AstRelation* rel : scc) {
        if (Global::config().has("incremental")) {
            addCondition(exitCond, std::make_unique<RamEmptinessCheck>(
                                           std::unique_ptr<RamRelationReference>(translateNewDiffPlusRelation(rel)->clone())));
            addCondition(exitCond, std::make_unique<RamEmptinessCheck>(
                                           std::unique_ptr<RamRelationReference>(translateNewDiffMinusRelation(rel)->clone())));
        } else {
            addCondition(exitCond, std::make_unique<RamEmptinessCheck>(
                                           std::unique_ptr<RamRelationReference>(relNew[rel]->clone())));
        }
    }

    if (Global::config().has("incremental")) {
        ramProg->addSubroutine("scc_" + std::to_string(indexOfScc) + "_exit", makeIncrementalExitCondSubroutine(*maxIterRelationRef));
        std::vector<std::unique_ptr<RamExpression>> exitCondArgs;
        exitCondArgs.push_back(std::make_unique<RamIterationNumber>());
        addCondition(exitCond, std::make_unique<RamSubroutineCondition>("scc_" + std::to_string(indexOfScc) + "_exit", std::move(exitCondArgs)));
    }

    /* construct fixpoint loop  */
    std::unique_ptr<RamStatement> res;
    if (preamble) appendStmt(res, std::move(preamble));
    if (!loopSeq->getStatements().empty() && exitCond && clearTable && updateTable) {
        appendStmt(res, std::make_unique<RamLoop>(std::move(loopSeq), std::move(clearTable), 
                                std::make_unique<RamExit>(std::move(exitCond)), std::move(updateTable)));
    }
    if (postamble) {
        appendStmt(res, std::move(postamble));
    }
    if (res) return res;

    assert(false && "Not Implemented");
    return nullptr;
}

std::unique_ptr<RamStatement> AstTranslator::makeIncrementalCleanupSubroutine(const AstProgram& program) {
    // create a RamSequence for cleaning up all relations
    std::unique_ptr<RamStatement> cleanupSequence;

    for (const auto& relation : program.getRelations()) {
        // update every tuple in relation so that the previous and current counts match
        // FOR t0 in relation:
        //   INSERT (t0.0, t0.2, ..., -1, -1)
        // insert -1 as both counts and handle this case in the B-Tree update method

        // make a RamRelationReference to be used to build the subroutine
        auto relationReference = translateRelation(relation);

        // perform merges of diff and diff_applied relations for incremental, we want:
        // MERGE R <- R_diff
        // MERGE R_diff_applied <- R
        appendStmt(cleanupSequence, std::make_unique<RamMerge>(
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)->clone()),
                    std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));

        appendStmt(cleanupSequence, std::make_unique<RamMerge>(
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)->clone()),
                    std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))));

        /*
        appendStmt(cleanupSequence, std::make_unique<RamMerge>(
                    std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)->clone())));

        appendStmt(cleanupSequence, std::make_unique<RamMerge>(
                    std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)->clone())));

        appendStmt(cleanupSequence, std::make_unique<RamMerge>(
                    std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation)),
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)->clone())));
                    */

        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)->clone())));
        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation)->clone())));
        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)->clone())));
        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(relation)->clone())));
        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)->clone())));
        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation)->clone())));
        appendStmt(cleanupSequence, std::make_unique<RamClear>(std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)->clone())));
        
        // the subroutine needs to be built from inside out
        // build the insertion step first
        std::vector<std::unique_ptr<RamExpression>> updateTuple;
        
        // insert the original tuple
        for (size_t i = 0; i < relation->getArity() - 2; i++) {
            updateTuple.push_back(std::make_unique<RamTupleElement>(0, i));
        }

        // insert -1 for both counts
        updateTuple.push_back(std::make_unique<RamNumber>(-1));
        updateTuple.push_back(std::make_unique<RamNumber>(-1));

        // create the projection
        auto insertUpdate = std::make_unique<RamProject>(std::unique_ptr<RamRelationReference>(relationReference->clone()), std::move(updateTuple));

        // create the scan
        auto cleanupScan = std::make_unique<RamScan>(std::unique_ptr<RamRelationReference>(relationReference->clone()), 0, std::move(insertUpdate));
        appendStmt(cleanupSequence, std::make_unique<RamQuery>(std::move(cleanupScan)));
    }

    return cleanupSequence;
}

std::unique_ptr<RamStatement> AstTranslator::makeIncrementalExitCondSubroutine(const RamRelationReference& maxIterRelationRef) {
    // we want a subroutine that looks like:
    // FOR t0 in maxIterRel:
    //   IF t0.0 >= arg(iter):
    //     RETURN 0 NOW

    auto exitCondSequence = std::make_unique<RamSequence>();

    // make RamSubroutineReturnValue
    std::vector<std::unique_ptr<RamExpression>> returnFalseVal;
    returnFalseVal.push_back(std::make_unique<RamNumber>(0));
    auto returnFalse = std::make_unique<RamSubroutineReturnValue>(std::move(returnFalseVal), true);

    // make a RamCondition checking whether the maxIterRel tuple is > current iteration
    auto iterationConstraint = std::make_unique<RamConstraint>(BinaryConstraintOp::GE, std::make_unique<RamTupleElement>(0, 0), std::make_unique<RamSubroutineArgument>(0));

    // make RamFilter that returns false if the iteration number is greater
    auto iterationFilter = std::make_unique<RamFilter>(std::move(iterationConstraint), std::move(returnFalse));

    // create the RamScan
    auto exitCondScan = std::make_unique<RamScan>(std::unique_ptr<RamRelationReference>(maxIterRelationRef.clone()), 0, std::move(iterationFilter));
    exitCondSequence->add(std::make_unique<RamQuery>(std::move(exitCondScan)));

    // default case
    std::vector<std::unique_ptr<RamExpression>> returnTrueVal;
    returnTrueVal.push_back(std::make_unique<RamNumber>(1));
    auto returnTrue = std::make_unique<RamSubroutineReturnValue>(std::move(returnTrueVal));
    exitCondSequence->add(std::make_unique<RamQuery>(std::move(returnTrue)));

    /*
    // make a subroutine as exitCond(iterationNumber)
    auto exitCondSequence = std::make_unique<RamSequence>();

    for (const auto relation : scc) {
        // make a scan that checks the iteration number
        // FOR t0 in relation:
        //   IF t0.iteration > arg(iteration):
        //     RETURN 1 NOW

        // make a RamRelationReference to be used to build the subroutine
        auto relationReference = translateRelation(relation);

        // make RamSubroutineReturnValue
        std::vector<std::unique_ptr<RamExpression>> returnFalseVal;
        returnFalseVal.push_back(std::make_unique<RamNumber>(0));
        auto returnFalse = std::make_unique<RamSubroutineReturnValue>(std::move(returnFalseVal), true);

        // make a RamCondition saying that the iteration number of the current tuple is > current iteration
        auto iterationConstraint = std::make_unique<RamConstraint>(BinaryConstraintOp::GE, std::make_unique<RamTupleElement>(0, relation->getArity() - 3), std::make_unique<RamSubroutineArgument>(0));

        // make RamFilter that returns true if the iteration number is greater
        auto iterationFilter = std::make_unique<RamFilter>(std::move(iterationConstraint), std::move(returnFalse));

        // create the scan
        auto exitCondScan = std::make_unique<RamScan>(std::unique_ptr<RamRelationReference>(relationReference->clone()), 0, std::move(iterationFilter));
        exitCondSequence->add(std::make_unique<RamQuery>(std::move(exitCondScan)));
    }

    // default case
    std::vector<std::unique_ptr<RamExpression>> returnTrueVal;
    returnTrueVal.push_back(std::make_unique<RamNumber>(1));
    auto returnTrue = std::make_unique<RamSubroutineReturnValue>(std::move(returnTrueVal));
    exitCondSequence->add(std::make_unique<RamQuery>(std::move(returnTrue)));
    */

    return exitCondSequence;
}

/** make a subroutine to search for subproofs */
std::unique_ptr<RamStatement> AstTranslator::makeSubproofSubroutine(const AstClause& clause) {
    // make intermediate clause with constraints
    std::unique_ptr<AstClause> intermediateClause(clause.clone());

    // name unnamed variables
    nameUnnamedVariables(intermediateClause.get());

    // add constraint for each argument in head of atom
    AstAtom* head = intermediateClause->getHead();
    size_t numberOfHeights = program->getRelation(head->getName())->numberOfHeightParameters();
    for (size_t i = 0; i < head->getArguments().size() - 1 - numberOfHeights; i++) {
        auto arg = head->getArgument(i);

        if (auto var = dynamic_cast<AstVariable*>(arg)) {
            intermediateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(var->clone()), std::make_unique<AstSubroutineArgument>(i)));
        } else if (auto func = dynamic_cast<AstFunctor*>(arg)) {
            intermediateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(func->clone()), std::make_unique<AstSubroutineArgument>(i)));
        } else if (auto rec = dynamic_cast<AstRecordInit*>(arg)) {
            intermediateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(rec->clone()), std::make_unique<AstSubroutineArgument>(i)));
        }
    }

    if (Global::config().get("provenance") == "subtreeHeights") {
        // starting index of subtree level arguments in argument list
        // starts immediately after original arguments as height and rulenumber of tuple are not passed to
        // subroutine
        size_t levelIndex = head->getArguments().size() - numberOfHeights - 1;

        // add level constraints
        for (size_t i = 0; i < intermediateClause->getBodyLiterals().size(); i++) {
            auto lit = intermediateClause->getBodyLiteral(i);
            if (auto atom = dynamic_cast<AstAtom*>(lit)) {
                auto arity = atom->getArity();
                auto literalHeights = program->getRelation(atom->getName())->numberOfHeightParameters();
                auto literalLevelIndex = arity - literalHeights;

                intermediateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                        std::unique_ptr<AstArgument>(atom->getArgument(literalLevelIndex)->clone()),
                        std::make_unique<AstSubroutineArgument>(levelIndex)));
            }
            levelIndex++;
        }
    } else {
        // index of level argument in argument list
        size_t levelIndex = head->getArguments().size() - numberOfHeights - 1;

        // add level constraints
        for (size_t i = 0; i < intermediateClause->getBodyLiterals().size(); i++) {
            auto lit = intermediateClause->getBodyLiteral(i);
            if (auto atom = dynamic_cast<AstAtom*>(lit)) {
                auto arity = atom->getArity();

                // arity - 1 is the level number in body atoms
                intermediateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                        std::unique_ptr<AstArgument>(atom->getArgument(arity - 1)->clone()),
                        std::make_unique<AstSubroutineArgument>(levelIndex)));
            }
        }
    }
    return ProvenanceClauseTranslator(*this).translateClause(*intermediateClause, clause);
}

/** make a subroutine to search for subproofs for the non-existence of a tuple */
std::unique_ptr<RamStatement> AstTranslator::makeNegationSubproofSubroutine(const AstClause& clause) {
    // TODO (taipan-snake): Currently we only deal with atoms (no constraints or negations or aggregates
    // or anything else...)

    // clone clause for mutation
    auto clauseReplacedAggregates = std::unique_ptr<AstClause>(clause.clone());

    int aggNumber = 0;
    struct AggregatesToVariables : public AstNodeMapper {
        int& aggNumber;

        AggregatesToVariables(int& aggNumber) : aggNumber(aggNumber) {}

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            if (dynamic_cast<AstAggregator*>(node.get())) {
                return std::make_unique<AstVariable>("agg_" + std::to_string(aggNumber++));
            }

            node->apply(*this);
            return node;
        }
    };

    AggregatesToVariables aggToVar(aggNumber);
    clauseReplacedAggregates->apply(aggToVar);

    // build a vector of unique variables
    std::vector<const AstVariable*> uniqueVariables;

    visitDepthFirst(*clauseReplacedAggregates, [&](const AstVariable& var) {
        if (var.getName().find("@level_num") == std::string::npos) {
            // use find_if since uniqueVariables stores pointers, and we need to dereference the pointer to
            // check equality
            if (std::find_if(uniqueVariables.begin(), uniqueVariables.end(),
                        [&](const AstVariable* v) { return *v == var; }) == uniqueVariables.end()) {
                uniqueVariables.push_back(&var);
            }
        }
    });

    // a mapper to replace variables with subroutine arguments
    struct VariablesToArguments : public AstNodeMapper {
        const std::vector<const AstVariable*>& uniqueVariables;

        VariablesToArguments() = default;
        VariablesToArguments(const std::vector<const AstVariable*>& uniqueVariables)
                : uniqueVariables(uniqueVariables) {}

        std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
            // replace unknown variables
            if (auto varPtr = dynamic_cast<const AstVariable*>(node.get())) {
                if (varPtr->getName().find("@level_num") == std::string::npos) {
                    size_t argNum = std::find_if(uniqueVariables.begin(), uniqueVariables.end(),
                                            [&](const AstVariable* v) { return *v == *varPtr; }) -
                                    uniqueVariables.begin();

                    return std::make_unique<AstSubroutineArgument>(argNum);
                } else {
                    return std::make_unique<AstUnnamedVariable>();
                }
            }

            // apply recursive
            node->apply(*this);

            // otherwise nothing
            return node;
        }
    };

    // the structure of this subroutine is a sequence where each nested statement is a search in each
    // relation
    std::unique_ptr<RamSequence> searchSequence = std::make_unique<RamSequence>();

    // make a copy so that when we mutate clause, pointers to objects in newClause are not affected
    auto newClause = std::unique_ptr<AstClause>(clauseReplacedAggregates->clone());

    // go through each body atom and create a return
    size_t litNumber = 0;
    for (const auto& lit : newClause->getBodyLiterals()) {
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            size_t numberOfHeights = program->getRelation(atom->getName())->numberOfHeightParameters();
            // get a RamRelationReference
            auto relRef = translateRelation(atom);

            // construct a query
            std::vector<std::unique_ptr<RamExpression>> query;

            // translate variables to subroutine arguments
            VariablesToArguments varsToArgs(uniqueVariables);
            atom->apply(varsToArgs);

            // add each value (subroutine argument) to the search query
            for (size_t i = 0; i < atom->getArity() - 1 - numberOfHeights; i++) {
                auto arg = atom->getArgument(i);
                query.push_back(translateValue(arg, ValueIndex()));
            }

            // fill up query with nullptrs for the provenance columns
            query.push_back(std::make_unique<RamUndefValue>());
            for (size_t h = 0; h < numberOfHeights; h++) {
                query.push_back(std::make_unique<RamUndefValue>());
            }

            // ensure the length of query tuple is correct
            assert(query.size() == atom->getArity() && "wrong query tuple size");

            // make the nested operation to return the atom number if it exists
            std::vector<std::unique_ptr<RamExpression>> returnValue;
            returnValue.push_back(std::make_unique<RamNumber>(litNumber));

            // create a search
            // a filter to find whether the current atom exists or not
            auto searchFilter = std::make_unique<RamFilter>(
                    std::make_unique<RamExistenceCheck>(
                            std::unique_ptr<RamRelationReference>(relRef->clone()), std::move(query)),
                    std::make_unique<RamSubroutineReturnValue>(std::move(returnValue)));

            // now, return the values of the atoms, with a separator
            // between atom number and atom
            std::vector<std::unique_ptr<RamExpression>> returnAtom;
            returnAtom.push_back(std::make_unique<RamUndefValue>());
            // the actual atom
            for (size_t i = 0; i < atom->getArity() - 1 - numberOfHeights; i++) {
                returnAtom.push_back(translateValue(atom->getArgument(i), ValueIndex()));
            }

            // chain the atom number and atom value together
            auto atomSequence = std::make_unique<RamSequence>();
            atomSequence->add(std::make_unique<RamQuery>(std::move(searchFilter)));
            atomSequence->add(std::make_unique<RamQuery>(
                    std::make_unique<RamSubroutineReturnValue>(std::move(returnAtom))));

            // append search to the sequence
            searchSequence->add(std::move(atomSequence));
        } else if (auto con = dynamic_cast<AstConstraint*>(lit)) {
            VariablesToArguments varsToArgs(uniqueVariables);
            con->apply(varsToArgs);

            // translate to a RamCondition
            auto condition = translateConstraint(con, ValueIndex());

            // create a return value
            std::vector<std::unique_ptr<RamExpression>> returnValue;
            returnValue.push_back(std::make_unique<RamNumber>(litNumber));

            // create a filter
            auto filter = std::make_unique<RamFilter>(
                    std::move(condition), std::make_unique<RamSubroutineReturnValue>(std::move(returnValue)));

            // now, return the values of the literal, with a separator
            // between atom number and atom
            std::vector<std::unique_ptr<RamExpression>> returnLit;
            returnLit.push_back(std::make_unique<RamUndefValue>());
            // add return values for binary constraints and negations
            if (auto binaryConstraint = dynamic_cast<AstBinaryConstraint*>(con)) {
                returnLit.push_back(translateValue(binaryConstraint->getLHS(), ValueIndex()));
                returnLit.push_back(translateValue(binaryConstraint->getRHS(), ValueIndex()));
            } else if (auto negation = dynamic_cast<AstNegation*>(con)) {
                auto vals = negation->getAtom()->getArguments();
                auto numberOfHeights =
                        program->getRelation(negation->getAtom()->getName())->numberOfHeightParameters();
                for (size_t i = 0; i < vals.size() - 1 - numberOfHeights; i++) {
                    returnLit.push_back(translateValue(vals[i], ValueIndex()));
                }
            }

            // chain the atom number and atom value together
            auto litSequence = std::make_unique<RamSequence>();
            litSequence->add(std::make_unique<RamQuery>(std::move(filter)));
            litSequence->add(std::make_unique<RamQuery>(
                    std::make_unique<RamSubroutineReturnValue>(std::move(returnLit))));

            // append search to the sequence
            searchSequence->add(std::move(litSequence));
        }

        litNumber++;
    }

    return std::move(searchSequence);
}

/** translates the given datalog program into an equivalent RAM program  */
void AstTranslator::translateProgram(const AstTranslationUnit& translationUnit) {
    // obtain type environment from analysis
    typeEnv = &translationUnit.getAnalysis<TypeEnvironmentAnalysis>()->getTypeEnvironment();

    // obtain recursive clauses from analysis
    const auto* recursiveClauses = translationUnit.getAnalysis<RecursiveClauses>();

    // obtain strongly connected component (SCC) graph from analysis
    const auto& sccGraph = *translationUnit.getAnalysis<SCCGraph>();

    // obtain some topological order over the nodes of the SCC graph
    const auto& sccOrder = *translationUnit.getAnalysis<TopologicallySortedSCCGraph>();

    // obtain the schedule of relations expired at each index of the topological order
    const auto& expirySchedule = translationUnit.getAnalysis<RelationSchedule>()->schedule();

    // start with an empty sequence of ram statements
    std::unique_ptr<RamStatement> res = std::make_unique<RamSequence>();

    // start with an empty program
    ramProg = std::make_unique<RamProgram>(std::make_unique<RamSequence>());

    // handle the case of an empty SCC graph
    if (sccGraph.getNumberOfSCCs() == 0) return;

    // a function to load relations
    const auto& makeRamLoad = [&](std::unique_ptr<RamStatement>& current, const AstRelation* relation,
                                      const std::string& inputDirectory, const std::string& fileExtension) {
        std::unique_ptr<RamStatement> statement;
        if (Global::config().has("incremental")) {
            statement =
                std::make_unique<RamLoad>(std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                        getInputIODirectives(relation, Global::config().get(inputDirectory), fileExtension));
        } else {
            statement =
                std::make_unique<RamLoad>(std::unique_ptr<RamRelationReference>(translateRelation(relation)),
                        getInputIODirectives(relation, Global::config().get(inputDirectory), fileExtension));
        }
        if (Global::config().has("profile")) {
            const std::string logTimerStatement =
                    LogStatement::tRelationLoadTime(toString(relation->getName()), relation->getSrcLoc());
            statement = std::make_unique<RamLogRelationTimer>(std::move(statement), logTimerStatement,
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)));
        }
        appendStmt(current, std::move(statement));
    };

    // a function to store relations
    const auto& makeRamStore = [&](std::unique_ptr<RamStatement>& current, const AstRelation* relation,
                                       const std::string& outputDirectory, const std::string& fileExtension) {
        std::unique_ptr<RamStatement> statement = std::make_unique<RamStore>(
                std::unique_ptr<RamRelationReference>(translateRelation(relation)),
                getOutputIODirectives(relation, Global::config().get(outputDirectory), fileExtension));
        if (Global::config().has("profile")) {
            const std::string logTimerStatement =
                    LogStatement::tRelationSaveTime(toString(relation->getName()), relation->getSrcLoc());
            statement = std::make_unique<RamLogRelationTimer>(std::move(statement), logTimerStatement,
                    std::unique_ptr<RamRelationReference>(translateRelation(relation)));
        }
        appendStmt(current, std::move(statement));
    };

    // a function to drop relations
    const auto& makeRamDrop = [&](std::unique_ptr<RamStatement>& current, const AstRelation* relation) {
        appendStmt(current, std::make_unique<RamDrop>(translateRelation(relation)));
    };

    // maintain the index of the SCC within the topological order
    size_t indexOfScc = 0;

    // iterate over each SCC according to the topological order
    for (const auto& scc : sccOrder.order()) {
        // make a new ram statement for the current SCC
        std::unique_ptr<RamStatement> current;

        // find out if the current SCC is recursive
        const auto& isRecursive = sccGraph.isRecursive(scc);

        // make variables for particular sets of relations contained within the current SCC, and,
        // predecessors and successor SCCs thereof
        const auto& allInterns = sccGraph.getInternalRelations(scc);
        const auto& internIns = sccGraph.getInternalInputRelations(scc);
        const auto& internOuts = sccGraph.getInternalOutputRelations(scc);
        const auto& externOutPreds = sccGraph.getExternalOutputPredecessorRelations(scc);
        const auto& externNonOutPreds = sccGraph.getExternalNonOutputPredecessorRelations(scc);

        // const auto& externPreds = sccGraph.getExternalPredecessorRelations(scc);
        // const auto& internsWithExternSuccs = sccGraph.getInternalRelationsWithExternalSuccessors(scc);
        const auto& internNonOutsWithExternSuccs =
                sccGraph.getInternalNonOutputRelationsWithExternalSuccessors(scc);

        // make a variable for all relations that are expired at the current SCC
        const auto& internExps = expirySchedule.at(indexOfScc).expired();

        // create all internal relations of the current scc
        for (const auto& relation : allInterns) {
            appendStmt(current, std::make_unique<RamCreate>(
                                        std::unique_ptr<RamRelationReference>(translateRelation(relation))));
            
            if (Global::config().has("incremental")) {
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(
                                            std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation))));
            }

            // create new and delta relations if required
            if (isRecursive) {
                appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                            translateDeltaRelation(relation))));
                appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                            translateNewRelation(relation))));
                if (Global::config().has("incremental")) {
                    appendStmt(current, std::make_unique<RamCreate>(
                                                std::unique_ptr<RamRelationReference>(translatePreviousIndexedRelation(relation))));

                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateNewDiffPlusRelation(relation))));
                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateNewDiffMinusRelation(relation))));

                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateDeltaDiffMinusAppliedRelation(relation))));
                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateDeltaDiffMinusCountRelation(relation))));
                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateDeltaDiffPlusCountRelation(relation))));
                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateTemporaryDeltaDiffAppliedRelation(relation))));
                    appendStmt(current, std::make_unique<RamCreate>(std::unique_ptr<RamRelationReference>(
                                                translateDeltaDiffAppliedRelation(relation))));
                }
            }
        }

        {
            // load all internal input relations from the facts dir with a .facts extension
            for (const auto& relation : internIns) {
                makeRamLoad(current, relation, "fact-dir", ".facts");

                /*
                // if incremental, perform a RamSwap so that the loaded facts are stored in the diff relation
                if (Global::config().has("incremental")) {
                    appendStmt(current, std::make_unique<RamSwap>(
                                std::unique_ptr<RamRelationReference>(translateRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffRelation(relation))));
                }
                */
            }

            // if a communication engine has been specified...
            if (Global::config().has("engine")) {
                // load all external output predecessor relations from the output dir with a .csv
                // extension
                for (const auto& relation : externOutPreds) {
                    makeRamLoad(current, relation, "output-dir", ".csv");
                }
                // load all external output predecessor relations from the output dir with a .facts
                // extension
                for (const auto& relation : externNonOutPreds) {
                    makeRamLoad(current, relation, "output-dir", ".facts");
                }
            }
        }

        if (Global::config().has("incremental")) {
            if (isRecursive) {
                for (const auto& relation : internIns) {
                    // populate diff_plus_applied relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))));
                    /*
                    appendStmt(current, std::make_unique<RamPositiveMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))
                                ));
                                */
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))
                                ));

                    // populate diff_minus_applied relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))));
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));

                    // populate diff_applied relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))));
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));
                    /*
                    appendStmt(current, std::make_unique<RamPositiveMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))
                                ));
                                */
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))
                                ));

                    // populate diff_plus_count relation
                    /*
                    appendStmt(current, std::make_unique<RamPositiveMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))
                                ));
                                */
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))
                                ));
                    appendStmt(current, std::make_unique<RamSemiMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation))));

                    // populate diff_minus_count relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));
                    appendStmt(current, std::make_unique<RamSemiMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation))));
                }
            }
        }

        // compute the relations themselves
        std::unique_ptr<RamStatement> bodyStatement =
                (!isRecursive) ? translateNonRecursiveRelation(
                                         *((const AstRelation*)*allInterns.begin()), recursiveClauses)
                               : translateRecursiveRelation(allInterns, recursiveClauses, indexOfScc);
        appendStmt(current, std::move(bodyStatement));

        if (Global::config().has("incremental")) {
            // make merges for non-recursive relations
            if (!isRecursive) {
                for (const auto& relation : allInterns) {
                    /*
                    if (relation->getClauses().size() == 0) {
                        continue;
                    }
                    */

                    // populate diff_plus_applied relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))));
                    /*
                    appendStmt(current, std::make_unique<RamPositiveMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))
                                ));
                                */
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))
                                ));

                    // populate diff_minus_applied relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))));
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));

                    // populate diff_applied relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))));
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));
                    /*
                    appendStmt(current, std::make_unique<RamPositiveMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))
                                ));
                                */
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffAppliedRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))
                                ));

                    // populate diff_plus_count relation
                    /*
                    appendStmt(current, std::make_unique<RamPositiveMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateRelation(relation))
                                ));
                                */
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusRelation(relation))
                                ));
                    appendStmt(current, std::make_unique<RamSemiMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffPlusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusAppliedRelation(relation))));

                    // populate diff_minus_count relation
                    appendStmt(current, std::make_unique<RamMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffMinusRelation(relation))));
                    appendStmt(current, std::make_unique<RamSemiMerge>(
                                std::unique_ptr<RamRelationReference>(translateDiffMinusCountRelation(relation)),
                                std::unique_ptr<RamRelationReference>(translateDiffPlusAppliedRelation(relation))));
                }

            }
        }

        {
            // if a communication engine is enabled...
            if (Global::config().has("engine")) {
                // store all internal non-output relations with external successors to the output dir with
                // a .facts extension
                for (const auto& relation : internNonOutsWithExternSuccs) {
                    makeRamStore(current, relation, "output-dir", ".facts");
                }
            }

            if (!Global::config().has("incremental")) {
                // store all internal output relations to the output dir with a .csv extension
                for (const auto& relation : internOuts) {
                    makeRamStore(current, relation, "output-dir", ".csv");
                }
            }
        }

        // if provenance is not enabled...
        if (!Global::config().has("provenance") && !Global::config().has("incremental")) {
            // if a communication engine is enabled...
            if (Global::config().has("engine")) {
                // drop all internal relations
                for (const auto& relation : allInterns) {
                    makeRamDrop(current, relation);
                }
                // drop external output predecessor relations
                for (const auto& relation : externOutPreds) {
                    makeRamDrop(current, relation);
                }
                // drop external non-output predecessor relations
                for (const auto& relation : externNonOutPreds) {
                    makeRamDrop(current, relation);
                }
            } else {
                // otherwise, drop all  relations expired as per the topological order
                for (const auto& relation : internExps) {
                    makeRamDrop(current, relation);
                }
            }
        }

        // add the cleanup subroutine
        if (Global::config().has("incremental") && indexOfScc == sccGraph.getNumberOfSCCs() - 1) {
            // make subroutine condition, don't actually use return value
            auto cleanupCond = std::make_unique<RamSubroutineCondition>("incremental_cleanup");

            // put it into a RamExit
            appendStmt(current, std::make_unique<RamExit>(std::move(cleanupCond), false));

            // process stores after cleanup
            for (const auto& scc : sccOrder.order()) {
                // store all internal output relations to the output dir with a .csv extension
                for (const auto& relation : sccGraph.getInternalOutputRelations(scc)) {
                    makeRamStore(current, relation, "output-dir", ".csv");
                }
            }
        }

        if (current) {
            // append the current SCC as a stratum to the sequence
            appendStmt(res, std::make_unique<RamStratum>(std::move(current), indexOfScc));

            // increment the index of the current SCC
            indexOfScc++;
        }

    }

    /*
    */

    // add main timer if profiling
    if (res && Global::config().has("profile")) {
        res = std::make_unique<RamLogTimer>(std::move(res), LogStatement::runtime());
    }

    // done for main prog
    ramProg->setMain(std::move(res));

    // add subroutines for each clause
    if (Global::config().has("provenance")) {
        visitDepthFirst(program->getRelations(), [&](const AstClause& clause) {
            std::stringstream relName;
            relName << clause.getHead()->getName();

            // do not add subroutines for info relations or facts
            if (relName.str().find("@info") != std::string::npos || clause.getBodyLiterals().empty()) {
                return;
            }

            std::string subroutineLabel =
                    relName.str() + "_" + std::to_string(clause.getClauseNum()) + "_subproof";
            ramProg->addSubroutine(subroutineLabel, makeSubproofSubroutine(clause));

            std::string negationSubroutineLabel =
                    relName.str() + "_" + std::to_string(clause.getClauseNum()) + "_negation_subproof";
            ramProg->addSubroutine(negationSubroutineLabel, makeNegationSubproofSubroutine(clause));
        });
    }

    // add cleanup subroutine for incremental
    if (Global::config().has("incremental")) {
        ramProg->addSubroutine("incremental_cleanup", makeIncrementalCleanupSubroutine(*translationUnit.getProgram()));
    }
}

std::unique_ptr<RamTranslationUnit> AstTranslator::translateUnit(AstTranslationUnit& tu) {
    auto ram_start = std::chrono::high_resolution_clock::now();
    program = tu.getProgram();
    translateProgram(tu);
    SymbolTable& symTab = tu.getSymbolTable();
    ErrorReport& errReport = tu.getErrorReport();
    DebugReport& debugReport = tu.getDebugReport();
    if (!Global::config().get("debug-report").empty()) {
        if (ramProg) {
            auto ram_end = std::chrono::high_resolution_clock::now();
            std::string runtimeStr =
                    "(" + std::to_string(std::chrono::duration<double>(ram_end - ram_start).count()) + "s)";
            std::stringstream ramProgStr;
            ramProgStr << *ramProg;
            debugReport.addSection(DebugReporter::getCodeSection(
                    "ram-program", "RAM Program " + runtimeStr, ramProgStr.str()));
        }
    }
    return std::make_unique<RamTranslationUnit>(std::move(ramProg), symTab, errReport, debugReport);
}

}  // end of namespace souffle
