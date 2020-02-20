/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2017, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file IncrementalTransformer.cpp
 *
 * Implements AstTransformer for adding incremental information via two extra columns
 *
 ***********************************************************************/

#include "AstArgument.h"
#include "AstAttribute.h"
#include "AstClause.h"
#include "AstLiteral.h"
#include "AstNode.h"
#include "AstProgram.h"
#include "AstRelation.h"
#include "AstRelationIdentifier.h"
#include "AstTransforms.h"
#include "AstTranslationUnit.h"
#include "AstType.h"
#include "AstVisitor.h"
#include "BinaryConstraintOps.h"
#include "FunctorOps.h"
#include "RelationRepresentation.h"
#include "PrecedenceGraph.h"
#include "Util.h"
#include <cassert>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace souffle {

/**
 * Adds unnamed variables to each atom
 * Intended to be used inside negations and constraints
 */
struct addUnnamedVariables : public AstNodeMapper {
    using AstNodeMapper::operator();

    std::unique_ptr<AstNode> operator()(std::unique_ptr<AstNode> node) const override {
        // add provenance columns
        if (auto atom = dynamic_cast<AstAtom*>(node.get())) {
            atom->addArgument(std::make_unique<AstUnnamedVariable>());
            atom->addArgument(std::make_unique<AstUnnamedVariable>());
        } else if (auto neg = dynamic_cast<AstNegation*>(node.get())) {
            auto atom = neg->getAtom();
            atom->addArgument(std::make_unique<AstUnnamedVariable>());
            atom->addArgument(std::make_unique<AstUnnamedVariable>());
        } else if (auto agg = dynamic_cast<AstAggregator*>(node.get())) {
            if (auto innerVar = dynamic_cast<const AstVariable*>(agg->getTargetExpression())) {
                if (innerVar->getName() == "@current_epoch_value") {
                    return node;
                }
            }
        }

        // otherwise - apply mapper recursively
        node->apply(*this);
        return node;
    }
};

// apply a functor to a list of vars
AstArgument* applyFunctorToVars(std::vector<AstArgument*> levels, FunctorOp op) {
    if (levels.empty()) {
        return static_cast<AstArgument*>(new AstNumberConstant(0));
    }

    if (levels.size() == 1) {
        return static_cast<AstArgument*>(levels[0]);
    }

    auto currentCombination = new AstIntrinsicFunctor(op, std::unique_ptr<AstArgument>(levels[0]),
            std::unique_ptr<AstArgument>(levels[1]));

    for (size_t i = 2; i < levels.size(); i++) {
        currentCombination = new AstIntrinsicFunctor(op, std::unique_ptr<AstArgument>(currentCombination),
                std::unique_ptr<AstArgument>(levels[i]));
    }

    return static_cast<AstArgument*>(currentCombination);
}

template <typename T>
std::vector<T*> vectorClone(std::vector<T*> orig) {
    std::vector<T*> clones;
    for (auto elem : orig) {
        clones.push_back(elem->clone());
    }
    return clones;
}

/*
AstRelationIdentifier translateDiffRelationName(const AstRelationIdentifier& rel) {
    AstRelationIdentifier diffRelation = rel;
    diffRelation.prepend("diff@");

    return diffRelation;
}

AstRelationIdentifier translateDiffAppliedRelationName(const AstRelationIdentifier& rel) {
    AstRelationIdentifier diffRelation = rel;
    diffRelation.prepend("diff_applied@");

    return diffRelation;
}
*/

/**
 * This transforms a clause to process tuple deletions
 */
std::vector<AstClause*> IncrementalTransformer::makeNegativeUpdateClause(const AstClause& clause, const AstTranslationUnit& translationUnit) {
    // get the scc graph for generating iteration conditions
    const SCCGraph& sccGraph = *translationUnit.getAnalysis<SCCGraph>();

    // make a clone of the clause
    auto negativeUpdateClause = clause.clone();

    // make vector of negativeUpdateClauses
    std::vector<AstClause*> negativeUpdateClauses;

    // store the body counts to allow building arguments in the head atom
    std::vector<AstArgument*> bodyLevels;
    std::vector<AstArgument*> bodyPreviousCounts;
    // std::vector<AstArgument*> bodyCountDiffs;
    std::vector<AstArgument*> bodyCounts;

    // store number of atoms to be used for reordering
    size_t numAtoms = 0;

    for (size_t i = 0; i < negativeUpdateClause->getBodyLiterals().size(); i++) {

        auto lit = negativeUpdateClause->getBodyLiteral(i);

        // add unnamed vars to each atom nested in arguments of lit
        lit->apply(addUnnamedVariables());

        // add three incremental columns to lit; first is iteration number, second is count in previous epoch, third is count in current epoch
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            numAtoms++;

            atom->addArgument(std::make_unique<AstVariable>("@iteration_" + std::to_string(i)));
            atom->addArgument(std::make_unique<AstVariable>("@prev_count_" + std::to_string(i)));
            atom->addArgument(std::make_unique<AstVariable>("@current_count_" + std::to_string(i)));
            
            // store the iterations and body counts to build arguments later
            // get relation of head atom
            auto headRelation = translationUnit.getProgram()->getRelation(clause.getHead()->getName());
            if (contains(sccGraph.getInternalRelations(sccGraph.getSCC(headRelation)), translationUnit.getProgram()->getRelation(atom->getName()))) {
                bodyLevels.push_back(new AstVariable("@iteration_" + std::to_string(i)));
            }
            bodyPreviousCounts.push_back(new AstVariable("@prev_count_" + std::to_string(i)));
            // bodyCountDiffs.push_back(new AstIntrinsicFunctor(FunctorOp::SUB, std::make_unique<AstVariable>("@current_count_" + std::to_string(i)), std::make_unique<AstVariable>("@prev_count_" + std::to_string(i))));
            bodyCounts.push_back(new AstVariable("@current_count_" + std::to_string(i)));
        }
    }

    // add three incremental columns to head lit
    // first is the iteration number, which we get by adding 1 to the max iteration number over the body atoms
    // negativeUpdateClause->getHead()->addArgument(std::make_unique<AstIntrinsicFunctor>(FunctorOp::ADD, std::make_unique<AstNumberConstant>(1), std::unique_ptr<AstArgument>(applyFunctorToVars(bodyLevels, FunctorOp::MAX))));

    const RecursiveClauses& recursiveClauses = *translationUnit.getAnalysis<RecursiveClauses>();

    // first is the iteration number, which is counted inside each SCC
    if (recursiveClauses.recursive(&clause)) {
        negativeUpdateClause->getHead()->addArgument(std::make_unique<AstIterationNumber>());
    } else {
        negativeUpdateClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(0));
    }

    // second is the previous epoch's count, which we set to 0 signifying that we are updating the head tuple
    negativeUpdateClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(1));

    // third is the current epoch's count, which we set to -1, triggering a decrement in the count
    negativeUpdateClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(-1));

    // all tuples must have existed in prior iterations, i.e., tuples that are already deleted should not be deleted again
    negativeUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyPreviousCounts), FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    if (bodyLevels.size() > 0) {
        // add constraint to the rule saying that at least one body atom must have generated in the previous iteration
        negativeUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyLevels), FunctorOp::MAX)),
                    std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
    }

    /*
    // create copies of the rule so we have count values in the body atoms so we can exploit indexes
    for (size_t i = 0; i < negativeUpdateClause->getBodyLiterals().size(); i++) {
        auto lit = negativeUpdateClause->getBodyLiteral(i);

        // add three incremental columns to lit; first is iteration number, second is count in previous epoch, third is count in current epoch
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            auto negativeUpdateClauseSpecialised = negativeUpdateClause->clone();

            // the i-th atom in the rule must be deleted
            auto bodyAtom = dynamic_cast<AstAtom*>(negativeUpdateClauseSpecialised->getBodyLiteral(i));
            bodyAtom->setName(translateDiffRelationName(bodyAtom->getName()));
            bodyAtom->setArgument(bodyAtom->getArity() - 1, std::make_unique<AstNumberConstant>(0));

            negativeUpdateClauseSpecialised->getHead()->setName(translateDiffRelationName(negativeUpdateClauseSpecialised->getHead()->getName()));

            // all tuples must have existed in prior iterations, i.e., tuples that are already deleted should not be deleted again
            negativeUpdateClauseSpecialised->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                        std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyPreviousCounts), FunctorOp::MIN)),
                        std::make_unique<AstNumberConstant>(0)));

            if (bodyLevels.size() > 0) {
                // add constraint to the rule saying that at least one body atom must have generated in the previous iteration
                negativeUpdateClauseSpecialised->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                            std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyLevels), FunctorOp::MAX)),
                            std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
            }

            for (size_t j = i + 1; j < negativeUpdateClause->getBodyLiterals().size(); j++) {
                if (auto positiveAtom = dynamic_cast<AstAtom*>(negativeUpdateClauseSpecialised->getBodyLiteral(j))) {
                    / *
                    negativeUpdateClauseSpecialised->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                std::unique_ptr<AstArgument>(positiveAtom->getArgument(positiveAtom->getArity() - 1)->clone()),
                                std::make_unique<AstNumberConstant>(0)));
                                * /
                    positiveAtom->setName(translateDiffAppliedRelationName(positiveAtom->getName()));
                }
            }

            // reorder the clause so the deleted tuple is processed first (hopefully using the index better)
            std::vector<unsigned int> reordering;
            reordering.push_back(i);
            for (unsigned int j = 0; j < numAtoms; j++) {
                if (j != i) {
                    reordering.push_back(j);
                }
            }
            negativeUpdateClauseSpecialised->reorderAtoms(reordering);

            negativeUpdateClauses.push_back(negativeUpdateClauseSpecialised);
        }
    }
    */

    /*
    // add constraint to the rule saying that all body tuples must have existed prior,
    // i.e. tuples that are deleted in prior epochs shouldn't be deleted again
    negativeUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(bodyPreviousCounts, FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    // add constraint to the rule saying that at least one body atom must have been updated in the current epoch
    // we do this by doing min(count_cur_1 - count_prev_1, count_cur_2 - count_prev_2, ...) < 0
    negativeUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(bodyCountDiffs, FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));
                */

    // add constraint to the rule saying that at least one body atom must have negative count
    negativeUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                std::unique_ptr<AstArgument>(applyFunctorToVars(bodyCounts, FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    negativeUpdateClauses.push_back(negativeUpdateClause);
    return negativeUpdateClauses;
}

/**
 * This transforms a clause to process tuple additions
 */
std::vector<AstClause*> IncrementalTransformer::makePositiveUpdateClause(const AstClause& clause, const AstTranslationUnit& translationUnit) {
    // get the scc graph for generating iteration conditions
    const SCCGraph& sccGraph = *translationUnit.getAnalysis<SCCGraph>();

    // make a clone of the clause
    auto positiveUpdateClause = clause.clone();

    // make vector of positiveUpdateClauses
    std::vector<AstClause*> positiveUpdateClauses;

    // store the body counts to allow building arguments in the head atom
    std::vector<AstArgument*> bodyLevels;
    // std::vector<AstArgument*> bodyCountDiffs;
    std::vector<AstArgument*> bodyPreviousCounts;
    std::vector<AstArgument*> bodyCounts;

    // store number of atoms to be used for reordering
    size_t numAtoms = 0;

    for (size_t i = 0; i < positiveUpdateClause->getBodyLiterals().size(); i++) {

        auto lit = positiveUpdateClause->getBodyLiterals()[i];

        // add unnamed vars to each atom nested in arguments of lit
        lit->apply(addUnnamedVariables());

        // add three incremental columns to lit; first is iteration number, second is count in previous epoch, third is count in current epoch
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            numAtoms++;

            atom->addArgument(std::make_unique<AstVariable>("@iteration_" + std::to_string(i)));
            atom->addArgument(std::make_unique<AstVariable>("@prev_count_" + std::to_string(i)));
            atom->addArgument(std::make_unique<AstVariable>("@current_count_" + std::to_string(i)));
            
            // store the iterations and body counts to build arguments later
            // get relation of head atom
            auto headRelation = translationUnit.getProgram()->getRelation(clause.getHead()->getName());
            if (contains(sccGraph.getInternalRelations(sccGraph.getSCC(headRelation)), translationUnit.getProgram()->getRelation(atom->getName()))) {
                bodyLevels.push_back(new AstVariable("@iteration_" + std::to_string(i)));
            }

            // TODO: comment this properly, it's really messed up
            // basically we want to encode 1 if (diff > 0 && prev_count == 0), 0 otherwise
            // we do this by (BNOT(prev_count BOR 0) LAND (count - prev_count))
            /*
            bodyCountDiffs.push_back((new AstIntrinsicFunctor(FunctorOp::LAND, 
                            (std::make_unique<AstIntrinsicFunctor>(FunctorOp::LNOT, std::make_unique<AstIntrinsicFunctor>(FunctorOp::BOR, std::make_unique<AstVariable>("@prev_count_" + std::to_string(i)), std::make_unique<AstNumberConstant>(0)))),
                            (std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstVariable>("@current_count_" + std::to_string(i)), std::make_unique<AstVariable>("@prev_count_" + std::to_string(i)))))));
                            */
            bodyPreviousCounts.push_back(new AstVariable("@prev_count_" + std::to_string(i)));
            bodyCounts.push_back(new AstVariable("@current_count_" + std::to_string(i)));
        }
    }

    // add three incremental columns to head lit
    // first is the iteration number, which we get by adding 1 to the max iteration number over the body atoms
    // positiveUpdateClause->getHead()->addArgument(std::make_unique<AstIntrinsicFunctor>(FunctorOp::ADD, std::make_unique<AstNumberConstant>(1), std::unique_ptr<AstArgument>(applyFunctorToVars(bodyLevels, FunctorOp::MAX))));

    const RecursiveClauses& recursiveClauses = *translationUnit.getAnalysis<RecursiveClauses>();

    // first is the iteration number, which is counted inside each SCC
    if (recursiveClauses.recursive(&clause)) {
        positiveUpdateClause->getHead()->addArgument(std::make_unique<AstIterationNumber>());
    } else {
        positiveUpdateClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(0));
    }
    
    // second is the previous epoch's count, which we set to 0, signifying that we are updating the head tuple
    positiveUpdateClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(0));

    // third is the current epoch's count, which we set to 1, triggering an increment in the count
    positiveUpdateClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(1));

    // all tuples must have existed in prior iterations, i.e., tuples that are already deleted should not be deleted again
    positiveUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyCounts), FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    if (bodyLevels.size() > 0) {
        // add constraint to the rule saying that at least one body atom must have generated in the previous iteration
        positiveUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyLevels), FunctorOp::MAX)),
                    std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
    }

    /*
    // create copies of the rule so we have count values in the body atoms so we can exploit indexes
    for (size_t i = 0; i < positiveUpdateClause->getBodyLiterals().size(); i++) {
        auto lit = positiveUpdateClause->getBodyLiteral(i);

        // add three incremental columns to lit; first is iteration number, second is count in previous epoch, third is count in current epoch
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            auto positiveUpdateClauseSpecialised = positiveUpdateClause->clone();

            // the i-th atom in the rule must be inserted
            auto bodyAtom = dynamic_cast<AstAtom*>(positiveUpdateClauseSpecialised->getBodyLiteral(i));
            bodyAtom->setName(translateDiffRelationName(bodyAtom->getName()));
            bodyAtom->setArgument(bodyAtom->getArity() - 2, std::make_unique<AstNumberConstant>(0));

            positiveUpdateClauseSpecialised->getHead()->setName(translateDiffRelationName(positiveUpdateClauseSpecialised->getHead()->getName()));

            // all tuples must have existed in prior iterations, i.e., tuples that are already deleted should not be deleted again
            positiveUpdateClauseSpecialised->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                        std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyCounts), FunctorOp::MIN)),
                        std::make_unique<AstNumberConstant>(0)));

            if (bodyLevels.size() > 0) {
                // add constraint to the rule saying that at least one body atom must have generated in the previous iteration
                positiveUpdateClauseSpecialised->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                            std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyLevels), FunctorOp::MAX)),
                            std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
            }

            for (size_t j = i + 1; j < positiveUpdateClause->getBodyLiterals().size(); j++) {
                if (auto positiveAtom = dynamic_cast<AstAtom*>(positiveUpdateClauseSpecialised->getBodyLiteral(j))) {
                    / *
                    positiveUpdateClauseSpecialised->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                                std::unique_ptr<AstArgument>(positiveAtom->getArgument(positiveAtom->getArity() - 2)->clone()),
                                std::make_unique<AstNumberConstant>(0)));
                                * /

                    positiveAtom->setName(translateDiffAppliedRelationName(positiveAtom->getName()));
                }
            }

            // reorder the clause so the inserted tuple is processed first (hopefully using the index better)
            std::vector<unsigned int> reordering;
            reordering.push_back(i);
            for (unsigned int j = 0; j < numAtoms; j++) {
                if (j != i) {
                    reordering.push_back(j);
                }
            }
            positiveUpdateClauseSpecialised->reorderAtoms(reordering);

            positiveUpdateClauses.push_back(positiveUpdateClauseSpecialised);
        }
    }
    */
    /*

    // add constraint to the rule saying that at least one body atom must have been updated in the current epoch
    // we do this by doing max(count_cur_1 - count_prev_1, count_cur_2 - count_prev_2, ...) > 0
    positiveUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(bodyCountDiffs, FunctorOp::MAX)),
                std::make_unique<AstNumberConstant>(0)));

    // add constraint to the rule saying that all body atoms must have positive count
    positiveUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(bodyCounts, FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    if (bodyLevels.size() > 0) {
        // add constraint to the rule saying that at least one body atom must have generated in the previous iteration
        positiveUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(applyFunctorToVars(bodyLevels, FunctorOp::MAX)),
                    std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
    }
    */

    // all tuples must have existed in prior iterations, i.e., tuples that are already deleted should not be deleted again
    positiveUpdateClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::LE,
                std::unique_ptr<AstArgument>(applyFunctorToVars(vectorClone(bodyPreviousCounts), FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    positiveUpdateClauses.push_back(positiveUpdateClause);
    return positiveUpdateClauses;
}

/**
 * This transforms a clause to process generation of new tuples in an epoch after the body tuples are already stable
 */
std::unique_ptr<AstClause> IncrementalTransformer::makePositiveGenerationClause(const AstClause& clause, const AstTranslationUnit& translationUnit) {
    // get the scc graph for generating iteration conditions
    const SCCGraph& sccGraph = *translationUnit.getAnalysis<SCCGraph>();

    // make a clone of the clause
    auto positiveGenerationClause = clause.clone();

    // store the body counts to allow building arguments in the head atom
    std::vector<AstArgument*> bodyLevels;
    std::vector<AstArgument*> bodyCountDiffs;
    std::vector<AstArgument*> bodyCounts;

    // store number of atoms to be used for reordering
    size_t numAtoms = 0;

    for (size_t i = 0; i < positiveGenerationClause->getBodyLiterals().size(); i++) {

        auto lit = positiveGenerationClause->getBodyLiterals()[i];

        // add unnamed vars to each atom nested in arguments of lit
        lit->apply(addUnnamedVariables());

        // add three incremental columns to lit; first is iteration number, second is count in previous epoch, third is count in current epoch
        if (auto atom = dynamic_cast<AstAtom*>(lit)) {
            numAtoms++;
            atom->addArgument(std::make_unique<AstVariable>("@iteration_" + std::to_string(i)));
            atom->addArgument(std::make_unique<AstVariable>("@prev_count_" + std::to_string(i)));
            atom->addArgument(std::make_unique<AstVariable>("@current_count_" + std::to_string(i)));
            
            // store the iterations and body counts to build arguments later
            // get relation of head atom
            auto headRelation = translationUnit.getProgram()->getRelation(clause.getHead()->getName());
            if (contains(sccGraph.getInternalRelations(sccGraph.getSCC(headRelation)), translationUnit.getProgram()->getRelation(atom->getName()))) {
                bodyLevels.push_back(new AstVariable("@iteration_" + std::to_string(i)));
            }
            bodyCountDiffs.push_back(new AstIntrinsicFunctor(FunctorOp::SUB, std::make_unique<AstVariable>("@current_count_" + std::to_string(i)), std::make_unique<AstVariable>("@prev_count_" + std::to_string(i))));
            bodyCounts.push_back(new AstVariable("@current_count_" + std::to_string(i)));
        }
    }

    // add three incremental columns to head lit
    // first is the iteration number, which we get by adding 1 to the max iteration number over the body atoms
    // positiveGenerationClause->getHead()->addArgument(std::make_unique<AstIntrinsicFunctor>(FunctorOp::ADD, std::make_unique<AstNumberConstant>(1), std::unique_ptr<AstArgument>(applyFunctorToVars(bodyLevels, FunctorOp::MAX))));
    
    const RecursiveClauses& recursiveClauses = *translationUnit.getAnalysis<RecursiveClauses>();

    // first is the iteration number, which is counted inside each SCC
    if (recursiveClauses.recursive(&clause)) {
        positiveGenerationClause->getHead()->addArgument(std::make_unique<AstIterationNumber>());
    } else {
        positiveGenerationClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(0));
    }

    // second is the previous epoch's count, which we set to 1, signifying that this tuple should have always existed, but was not generated in a prior epoch for some other reason
    positiveGenerationClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(1));

    // third is the current epoch's count, which we set to 1, inserting a new tuple with positive count
    positiveGenerationClause->getHead()->addArgument(std::make_unique<AstNumberConstant>(1));

    // add constraint to the rule saying that at least one body atom must have been updated in the current epoch
    // we do this by doing min(count_cur_1 - count_prev_1, count_cur_2 - count_prev_2, ...) > 0
    /*
    for (auto bodyCountDiff : bodyCountDiffs) {
        positiveGenerationClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ, std::unique_ptr<AstArgument>(bodyCountDiff), std::make_unique<AstNumberConstant>(0)));
    }
    */

    // make an atom saying that the current tuple must already exist with 0 count in earlier iteration
    /*
    auto prevTuple = positiveGenerationClause->getHead()->clone();
    prevTuple->setArgument(prevTuple->getArity() - 1, std::make_unique<AstNumberConstant>(0));
    prevTuple->setArgument(prevTuple->getArity() - 3, std::make_unique<AstUnnamedVariable>());

    positiveGenerationClause->addToBody(std::unique_ptr<AstAtom>(prevTuple));

    // reorder the clause so the inserted tuple is processed first (hopefully using the index better)
    std::vector<unsigned int> reordering;
    reordering.push_back(numAtoms);
    for (unsigned int i = 0; i < numAtoms; i++) {
        reordering.push_back(i);
    }
    positiveGenerationClause->reorderAtoms(reordering);
    */

    // add constraint to the rule saying that all body atoms must have positive count
    positiveGenerationClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::GT,
                std::unique_ptr<AstArgument>(applyFunctorToVars(bodyCounts, FunctorOp::MIN)),
                std::make_unique<AstNumberConstant>(0)));

    if (bodyLevels.size() > 0) {
        // add constraint to the rule saying that at least one body atom must have generated in the previous iteration
        positiveGenerationClause->addToBody(std::make_unique<AstBinaryConstraint>(BinaryConstraintOp::EQ,
                    std::unique_ptr<AstArgument>(applyFunctorToVars(bodyLevels, FunctorOp::MAX)),
                    std::make_unique<AstIntrinsicFunctor>(FunctorOp::SUB, std::make_unique<AstIterationNumber>(), std::make_unique<AstNumberConstant>(1))));
    }

    return std::unique_ptr<AstClause>(positiveGenerationClause);
}

bool IncrementalTransformer::transform(AstTranslationUnit& translationUnit) {
    auto program = translationUnit.getProgram();

    std::cout << "before:\n";
    program->print(std::cout);
    std::cout << std::endl;

    std::vector<AstRelation*> auxiliaryRelations;
    auto originalRelations = program->getRelations();

    // go through each relation and its rules to add annotations
    for (auto relation : program->getRelations()) {
        relation->addAttribute(
                std::make_unique<AstAttribute>(std::string("@iteration"), AstTypeIdentifier("number")));
        relation->addAttribute(
                std::make_unique<AstAttribute>(std::string("@prev_count"), AstTypeIdentifier("number")));
        relation->addAttribute(
                std::make_unique<AstAttribute>(std::string("@current_count"), AstTypeIdentifier("number")));

        /*
        auto diffRelation = new AstRelation();
        diffRelation->setName(translateDiffRelationName(relation->getName()));
        for (auto& attr : relation->getAttributes()) {
            diffRelation->addAttribute(std::unique_ptr<AstAttribute>(attr->clone()));
        }
        auxiliaryRelations.push_back(diffRelation);

        auto diffAppliedRelation = new AstRelation();
        diffAppliedRelation->setName(translateDiffAppliedRelationName(relation->getName()));
        for (auto& attr : relation->getAttributes()) {
            diffAppliedRelation->addAttribute(std::unique_ptr<AstAttribute>(attr->clone()));
        }
        auxiliaryRelations.push_back(diffAppliedRelation);
        */
    }

    /*
    for (auto relation : auxiliaryRelations) {
        program->addRelation(std::unique_ptr<AstRelation>(relation));
    }
    */

    for (auto relation : originalRelations) {
        // store a list of original clauses in the relation, to be deleted
        std::vector<AstClause*> originalClauses;

        for (auto clause : relation->getClauses()) {
            // mapper to add two provenance columns to atoms
            // add unnamed vars to each atom nested in arguments of head
            clause->getHead()->apply(addUnnamedVariables());

            // if fact, level number is 0
            if (clause->isFact()) {
                clause->getHead()->addArgument(std::make_unique<AstNumberConstant>(0));
                clause->getHead()->addArgument(std::make_unique<AstNumberConstant>(0));
                clause->getHead()->addArgument(std::make_unique<AstNumberConstant>(1));
            } else {
                for (auto clause : makeNegativeUpdateClause(*clause, translationUnit)) {
                    program->appendClause(std::unique_ptr<AstClause>(clause));
                }
                for (auto clause : makePositiveUpdateClause(*clause, translationUnit)) {
                    program->appendClause(std::unique_ptr<AstClause>(clause));
                }
                relation->addClause(std::unique_ptr<AstClause>(makePositiveGenerationClause(*clause, translationUnit)));

                originalClauses.push_back(clause);
            }
        }

        // delete original clauses once instrumented clauses are added
        for (auto clause : originalClauses) {
            relation->removeClause(clause);
        }
    }


    std::cout << "after:\n";
    program->print(std::cout);
    std::cout << std::endl;
    return true;
}

}  // end of namespace souffle
