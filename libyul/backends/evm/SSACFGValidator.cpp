/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#include <libyul/backends/evm/SSACFGValidator.h>

#include <libyul/Utilities.h>

#include <libsolutil/Visitor.h>

#include <range/v3/algorithm/all_of.hpp>
#include <range/v3/algorithm/set_algorithm.hpp>
#include <range/v3/algorithm/transform.hpp>
#include <range/v3/to_container.hpp>
#include <range/v3/view/drop_last.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/zip.hpp>

#include <fmt/ranges.h>

using namespace solidity::yul;

namespace
{

template<typename Set1, typename Set2>
bool intersectionEmpty(Set1 const& _set1, Set2 const& _set2)
{
	auto it1 = _set1.begin();
	auto it2 = _set2.begin();
	while (it1 != _set1.end() && it2 != _set2.end())
	{
		if (*it1 < *it2)
			++it1;
		else if (*it2 < *it1)
			++it2;
		else
			return false;
	}
	return true;
}

}

std::string SSACFGValidator::debug(VariableMapping const& _mapping) const
{
	std::stringstream ss;
	for (auto const& [var, values]: _mapping)
	{
		ss << "Variable " << var->name.str() << ": {";
		auto const valToString = [&](SSACFG::ValueId const& _val)
		{
			auto const& info = m_context.cfg.valueInfo(_val);
			auto const rhs = std::visit(util::GenericVisitor{
				[](SSACFG::LiteralValue const& _literal) -> std::string { return _literal.value.str(); },
				[](SSACFG::PhiValue const&) -> std::string { return "phi";},
				[](SSACFG::VariableValue const&) -> std::string { return "var"; },
				[](SSACFG::UnreachableValue const&) -> std::string { return "unreachable"; },
			}, info);
			return fmt::format("v{} = {}", _val.value, rhs);
		};
		ss << fmt::format("{}", fmt::join(values | ranges::views::transform(valToString), ", "));
		ss << "}\n";
	}
	return ss.str();
}

void SSACFGValidator::validate(ControlFlow const& _controlFlow, Block const& _ast, AsmAnalysisInfo const& _analysisInfo, Dialect const& _dialect)
{
	std::cout << _controlFlow.toDot() << std::endl;
    Context context{_analysisInfo, _dialect, _controlFlow, *_controlFlow.mainGraph};
    SSACFGValidator validator(context);
    validator.m_currentBlock = context.cfg.entry;
	validator.m_currentOperation = 0;
    validator.consumeBlock(_ast);
}

bool SSACFGValidator::consumeBlock(Block const& _block)
{
	ScopedSaveAndRestore saveScope(m_scope, m_context.analysisInfo.scopes.at(&_block).get());
	for (auto const& stmt: _block.statements)
		if (std::holds_alternative<FunctionDefinition>(stmt) && !consumeStatement(stmt))
			return false;
	for (auto const& stmt: _block.statements)
		if (!std::holds_alternative<FunctionDefinition>(stmt) && !consumeStatement(stmt))
			return false;
	if (std::holds_alternative<SSACFG::BasicBlock::FunctionReturn>(currentBlock().exit))
		expectFunctionReturn();
	return true;
}

void SSACFGValidator::consolidateVariables(VariableMapping const& _variables, std::vector<VariableMapping> const& _toBeConsolidated)
{
	for (auto const& [var, valueId]: _variables)
	{
		for (auto const& parent: _toBeConsolidated)
		{
			m_currentVariableValues[var] += parent.at(var);
			/*if (!parent.count(var) || intersectionEmpty(valueId, parent.at(var)))
				// set to no value, should be asserted against if that variable is consumed (eg as function arg)
				m_currentVariableValues[var] = {};*/
		}
	}
}
void SSACFGValidator::advanceToBlock(SSACFG::BlockId _target)
{
	m_currentBlock = _target;
	m_currentOperation = 0;
}

bool SSACFGValidator::consumeStatement(Statement const& _statement)
{
	return std::visit(
		util::GenericVisitor{
			[&](ExpressionStatement const& _stmt)
			{
				if (auto results = consumeExpression(_stmt.expression))
				{
					yulAssert(results->empty());
					return true;
				}
				return false;
			},
			[&](Assignment const& _assignment)
			{
				if (auto results = consumeExpression(*_assignment.value))
				{
					yulAssert(results->size() == _assignment.variableNames.size());
					for (auto&& [variable, value]: ranges::zip_view(_assignment.variableNames, *results))
					{
						yulAssert(
							m_currentVariableValues.count(resolveVariable(variable.name)),
							"Could not find variable " + variable.name.str());
						m_currentVariableValues.at(resolveVariable(variable.name)) = std::set{value};
					}
					return true;
				}
				return false;
			},
			[&](VariableDeclaration const& _variableDeclaration)
			{
				if (!_variableDeclaration.value)
				{
					for (auto&& variable: _variableDeclaration.variables)
					{
						yulAssert(m_context.cfg.zeroLiteral());
						m_currentVariableValues[resolveVariable(variable.name)] = {*m_context.cfg.zeroLiteral()};
					}
					return true;
				}
				if (auto results = consumeExpression(*_variableDeclaration.value))
				{
					yulAssert(results->size() == _variableDeclaration.variables.size());
					for (auto&& [variable, value]: ranges::zip_view(_variableDeclaration.variables, *results))
					{
						yulAssert(!m_currentVariableValues.count(resolveVariable(variable.name)));
						m_currentVariableValues[resolveVariable(variable.name)] = std::set{value};
					}
					return true;
				}
				return false;
			},
			[&](FunctionDefinition const& _function)
			{
				auto* function = resolveFunction(_function.name);
				auto const* functionGraph = m_context.controlFlow.functionGraph(function);
				yulAssert(functionGraph);
				Context nestedContext{m_context.analysisInfo, m_context.dialect, m_context.controlFlow, *functionGraph};
				SSACFGValidator nestedValidator(nestedContext);
				nestedValidator.advanceToBlock(functionGraph->entry);
				yulAssert(_function.parameters.size() == functionGraph->arguments.size());
				yulAssert(_function.returnVariables.size() == functionGraph->returns.size());
				nestedValidator.m_currentVariableValues
					= functionGraph->arguments
					  | ranges::views::transform([](auto const& tup)
												 { return std::make_pair(&std::get<0>(tup).get(), std::set{std::get<1>(tup)}); })
					  | ranges::to<std::map>;
				for (auto const& returnVar: functionGraph->returns)
				{
					yulAssert(functionGraph->zeroLiteral());
					nestedValidator.m_currentVariableValues[&returnVar.get()] = std::set{*functionGraph->zeroLiteral()};
				}
				nestedValidator.consumeBlock(_function.body);
				for (auto const& returnVar: functionGraph->returns)
				{
					auto const& returnValues = nestedValidator.m_currentVariableValues.at(&returnVar.get());
					yulAssert(returnValues.size() == 1);
					yulAssert(nestedValidator.m_currentVariableValues.at(&returnVar.get()).begin()->hasValue());
				}
				return true;
			},
			[&](If const& _if)
			{
				if (auto cond = consumeUnaryExpression(*_if.condition))
				{
					auto const& exit = expectConditionalJump();
					// yulAssert(cond.value().size() == 1 && *cond->begin() == exit.condition);
					yulAssert(!intersectionEmpty(cond.value(), std::set{exit.condition}));
					// todo here the variable must match exit condition, restrict set to that
					// todo here i should reduce the variable corresponding to the if cond to exit or is it already happening in consumeUnaryExpression??
					// yulAssert(exit.condition == *cond);
					auto zeroBranchVariableValues = applyPhis(m_currentBlock, exit.zero);

					advanceToBlock(exit.nonZero);
					if (consumeBlock(_if.body))
					{
						auto const& jumpBack = expectUnconditionalJump();
						yulAssert(exit.zero == jumpBack.target);
						auto nonZeroBranchVariableValues = applyPhis(m_currentBlock, jumpBack.target);
						consolidateVariables(zeroBranchVariableValues, std::vector{nonZeroBranchVariableValues});
					}

					advanceToBlock(exit.zero);
					m_currentVariableValues = zeroBranchVariableValues;
					return true;
				}
				return false;
			},
			[&](Switch const& _switch)
			{
				if (auto cond = consumeUnaryExpression(*_switch.expression))
				{
					yulAssert(!_switch.cases.empty());
					auto const validateGhostEq = [this, cond](SSACFG::Operation const& _operation)
					{
						yulAssert(m_context.dialect.equalityFunctionHandle());
						yulAssert(std::holds_alternative<SSACFG::BuiltinCall>(_operation.kind));
						yulAssert(
							&std::get<SSACFG::BuiltinCall>(_operation.kind).builtin.get()
							== &m_context.dialect.builtin(*m_context.dialect.equalityFunctionHandle()));
						yulAssert(_operation.inputs.size() == 2);
						yulAssert(cond->size() == 1);
						yulAssert(_operation.inputs.back() == *cond->begin());
						yulAssert(_operation.outputs.size() == 1);
					};
					yulAssert(currentBlock().operations.size() >= 2, "switch at least has the expression and ghost eq");
					std::optional<SSACFG::BlockId> afterSwitch{std::nullopt};
					SSACFG::BasicBlock::ConditionalJump const* exit{nullptr};
					std::vector<VariableMapping> parentsOfJumpBackTarget;
					for (auto const& switchCase: _switch.cases)
					{
						if (!switchCase.value)
							break; // default case, always comes last, requires special handling
						yulAssert(m_currentOperation == currentBlock().operations.size() - 1);
						auto const& ghostEq = currentBlock().operations.back();
						validateGhostEq(ghostEq);
						m_currentOperation = currentBlock().operations.size();
						exit = &expectConditionalJump();
						yulAssert(exit->condition == ghostEq.outputs[0]);
						auto zeroBranchVariableValues = applyPhis(m_currentBlock, exit->zero);
						advanceToBlock(exit->nonZero);
						if (consumeBlock(switchCase.body))
						{
							auto const& jumpBack = expectUnconditionalJump();
							yulAssert(!afterSwitch || *afterSwitch == jumpBack.target);
							afterSwitch = jumpBack.target;
							parentsOfJumpBackTarget.push_back(applyPhis(m_currentBlock, jumpBack.target));
						}
						m_currentVariableValues = zeroBranchVariableValues;
						advanceToBlock(exit->zero);
					}

					// no default case, we're done
					if (_switch.cases.back().value)
					{
						consolidateVariables(m_currentVariableValues, parentsOfJumpBackTarget);
						return true;
					}

					if (consumeBlock(_switch.cases.back().body))
					{
						auto const& jumpBack = expectUnconditionalJump();
						yulAssert(!afterSwitch || *afterSwitch == jumpBack.target);
						afterSwitch = jumpBack.target;
						auto zeroBranchVariableValues = applyPhis(m_currentBlock, *afterSwitch);
						parentsOfJumpBackTarget.push_back(applyPhis(m_currentBlock, jumpBack.target));
						m_currentVariableValues = zeroBranchVariableValues;
					}
					consolidateVariables(m_currentVariableValues, parentsOfJumpBackTarget);

					if (!afterSwitch)
						return false;
					advanceToBlock(*afterSwitch);
					return true;
				}
				return false;
			},
			[&](ForLoop const& _loop)
			{
				std::optional<bool> constantCondition;
				if (auto const* literalCondition = std::get_if<Literal>(_loop.condition.get()))
					constantCondition = literalCondition->value.value() == 0;

				if (constantCondition)
					return consumeConstantForLoop(_loop, *constantCondition);
				else
					return consumeDynamicForLoop(_loop);
			},
			[&](Break const&)
			{
				yulAssert(m_currentLoopInfo);
				auto const& breakJump = expectUnconditionalJump();
				yulAssert(!m_currentLoopInfo->exitBlock.hasValue() || breakJump.target == m_currentLoopInfo->exitBlock); // can have no value for constant infinite loops
				m_currentVariableValues = applyPhis(m_currentBlock, breakJump.target);
				consolidateVariables(m_currentVariableValues, std::vector{m_currentLoopInfo->loopExitVariableValues});
				return false;
			},
			[&](Continue const&)
			{
				// TODO: check if this can be simplified
				yulAssert(m_currentLoopInfo);
				auto const& continueJump = expectUnconditionalJump();
				if (m_currentLoopInfo->postBlock)
					yulAssert(continueJump.target == *m_currentLoopInfo->postBlock);
				else
					m_currentLoopInfo->postBlock = continueJump.target;
				m_currentVariableValues = applyPhis(m_currentBlock, continueJump.target);
				if (m_currentLoopInfo->loopPostVariableValues)
					consolidateVariables(
						m_currentVariableValues, std::vector{*m_currentLoopInfo->loopPostVariableValues});
				else
					m_currentLoopInfo->loopPostVariableValues = m_currentVariableValues;
				return false;
			},
			[&](Leave const&)
			{
				auto const& functionReturn = expectFunctionReturn();
				yulAssert(functionReturn.returnValues.size() == m_context.cfg.returns.size());
				for (auto&& [variable, value]: ranges::zip_view(m_context.cfg.returns, functionReturn.returnValues))
				{
					yulAssert(m_currentVariableValues.count(&variable.get()));
					yulAssert(value.hasValue());
					m_currentVariableValues[&variable.get()] = std::set{value};
				}
				return false;
			},
			[&](Block const& _nestedBlock) { return consumeBlock(_nestedBlock); }},
		_statement);
}
bool SSACFGValidator::consumeConstantForLoop(ForLoop const& _loop, bool _conditionIsZero)
{
	yulAssert(std::holds_alternative<Literal>(*_loop.condition));

	ScopedSaveAndRestore scopeRestore(m_scope, m_context.analysisInfo.scopes.at(&_loop.pre).get());
	consumeBlock(_loop.pre);

	auto const& entryJump = expectUnconditionalJump();

	auto entryVariableValues = applyPhis(m_currentBlock, entryJump.target);
	advanceToBlock(entryJump.target);
	m_currentVariableValues = entryVariableValues;

	if (auto cond = consumeUnaryExpression(*_loop.condition))
	{
		yulAssert(cond.value().size() == 1);
		auto const* ssaLiteral = std::get_if<SSACFG::LiteralValue>(&m_context.cfg.valueInfo(*cond->begin()));
		yulAssert(ssaLiteral && ssaLiteral->value == std::get<Literal>(*_loop.condition).value.value());
	}
	else
		return false;

	if (_conditionIsZero)
		return true;
	else
	{
		// yulAssert(!cond.value().hasValue() || exit.condition == cond); // todo what about hasValue, ie stuff that .... i don't even know anymore lol
		std::unique_ptr<LoopInfo> loopInfo = std::make_unique<LoopInfo>(LoopInfo{
			entryVariableValues | ranges::views::keys | ranges::to<std::set<Scope::Variable const*>>,
			m_currentVariableValues,
			SSACFG::BlockId{},
			std::nullopt,
			std::nullopt});
		std::swap(m_currentLoopInfo, loopInfo);
		if (consumeBlock(_loop.body))
		{
			std::swap(m_currentLoopInfo, loopInfo);

			auto const& jumpToPost = expectUnconditionalJump();
			m_currentVariableValues = applyPhis(m_currentBlock, jumpToPost.target);
			if (loopInfo->postBlock)
				yulAssert(loopInfo->postBlock == jumpToPost.target);
			if (loopInfo->loopPostVariableValues)
				for (auto var: entryVariableValues | ranges::views::keys)
					yulAssert(loopInfo->loopPostVariableValues->at(var) == m_currentVariableValues.at(var));
			advanceToBlock(jumpToPost.target);
			if (consumeBlock(_loop.post))
			{
				auto const& jumpBackToCondition = expectUnconditionalJump();
				m_currentVariableValues = applyPhis(m_currentBlock, jumpBackToCondition.target);
				advanceToBlock(jumpBackToCondition.target);
				yulAssert(m_currentBlock == entryJump.target);
				consolidateVariables(m_currentVariableValues, {entryVariableValues});
			}
		}
		else
			std::swap(m_currentLoopInfo, loopInfo);

		return false;
	}
}

bool SSACFGValidator::consumeDynamicForLoop(ForLoop const& _loop)
{
	// TODO: try to clean up and simplify especially the "continue" validation
	ScopedSaveAndRestore scopeRestore(m_scope, m_context.analysisInfo.scopes.at(&_loop.pre).get());
	consumeBlock(_loop.pre);

	auto const& entryJump = expectUnconditionalJump();

	auto entryVariableValues = applyPhis(m_currentBlock, entryJump.target);
	advanceToBlock(entryJump.target);
	m_currentVariableValues = entryVariableValues;

	if (auto cond = consumeUnaryExpression(*_loop.condition))
	{
		auto const& exit = expectConditionalJump();
		yulAssert(cond.value().count(exit.condition));
		if (auto const* identifier = std::get_if<Identifier>(&*_loop.condition))
			m_currentVariableValues.at(resolveVariable(identifier->name)) = std::set{exit.condition};
		// yulAssert(!cond.value().hasValue() || exit.condition == cond); // todo what about hasValue, ie stuff that .... i don't even know anymore lol
		auto loopExitVariableValues = applyPhis(m_currentBlock, exit.zero);

		std::unique_ptr<LoopInfo> loopInfo = std::make_unique<LoopInfo>(LoopInfo{
			entryVariableValues | ranges::views::keys | ranges::to<std::set<Scope::Variable const*>>,
			loopExitVariableValues,
			exit.zero,
			std::nullopt,
			std::nullopt});
		std::swap(m_currentLoopInfo, loopInfo);
		advanceToBlock(exit.nonZero);
		if (consumeBlock(_loop.body))
		{
			std::swap(m_currentLoopInfo, loopInfo);

			auto const& jumpToPost = expectUnconditionalJump();
  			m_currentVariableValues = applyPhis(m_currentBlock, jumpToPost.target);
			if (loopInfo->postBlock)
				yulAssert(loopInfo->postBlock == jumpToPost.target);
			// in case of continue, loop post variable values contain the possible values for each variable as
			// the loop is 'continue'd. in this case, we have more possible current values per variable, also
			// possibly non-intersecting sets of them
			if (loopInfo->loopPostVariableValues)
				for (auto var: entryVariableValues | ranges::views::keys)
					m_currentVariableValues.at(var) += loopInfo->loopPostVariableValues->at(var);
			advanceToBlock(jumpToPost.target);
			if (consumeBlock(_loop.post))
			{
				auto const& jumpBackToCondition = expectUnconditionalJump();
				m_currentVariableValues = applyPhis(m_currentBlock, jumpBackToCondition.target);
				advanceToBlock(jumpBackToCondition.target);
				yulAssert(m_currentBlock == entryJump.target);
				consolidateVariables(m_currentVariableValues, {entryVariableValues});
			}
		}
		else
			std::swap(m_currentLoopInfo, loopInfo);
		advanceToBlock(exit.zero);
		m_currentVariableValues = loopExitVariableValues;
		return true;
	}
	return false;
}

std::optional<std::set<SSACFG::ValueId>> SSACFGValidator::consumeUnaryExpression(Expression const& _expression)
{
	if (auto result = consumeExpression(_expression))
	{
		yulAssert(result->size() == 1, fmt::format("Expected exactly one result value but got {}", result->size()));
		return result->front();
	}
	return std::nullopt;
}

std::optional<std::vector<std::set<SSACFG::ValueId>>> SSACFGValidator::consumeExpression(Expression const& _expression)
{
	return std::visit(util::GenericVisitor{
		[&](FunctionCall const& _call) -> std::optional<std::vector<std::set<SSACFG::ValueId>>>
		{
			std::optional<BuiltinHandle> builtinHandle = m_context.dialect.findBuiltin(_call.functionName.name.str());
			BuiltinFunction const* builtin = builtinHandle ? &m_context.dialect.builtin(*builtinHandle) : nullptr;
			size_t idx = _call.arguments.size();
			std::vector<std::tuple<size_t, Expression const*, std::vector<std::set<SSACFG::ValueId>>>> gatheredArguments;
			for(auto& _arg: _call.arguments | ranges::views::reverse)
			{
				--idx;
				if (builtinHandle && builtin->literalArgument(idx).has_value())
					continue;
				if (auto arg = consumeExpression(_arg))
					gatheredArguments.emplace_back(idx, &_arg, *arg);
				else
					return std::nullopt;
			}

			std::vector<SSACFG::ValueId> arguments;
			for (auto const& [idx, _arg, arg]: gatheredArguments)
			{
				if (arg.at(0).size() > 1)
				{
					yulAssert(std::holds_alternative<Identifier>(*_arg));
					auto const& currentOpArg = currentBlock().operations.at(m_currentOperation).inputs.at(_call.arguments.size() - idx - 1);
					yulAssert(arg.at(0).count(currentOpArg));
					auto const* var = resolveVariable(std::get<Identifier>(*_arg).name);
					m_currentVariableValues[var] = std::set{currentOpArg};
					arguments.push_back(currentOpArg);
				}
				else
				{
					yulAssert(arg.size() == 1 && arg.front().size() == 1);
					arguments.push_back(*arg.front().begin());
				}
			}

			auto const& currentOp = currentBlock().operations.at(m_currentOperation);
			if (auto f = std::get_if<SSACFG::Call>(&currentOp.kind); f && !f->canContinue)
				return std::nullopt;
			if (auto f = std::get_if<SSACFG::BuiltinCall>(&currentOp.kind); f && !f->builtin.get().controlFlowSideEffects.canContinue)
				return std::nullopt;
			++m_currentOperation;

			yulAssert(ranges::all_of(arguments, [](SSACFG::ValueId const& _arg) { return _arg.hasValue(); }));
			yulAssert(currentOp.inputs == arguments);
			if (validateCall(currentOp.kind, _call.functionName, currentOp.outputs.size()))
				return currentOp.outputs | ranges::views::transform([](auto const& _valueId) { return std::set{_valueId}; }) | ranges::to<std::vector>;
			else
				return std::nullopt;
		},
		[&](Identifier const& _identifier) -> std::optional<std::vector<std::set<SSACFG::ValueId>>>
		{
			return {{lookupIdentifier(_identifier)}};
		},
		[&](Literal const& _literal) -> std::optional<std::vector<std::set<SSACFG::ValueId>>>
		{
			return {{{lookupLiteral(_literal)}}};
		}
	}, _expression);
}

SSACFG::BasicBlock::ConditionalJump const& SSACFGValidator::expectConditionalJump() const
{
	yulAssert(m_currentOperation == currentBlock().operations.size());
	auto const* exit = std::get_if<SSACFG::BasicBlock::ConditionalJump>(&currentBlock().exit);
	yulAssert(exit);
	return *exit;
}

SSACFG::BasicBlock::Jump const& SSACFGValidator::expectUnconditionalJump() const
{
	yulAssert(m_currentOperation == currentBlock().operations.size());
	auto const* exit = std::get_if<SSACFG::BasicBlock::Jump>(&currentBlock().exit);
	yulAssert(exit);
	return *exit;
}

SSACFG::BasicBlock::FunctionReturn const& SSACFGValidator::expectFunctionReturn()
{
	yulAssert(m_currentOperation == currentBlock().operations.size(), fmt::format("{}: Expecting function return in block {} after last operation. Current operation = {}, num operations = {}.", m_context.cfg.function->name.str(), m_currentBlock.value, m_currentOperation, currentBlock().operations.size()));
	auto const* functionReturn = std::get_if<SSACFG::BasicBlock::FunctionReturn>(&currentBlock().exit);
	yulAssert(functionReturn);
	for (auto&& [variable, value]: ranges::zip_view(m_context.cfg.returns, functionReturn->returnValues))
	{
		yulAssert(value.hasValue());
		yulAssert(m_currentVariableValues.at(&variable.get()).count(value));
		m_currentVariableValues[&variable.get()] = std::set{value};
	}
	return *functionReturn;
}

bool SSACFGValidator::validateCall(std::variant<SSACFG::BuiltinCall, SSACFG::Call> const& _kind, Identifier const& _functionName, size_t _numOutputs) const
{
	return std::visit(util::GenericVisitor{
		[&](SSACFG::BuiltinCall const& _call)
		{
			std::optional<BuiltinHandle> builtinHandle = m_context.dialect.findBuiltin(_functionName.name.str());
			BuiltinFunction const* builtin = builtinHandle ? &m_context.dialect.builtin(*builtinHandle) : nullptr;
			yulAssert(&_call.builtin.get() == builtin);
			yulAssert(builtin->numReturns == _numOutputs);
			return builtin->controlFlowSideEffects.canContinue;
		},
		[&](SSACFG::Call const& _call)
		{
			auto const* function = resolveFunction(_functionName.name);
			yulAssert(function);
			yulAssert(m_context.controlFlow.functionGraph(function)->returns.size() == _numOutputs);
			yulAssert(&_call.function.get() == function);
			return _call.canContinue;
		}
	}, _kind);
}


Scope::Variable const* SSACFGValidator::resolveVariable(YulName _name) const
{
	yulAssert(m_scope, "");
	Scope::Variable const* var = nullptr;
	if (m_scope->lookup(_name, util::GenericVisitor{
		[&](Scope::Variable& _var) { var = &_var; },
		[](Scope::Function&)
		{
			yulAssert(false, "Function not removed during desugaring.");
		}
	}))
	{
		yulAssert(var, "");
		return var;
	};
	yulAssert(false, "External identifier access unimplemented.");
}

Scope::Function const* SSACFGValidator::resolveFunction(YulName _name) const
{
	Scope::Function const* function = nullptr;
	yulAssert(m_scope->lookup(_name, util::GenericVisitor{
		[](Scope::Variable&) { yulAssert(false, "Expected function name."); },
		[&](Scope::Function& _function) { function = &_function; }
	}), "Function name not found.");
	yulAssert(function, "");
	return function;
}

std::set<SSACFG::ValueId> const& SSACFGValidator::lookupIdentifier(Identifier const& _identifier) const
{
	auto const* var = resolveVariable(_identifier.name);
	return m_currentVariableValues.at(var);
}

SSACFG::ValueId SSACFGValidator::lookupLiteral(Literal const& _literal) const
{
	auto const valueId = m_context.cfg.lookupLiteral(_literal.value);
	yulAssert(
		valueId.hasValue(),
		fmt::format(
			"Block {}: Literal with value \"{}\" has no value associated to it in the CFG.",
			m_currentBlock.value,
			formatLiteral(_literal)
		)
	);
	return valueId;
}

SSACFGValidator::VariableMapping SSACFGValidator::applyPhis(SSACFG::BlockId _source, SSACFG::BlockId _target)
{
	auto const& targetBlock = m_context.cfg.block(_target);
	auto entryOffset = util::findOffset(targetBlock.entries, _source);
	yulAssert(entryOffset);
	std::map<SSACFG::ValueId, std::set<SSACFG::ValueId>> phiMap;
	for (auto phi: targetBlock.phis)
	{
		yulAssert(phi.hasValue());
		auto const* phiInfo = std::get_if<SSACFG::PhiValue>(&m_context.cfg.valueInfo(phi));
		yulAssert(phiInfo);
		auto const argumentValueId = phiInfo->arguments.at(*entryOffset);
		yulAssert(argumentValueId.hasValue());
		phiMap[argumentValueId].insert(phi);
	}
	VariableMapping result = m_currentVariableValues;
	for (auto& [var, values]: m_currentVariableValues)
		for (auto const& val: values)
			result[var] += util::valueOrDefault(phiMap, val, std::set{val}, util::allow_copy);
	return result;
}

void SSACFGValidator::validatePhis(SSACFG::BlockId const& _blockId) const
{
	auto const& block = m_context.cfg.block(_blockId);
	for (auto const& phi: block.phis)
	{
		yulAssert(phi.hasValue());
		auto const* phiInfo = std::get_if<SSACFG::PhiValue>(&m_context.cfg.valueInfo(phi));
		yulAssert(phiInfo);
		// todo check for each arg that it refers to the same ast variable
		// phiInfo->arguments
	}
}