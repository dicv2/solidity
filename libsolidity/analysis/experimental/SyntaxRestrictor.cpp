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

#include <libsolidity/analysis/experimental/SyntaxRestrictor.h>

#include <liblangutil/Exceptions.h>

using namespace solidity::frontend;
using namespace solidity::frontend::experimental;
using namespace solidity::langutil;

bool SyntaxRestrictor::check(ASTNode const& _astRoot)
{
	_astRoot.accept(*this);
	return !Error::containsErrors(m_errorReporter.errors());
}

bool SyntaxRestrictor::visitNode(ASTNode const& _node)
{
	if (!_node.experimentalSolidityOnly())
		m_errorReporter.syntaxError(0000_error, _node.location(), "Unsupported AST node.");
	return false;
}

bool SyntaxRestrictor::visit(ContractDefinition const& _contractDefinition)
{
	if (_contractDefinition.contractKind() != ContractKind::Contract)
		m_errorReporter.syntaxError(0000_error, _contractDefinition.location(), "Only contracts are supported.");
	if (!_contractDefinition.baseContracts().empty())
		m_errorReporter.syntaxError(0000_error, _contractDefinition.location(), "Inheritance unsupported.");
	return true;
}

bool SyntaxRestrictor::visit(FunctionDefinition const& _functionDefinition)
{
	if (!_functionDefinition.isImplemented())
		m_errorReporter.syntaxError(0000_error, _functionDefinition.location(), "Functions must be implemented.");
	if (!_functionDefinition.modifiers().empty())
		m_errorReporter.syntaxError(0000_error, _functionDefinition.location(), "Function may not have modifiers.");
	if (_functionDefinition.overrides())
		m_errorReporter.syntaxError(0000_error, _functionDefinition.location(), "Function may not have override specifiers.");
	if (_functionDefinition.isFree())
	{
		if (_functionDefinition.stateMutability() != StateMutability::NonPayable)
			m_errorReporter.syntaxError(0000_error, _functionDefinition.location(), "Free functions may not have a mutability.");
	}
	else
	{
		if (_functionDefinition.isFallback())
		{
			if (_functionDefinition.visibility() != Visibility::External)
				m_errorReporter.syntaxError(0000_error, _functionDefinition.location(), "Fallback function must be external.");
		}
		else
			m_errorReporter.syntaxError(0000_error, _functionDefinition.location(), "Only fallback functions are supported in contracts.");
	}

	return true;
}

bool SyntaxRestrictor::visit(VariableDeclarationStatement const& _variableDeclarationStatement)
{
	if (_variableDeclarationStatement.initialValue())
		m_errorReporter.syntaxError(0000_error, _variableDeclarationStatement.initialValue()->location(), "Variable declarations with initial value not supported.");
	if (_variableDeclarationStatement.declarations().size() == 1)
	{
		if (!_variableDeclarationStatement.declarations().front())
			m_errorReporter.syntaxError(0000_error, _variableDeclarationStatement.initialValue()->location(), "Variable declaration has to declare a single variable.");
	}
	else
		m_errorReporter.syntaxError(0000_error, _variableDeclarationStatement.initialValue()->location(), "Variable declarations can only declare a single variable.");
	return true;
}

bool SyntaxRestrictor::visit(VariableDeclaration const& _variableDeclaration)
{
	if (_variableDeclaration.value())
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.value()->location(), "Variable declarations with initial value not supported.");
	if (_variableDeclaration.isStateVariable())
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "State variables are not supported.");
	if (!_variableDeclaration.isLocalVariable())
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "Only local variables are supported.");
	if (_variableDeclaration.mutability() != VariableDeclaration::Mutability::Mutable)
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "Only mutable variables are supported.");
	if (_variableDeclaration.isIndexed())
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "Indexed variables are not supported.");
	if (!_variableDeclaration.noVisibilitySpecified())
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "Variables with visibility not supported.");
	if (_variableDeclaration.overrides())
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "Variables with override specifier not supported.");
	if (_variableDeclaration.referenceLocation() != VariableDeclaration::Location::Unspecified)
		m_errorReporter.syntaxError(0000_error, _variableDeclaration.location(), "Variables with reference location not supported.");
	return true;
}