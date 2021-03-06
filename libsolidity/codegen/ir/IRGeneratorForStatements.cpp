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
/**
 * Component that translates Solidity code into Yul at statement level and below.
 */

#include <libsolidity/codegen/ir/IRGeneratorForStatements.h>

#include <libsolidity/codegen/ABIFunctions.h>
#include <libsolidity/codegen/ir/IRGenerationContext.h>
#include <libsolidity/codegen/ir/IRLValue.h>
#include <libsolidity/codegen/ir/IRVariable.h>
#include <libsolidity/codegen/YulUtilFunctions.h>
#include <libsolidity/codegen/ABIFunctions.h>
#include <libsolidity/codegen/CompilerUtils.h>
#include <libsolidity/ast/TypeProvider.h>

#include <libevmasm/GasMeter.h>

#include <libyul/AsmPrinter.h>
#include <libyul/AsmData.h>
#include <libyul/Dialect.h>
#include <libyul/optimiser/ASTCopier.h>

#include <libsolutil/Whiskers.h>
#include <libsolutil/StringUtils.h>
#include <libsolutil/Keccak256.h>
#include <libsolutil/Visitor.h>

#include <boost/range/adaptor/transformed.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::frontend;

namespace
{

struct CopyTranslate: public yul::ASTCopier
{
	using ExternalRefsMap = std::map<yul::Identifier const*, InlineAssemblyAnnotation::ExternalIdentifierInfo>;

	CopyTranslate(yul::Dialect const& _dialect, IRGenerationContext& _context, ExternalRefsMap const& _references):
		m_dialect(_dialect), m_context(_context), m_references(_references) {}

	using ASTCopier::operator();

	yul::Expression operator()(yul::Identifier const& _identifier) override
	{
		if (m_references.count(&_identifier))
		{
			auto const& reference = m_references.at(&_identifier);
			auto const varDecl = dynamic_cast<VariableDeclaration const*>(reference.declaration);
			solUnimplementedAssert(varDecl, "");

			if (reference.isOffset || reference.isSlot)
			{
				solAssert(reference.isOffset != reference.isSlot, "");

				pair<u256, unsigned> slot_offset = m_context.storageLocationOfVariable(*varDecl);

				string const value = reference.isSlot ?
					slot_offset.first.str() :
					to_string(slot_offset.second);

				return yul::Literal{
					_identifier.location,
					yul::LiteralKind::Number,
					yul::YulString{value},
					{}
				};
			}
		}
		return ASTCopier::operator()(_identifier);
	}

	yul::YulString translateIdentifier(yul::YulString _name) override
	{
		// Strictly, the dialect used by inline assembly (m_dialect) could be different
		// from the Yul dialect we are compiling to. So we are assuming here that the builtin
		// functions are identical. This should not be a problem for now since everything
		// is EVM anyway.
		if (m_dialect.builtin(_name))
			return _name;
		else
			return yul::YulString{"usr$" + _name.str()};
	}

	yul::Identifier translate(yul::Identifier const& _identifier) override
	{
		if (!m_references.count(&_identifier))
			return ASTCopier::translate(_identifier);

		auto const& reference = m_references.at(&_identifier);
		auto const varDecl = dynamic_cast<VariableDeclaration const*>(reference.declaration);
		solUnimplementedAssert(varDecl, "");

		solAssert(
			reference.isOffset == false && reference.isSlot == false,
			"Should not be called for offset/slot"
		);

		return yul::Identifier{
			_identifier.location,
			yul::YulString{m_context.localVariable(*varDecl).name()}
		};
	}

private:
	yul::Dialect const& m_dialect;
	IRGenerationContext& m_context;
	ExternalRefsMap const& m_references;
};

}

string IRGeneratorForStatements::code() const
{
	solAssert(!m_currentLValue, "LValue not reset!");
	return m_code.str();
}

void IRGeneratorForStatements::initializeStateVar(VariableDeclaration const& _varDecl)
{
	solAssert(m_context.isStateVariable(_varDecl), "Must be a state variable.");
	solAssert(!_varDecl.isConstant(), "");
	if (_varDecl.value())
	{
		_varDecl.value()->accept(*this);
		writeToLValue(IRLValue{
			*_varDecl.annotation().type,
			IRLValue::Storage{
				util::toCompactHexWithPrefix(m_context.storageLocationOfVariable(_varDecl).first),
				m_context.storageLocationOfVariable(_varDecl).second
			}
		}, *_varDecl.value());
	}
}

void IRGeneratorForStatements::endVisit(VariableDeclarationStatement const& _varDeclStatement)
{
	if (Expression const* expression = _varDeclStatement.initialValue())
	{
		if (_varDeclStatement.declarations().size() > 1)
		{
			auto const* tupleType = dynamic_cast<TupleType const*>(expression->annotation().type);
			solAssert(tupleType, "Expected expression of tuple type.");
			solAssert(_varDeclStatement.declarations().size() == tupleType->components().size(), "Invalid number of tuple components.");
			for (size_t i = 0; i < _varDeclStatement.declarations().size(); ++i)
				if (auto const& decl = _varDeclStatement.declarations()[i])
				{
					solAssert(tupleType->components()[i], "");
					define(m_context.addLocalVariable(*decl), IRVariable(*expression).tupleComponent(i));
				}
		}
		else
		{
			VariableDeclaration const& varDecl = *_varDeclStatement.declarations().front();
			define(m_context.addLocalVariable(varDecl), *expression);
		}
	}
	else
		for (auto const& decl: _varDeclStatement.declarations())
			if (decl)
				declare(m_context.addLocalVariable(*decl));
}

bool IRGeneratorForStatements::visit(Conditional const& _conditional)
{
	_conditional.condition().accept(*this);

	string condition = expressionAsType(_conditional.condition(), *TypeProvider::boolean());
	declare(_conditional);

	m_code << "switch " << condition << "\n" "case 0 {\n";
	_conditional.falseExpression().accept(*this);
	assign(_conditional, _conditional.falseExpression());
	m_code << "}\n" "default {\n";
	_conditional.trueExpression().accept(*this);
	assign(_conditional, _conditional.trueExpression());
	m_code << "}\n";

	return false;
}

bool IRGeneratorForStatements::visit(Assignment const& _assignment)
{
	_assignment.rightHandSide().accept(*this);
	Type const* intermediateType = type(_assignment.rightHandSide()).closestTemporaryType(
		&type(_assignment.leftHandSide())
	);
	IRVariable value = convert(_assignment.rightHandSide(), *intermediateType);

	_assignment.leftHandSide().accept(*this);
	solAssert(!!m_currentLValue, "LValue not retrieved.");

	if (_assignment.assignmentOperator() != Token::Assign)
	{
		solAssert(type(_assignment.leftHandSide()) == *intermediateType, "");
		solAssert(intermediateType->isValueType(), "Compound operators only available for value types.");

		IRVariable leftIntermediate = readFromLValue(*m_currentLValue);
		m_code << value.name() << " := " << binaryOperation(
			TokenTraits::AssignmentToBinaryOp(_assignment.assignmentOperator()),
			*intermediateType,
			leftIntermediate.name(),
			value.name()
		);
	}

	writeToLValue(*m_currentLValue, value);
	m_currentLValue.reset();
	if (*_assignment.annotation().type != *TypeProvider::emptyTuple())
		define(_assignment, value);

	return false;
}

bool IRGeneratorForStatements::visit(TupleExpression const& _tuple)
{
	if (_tuple.isInlineArray())
		solUnimplementedAssert(false, "");
	else
	{
		bool lValueRequested = _tuple.annotation().lValueRequested;
		if (lValueRequested)
			solAssert(!m_currentLValue, "");
		if (_tuple.components().size() == 1)
		{
			solAssert(_tuple.components().front(), "");
			_tuple.components().front()->accept(*this);
			if (lValueRequested)
				solAssert(!!m_currentLValue, "");
			else
				define(_tuple, *_tuple.components().front());
		}
		else
		{
			vector<optional<IRLValue>> lvalues;
			for (size_t i = 0; i < _tuple.components().size(); ++i)
				if (auto const& component = _tuple.components()[i])
				{
					component->accept(*this);
					if (lValueRequested)
					{
						solAssert(!!m_currentLValue, "");
						lvalues.emplace_back(std::move(m_currentLValue));
						m_currentLValue.reset();
					}
					else
						define(IRVariable(_tuple).tupleComponent(i), *component);
				}
				else if (lValueRequested)
					lvalues.emplace_back();

			if (_tuple.annotation().lValueRequested)
				m_currentLValue.emplace(IRLValue{
					*_tuple.annotation().type,
					IRLValue::Tuple{std::move(lvalues)}
				});
		}
	}
	return false;
}

bool IRGeneratorForStatements::visit(IfStatement const& _ifStatement)
{
	_ifStatement.condition().accept(*this);
	string condition = expressionAsType(_ifStatement.condition(), *TypeProvider::boolean());

	if (_ifStatement.falseStatement())
	{
		m_code << "switch " << condition << "\n" "case 0 {\n";
		_ifStatement.falseStatement()->accept(*this);
		m_code << "}\n" "default {\n";
	}
	else
		m_code << "if " << condition << " {\n";
	_ifStatement.trueStatement().accept(*this);
	m_code << "}\n";

	return false;
}

bool IRGeneratorForStatements::visit(ForStatement const& _forStatement)
{
	generateLoop(
		_forStatement.body(),
		_forStatement.condition(),
		_forStatement.initializationExpression(),
		_forStatement.loopExpression()
	);

	return false;
}

bool IRGeneratorForStatements::visit(WhileStatement const& _whileStatement)
{
	generateLoop(
		_whileStatement.body(),
		&_whileStatement.condition(),
		nullptr,
		nullptr,
		_whileStatement.isDoWhile()
	);

	return false;
}

bool IRGeneratorForStatements::visit(Continue const&)
{
	m_code << "continue\n";
	return false;
}

bool IRGeneratorForStatements::visit(Break const&)
{
	m_code << "break\n";
	return false;
}

void IRGeneratorForStatements::endVisit(Return const& _return)
{
	if (Expression const* value = _return.expression())
	{
		solAssert(_return.annotation().functionReturnParameters, "Invalid return parameters pointer.");
		vector<ASTPointer<VariableDeclaration>> const& returnParameters =
			_return.annotation().functionReturnParameters->parameters();
		if (returnParameters.size() > 1)
			for (size_t i = 0; i < returnParameters.size(); ++i)
				assign(m_context.localVariable(*returnParameters[i]), IRVariable(*value).tupleComponent(i));
		else if (returnParameters.size() == 1)
			assign(m_context.localVariable(*returnParameters.front()), *value);
	}
	m_code << "leave\n";
}

void IRGeneratorForStatements::endVisit(UnaryOperation const& _unaryOperation)
{
	Type const& resultType = type(_unaryOperation);
	Token const op = _unaryOperation.getOperator();

	if (op == Token::Delete)
	{
		solAssert(!!m_currentLValue, "LValue not retrieved.");
		std::visit(
			util::GenericVisitor{
				[&](IRLValue::Storage const& _storage) {
					m_code <<
						m_utils.storageSetToZeroFunction(m_currentLValue->type) <<
						"(" <<
						_storage.slot <<
						", " <<
						_storage.offsetString() <<
						")\n";
					m_currentLValue.reset();
				},
				[&](auto const&) {
					IRVariable zeroValue(m_context.newYulVariable(), m_currentLValue->type);
					define(zeroValue) << m_utils.zeroValueFunction(m_currentLValue->type) << "()\n";
					writeToLValue(*m_currentLValue, zeroValue);
					m_currentLValue.reset();
				}
			},
			m_currentLValue->kind
		);
	}
	else if (resultType.category() == Type::Category::RationalNumber)
		define(_unaryOperation) << formatNumber(resultType.literalValue(nullptr)) << "\n";
	else if (resultType.category() == Type::Category::Integer)
	{
		solAssert(resultType == type(_unaryOperation.subExpression()), "Result type doesn't match!");

		if (op == Token::Inc || op == Token::Dec)
		{
			solAssert(!!m_currentLValue, "LValue not retrieved.");
			IRVariable modifiedValue(m_context.newYulVariable(), resultType);
			IRVariable originalValue = readFromLValue(*m_currentLValue);

			define(modifiedValue) <<
				(op == Token::Inc ?
					m_utils.incrementCheckedFunction(resultType) :
					m_utils.decrementCheckedFunction(resultType)
				) <<
				"(" <<
				originalValue.name() <<
				")\n";
			writeToLValue(*m_currentLValue, modifiedValue);
			m_currentLValue.reset();

			define(_unaryOperation, _unaryOperation.isPrefixOperation() ? modifiedValue : originalValue);
		}
		else if (op == Token::BitNot)
			appendSimpleUnaryOperation(_unaryOperation, _unaryOperation.subExpression());
		else if (op == Token::Add)
			// According to SyntaxChecker...
			solAssert(false, "Use of unary + is disallowed.");
		else if (op == Token::Sub)
		{
			IntegerType const& intType = *dynamic_cast<IntegerType const*>(&resultType);
			define(_unaryOperation) <<
				m_utils.negateNumberCheckedFunction(intType) <<
				"(" <<
				IRVariable(_unaryOperation.subExpression()).name() <<
				")\n";
		}
		else
			solUnimplementedAssert(false, "Unary operator not yet implemented");
	}
	else if (resultType.category() == Type::Category::Bool)
	{
		solAssert(
			_unaryOperation.getOperator() != Token::BitNot,
			"Bitwise Negation can't be done on bool!"
		);

		appendSimpleUnaryOperation(_unaryOperation, _unaryOperation.subExpression());
	}
	else
		solUnimplementedAssert(false, "Unary operator not yet implemented");
}

bool IRGeneratorForStatements::visit(BinaryOperation const& _binOp)
{
	solAssert(!!_binOp.annotation().commonType, "");
	TypePointer commonType = _binOp.annotation().commonType;
	langutil::Token op = _binOp.getOperator();

	if (op == Token::And || op == Token::Or)
	{
		// This can short-circuit!
		appendAndOrOperatorCode(_binOp);
		return false;
	}

	_binOp.leftExpression().accept(*this);
	_binOp.rightExpression().accept(*this);

	if (commonType->category() == Type::Category::RationalNumber)
		define(_binOp) << toCompactHexWithPrefix(commonType->literalValue(nullptr)) << "\n";
	else if (TokenTraits::isCompareOp(op))
	{
		if (auto type = dynamic_cast<FunctionType const*>(commonType))
		{
			solAssert(op == Token::Equal || op == Token::NotEqual, "Invalid function pointer comparison!");
			solAssert(type->kind() != FunctionType::Kind::External, "External function comparison not allowed!");
		}

		solAssert(commonType->isValueType(), "");
		bool isSigned = false;
		if (auto type = dynamic_cast<IntegerType const*>(commonType))
			isSigned = type->isSigned();

		string args =
			expressionAsType(_binOp.leftExpression(), *commonType) +
			", " +
			expressionAsType(_binOp.rightExpression(), *commonType);

		string expr;
		if (op == Token::Equal)
			expr = "eq(" + move(args) + ")";
		else if (op == Token::NotEqual)
			expr = "iszero(eq(" + move(args) + "))";
		else if (op == Token::GreaterThanOrEqual)
			expr = "iszero(" + string(isSigned ? "slt(" : "lt(") + move(args) + "))";
		else if (op == Token::LessThanOrEqual)
			expr = "iszero(" + string(isSigned ? "sgt(" : "gt(") + move(args) + "))";
		else if (op == Token::GreaterThan)
			expr = (isSigned ? "sgt(" : "gt(") + move(args) + ")";
		else if (op == Token::LessThan)
			expr = (isSigned ? "slt(" : "lt(") + move(args) + ")";
		else
			solAssert(false, "Unknown comparison operator.");
		define(_binOp) << expr << "\n";
	}
	else
	{
		string left = expressionAsType(_binOp.leftExpression(), *commonType);
		string right = expressionAsType(_binOp.rightExpression(), *commonType);
		define(_binOp) << binaryOperation(_binOp.getOperator(), *commonType, left, right) << "\n";
	}
	return false;
}

void IRGeneratorForStatements::endVisit(FunctionCall const& _functionCall)
{
	solUnimplementedAssert(
		_functionCall.annotation().kind == FunctionCallKind::FunctionCall ||
		_functionCall.annotation().kind == FunctionCallKind::TypeConversion,
		"This type of function call is not yet implemented"
	);

	Type const& funcType = type(_functionCall.expression());

	if (_functionCall.annotation().kind == FunctionCallKind::TypeConversion)
	{
		solAssert(funcType.category() == Type::Category::TypeType, "Expected category to be TypeType");
		solAssert(_functionCall.arguments().size() == 1, "Expected one argument for type conversion");
		define(_functionCall, *_functionCall.arguments().front());
		return;
	}

	FunctionTypePointer functionType = dynamic_cast<FunctionType const*>(&funcType);

	TypePointers parameterTypes = functionType->parameterTypes();
	vector<ASTPointer<Expression const>> const& callArguments = _functionCall.arguments();
	vector<ASTPointer<ASTString>> const& callArgumentNames = _functionCall.names();
	if (!functionType->takesArbitraryParameters())
		solAssert(callArguments.size() == parameterTypes.size(), "");

	vector<ASTPointer<Expression const>> arguments;
	if (callArgumentNames.empty())
		// normal arguments
		arguments = callArguments;
	else
		// named arguments
		for (auto const& parameterName: functionType->parameterNames())
		{
			auto const it = std::find_if(callArgumentNames.cbegin(), callArgumentNames.cend(), [&](ASTPointer<ASTString> const& _argName) {
				return *_argName == parameterName;
			});

			solAssert(it != callArgumentNames.cend(), "");
			arguments.push_back(callArguments[std::distance(callArgumentNames.begin(), it)]);
		}

	solUnimplementedAssert(!functionType->bound(), "");
	switch (functionType->kind())
	{
	case FunctionType::Kind::Internal:
	{
		vector<string> args;
		for (unsigned i = 0; i < arguments.size(); ++i)
			if (functionType->takesArbitraryParameters())
				args.emplace_back(IRVariable(*arguments[i]).commaSeparatedList());
			else
				args.emplace_back(convert(*arguments[i], *parameterTypes[i]).commaSeparatedList());

		if (auto identifier = dynamic_cast<Identifier const*>(&_functionCall.expression()))
		{
			solAssert(!functionType->bound(), "");
			if (auto functionDef = dynamic_cast<FunctionDefinition const*>(identifier->annotation().referencedDeclaration))
			{
				define(_functionCall) <<
					m_context.virtualFunctionName(*functionDef) <<
					"(" <<
					joinHumanReadable(args) <<
					")\n";
				return;
			}
		}

		define(_functionCall) <<
			m_context.internalDispatch(functionType->parameterTypes().size(), functionType->returnParameterTypes().size()) <<
			"(" <<
			IRVariable(_functionCall.expression()).part("functionIdentifier").name() <<
			joinHumanReadablePrefixed(args) <<
			")\n";
		break;
	}
	case FunctionType::Kind::External:
	case FunctionType::Kind::DelegateCall:
	case FunctionType::Kind::BareCall:
	case FunctionType::Kind::BareDelegateCall:
	case FunctionType::Kind::BareStaticCall:
		appendExternalFunctionCall(_functionCall, arguments);
		break;
	case FunctionType::Kind::BareCallCode:
		solAssert(false, "Callcode has been removed.");
	case FunctionType::Kind::Event:
	{
		auto const& event = dynamic_cast<EventDefinition const&>(functionType->declaration());
		TypePointers paramTypes = functionType->parameterTypes();
		ABIFunctions abi(m_context.evmVersion(), m_context.revertStrings(), m_context.functionCollector());

		vector<IRVariable> indexedArgs;
		string nonIndexedArgs;
		TypePointers nonIndexedArgTypes;
		TypePointers nonIndexedParamTypes;
		if (!event.isAnonymous())
			define(indexedArgs.emplace_back(m_context.newYulVariable(), *TypeProvider::uint256())) <<
				formatNumber(u256(h256::Arith(keccak256(functionType->externalSignature())))) << "\n";
		for (size_t i = 0; i < event.parameters().size(); ++i)
		{
			Expression const& arg = *arguments[i];
			if (event.parameters()[i]->isIndexed())
			{
				string value;
				if (auto const& referenceType = dynamic_cast<ReferenceType const*>(paramTypes[i]))
					define(indexedArgs.emplace_back(m_context.newYulVariable(), *TypeProvider::uint256())) <<
						m_utils.packedHashFunction({arg.annotation().type}, {referenceType}) <<
						"(" <<
						IRVariable(arg).commaSeparatedList() <<
						")";
				else
					indexedArgs.emplace_back(convert(arg, *paramTypes[i]));
			}
			else
			{
				string vars = IRVariable(arg).commaSeparatedList();
				if (!vars.empty())
					// In reverse because abi_encode expects it like that.
					nonIndexedArgs = ", " + move(vars) + nonIndexedArgs;
				nonIndexedArgTypes.push_back(arg.annotation().type);
				nonIndexedParamTypes.push_back(paramTypes[i]);
			}
		}
		solAssert(indexedArgs.size() <= 4, "Too many indexed arguments.");
		Whiskers templ(R"({
			let <pos> := mload(<freeMemoryPointer>)
			let <end> := <encode>(<pos> <nonIndexedArgs>)
			<log>(<pos>, sub(<end>, <pos>) <indexedArgs>)
		})");
		templ("pos", m_context.newYulVariable());
		templ("end", m_context.newYulVariable());
		templ("freeMemoryPointer", to_string(CompilerUtils::freeMemoryPointer));
		templ("encode", abi.tupleEncoder(nonIndexedArgTypes, nonIndexedParamTypes));
		templ("nonIndexedArgs", nonIndexedArgs);
		templ("log", "log" + to_string(indexedArgs.size()));
		templ("indexedArgs", joinHumanReadablePrefixed(indexedArgs | boost::adaptors::transformed([&](auto const& _arg) {
			return _arg.commaSeparatedList();
		})));
		m_code << templ.render();
		break;
	}
	case FunctionType::Kind::Assert:
	case FunctionType::Kind::Require:
	{
		solAssert(arguments.size() > 0, "Expected at least one parameter for require/assert");
		solAssert(arguments.size() <= 2, "Expected no more than two parameters for require/assert");

		Type const* messageArgumentType = arguments.size() > 1 ? arguments[1]->annotation().type : nullptr;
		string requireOrAssertFunction = m_utils.requireOrAssertFunction(
			functionType->kind() == FunctionType::Kind::Assert,
			messageArgumentType
		);

		m_code << move(requireOrAssertFunction) << "(" << IRVariable(*arguments[0]).name();
		if (messageArgumentType && messageArgumentType->sizeOnStack() > 0)
			m_code << ", " << IRVariable(*arguments[1]).commaSeparatedList();
		m_code << ")\n";

		break;
	}
	// Array creation using new
	case FunctionType::Kind::ObjectCreation:
	{
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(*_functionCall.annotation().type);
		solAssert(arguments.size() == 1, "");

		IRVariable value = convert(*arguments[0], *TypeProvider::uint256());
		define(_functionCall) <<
			m_utils.allocateMemoryArrayFunction(arrayType) <<
			"(" <<
			value.commaSeparatedList() <<
			")\n";
		break;
	}
	case FunctionType::Kind::KECCAK256:
	{
		solAssert(arguments.size() == 1, "");

		ArrayType const* arrayType = TypeProvider::bytesMemory();
		auto array = convert(*arguments[0], *arrayType);

		define(_functionCall) <<
			"keccak256(" <<
			m_utils.arrayDataAreaFunction(*arrayType) <<
			"(" <<
			array.commaSeparatedList() <<
			"), " <<
			m_utils.arrayLengthFunction(*arrayType) <<
			"(" <<
			array.commaSeparatedList() <<
			"))\n";
		break;
	}
	case FunctionType::Kind::ArrayPop:
	{
		auto const& memberAccessExpression = dynamic_cast<MemberAccess const&>(_functionCall.expression()).expression();
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(*memberAccessExpression.annotation().type);
		define(_functionCall) <<
			m_utils.storageArrayPopFunction(arrayType) <<
			"(" <<
			IRVariable(_functionCall.expression()).commaSeparatedList() <<
			")\n";
		break;
	}
	case FunctionType::Kind::ArrayPush:
	{
		auto const& memberAccessExpression = dynamic_cast<MemberAccess const&>(_functionCall.expression()).expression();
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(*memberAccessExpression.annotation().type);
		if (arguments.empty())
		{
			auto slotName = m_context.newYulVariable();
			auto offsetName = m_context.newYulVariable();
			m_code << "let " << slotName << ", " << offsetName << " := " <<
				m_utils.storageArrayPushZeroFunction(arrayType) <<
				"(" << IRVariable(_functionCall.expression()).commaSeparatedList() << ")\n";
			setLValue(_functionCall, IRLValue{
				*arrayType.baseType(),
				IRLValue::Storage{
					slotName,
					offsetName,
				}
			});
		}
		else
		{
			IRVariable argument = convert(*arguments.front(), *arrayType.baseType());
			m_code <<
				m_utils.storageArrayPushFunction(arrayType) <<
				"(" <<
				IRVariable(_functionCall.expression()).commaSeparatedList() <<
				", " <<
				argument.commaSeparatedList() <<
				")\n";
		}
		break;
	}
	default:
		solUnimplemented("FunctionKind " + toString(static_cast<int>(functionType->kind())) + " not yet implemented");
	}
}

void IRGeneratorForStatements::endVisit(MemberAccess const& _memberAccess)
{
	ASTString const& member = _memberAccess.memberName();
	if (auto funType = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type))
		if (funType->bound())
		{
			solUnimplementedAssert(false, "");
		}

	switch (_memberAccess.expression().annotation().type->category())
	{
	case Type::Category::Contract:
	{
		ContractType const& type = dynamic_cast<ContractType const&>(*_memberAccess.expression().annotation().type);
		if (type.isSuper())
		{
			solUnimplementedAssert(false, "");
		}
		// ordinary contract type
		else if (Declaration const* declaration = _memberAccess.annotation().referencedDeclaration)
		{
			u256 identifier;
			if (auto const* variable = dynamic_cast<VariableDeclaration const*>(declaration))
				identifier = FunctionType(*variable).externalIdentifier();
			else if (auto const* function = dynamic_cast<FunctionDefinition const*>(declaration))
				identifier = FunctionType(*function).externalIdentifier();
			else
				solAssert(false, "Contract member is neither variable nor function.");

			define(IRVariable(_memberAccess).part("address"), _memberAccess.expression());
			define(IRVariable(_memberAccess).part("functionIdentifier")) << formatNumber(identifier) << "\n";
		}
		else
			solAssert(false, "Invalid member access in contract");
		break;
	}
	case Type::Category::Integer:
	{
		solAssert(false, "Invalid member access to integer");
		break;
	}
	case Type::Category::Address:
	{
		if (member == "balance")
			define(_memberAccess) <<
				"balance(" <<
				expressionAsType(_memberAccess.expression(), *TypeProvider::address()) <<
				")\n";
		else if (set<string>{"send", "transfer"}.count(member))
		{
			solAssert(dynamic_cast<AddressType const&>(*_memberAccess.expression().annotation().type).stateMutability() == StateMutability::Payable, "");
			define(IRVariable{_memberAccess}.part("address"), _memberAccess.expression());
		}
		else if (set<string>{"call", "callcode", "delegatecall", "staticcall"}.count(member))
			define(IRVariable{_memberAccess}.part("address"), _memberAccess.expression());
		else
			solAssert(false, "Invalid member access to address");
		break;
	}
	case Type::Category::Function:
		if (member == "selector")
		{
			solUnimplementedAssert(false, "");
		}
		else if (member == "address")
		{
			solUnimplementedAssert(false, "");
		}
		else
			solAssert(
				!!_memberAccess.expression().annotation().type->memberType(member),
				"Invalid member access to function."
			);
		break;
	case Type::Category::Magic:
		// we can ignore the kind of magic and only look at the name of the member
		if (member == "coinbase")
			define(_memberAccess) << "coinbase()\n";
		else if (member == "timestamp")
			define(_memberAccess) << "timestamp()\n";
		else if (member == "difficulty")
			define(_memberAccess) << "difficulty()\n";
		else if (member == "number")
			define(_memberAccess) << "number()\n";
		else if (member == "gaslimit")
			define(_memberAccess) << "gaslimit()\n";
		else if (member == "sender")
			define(_memberAccess) << "caller()\n";
		else if (member == "value")
			define(_memberAccess) << "callvalue()\n";
		else if (member == "origin")
			define(_memberAccess) << "origin()\n";
		else if (member == "gasprice")
			define(_memberAccess) << "gasprice()\n";
		else if (member == "data")
		{
			IRVariable var(_memberAccess);
			declare(var);
			define(var.part("offset")) << "0\n";
			define(var.part("length")) << "calldatasize()\n";
		}
		else if (member == "sig")
			define(_memberAccess) <<
				"and(calldataload(0), " <<
				formatNumber(u256(0xffffffff) << (256 - 32)) <<
				")\n";
		else if (member == "gas")
			solAssert(false, "Gas has been removed.");
		else if (member == "blockhash")
			solAssert(false, "Blockhash has been removed.");
		else if (member == "creationCode" || member == "runtimeCode")
		{
			solUnimplementedAssert(false, "");
		}
		else if (member == "name")
		{
			solUnimplementedAssert(false, "");
		}
		else if (set<string>{"encode", "encodePacked", "encodeWithSelector", "encodeWithSignature", "decode"}.count(member))
		{
			// no-op
		}
		else
			solAssert(false, "Unknown magic member.");
		break;
	case Type::Category::Struct:
	{
		solUnimplementedAssert(false, "");
	}
	case Type::Category::Enum:
	{
		EnumType const& type = dynamic_cast<EnumType const&>(*_memberAccess.expression().annotation().type);
		define(_memberAccess) << to_string(type.memberValue(_memberAccess.memberName())) << "\n";
		break;
	}
	case Type::Category::Array:
	{
		auto const& type = dynamic_cast<ArrayType const&>(*_memberAccess.expression().annotation().type);

		if (member == "length")
		{
			if (!type.isDynamicallySized())
				define(_memberAccess) << type.length() << "\n";
			else
				switch (type.location())
				{
					case DataLocation::CallData:
						define(_memberAccess, IRVariable(_memberAccess.expression()).part("length"));
						break;
					case DataLocation::Storage:
					{
						define(_memberAccess) <<
							m_utils.arrayLengthFunction(type) <<
							"(" <<
							IRVariable(_memberAccess.expression()).commaSeparatedList() <<
							")\n";
						break;
					}
					case DataLocation::Memory:
						define(_memberAccess) <<
							"mload(" <<
							IRVariable(_memberAccess.expression()).commaSeparatedList() <<
							")\n";
						break;
				}
		}
		else if (member == "pop" || member == "push")
		{
			solAssert(type.location() == DataLocation::Storage, "");
			define(IRVariable{_memberAccess}.part("slot"), IRVariable{_memberAccess.expression()}.part("slot"));
		}
		else
			solAssert(false, "Invalid array member access.");

		break;
	}
	case Type::Category::FixedBytes:
	{
		auto const& type = dynamic_cast<FixedBytesType const&>(*_memberAccess.expression().annotation().type);
		if (member == "length")
			define(_memberAccess) << to_string(type.numBytes()) << "\n";
		else
			solAssert(false, "Illegal fixed bytes member.");
		break;
	}
	default:
		solAssert(false, "Member access to unknown type.");
	}
}

bool IRGeneratorForStatements::visit(InlineAssembly const& _inlineAsm)
{
	CopyTranslate bodyCopier{_inlineAsm.dialect(), m_context, _inlineAsm.annotation().externalReferences};

	yul::Statement modified = bodyCopier(_inlineAsm.operations());

	solAssert(holds_alternative<yul::Block>(modified), "");

	// Do not provide dialect so that we get the full type information.
	m_code << yul::AsmPrinter()(std::get<yul::Block>(std::move(modified))) << "\n";
	return false;
}


void IRGeneratorForStatements::endVisit(IndexAccess const& _indexAccess)
{
	Type const& baseType = *_indexAccess.baseExpression().annotation().type;

	if (baseType.category() == Type::Category::Mapping)
	{
		solAssert(_indexAccess.indexExpression(), "Index expression expected.");

		MappingType const& mappingType = dynamic_cast<MappingType const&>(baseType);
		Type const& keyType = *_indexAccess.indexExpression()->annotation().type;
		solAssert(keyType.sizeOnStack() <= 1, "");

		string slot = m_context.newYulVariable();
		Whiskers templ("let <slot> := <indexAccess>(<base> <key>)\n");
		templ("slot", slot);
		templ("indexAccess", m_utils.mappingIndexAccessFunction(mappingType, keyType));
		templ("base", IRVariable(_indexAccess.baseExpression()).commaSeparatedList());
		if (keyType.sizeOnStack() == 0)
			templ("key", "");
		else
			templ("key", ", " + IRVariable(*_indexAccess.indexExpression()).commaSeparatedList());
		m_code << templ.render();
		setLValue(_indexAccess, IRLValue{
			*_indexAccess.annotation().type,
			IRLValue::Storage{
				slot,
				0
			}
		});
	}
	else if (baseType.category() == Type::Category::Array)
	{
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(baseType);
		solAssert(_indexAccess.indexExpression(), "Index expression expected.");

		switch (arrayType.location())
		{
			case DataLocation::Storage:
			{
				string slot = m_context.newYulVariable();
				string offset = m_context.newYulVariable();

				m_code << Whiskers(R"(
					let <slot>, <offset> := <indexFunc>(<array>, <index>)
				)")
				("slot", slot)
				("offset", offset)
				("indexFunc", m_utils.storageArrayIndexAccessFunction(arrayType))
				("array", IRVariable(_indexAccess.baseExpression()).part("slot").name())
				("index", IRVariable(*_indexAccess.indexExpression()).name())
				.render();

				setLValue(_indexAccess, IRLValue{
					*_indexAccess.annotation().type,
					IRLValue::Storage{slot, offset}
				});

				break;
			}
			case DataLocation::Memory:
			{
				string const memAddress =
					m_utils.memoryArrayIndexAccessFunction(arrayType) +
					"(" +
					IRVariable(_indexAccess.baseExpression()).part("mpos").name() +
					", " +
					expressionAsType(*_indexAccess.indexExpression(), *TypeProvider::uint256()) +
					")";

				setLValue(_indexAccess, IRLValue{
					*arrayType.baseType(),
					IRLValue::Memory{memAddress}
				});
				break;
			}
			case DataLocation::CallData:
			{
				IRVariable var(m_context.newYulVariable(), *arrayType.baseType());
				define(var) <<
					m_utils.calldataArrayIndexAccessFunction(arrayType) <<
					"(" <<
					IRVariable(_indexAccess.baseExpression()).commaSeparatedList() <<
					", " <<
					expressionAsType(*_indexAccess.indexExpression(), *TypeProvider::uint256()) <<
					")\n";
				if (arrayType.isByteArray())
					define(_indexAccess) <<
						m_utils.cleanupFunction(*arrayType.baseType()) <<
						"(calldataload(" <<
						var.name() <<
						"))\n";
				else if (arrayType.baseType()->isValueType())
					define(_indexAccess) <<
						m_utils.readFromCalldata(*arrayType.baseType()) <<
						"(" <<
						var.commaSeparatedList() <<
						")\n";
				else
					define(_indexAccess, var);
				break;
			}
		}
	}
	else if (baseType.category() == Type::Category::FixedBytes)
		solUnimplementedAssert(false, "");
	else if (baseType.category() == Type::Category::TypeType)
	{
		solAssert(baseType.sizeOnStack() == 0, "");
		solAssert(_indexAccess.annotation().type->sizeOnStack() == 0, "");
		// no-op - this seems to be a lone array type (`structType[];`)
	}
	else
		solAssert(false, "Index access only allowed for mappings or arrays.");
}

void IRGeneratorForStatements::endVisit(IndexRangeAccess const&)
{
	solUnimplementedAssert(false, "Index range accesses not yet implemented.");
}

void IRGeneratorForStatements::endVisit(Identifier const& _identifier)
{
	Declaration const* declaration = _identifier.annotation().referencedDeclaration;
	if (MagicVariableDeclaration const* magicVar = dynamic_cast<MagicVariableDeclaration const*>(declaration))
	{
		switch (magicVar->type()->category())
		{
		case Type::Category::Contract:
			if (dynamic_cast<ContractType const&>(*magicVar->type()).isSuper())
				solAssert(_identifier.name() == "super", "");
			else
			{
				solAssert(_identifier.name() == "this", "");
				define(_identifier) << "address()\n";
			}
			break;
		case Type::Category::Integer:
			solAssert(_identifier.name() == "now", "");
			define(_identifier) << "timestamp()\n";
			break;
		default:
			break;
		}
		return;
	}
	else if (FunctionDefinition const* functionDef = dynamic_cast<FunctionDefinition const*>(declaration))
		define(_identifier) << to_string(m_context.virtualFunction(*functionDef).id()) << "\n";
	else if (VariableDeclaration const* varDecl = dynamic_cast<VariableDeclaration const*>(declaration))
	{
		// TODO for the constant case, we have to be careful:
		// If the value is visited twice, `defineExpression` is called twice on
		// the same expression.
		solUnimplementedAssert(!varDecl->isConstant(), "");
		if (m_context.isLocalVariable(*varDecl))
			setLValue(_identifier, IRLValue{
				*varDecl->annotation().type,
				IRLValue::Stack{m_context.localVariable(*varDecl)}
			});
		else if (m_context.isStateVariable(*varDecl))
			setLValue(_identifier, IRLValue{
				*varDecl->annotation().type,
				IRLValue::Storage{
					toCompactHexWithPrefix(m_context.storageLocationOfVariable(*varDecl).first),
					m_context.storageLocationOfVariable(*varDecl).second
				}
			});
		else
			solAssert(false, "Invalid variable kind.");
	}
	else if (auto contract = dynamic_cast<ContractDefinition const*>(declaration))
	{
		solUnimplementedAssert(!contract->isLibrary(), "Libraries not yet supported.");
	}
	else if (dynamic_cast<EventDefinition const*>(declaration))
	{
		// no-op
	}
	else if (dynamic_cast<EnumDefinition const*>(declaration))
	{
		// no-op
	}
	else if (dynamic_cast<StructDefinition const*>(declaration))
	{
		// no-op
	}
	else
	{
		solAssert(false, "Identifier type not expected in expression context.");
	}
}

bool IRGeneratorForStatements::visit(Literal const& _literal)
{
	Type const& literalType = type(_literal);

	switch (literalType.category())
	{
	case Type::Category::RationalNumber:
	case Type::Category::Bool:
	case Type::Category::Address:
		define(_literal) << toCompactHexWithPrefix(literalType.literalValue(&_literal)) << "\n";
		break;
	case Type::Category::StringLiteral:
		break; // will be done during conversion
	default:
		solUnimplemented("Only integer, boolean and string literals implemented for now.");
	}
	return false;
}

void IRGeneratorForStatements::appendExternalFunctionCall(
	FunctionCall const& _functionCall,
	vector<ASTPointer<Expression const>> const& _arguments
)
{
	FunctionType const& funType = dynamic_cast<FunctionType const&>(type(_functionCall.expression()));
	solAssert(
		funType.takesArbitraryParameters() ||
		_arguments.size() == funType.parameterTypes().size(), ""
	);
	solUnimplementedAssert(!funType.bound(), "");
	FunctionType::Kind funKind = funType.kind();

	solAssert(funKind != FunctionType::Kind::BareStaticCall || m_context.evmVersion().hasStaticCall(), "");
	solAssert(funKind != FunctionType::Kind::BareCallCode, "Callcode has been removed.");

	bool returnSuccessConditionAndReturndata = funKind == FunctionType::Kind::BareCall || funKind == FunctionType::Kind::BareDelegateCall || funKind == FunctionType::Kind::BareStaticCall;
	bool isDelegateCall = funKind == FunctionType::Kind::BareDelegateCall || funKind == FunctionType::Kind::DelegateCall;
	bool useStaticCall = funKind == FunctionType::Kind::BareStaticCall || (funType.stateMutability() <= StateMutability::View && m_context.evmVersion().hasStaticCall());

	bool haveReturndatacopy = m_context.evmVersion().supportsReturndata();
	unsigned retSize = 0;
	bool dynamicReturnSize = false;
	TypePointers returnTypes;
	if (!returnSuccessConditionAndReturndata)
	{
		if (haveReturndatacopy)
			returnTypes = funType.returnParameterTypes();
		else
			returnTypes = funType.returnParameterTypesWithoutDynamicTypes();

		for (auto const& retType: returnTypes)
			if (retType->isDynamicallyEncoded())
			{
				solAssert(haveReturndatacopy, "");
				dynamicReturnSize = true;
				retSize = 0;
				break;
			}
			else if (retType->decodingType())
				retSize += retType->decodingType()->calldataEncodedSize();
			else
				retSize += retType->calldataEncodedSize();
	}

	TypePointers argumentTypes;
	vector<string> argumentStrings;
	for (auto const& arg: _arguments)
	{
		argumentTypes.emplace_back(&type(*arg));
		argumentStrings.emplace_back(IRVariable(*arg).commaSeparatedList());
	}
	string argumentString = joinHumanReadable(argumentStrings);

	solUnimplementedAssert(funKind != FunctionType::Kind::ECRecover, "");

	if (!m_context.evmVersion().canOverchargeGasForCall())
	{
		// Touch the end of the output area so that we do not pay for memory resize during the call
		// (which we would have to subtract from the gas left)
		// We could also just use MLOAD; POP right before the gas calculation, but the optimizer
		// would remove that, so we use MSTORE here.
		if (!funType.gasSet() && retSize > 0)
			m_code << "mstore(add(" << fetchFreeMem() << ", " << to_string(retSize) << "), 0)\n";
	}

	ABIFunctions abi(m_context.evmVersion(), m_context.revertStrings(), m_context.functionCollector());

	solUnimplementedAssert(!funType.isBareCall(), "");
	Whiskers templ(R"(
		<?checkExistence>
			if iszero(extcodesize(<address>)) { revert(0, 0) }
		</checkExistence>

		let <pos> := <freeMem>
		mstore(<pos>, <shl28>(<funId>))
		let <end> := <encodeArgs>(add(<pos>, 4) <argumentString>)

		let <result> := <call>(<gas>, <address>, <value>, <pos>, sub(<end>, <pos>), <pos>, <retSize>)
		if iszero(<result>) { <forwardingRevert> }

		<?dynamicReturnSize>
			returndatacopy(<pos>, 0, returndatasize())
		</dynamicReturnSize>
		<allocate>
		mstore(<freeMem>, add(<pos>, and(add(<retSize>, 0x1f), not(0x1f))))
		<?returns> let <retvars> := </returns> <abiDecode>(<pos>, <retSize>)
	)");
	templ("pos", m_context.newYulVariable());
	templ("end", m_context.newYulVariable());
	templ("result", m_context.newYulVariable());
	templ("freeMem", fetchFreeMem());
	templ("shl28", m_utils.shiftLeftFunction(8 * (32 - 4)));
	templ("funId", IRVariable(_functionCall.expression()).part("functionIdentifier").name());

	// If the function takes arbitrary parameters or is a bare call, copy dynamic length data in place.
	// Move arguments to memory, will not update the free memory pointer (but will update the memory
	// pointer on the stack).
	bool encodeInPlace = funType.takesArbitraryParameters() || funType.isBareCall();
	if (funType.kind() == FunctionType::Kind::ECRecover)
		// This would be the only combination of padding and in-place encoding,
		// but all parameters of ecrecover are value types anyway.
		encodeInPlace = false;
	bool encodeForLibraryCall = funKind == FunctionType::Kind::DelegateCall;
	solUnimplementedAssert(!encodeInPlace, "");
	solUnimplementedAssert(!funType.padArguments(), "");
	templ("encodeArgs", abi.tupleEncoder(argumentTypes, funType.parameterTypes(), encodeForLibraryCall));
	templ("argumentString", argumentString);

	// Output data will replace input data, unless we have ECRecover (then, output
	// area will be 32 bytes just before input area).
	templ("retSize", to_string(retSize));
	solUnimplementedAssert(funKind != FunctionType::Kind::ECRecover, "");

	if (isDelegateCall)
		solAssert(!funType.valueSet(), "Value set for delegatecall");
	else if (useStaticCall)
		solAssert(!funType.valueSet(), "Value set for staticcall");
	else if (funType.valueSet())
		templ("value", IRVariable(_functionCall.expression()).part("value").name());
	else
		templ("value", "0");

	// Check that the target contract exists (has code) for non-low-level calls.
	bool checkExistence = (funKind == FunctionType::Kind::External || funKind == FunctionType::Kind::DelegateCall);
	templ("checkExistence", checkExistence);

	if (funType.gasSet())
		templ("gas", IRVariable(_functionCall.expression()).part("gas").name());
	else if (m_context.evmVersion().canOverchargeGasForCall())
		// Send all gas (requires tangerine whistle EVM)
		templ("gas", "gas()");
	else
	{
		// send all gas except the amount needed to execute "SUB" and "CALL"
		// @todo this retains too much gas for now, needs to be fine-tuned.
		u256 gasNeededByCaller = evmasm::GasCosts::callGas(m_context.evmVersion()) + 10;
		if (funType.valueSet())
			gasNeededByCaller += evmasm::GasCosts::callValueTransferGas;
		if (!checkExistence)
			gasNeededByCaller += evmasm::GasCosts::callNewAccountGas; // we never know
		templ("gas", "sub(gas(), " + formatNumber(gasNeededByCaller) + ")");
	}
	// Order is important here, STATICCALL might overlap with DELEGATECALL.
	if (isDelegateCall)
		templ("call", "delegatecall");
	else if (useStaticCall)
		templ("call", "staticcall");
	else
		templ("call", "call");

	templ("forwardingRevert", m_utils.forwardingRevertFunction());

	solUnimplementedAssert(!returnSuccessConditionAndReturndata, "");
	solUnimplementedAssert(funKind != FunctionType::Kind::RIPEMD160, "");
	solUnimplementedAssert(funKind != FunctionType::Kind::ECRecover, "");

	templ("dynamicReturnSize", dynamicReturnSize);
	// Always use the actual return length, and not our calculated expected length, if returndatacopy is supported.
	// This ensures it can catch badly formatted input from external calls.
	if (haveReturndatacopy)
		templ("returnSize", "returndatasize()");
	else
		templ("returnSize", to_string(retSize));
	templ("abiDecode", abi.tupleDecoder(returnTypes, true));
	templ("returns", !returnTypes.empty());
	templ("retVars", IRVariable(_functionCall).commaSeparatedList());
}

string IRGeneratorForStatements::fetchFreeMem() const
{
	return "mload(" + to_string(CompilerUtils::freeMemoryPointer) + ")";
}

IRVariable IRGeneratorForStatements::convert(IRVariable const& _from, Type const& _to)
{
	if (_from.type() == _to)
		return _from;
	else
	{
		IRVariable converted(m_context.newYulVariable(), _to);
		define(converted, _from);
		return converted;
	}
}

std::string IRGeneratorForStatements::expressionAsType(Expression const& _expression, Type const& _to)
{
	IRVariable from(_expression);
	if (from.type() == _to)
		return from.commaSeparatedList();
	else
		return m_utils.conversionFunction(from.type(), _to) + "(" + from.commaSeparatedList() + ")";
}

std::ostream& IRGeneratorForStatements::define(IRVariable const& _var)
{
	if (_var.type().sizeOnStack() > 0)
		m_code << "let " << _var.commaSeparatedList() << " := ";
	return m_code;
}

void IRGeneratorForStatements::declareAssign(IRVariable const& _lhs, IRVariable const& _rhs, bool _declare)
{
	string output;
	if (_lhs.type() == _rhs.type())
		for (auto const& [stackItemName, stackItemType]: _lhs.type().stackItems())
			if (stackItemType)
				declareAssign(_lhs.part(stackItemName), _rhs.part(stackItemName), _declare);
			else
				m_code << (_declare ? "let ": "") << _lhs.part(stackItemName).name() << " := " << _rhs.part(stackItemName).name() << "\n";
	else
		m_code <<
			(_declare ? "let ": "") <<
			_lhs.commaSeparatedList() <<
			" := " <<
			m_context.utils().conversionFunction(_rhs.type(), _lhs.type()) <<
			"(" <<
			_rhs.commaSeparatedList() <<
			")\n";
}
void IRGeneratorForStatements::declare(IRVariable const& _var)
{
	if (_var.type().sizeOnStack() > 0)
		m_code << "let " << _var.commaSeparatedList() << "\n";
}

void IRGeneratorForStatements::appendSimpleUnaryOperation(UnaryOperation const& _operation, Expression const& _expr)
{
	string func;

	if (_operation.getOperator() == Token::Not)
		func = "iszero";
	else if (_operation.getOperator() == Token::BitNot)
		func = "not";
	else
		solAssert(false, "Invalid Token!");

	define(_operation) <<
		m_utils.cleanupFunction(type(_expr)) <<
		"(" <<
			func <<
			"(" <<
			IRVariable(_expr).commaSeparatedList() <<
			")" <<
		")\n";
}

string IRGeneratorForStatements::binaryOperation(
	langutil::Token _operator,
	Type const& _type,
	string const& _left,
	string const& _right
)
{
	if (IntegerType const* type = dynamic_cast<IntegerType const*>(&_type))
	{
		string fun;
		// TODO: Implement all operations for signed and unsigned types.
		switch (_operator)
		{
			case Token::Add:
				fun = m_utils.overflowCheckedIntAddFunction(*type);
				break;
			case Token::Sub:
				fun = m_utils.overflowCheckedIntSubFunction(*type);
				break;
			case Token::Mul:
				fun = m_utils.overflowCheckedIntMulFunction(*type);
				break;
			case Token::Div:
				fun = m_utils.overflowCheckedIntDivFunction(*type);
				break;
			case Token::Mod:
				fun = m_utils.checkedIntModFunction(*type);
				break;
			default:
				break;
		}

		solUnimplementedAssert(!fun.empty(), "");
		return fun + "(" + _left + ", " + _right + ")\n";
	}
	else
		solUnimplementedAssert(false, "");

	return {};
}

void IRGeneratorForStatements::appendAndOrOperatorCode(BinaryOperation const& _binOp)
{
	langutil::Token const op = _binOp.getOperator();
	solAssert(op == Token::Or || op == Token::And, "");

	_binOp.leftExpression().accept(*this);

	IRVariable value(_binOp);
	define(value, _binOp.leftExpression());
	if (op == Token::Or)
		m_code << "if iszero(" << value.name() << ") {\n";
	else
		m_code << "if " << value.name() << " {\n";
	_binOp.rightExpression().accept(*this);
	assign(value, _binOp.rightExpression());
	m_code << "}\n";
}

void IRGeneratorForStatements::writeToLValue(IRLValue const& _lvalue, IRVariable const& _value)
{
	std::visit(
		util::GenericVisitor{
			[&](IRLValue::Storage const& _storage) {
				std::optional<unsigned> offset;

				if (std::holds_alternative<unsigned>(_storage.offset))
					offset = std::get<unsigned>(_storage.offset);

				m_code <<
					m_utils.updateStorageValueFunction(_lvalue.type, offset) <<
					"(" <<
					_storage.slot <<
					(
						std::holds_alternative<string>(_storage.offset) ?
						(", " + std::get<string>(_storage.offset)) :
						""
					) <<
					_value.commaSeparatedListPrefixed() <<
					")\n";
			},
			[&](IRLValue::Memory const& _memory) {
				if (_lvalue.type.isValueType())
				{
					IRVariable prepared(m_context.newYulVariable(), _lvalue.type);
					define(prepared, _value);

					if (_memory.byteArrayElement)
					{
						solAssert(_lvalue.type == *TypeProvider::byte(), "");
						m_code << "mstore8(" + _memory.address + ", byte(0, " + prepared.commaSeparatedList() + "))\n";
					}
					else
						m_code << m_utils.writeToMemoryFunction(_lvalue.type) <<
							"(" <<
							_memory.address <<
							", " <<
							prepared.commaSeparatedList() <<
							")\n";
				}
				else
				{
					solAssert(_lvalue.type.sizeOnStack() == 1, "");
					solAssert(dynamic_cast<ReferenceType const*>(&_lvalue.type), "");
					auto const* valueReferenceType = dynamic_cast<ReferenceType const*>(&_value.type());
					solAssert(valueReferenceType && valueReferenceType->dataStoredIn(DataLocation::Memory), "");
					m_code << "mstore(" + _memory.address + ", " + _value.name() + ")\n";
				}
			},
			[&](IRLValue::Stack const& _stack) { assign(_stack.variable, _value); },
			[&](IRLValue::Tuple const& _tuple) {
				auto components = std::move(_tuple.components);
				for (size_t i = 0; i < components.size(); i++)
				{
					size_t idx = components.size() - i - 1;
					if (components[idx])
						writeToLValue(*components[idx], _value.tupleComponent(idx));
				}
			}
		},
		_lvalue.kind
	);
}

IRVariable IRGeneratorForStatements::readFromLValue(IRLValue const& _lvalue)
{
	IRVariable result{m_context.newYulVariable(), _lvalue.type};
	std::visit(GenericVisitor{
		[&](IRLValue::Storage const& _storage) {
			if (!_lvalue.type.isValueType())
				define(result) << _storage.slot << "\n";
			else if (std::holds_alternative<string>(_storage.offset))
				define(result) <<
					m_utils.readFromStorageDynamic(_lvalue.type, false) <<
					"(" <<
					_storage.slot <<
					", " <<
					std::get<string>(_storage.offset) <<
					")\n";
			else
				define(result) <<
					m_utils.readFromStorage(_lvalue.type, std::get<unsigned>(_storage.offset), false) <<
					"(" <<
					_storage.slot <<
					")\n";
		},
		[&](IRLValue::Memory const& _memory) {
			if (_memory.byteArrayElement)
				define(result) <<
					m_utils.cleanupFunction(_lvalue.type) <<
					"(mload(" <<
					_memory.address <<
					"))\n";
			else if (_lvalue.type.isValueType())
				define(result) <<
					m_utils.readFromMemory(_lvalue.type) <<
					"(" <<
					_memory.address <<
					")\n";
			else
				define(result) << "mload(" << _memory.address << ")\n";
		},
		[&](IRLValue::Stack const& _stack) {
			define(result, _stack.variable);
		},
		[&](IRLValue::Tuple const&) {
			solAssert(false, "Attempted to read from tuple lvalue.");
		}
	}, _lvalue.kind);
	return result;
}

void IRGeneratorForStatements::setLValue(Expression const& _expression, IRLValue _lvalue)
{
	solAssert(!m_currentLValue, "");

	if (_expression.annotation().lValueRequested)
	{
		m_currentLValue.emplace(std::move(_lvalue));
		solAssert(!_lvalue.type.dataStoredIn(DataLocation::CallData), "");
	}
	else
		// Only define the expression, if it will not be written to.
		define(_expression, readFromLValue(_lvalue));
}

void IRGeneratorForStatements::generateLoop(
	Statement const& _body,
	Expression const* _conditionExpression,
	Statement const*  _initExpression,
	ExpressionStatement const* _loopExpression,
	bool _isDoWhile
)
{
	string firstRun;

	if (_isDoWhile)
	{
		solAssert(_conditionExpression, "Expected condition for doWhile");
		firstRun = m_context.newYulVariable();
		m_code << "let " << firstRun << " := 1\n";
	}

	m_code << "for {\n";
	if (_initExpression)
		_initExpression->accept(*this);
	m_code << "} 1 {\n";
	if (_loopExpression)
		_loopExpression->accept(*this);
	m_code << "}\n";
	m_code << "{\n";

	if (_conditionExpression)
	{
		if (_isDoWhile)
			m_code << "if iszero(" << firstRun << ") {\n";

		_conditionExpression->accept(*this);
		m_code <<
			"if iszero(" <<
			expressionAsType(*_conditionExpression, *TypeProvider::boolean()) <<
			") { break }\n";

		if (_isDoWhile)
			m_code << "}\n" << firstRun << " := 0\n";
	}

	_body.accept(*this);

	m_code << "}\n";
}

Type const& IRGeneratorForStatements::type(Expression const& _expression)
{
	solAssert(_expression.annotation().type, "Type of expression not set.");
	return *_expression.annotation().type;
}
