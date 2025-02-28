//===- Deserializer.cpp - MLIR SPIR-V Deserialization ---------------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file defines the SPIR-V binary to MLIR SPIR-V module deseralization.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/SPIRV/Serialization.h"

#include "mlir/Dialect/SPIRV/SPIRVBinaryUtils.h"
#include "mlir/Dialect/SPIRV/SPIRVOps.h"
#include "mlir/Dialect/SPIRV/SPIRVTypes.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Location.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Support/StringExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Sequence.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

#define DEBUG_TYPE "spirv-deserialization"

// Decodes a string literal in `words` starting at `wordIndex`. Update the
// latter to point to the position in words after the string literal.
static inline StringRef decodeStringLiteral(ArrayRef<uint32_t> words,
                                            unsigned &wordIndex) {
  StringRef str(reinterpret_cast<const char *>(words.data() + wordIndex));
  wordIndex += str.size() / 4 + 1;
  return str;
}

// Extracts the opcode from the given first word of a SPIR-V instruction.
static inline spirv::Opcode extractOpcode(uint32_t word) {
  return static_cast<spirv::Opcode>(word & 0xffff);
}

namespace {
/// A SPIR-V module serializer.
///
/// A SPIR-V binary module is a single linear stream of instructions; each
/// instruction is composed of 32-bit words. The first word of an instruction
/// records the total number of words of that instruction using the 16
/// higher-order bits. So this deserializer uses that to get instruction
/// boundary and parse instructions and build a SPIR-V ModuleOp gradually.
///
// TODO(antiagainst): clean up created ops on errors
class Deserializer {
public:
  /// Creates a deserializer for the given SPIR-V `binary` module.
  /// The SPIR-V ModuleOp will be created into `context.
  explicit Deserializer(ArrayRef<uint32_t> binary, MLIRContext *context);

  /// Deserializes the remembered SPIR-V binary module.
  LogicalResult deserialize();

  /// Collects the final SPIR-V ModuleOp.
  Optional<spirv::ModuleOp> collect();

private:
  //===--------------------------------------------------------------------===//
  // Module structure
  //===--------------------------------------------------------------------===//

  /// Initializes the `module` ModuleOp in this deserializer instance.
  spirv::ModuleOp createModuleOp();

  /// Processes SPIR-V module header in `binary`.
  LogicalResult processHeader();

  /// Processes the SPIR-V OpCapability with `operands` and updates bookkeeping
  /// in the deserializer.
  LogicalResult processCapability(ArrayRef<uint32_t> operands);

  /// Attaches all collected capabilites to `module` as an attribute.
  void attachCapabilities();

  /// Processes the SPIR-V OpExtension with `operands` and updates bookkeeping
  /// in the deserializer.
  LogicalResult processExtension(ArrayRef<uint32_t> words);

  /// Processes the SPIR-V OpExtInstImport with `operands` and updates
  /// bookkeeping in the deserializer.
  LogicalResult processExtInstImport(ArrayRef<uint32_t> words);

  /// Attaches all collected extensions to `module` as an attribute.
  void attachExtensions();

  /// Processes the SPIR-V OpMemoryModel with `operands` and updates `module`.
  LogicalResult processMemoryModel(ArrayRef<uint32_t> operands);

  /// Process SPIR-V OpName with `operands`.
  LogicalResult processName(ArrayRef<uint32_t> operands);

  /// Method to process an OpDecorate instruction.
  LogicalResult processDecoration(ArrayRef<uint32_t> words);

  // Method to process an OpMemberDecorate instruction.
  LogicalResult processMemberDecoration(ArrayRef<uint32_t> words);

  /// Gets the FuncOp associated with a result <id> of OpFunction.
  FuncOp getFunction(uint32_t id) { return funcMap.lookup(id); }

  /// Processes the SPIR-V function at the current `offset` into `binary`.
  /// The operands to the OpFunction instruction is passed in as ``operands`.
  /// This method processes each instruction inside the function and dispatches
  /// them to their handler method accordingly.
  LogicalResult processFunction(ArrayRef<uint32_t> operands);

  LogicalResult processFunctionEnd(ArrayRef<uint32_t> operands);

  /// Gets the constant's attribute and type associated with the given <id>.
  Optional<std::pair<Attribute, Type>> getConstant(uint32_t id);

  /// Returns a symbol to be used for the function name with the given
  /// result <id>. This tries to use the function's OpName if
  /// exists; otherwise creates one based on the <id>.
  std::string getFunctionSymbol(uint32_t id);

  /// Returns a symbol to be used for the specialization constant with the given
  /// result <id>. This tries to use the specialization constant's OpName if
  /// exists; otherwise creates one based on the <id>.
  std::string getSpecConstantSymbol(uint32_t id);

  /// Gets the specialization constant with the given result <id>.
  spirv::SpecConstantOp getSpecConstant(uint32_t id) {
    return specConstMap.lookup(id);
  }

  /// Processes the OpVariable instructions at current `offset` into `binary`.
  /// It is expected that this method is used for variables that are to be
  /// defined at module scope and will be deserialized into a spv.globalVariable
  /// instruction.
  LogicalResult processGlobalVariable(ArrayRef<uint32_t> operands);

  /// Gets the global variable associated with a result <id> of OpVariable.
  spirv::GlobalVariableOp getGlobalVariable(uint32_t id) {
    return globalVariableMap.lookup(id);
  }

  //===--------------------------------------------------------------------===//
  // Type
  //===--------------------------------------------------------------------===//

  /// Gets type for a given result <id>.
  Type getType(uint32_t id) { return typeMap.lookup(id); }

  /// Returns true if the given `type` is for SPIR-V void type.
  bool isVoidType(Type type) const { return type.isa<NoneType>(); }

  /// Processes a SPIR-V type instruction with given `opcode` and `operands` and
  /// registers the type into `module`.
  LogicalResult processType(spirv::Opcode opcode, ArrayRef<uint32_t> operands);

  LogicalResult processArrayType(ArrayRef<uint32_t> operands);

  LogicalResult processFunctionType(ArrayRef<uint32_t> operands);

  LogicalResult processRuntimeArrayType(ArrayRef<uint32_t> operands);

  LogicalResult processStructType(ArrayRef<uint32_t> operands);

  //===--------------------------------------------------------------------===//
  // Constant
  //===--------------------------------------------------------------------===//

  /// Processes a SPIR-V Op{|Spec}Constant instruction with the given
  /// `operands`. `isSpec` indicates whether this is a specialization constant.
  LogicalResult processConstant(ArrayRef<uint32_t> operands, bool isSpec);

  /// Processes a SPIR-V Op{|Spec}Constant{True|False} instruction with the
  /// given `operands`. `isSpec` indicates whether this is a specialization
  /// constant.
  LogicalResult processConstantBool(bool isTrue, ArrayRef<uint32_t> operands,
                                    bool isSpec);

  /// Processes a SPIR-V OpConstantComposite instruction with the given
  /// `operands`.
  LogicalResult processConstantComposite(ArrayRef<uint32_t> operands);

  /// Processes a SPIR-V OpConstantNull instruction with the given `operands`.
  LogicalResult processConstantNull(ArrayRef<uint32_t> operands);

  //===--------------------------------------------------------------------===//
  // Control flow
  //===--------------------------------------------------------------------===//

  // In SPIR-V, structured control flow is explicitly declared using merge
  // instructions (OpSelectionMerge and OpLoopMerge). In the SPIR-V dialect,
  // we use spv.selection and spv.loop to group structured control flow.
  // The deserializer need to turn structured control flow marked with merge
  // instructions into using spv.selection/spv.loop ops.
  //
  // Because structured control flow can nest and the basic block order have
  // flexibility, we cannot isolate a structured selection/loop without
  // deserializing all the blocks. So we use the following approach:
  //
  // 1. Deserialize all basic blocks in a function and create MLIR blocks for
  //    them into the function's region. In the meanwhile, keep a map between
  //    selection/loop header blocks to their corresponding merge (and continue)
  //    target blocks.
  // 2. For each selection/loop header block, recursively get all basic blocks
  //    reachable (except the merge block) and put them in a newly created
  //    spv.selection/spv.loop's region. Structured control flow guarantees
  //    that we enter and exit in structured ways and the construct is nestable.
  // 3. Put the new spv.selection/spv.loop op at the beginning of the old merge
  //    block and redirect all branches to the old header block to the old
  //    merge block (which contains the spv.selection/spv.loop op now).

  /// Returns the block for the given label <id>.
  Block *getBlock(uint32_t id) const { return blockMap.lookup(id); }

  /// A struct for containing a header block's merge and continue targets.
  struct BlockMergeInfo {
    Block *mergeBlock;
    Block *continueBlock;

    BlockMergeInfo() : mergeBlock(nullptr), continueBlock(nullptr) {}
    BlockMergeInfo(Block *m, Block *c) : mergeBlock(m), continueBlock(c) {}

    operator bool() const { return continueBlock && mergeBlock; }
  };

  /// Returns the merge and continue target info for the given `block` if it is
  /// a header block.
  BlockMergeInfo getBlockMergeInfo(Block *block) const {
    return blockMergeInfo.lookup(block);
  }

  /// Gets or creates the block corresponding to the given label <id>. The newly
  /// created block will always be placed at the end of the current function.
  Block *getOrCreateBlock(uint32_t id);

  LogicalResult processBranch(ArrayRef<uint32_t> operands);

  LogicalResult processBranchConditional(ArrayRef<uint32_t> operands);

  /// Processes a SPIR-V OpLabel instruction with the given `operands`.
  LogicalResult processLabel(ArrayRef<uint32_t> operands);

  /// Processes a SPIR-V OpLoopMerge instruction with the given `operands`.
  LogicalResult processLoopMerge(ArrayRef<uint32_t> operands);

  /// Extracts blocks belonging to a structured selection/loop into a
  /// spv.selection/spv.loop op. This method iterates until all blocks
  /// declared as selection/loop headers are handled.
  LogicalResult structurizeControlFlow();

  //===--------------------------------------------------------------------===//
  // Instruction
  //===--------------------------------------------------------------------===//

  /// Get the Value associated with a result <id>.
  ///
  /// This method materializes normal constants and inserts "casting" ops
  /// (`spv._address_of` and `spv._reference_of`) to turn an symbol into a SSA
  /// value for handling uses of module scope constants/variables in functions.
  Value *getValue(uint32_t id);

  /// Slices the first instruction out of `binary` and returns its opcode and
  /// operands via `opcode` and `operands` respectively. Returns failure if
  /// there is no more remaining instructions (`expectedOpcode` will be used to
  /// compose the error message) or the next instruction is malformed.
  LogicalResult
  sliceInstruction(spirv::Opcode &opcode, ArrayRef<uint32_t> &operands,
                   Optional<spirv::Opcode> expectedOpcode = llvm::None);

  /// Returns the next instruction's opcode if exists.
  Optional<spirv::Opcode> peekOpcode();

  /// Processes a SPIR-V instruction with the given `opcode` and `operands`.
  /// This method is the main entrance for handling SPIR-V instruction; it
  /// checks the instruction opcode and dispatches to the corresponding handler.
  /// Processing of Some instructions (like OpEntryPoint and OpExecutionMode)
  /// might need to be defered, since they contain forward references to <id>s
  /// in the deserialized binary, but module in SPIR-V dialect expects these to
  /// be ssa-uses.
  LogicalResult processInstruction(spirv::Opcode opcode,
                                   ArrayRef<uint32_t> operands,
                                   bool deferInstructions = true);

  /// Method to dispatch to the specialized deserialization function for an
  /// operation in SPIR-V dialect that is a mirror of an instruction in the
  /// SPIR-V spec. This is auto-generated from ODS. Dispatch is handled for
  /// all operations in SPIR-V dialect that have hasOpcode == 1.
  LogicalResult dispatchToAutogenDeserialization(spirv::Opcode opcode,
                                                 ArrayRef<uint32_t> words);

  /// Processes a SPIR-V OpExtInst with given `operands`. This slices the
  /// entries of `operands` that specify the extended instruction set <id> and
  /// the instruction opcode. The op deserializer is then invoked using the
  /// other entries.
  LogicalResult processExtInst(ArrayRef<uint32_t> operands);

  /// Dispatches the deserialization of extended instruction set operation based
  /// on the extended instruction set name, and instruction opcode. This is
  /// autogenerated from ODS.
  LogicalResult
  dispatchToExtensionSetAutogenDeserialization(StringRef extensionSetName,
                                               uint32_t instructionID,
                                               ArrayRef<uint32_t> words);

  /// Method to deserialize an operation in the SPIR-V dialect that is a mirror
  /// of an instruction in the SPIR-V spec. This is auto generated if hasOpcode
  /// == 1 and autogenSerialization == 1 in ODS.
  template <typename OpTy> LogicalResult processOp(ArrayRef<uint32_t> words) {
    return emitError(unknownLoc, "unsupported deserialization for ")
           << OpTy::getOperationName() << " op";
  }

private:
  /// The SPIR-V binary module.
  ArrayRef<uint32_t> binary;

  /// The current word offset into the binary module.
  unsigned curOffset = 0;

  /// MLIRContext to create SPIR-V ModuleOp into.
  MLIRContext *context;

  // TODO(antiagainst): create Location subclass for binary blob
  Location unknownLoc;

  /// The SPIR-V ModuleOp.
  Optional<spirv::ModuleOp> module;

  /// The current function under construction.
  Optional<FuncOp> curFunction;

  /// The current block under construction.
  Block *curBlock = nullptr;

  OpBuilder opBuilder;

  /// The list of capabilities used by the module.
  llvm::SmallSetVector<spirv::Capability, 4> capabilities;

  /// The list of extensions used by the module.
  llvm::SmallSetVector<StringRef, 2> extensions;

  // Result <id> to type mapping.
  DenseMap<uint32_t, Type> typeMap;

  // Result <id> to constant attribute and type mapping.
  ///
  /// In the SPIR-V binary format, all constants are placed in the module and
  /// shared by instructions at module level and in subsequent functions. But in
  /// the SPIR-V dialect, we materialize the constant to where it's used in the
  /// function. So when seeing a constant instruction in the binary format, we
  /// don't immediately emit a constant op into the module, we keep its value
  /// (and type) here. Later when it's used, we materialize the constant.
  DenseMap<uint32_t, std::pair<Attribute, Type>> constantMap;

  // Result <id> to variable mapping.
  DenseMap<uint32_t, spirv::SpecConstantOp> specConstMap;

  // Result <id> to variable mapping.
  DenseMap<uint32_t, spirv::GlobalVariableOp> globalVariableMap;

  // Result <id> to function mapping.
  DenseMap<uint32_t, FuncOp> funcMap;

  // Result <id> to block mapping.
  DenseMap<uint32_t, Block *> blockMap;

  // Header block to its merge (and continue) target mapping.
  DenseMap<Block *, BlockMergeInfo> blockMergeInfo;

  // Result <id> to value mapping.
  DenseMap<uint32_t, Value *> valueMap;

  // Result <id> to name mapping.
  DenseMap<uint32_t, StringRef> nameMap;

  // Result <id> to decorations mapping.
  DenseMap<uint32_t, NamedAttributeList> decorations;

  // Result <id> to type decorations.
  DenseMap<uint32_t, uint32_t> typeDecorations;

  // Result <id> to member decorations.
  // decorated-struct-type-<id> ->
  //    (struct-member-index -> (decoration -> decoration-operands))
  DenseMap<uint32_t,
           DenseMap<uint32_t, DenseMap<spirv::Decoration, ArrayRef<uint32_t>>>>
      memberDecorationMap;

  // Result <id> to extended instruction set name.
  DenseMap<uint32_t, StringRef> extendedInstSets;

  // List of instructions that are processed in a defered fashion (after an
  // initial processing of the entire binary). Some operations like
  // OpEntryPoint, and OpExecutionMode use forward references to function
  // <id>s. In SPIR-V dialect the corresponding operations (spv.EntryPoint and
  // spv.ExecutionMode) need these references resolved. So these instructions
  // are deserialized and stored for processing once the entire binary is
  // processed.
  SmallVector<std::pair<spirv::Opcode, ArrayRef<uint32_t>>, 4>
      deferedInstructions;
};
} // namespace

Deserializer::Deserializer(ArrayRef<uint32_t> binary, MLIRContext *context)
    : binary(binary), context(context), unknownLoc(UnknownLoc::get(context)),
      module(createModuleOp()), opBuilder(module->body()) {}

LogicalResult Deserializer::deserialize() {
  LLVM_DEBUG(llvm::dbgs() << "++ deserialization started\n");
  if (failed(processHeader()))
    return failure();

  spirv::Opcode opcode = spirv::Opcode::OpNop;
  ArrayRef<uint32_t> operands;
  auto binarySize = binary.size();
  while (curOffset < binarySize) {
    // Slice the next instruction out and populate `opcode` and `operands`.
    // Interally this also updates `curOffset`.
    if (failed(sliceInstruction(opcode, operands)))
      return failure();

    if (failed(processInstruction(opcode, operands)))
      return failure();
  }

  assert(curOffset == binarySize &&
         "deserializer should never index beyond the binary end");

  for (auto &defered : deferedInstructions) {
    if (failed(processInstruction(defered.first, defered.second, false))) {
      return failure();
    }
  }

  // Attaches the capabilities/extensions as an attribute to the module.
  attachCapabilities();
  attachExtensions();

  LLVM_DEBUG(llvm::dbgs() << "++ deserialization succeeded\n");
  return success();
}

Optional<spirv::ModuleOp> Deserializer::collect() { return module; }

//===----------------------------------------------------------------------===//
// Module structure
//===----------------------------------------------------------------------===//

spirv::ModuleOp Deserializer::createModuleOp() {
  Builder builder(context);
  OperationState state(unknownLoc, spirv::ModuleOp::getOperationName());
  // TODO(antiagainst): use target environment to select the version
  state.addAttribute("major_version", builder.getI32IntegerAttr(1));
  state.addAttribute("minor_version", builder.getI32IntegerAttr(0));
  spirv::ModuleOp::build(&builder, state);
  return cast<spirv::ModuleOp>(Operation::create(state));
}

LogicalResult Deserializer::processHeader() {
  if (binary.size() < spirv::kHeaderWordCount)
    return emitError(unknownLoc,
                     "SPIR-V binary module must have a 5-word header");

  if (binary[0] != spirv::kMagicNumber)
    return emitError(unknownLoc, "incorrect magic number");

  // TODO(antiagainst): generator number, bound, schema
  curOffset = spirv::kHeaderWordCount;
  return success();
}

LogicalResult Deserializer::processCapability(ArrayRef<uint32_t> operands) {
  if (operands.size() != 1)
    return emitError(unknownLoc, "OpMemoryModel must have one parameter");

  auto cap = spirv::symbolizeCapability(operands[0]);
  if (!cap)
    return emitError(unknownLoc, "unknown capability: ") << operands[0];

  capabilities.insert(*cap);
  return success();
}

void Deserializer::attachCapabilities() {
  if (capabilities.empty())
    return;

  SmallVector<StringRef, 2> caps;
  caps.reserve(capabilities.size());

  for (auto cap : capabilities) {
    caps.push_back(spirv::stringifyCapability(cap));
  }

  module->setAttr("capabilities", opBuilder.getStrArrayAttr(caps));
}

LogicalResult Deserializer::processExtension(ArrayRef<uint32_t> words) {
  if (words.empty()) {
    return emitError(
        unknownLoc,
        "OpExtension must have a literal string for the extension name");
  }

  unsigned wordIndex = 0;
  StringRef extName = decodeStringLiteral(words, wordIndex);
  if (wordIndex != words.size()) {
    return emitError(unknownLoc,
                     "unexpected trailing words in OpExtension instruction");
  }

  extensions.insert(extName);
  return success();
}

LogicalResult Deserializer::processExtInstImport(ArrayRef<uint32_t> words) {
  if (words.size() < 2) {
    return emitError(unknownLoc,
                     "OpExtInstImport must have a result <id> and a literal "
                     "string for the extensed instruction set name");
  }

  unsigned wordIndex = 1;
  extendedInstSets[words[0]] = decodeStringLiteral(words, wordIndex);
  if (wordIndex != words.size()) {
    return emitError(unknownLoc,
                     "unexpected trailing words in OpExtInstImport");
  }
  return success();
}

void Deserializer::attachExtensions() {
  if (extensions.empty())
    return;

  module->setAttr("extensions",
                  opBuilder.getStrArrayAttr(extensions.getArrayRef()));
}

LogicalResult Deserializer::processMemoryModel(ArrayRef<uint32_t> operands) {
  if (operands.size() != 2)
    return emitError(unknownLoc, "OpMemoryModel must have two operands");

  module->setAttr(
      "addressing_model",
      opBuilder.getI32IntegerAttr(llvm::bit_cast<int32_t>(operands.front())));
  module->setAttr(
      "memory_model",
      opBuilder.getI32IntegerAttr(llvm::bit_cast<int32_t>(operands.back())));

  return success();
}

LogicalResult Deserializer::processDecoration(ArrayRef<uint32_t> words) {
  // TODO : This function should also be auto-generated. For now, since only a
  // few decorations are processed/handled in a meaningful manner, going with a
  // manual implementation.
  if (words.size() < 2) {
    return emitError(
        unknownLoc, "OpDecorate must have at least result <id> and Decoration");
  }
  auto decorationName =
      stringifyDecoration(static_cast<spirv::Decoration>(words[1]));
  if (decorationName.empty()) {
    return emitError(unknownLoc, "invalid Decoration code : ") << words[1];
  }
  auto attrName = convertToSnakeCase(decorationName);
  switch (static_cast<spirv::Decoration>(words[1])) {
  case spirv::Decoration::DescriptorSet:
  case spirv::Decoration::Binding:
    if (words.size() != 3) {
      return emitError(unknownLoc, "OpDecorate with ")
             << decorationName << " needs a single integer literal";
    }
    decorations[words[0]].set(
        opBuilder.getIdentifier(attrName),
        opBuilder.getI32IntegerAttr(static_cast<int32_t>(words[2])));
    break;
  case spirv::Decoration::BuiltIn:
    if (words.size() != 3) {
      return emitError(unknownLoc, "OpDecorate with ")
             << decorationName << " needs a single integer literal";
    }
    decorations[words[0]].set(opBuilder.getIdentifier(attrName),
                              opBuilder.getStringAttr(stringifyBuiltIn(
                                  static_cast<spirv::BuiltIn>(words[2]))));
    break;
  case spirv::Decoration::ArrayStride:
    if (words.size() != 3) {
      return emitError(unknownLoc, "OpDecorate with ")
             << decorationName << " needs a single integer literal";
    }
    typeDecorations[words[0]] = static_cast<uint32_t>(words[2]);
    break;
  case spirv::Decoration::Block:
    if (words.size() != 2) {
      return emitError(unknownLoc, "OpDecoration with ")
             << decorationName << "needs a single target <id>";
    }
    // Block decoration does not affect spv.struct type.
    break;
  default:
    return emitError(unknownLoc, "unhandled Decoration : '") << decorationName;
  }
  return success();
}

LogicalResult Deserializer::processMemberDecoration(ArrayRef<uint32_t> words) {
  // The binary layout of OpMemberDecorate is different comparing to OpDecorate
  if (words.size() < 3) {
    return emitError(unknownLoc,
                     "OpMemberDecorate must have at least 3 operands");
  }

  auto decoration = static_cast<spirv::Decoration>(words[2]);
  if (decoration == spirv::Decoration::Offset && words.size() != 4) {
    return emitError(unknownLoc,
                     " missing offset specification in OpMemberDecorate with "
                     "Offset decoration");
  }
  ArrayRef<uint32_t> decorationOperands;
  if (words.size() > 3) {
    decorationOperands = words.slice(3);
  }
  memberDecorationMap[words[0]][words[1]][decoration] = decorationOperands;
  return success();
}

LogicalResult Deserializer::processFunction(ArrayRef<uint32_t> operands) {
  if (curFunction) {
    return emitError(unknownLoc, "found function inside function");
  }

  // Get the result type
  if (operands.size() != 4) {
    return emitError(unknownLoc, "OpFunction must have 4 parameters");
  }
  Type resultType = getType(operands[0]);
  if (!resultType) {
    return emitError(unknownLoc, "undefined result type from <id> ")
           << operands[0];
  }

  if (funcMap.count(operands[1])) {
    return emitError(unknownLoc, "duplicate function definition/declaration");
  }

  auto functionControl = spirv::symbolizeFunctionControl(operands[2]);
  if (!functionControl) {
    return emitError(unknownLoc, "unknown Function Control: ") << operands[2];
  }
  if (functionControl.getValue() != spirv::FunctionControl::None) {
    /// TODO : Handle different function controls
    return emitError(unknownLoc, "unhandled Function Control: '")
           << spirv::stringifyFunctionControl(functionControl.getValue())
           << "'";
  }

  Type fnType = getType(operands[3]);
  if (!fnType || !fnType.isa<FunctionType>()) {
    return emitError(unknownLoc, "unknown function type from <id> ")
           << operands[3];
  }
  auto functionType = fnType.cast<FunctionType>();

  if ((isVoidType(resultType) && functionType.getNumResults() != 0) ||
      (functionType.getNumResults() == 1 &&
       functionType.getResult(0) != resultType)) {
    return emitError(unknownLoc, "mismatch in function type ")
           << functionType << " and return type " << resultType << " specified";
  }

  std::string fnName = getFunctionSymbol(operands[1]);
  auto funcOp = opBuilder.create<FuncOp>(unknownLoc, fnName, functionType,
                                         ArrayRef<NamedAttribute>());
  curFunction = funcMap[operands[1]] = funcOp;
  LLVM_DEBUG(llvm::dbgs() << "[fn] processing function " << fnName << " (type="
                          << fnType << ", id=" << operands[1] << ")\n");
  auto *entryBlock = funcOp.addEntryBlock();
  LLVM_DEBUG(llvm::dbgs() << "[block] created entry block @ " << entryBlock
                          << "\n");

  // Parse the op argument instructions
  if (functionType.getNumInputs()) {
    for (size_t i = 0, e = functionType.getNumInputs(); i != e; ++i) {
      auto argType = functionType.getInput(i);
      spirv::Opcode opcode = spirv::Opcode::OpNop;
      ArrayRef<uint32_t> operands;
      if (failed(sliceInstruction(opcode, operands,
                                  spirv::Opcode::OpFunctionParameter))) {
        return failure();
      }
      if (opcode != spirv::Opcode::OpFunctionParameter) {
        return emitError(
                   unknownLoc,
                   "missing OpFunctionParameter instruction for argument ")
               << i;
      }
      if (operands.size() != 2) {
        return emitError(
            unknownLoc,
            "expected result type and result <id> for OpFunctionParameter");
      }
      auto argDefinedType = getType(operands[0]);
      if (!argDefinedType || argDefinedType != argType) {
        return emitError(unknownLoc,
                         "mismatch in argument type between function type "
                         "definition ")
               << functionType << " and argument type definition "
               << argDefinedType << " at argument " << i;
      }
      if (getValue(operands[1])) {
        return emitError(unknownLoc, "duplicate definition of result <id> '")
               << operands[1];
      }
      auto argValue = funcOp.getArgument(i);
      valueMap[operands[1]] = argValue;
    }
  }

  // RAII guard to reset the insertion point to the module's region after
  // deserializing the body of this function.
  OpBuilder::InsertionGuard moduleInsertionGuard(opBuilder);

  spirv::Opcode opcode = spirv::Opcode::OpNop;
  ArrayRef<uint32_t> instOperands;

  // Special handling for the entry block. We need to make sure it starts with
  // an OpLabel instruction. The entry block takes the same parameters as the
  // function. All other blocks do not take any parameter. We have already
  // created the entry block, here we need to register it to the correct label
  // <id>.
  if (failed(sliceInstruction(opcode, instOperands,
                              spirv::Opcode::OpFunctionEnd))) {
    return failure();
  }
  if (opcode == spirv::Opcode::OpFunctionEnd) {
    LLVM_DEBUG(llvm::dbgs() << "[fn] completed function " << fnName << " (type="
                            << fnType << ", id=" << operands[1] << ")\n");
    return processFunctionEnd(instOperands);
  }
  if (opcode != spirv::Opcode::OpLabel) {
    return emitError(unknownLoc, "a basic block must start with OpLabel");
  }
  if (instOperands.size() != 1) {
    return emitError(unknownLoc, "OpLabel should only have result <id>");
  }
  blockMap[instOperands[0]] = entryBlock;
  if (failed(processLabel(instOperands))) {
    return failure();
  }

  // Then process all the other instructions in the function until we hit
  // OpFunctionEnd.
  while (succeeded(sliceInstruction(opcode, instOperands,
                                    spirv::Opcode::OpFunctionEnd)) &&
         opcode != spirv::Opcode::OpFunctionEnd) {
    if (failed(processInstruction(opcode, instOperands))) {
      return failure();
    }
  }
  if (opcode != spirv::Opcode::OpFunctionEnd) {
    return failure();
  }

  LLVM_DEBUG(llvm::dbgs() << "[fn] completed function " << fnName << " (type="
                          << fnType << ", id=" << operands[1] << ")\n");
  return processFunctionEnd(instOperands);
}

LogicalResult Deserializer::processFunctionEnd(ArrayRef<uint32_t> operands) {
  // Process OpFunctionEnd.
  if (!operands.empty()) {
    return emitError(unknownLoc, "unexpected operands for OpFunctionEnd");
  }

  // Put all structured control flow in spv.selection/spv.loop ops.
  if (failed(structurizeControlFlow())) {
    return failure();
  }

  curBlock = nullptr;
  curFunction = llvm::None;

  return success();
}

Optional<std::pair<Attribute, Type>> Deserializer::getConstant(uint32_t id) {
  auto constIt = constantMap.find(id);
  if (constIt == constantMap.end())
    return llvm::None;
  return constIt->getSecond();
}

std::string Deserializer::getFunctionSymbol(uint32_t id) {
  auto funcName = nameMap.lookup(id).str();
  if (funcName.empty()) {
    funcName = "spirv_fn_" + std::to_string(id);
  }
  return funcName;
}

std::string Deserializer::getSpecConstantSymbol(uint32_t id) {
  auto constName = nameMap.lookup(id).str();
  if (constName.empty()) {
    constName = "spirv_spec_const_" + std::to_string(id);
  }
  return constName;
}

LogicalResult Deserializer::processGlobalVariable(ArrayRef<uint32_t> operands) {
  unsigned wordIndex = 0;
  if (operands.size() < 3) {
    return emitError(
        unknownLoc,
        "OpVariable needs at least 3 operands, type, <id> and storage class");
  }

  // Result Type.
  auto type = getType(operands[wordIndex]);
  if (!type) {
    return emitError(unknownLoc, "unknown result type <id> : ")
           << operands[wordIndex];
  }
  auto ptrType = type.dyn_cast<spirv::PointerType>();
  if (!ptrType) {
    return emitError(unknownLoc,
                     "expected a result type <id> to be a spv.ptr, found : ")
           << type;
  }
  wordIndex++;

  // Result <id>.
  auto variableID = operands[wordIndex];
  auto variableName = nameMap.lookup(variableID).str();
  if (variableName.empty()) {
    variableName = "spirv_var_" + std::to_string(variableID);
  }
  wordIndex++;

  // Storage class.
  auto storageClass = static_cast<spirv::StorageClass>(operands[wordIndex]);
  if (ptrType.getStorageClass() != storageClass) {
    return emitError(unknownLoc, "mismatch in storage class of pointer type ")
           << type << " and that specified in OpVariable instruction  : "
           << stringifyStorageClass(storageClass);
  }
  wordIndex++;

  // Initializer.
  SymbolRefAttr initializer = nullptr;
  if (wordIndex < operands.size()) {
    auto initializerOp = getGlobalVariable(operands[wordIndex]);
    if (!initializerOp) {
      return emitError(unknownLoc, "unknown <id> ")
             << operands[wordIndex] << "used as initializer";
    }
    wordIndex++;
    initializer = opBuilder.getSymbolRefAttr(initializerOp.getOperation());
  }
  if (wordIndex != operands.size()) {
    return emitError(unknownLoc,
                     "found more operands than expected when deserializing "
                     "OpVariable instruction, only ")
           << wordIndex << " of " << operands.size() << " processed";
  }
  auto varOp = opBuilder.create<spirv::GlobalVariableOp>(
      unknownLoc, opBuilder.getTypeAttr(type),
      opBuilder.getStringAttr(variableName), initializer);

  // Decorations.
  if (decorations.count(variableID)) {
    for (auto attr : decorations[variableID].getAttrs()) {
      varOp.setAttr(attr.first, attr.second);
    }
  }
  globalVariableMap[variableID] = varOp;
  return success();
}

LogicalResult Deserializer::processName(ArrayRef<uint32_t> operands) {
  if (operands.size() < 2) {
    return emitError(unknownLoc, "OpName needs at least 2 operands");
  }
  if (!nameMap.lookup(operands[0]).empty()) {
    return emitError(unknownLoc, "duplicate name found for result <id> ")
           << operands[0];
  }
  unsigned wordIndex = 1;
  StringRef name = decodeStringLiteral(operands, wordIndex);
  if (wordIndex != operands.size()) {
    return emitError(unknownLoc,
                     "unexpected trailing words in OpName instruction");
  }
  nameMap[operands[0]] = name;
  return success();
}

//===----------------------------------------------------------------------===//
// Type
//===----------------------------------------------------------------------===//

LogicalResult Deserializer::processType(spirv::Opcode opcode,
                                        ArrayRef<uint32_t> operands) {
  if (operands.empty()) {
    return emitError(unknownLoc, "type instruction with opcode ")
           << spirv::stringifyOpcode(opcode) << " needs at least one <id>";
  }

  /// TODO: Types might be forward declared in some instructions and need to be
  /// handled appropriately.
  if (typeMap.count(operands[0])) {
    return emitError(unknownLoc, "duplicate definition for result <id> ")
           << operands[0];
  }

  switch (opcode) {
  case spirv::Opcode::OpTypeVoid:
    if (operands.size() != 1) {
      return emitError(unknownLoc, "OpTypeVoid must have no parameters");
    }
    typeMap[operands[0]] = opBuilder.getNoneType();
    break;
  case spirv::Opcode::OpTypeBool:
    if (operands.size() != 1) {
      return emitError(unknownLoc, "OpTypeBool must have no parameters");
    }
    typeMap[operands[0]] = opBuilder.getI1Type();
    break;
  case spirv::Opcode::OpTypeInt:
    if (operands.size() != 3) {
      return emitError(
          unknownLoc, "OpTypeInt must have bitwidth and signedness parameters");
    }
    if (operands[2] == 0) {
      return emitError(unknownLoc, "unhandled unsigned OpTypeInt");
    }
    typeMap[operands[0]] = opBuilder.getIntegerType(operands[1]);
    break;
  case spirv::Opcode::OpTypeFloat: {
    if (operands.size() != 2) {
      return emitError(unknownLoc, "OpTypeFloat must have bitwidth parameter");
    }
    Type floatTy;
    switch (operands[1]) {
    case 16:
      floatTy = opBuilder.getF16Type();
      break;
    case 32:
      floatTy = opBuilder.getF32Type();
      break;
    case 64:
      floatTy = opBuilder.getF64Type();
      break;
    default:
      return emitError(unknownLoc, "unsupported OpTypeFloat bitwdith: ")
             << operands[1];
    }
    typeMap[operands[0]] = floatTy;
  } break;
  case spirv::Opcode::OpTypeVector: {
    if (operands.size() != 3) {
      return emitError(
          unknownLoc,
          "OpTypeVector must have element type and count parameters");
    }
    Type elementTy = getType(operands[1]);
    if (!elementTy) {
      return emitError(unknownLoc, "OpTypeVector references undefined <id> ")
             << operands[1];
    }
    typeMap[operands[0]] = opBuilder.getVectorType({operands[2]}, elementTy);
  } break;
  case spirv::Opcode::OpTypePointer: {
    if (operands.size() != 3) {
      return emitError(unknownLoc, "OpTypePointer must have two parameters");
    }
    auto pointeeType = getType(operands[2]);
    if (!pointeeType) {
      return emitError(unknownLoc, "unknown OpTypePointer pointee type <id> ")
             << operands[2];
    }
    auto storageClass = static_cast<spirv::StorageClass>(operands[1]);
    typeMap[operands[0]] = spirv::PointerType::get(pointeeType, storageClass);
  } break;
  case spirv::Opcode::OpTypeArray:
    return processArrayType(operands);
  case spirv::Opcode::OpTypeFunction:
    return processFunctionType(operands);
  case spirv::Opcode::OpTypeRuntimeArray:
    return processRuntimeArrayType(operands);
  case spirv::Opcode::OpTypeStruct:
    return processStructType(operands);
  default:
    return emitError(unknownLoc, "unhandled type instruction");
  }
  return success();
}

LogicalResult Deserializer::processArrayType(ArrayRef<uint32_t> operands) {
  if (operands.size() != 3) {
    return emitError(unknownLoc,
                     "OpTypeArray must have element type and count parameters");
  }

  Type elementTy = getType(operands[1]);
  if (!elementTy) {
    return emitError(unknownLoc, "OpTypeArray references undefined <id> ")
           << operands[1];
  }

  unsigned count = 0;
  // TODO(antiagainst): The count can also come frome a specialization constant.
  auto countInfo = getConstant(operands[2]);
  if (!countInfo) {
    return emitError(unknownLoc, "OpTypeArray count <id> ")
           << operands[2] << "can only come from normal constant right now";
  }

  if (auto intVal = countInfo->first.dyn_cast<IntegerAttr>()) {
    count = intVal.getInt();
  } else {
    return emitError(unknownLoc, "OpTypeArray count must come from a "
                                 "scalar integer constant instruction");
  }

  typeMap[operands[0]] = spirv::ArrayType::get(
      elementTy, count, typeDecorations.lookup(operands[0]));
  return success();
}

LogicalResult Deserializer::processFunctionType(ArrayRef<uint32_t> operands) {
  assert(!operands.empty() && "No operands for processing function type");
  if (operands.size() == 1) {
    return emitError(unknownLoc, "missing return type for OpTypeFunction");
  }
  auto returnType = getType(operands[1]);
  if (!returnType) {
    return emitError(unknownLoc, "unknown return type in OpTypeFunction");
  }
  SmallVector<Type, 1> argTypes;
  for (size_t i = 2, e = operands.size(); i < e; ++i) {
    auto ty = getType(operands[i]);
    if (!ty) {
      return emitError(unknownLoc, "unknown argument type in OpTypeFunction");
    }
    argTypes.push_back(ty);
  }
  ArrayRef<Type> returnTypes;
  if (!isVoidType(returnType)) {
    returnTypes = llvm::makeArrayRef(returnType);
  }
  typeMap[operands[0]] = FunctionType::get(argTypes, returnTypes, context);
  return success();
}

LogicalResult
Deserializer::processRuntimeArrayType(ArrayRef<uint32_t> operands) {
  if (operands.size() != 2) {
    return emitError(unknownLoc, "OpTypeRuntimeArray must have two operands");
  }
  Type memberType = getType(operands[1]);
  if (!memberType) {
    return emitError(unknownLoc,
                     "OpTypeRuntimeArray references undefined <id> ")
           << operands[1];
  }
  typeMap[operands[0]] = spirv::RuntimeArrayType::get(memberType);
  return success();
}

LogicalResult Deserializer::processStructType(ArrayRef<uint32_t> operands) {
  // TODO(ravishankarm) : Regarding to the spec spv.struct must support zero
  // amount of members.
  if (operands.size() < 2) {
    return emitError(unknownLoc, "OpTypeStruct must have at least 2 operand");
  }

  SmallVector<Type, 0> memberTypes;
  for (auto op : llvm::drop_begin(operands, 1)) {
    Type memberType = getType(op);
    if (!memberType) {
      return emitError(unknownLoc, "OpTypeStruct references undefined <id> ")
             << op;
    }
    memberTypes.push_back(memberType);
  }

  SmallVector<spirv::StructType::LayoutInfo, 0> layoutInfo;
  SmallVector<spirv::StructType::MemberDecorationInfo, 0> memberDecorationsInfo;
  if (memberDecorationMap.count(operands[0])) {
    auto &allMemberDecorations = memberDecorationMap[operands[0]];
    for (auto memberIndex : llvm::seq<uint32_t>(0, memberTypes.size())) {
      if (allMemberDecorations.count(memberIndex)) {
        for (auto &memberDecoration : allMemberDecorations[memberIndex]) {
          // Check for offset.
          if (memberDecoration.first == spirv::Decoration::Offset) {
            // If layoutInfo is empty, resize to the number of members;
            if (layoutInfo.empty()) {
              layoutInfo.resize(memberTypes.size());
            }
            layoutInfo[memberIndex] = memberDecoration.second[0];
          } else {
            if (!memberDecoration.second.empty()) {
              return emitError(unknownLoc,
                               "unhandled OpMemberDecoration with decoration ")
                     << stringifyDecoration(memberDecoration.first)
                     << " which has additional operands";
            }
            memberDecorationsInfo.emplace_back(memberIndex,
                                               memberDecoration.first);
          }
        }
      }
    }
  }
  typeMap[operands[0]] =
      spirv::StructType::get(memberTypes, layoutInfo, memberDecorationsInfo);
  return success();
}

//===----------------------------------------------------------------------===//
// Constant
//===----------------------------------------------------------------------===//

LogicalResult Deserializer::processConstant(ArrayRef<uint32_t> operands,
                                            bool isSpec) {
  StringRef opname = isSpec ? "OpSpecConstant" : "OpConstant";

  if (operands.size() < 2) {
    return emitError(unknownLoc)
           << opname << " must have type <id> and result <id>";
  }
  if (operands.size() < 3) {
    return emitError(unknownLoc)
           << opname << " must have at least 1 more parameter";
  }

  Type resultType = getType(operands[0]);
  if (!resultType) {
    return emitError(unknownLoc, "undefined result type from <id> ")
           << operands[0];
  }

  auto checkOperandSizeForBitwidth = [&](unsigned bitwidth) -> LogicalResult {
    if (bitwidth == 64) {
      if (operands.size() == 4) {
        return success();
      }
      return emitError(unknownLoc)
             << opname << " should have 2 parameters for 64-bit values";
    }
    if (bitwidth <= 32) {
      if (operands.size() == 3) {
        return success();
      }

      return emitError(unknownLoc)
             << opname
             << " should have 1 parameter for values with no more than 32 bits";
    }
    return emitError(unknownLoc, "unsupported OpConstant bitwidth: ")
           << bitwidth;
  };

  auto resultID = operands[1];

  if (auto intType = resultType.dyn_cast<IntegerType>()) {
    auto bitwidth = intType.getWidth();
    if (failed(checkOperandSizeForBitwidth(bitwidth))) {
      return failure();
    }

    APInt value;
    if (bitwidth == 64) {
      // 64-bit integers are represented with two SPIR-V words. According to
      // SPIR-V spec: "When the type’s bit width is larger than one word, the
      // literal’s low-order words appear first."
      struct DoubleWord {
        uint32_t word1;
        uint32_t word2;
      } words = {operands[2], operands[3]};
      value = APInt(64, llvm::bit_cast<uint64_t>(words), /*isSigned=*/true);
    } else if (bitwidth <= 32) {
      value = APInt(bitwidth, operands[2], /*isSigned=*/true);
    }

    auto attr = opBuilder.getIntegerAttr(intType, value);

    if (isSpec) {
      auto symName = opBuilder.getStringAttr(getSpecConstantSymbol(resultID));
      auto op =
          opBuilder.create<spirv::SpecConstantOp>(unknownLoc, symName, attr);
      specConstMap[resultID] = op;
    } else {
      // For normal constants, we just record the attribute (and its type) for
      // later materialization at use sites.
      constantMap.try_emplace(resultID, attr, intType);
    }

    return success();
  }

  if (auto floatType = resultType.dyn_cast<FloatType>()) {
    auto bitwidth = floatType.getWidth();
    if (failed(checkOperandSizeForBitwidth(bitwidth))) {
      return failure();
    }

    APFloat value(0.f);
    if (floatType.isF64()) {
      // Double values are represented with two SPIR-V words. According to
      // SPIR-V spec: "When the type’s bit width is larger than one word, the
      // literal’s low-order words appear first."
      struct DoubleWord {
        uint32_t word1;
        uint32_t word2;
      } words = {operands[2], operands[3]};
      value = APFloat(llvm::bit_cast<double>(words));
    } else if (floatType.isF32()) {
      value = APFloat(llvm::bit_cast<float>(operands[2]));
    } else if (floatType.isF16()) {
      APInt data(16, operands[2]);
      value = APFloat(APFloat::IEEEhalf(), data);
    }

    auto attr = opBuilder.getFloatAttr(floatType, value);
    if (isSpec) {
      auto symName = opBuilder.getStringAttr(getSpecConstantSymbol(resultID));
      auto op =
          opBuilder.create<spirv::SpecConstantOp>(unknownLoc, symName, attr);
      specConstMap[resultID] = op;
    } else {
      // For normal constants, we just record the attribute (and its type) for
      // later materialization at use sites.
      constantMap.try_emplace(resultID, attr, floatType);
    }

    return success();
  }

  return emitError(unknownLoc, "OpConstant can only generate values of "
                               "scalar integer or floating-point type");
}

LogicalResult Deserializer::processConstantBool(bool isTrue,
                                                ArrayRef<uint32_t> operands,
                                                bool isSpec) {
  if (operands.size() != 2) {
    return emitError(unknownLoc, "Op")
           << (isSpec ? "Spec" : "") << "Constant"
           << (isTrue ? "True" : "False")
           << " must have type <id> and result <id>";
  }

  auto attr = opBuilder.getBoolAttr(isTrue);
  auto resultID = operands[1];
  if (isSpec) {
    auto symName = opBuilder.getStringAttr(getSpecConstantSymbol(resultID));
    auto op =
        opBuilder.create<spirv::SpecConstantOp>(unknownLoc, symName, attr);
    specConstMap[resultID] = op;
  } else {
    // For normal constants, we just record the attribute (and its type) for
    // later materialization at use sites.
    constantMap.try_emplace(resultID, attr, opBuilder.getI1Type());
  }

  return success();
}

LogicalResult
Deserializer::processConstantComposite(ArrayRef<uint32_t> operands) {
  if (operands.size() < 2) {
    return emitError(unknownLoc,
                     "OpConstantComposite must have type <id> and result <id>");
  }
  if (operands.size() < 3) {
    return emitError(unknownLoc,
                     "OpConstantComposite must have at least 1 parameter");
  }

  Type resultType = getType(operands[0]);
  if (!resultType) {
    return emitError(unknownLoc, "undefined result type from <id> ")
           << operands[0];
  }

  SmallVector<Attribute, 4> elements;
  elements.reserve(operands.size() - 2);
  for (unsigned i = 2, e = operands.size(); i < e; ++i) {
    auto elementInfo = getConstant(operands[i]);
    if (!elementInfo) {
      return emitError(unknownLoc, "OpConstantComposite component <id> ")
             << operands[i] << " must come from a normal constant";
    }
    elements.push_back(elementInfo->first);
  }

  auto resultID = operands[1];
  if (auto vectorType = resultType.dyn_cast<VectorType>()) {
    auto attr = opBuilder.getDenseElementsAttr(vectorType, elements);
    // For normal constants, we just record the attribute (and its type) for
    // later materialization at use sites.
    constantMap.try_emplace(resultID, attr, resultType);
  } else if (auto arrayType = resultType.dyn_cast<spirv::ArrayType>()) {
    auto attr = opBuilder.getArrayAttr(elements);
    constantMap.try_emplace(resultID, attr, resultType);
  } else {
    return emitError(unknownLoc, "unsupported OpConstantComposite type: ")
           << resultType;
  }

  return success();
}

LogicalResult Deserializer::processConstantNull(ArrayRef<uint32_t> operands) {
  if (operands.size() != 2) {
    return emitError(unknownLoc,
                     "OpConstantNull must have type <id> and result <id>");
  }

  Type resultType = getType(operands[0]);
  if (!resultType) {
    return emitError(unknownLoc, "undefined result type from <id> ")
           << operands[0];
  }

  auto resultID = operands[1];
  if (resultType.isa<IntegerType>() || resultType.isa<FloatType>() ||
      resultType.isa<VectorType>()) {
    auto attr = opBuilder.getZeroAttr(resultType);
    // For normal constants, we just record the attribute (and its type) for
    // later materialization at use sites.
    constantMap.try_emplace(resultID, attr, resultType);
    return success();
  }

    return emitError(unknownLoc, "unsupported OpConstantNull type: ")
           << resultType;
}

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

Block *Deserializer::getOrCreateBlock(uint32_t id) {
  if (auto *block = getBlock(id)) {
    LLVM_DEBUG(llvm::dbgs() << "[block] got exiting block for id=" << id
                            << " @ " << block << "\n");
    return block;
  }

  // We don't know where this block will be placed finally (in a spv.selection
  // or spv.loop or function). Create it into the function for now and sort
  // out the proper place later.
  auto *block = curFunction->addBlock();
  LLVM_DEBUG(llvm::dbgs() << "[block] created block for id=" << id << " @ "
                          << block << "\n");
  return blockMap[id] = block;
}

LogicalResult Deserializer::processBranch(ArrayRef<uint32_t> operands) {
  if (!curBlock) {
    return emitError(unknownLoc, "OpBranch must appear inside a block");
  }

  if (operands.size() != 1) {
    return emitError(unknownLoc, "OpBranch must take exactly one target label");
  }

  auto *target = getOrCreateBlock(operands[0]);
  opBuilder.create<spirv::BranchOp>(unknownLoc, target);

  return success();
}

LogicalResult
Deserializer::processBranchConditional(ArrayRef<uint32_t> operands) {
  if (!curBlock) {
    return emitError(unknownLoc,
                     "OpBranchConditional must appear inside a block");
  }

  if (operands.size() != 3 && operands.size() != 5) {
    return emitError(unknownLoc,
                     "OpBranchConditional must have condition, true label, "
                     "false label, and optionally two branch weights");
  }

  auto *condition = getValue(operands[0]);
  auto *trueBlock = getOrCreateBlock(operands[1]);
  auto *falseBlock = getOrCreateBlock(operands[2]);

  Optional<std::pair<uint32_t, uint32_t>> weights;
  if (operands.size() == 5) {
    weights = std::make_pair(operands[3], operands[4]);
  }

  opBuilder.create<spirv::BranchConditionalOp>(unknownLoc, condition, trueBlock,
                                               falseBlock, weights);

  return success();
}

LogicalResult Deserializer::processLabel(ArrayRef<uint32_t> operands) {
  if (!curFunction) {
    return emitError(unknownLoc, "OpLabel must appear inside a function");
  }

  if (operands.size() != 1) {
    return emitError(unknownLoc, "OpLabel should only have result <id>");
  }

  auto labelID = operands[0];
  // We may have forward declared this block.
  auto *block = getOrCreateBlock(labelID);
  LLVM_DEBUG(llvm::dbgs() << "[block] populating block @ " << block << "\n");
  // If we have seen this block, make sure it was just a forward declaration.
  assert(block->empty() && "re-deserialize the same block!");

  opBuilder.setInsertionPointToStart(block);
  blockMap[labelID] = curBlock = block;

  return success();
}

LogicalResult Deserializer::processLoopMerge(ArrayRef<uint32_t> operands) {
  if (!curBlock) {
    return emitError(unknownLoc, "OpLoopMerge must appear in a block");
  }

  if (operands.size() < 3) {
    return emitError(unknownLoc, "OpLoopMerge must specify merge target, "
                                 "continue target and loop control");
  }

  if (static_cast<uint32_t>(spirv::LoopControl::None) != operands[2]) {
    return emitError(unknownLoc, "unimplmented OpLoopMerge loop control: ")
           << operands[2];
  }

  auto *mergeBlock = getOrCreateBlock(operands[0]);
  auto *continueBlock = getOrCreateBlock(operands[1]);

  if (!blockMergeInfo.try_emplace(curBlock, mergeBlock, continueBlock).second) {
    return emitError(
        unknownLoc,
        "a block cannot have more than one OpLoopMerge instruction");
  }

  return success();
}

namespace {
/// A class for putting all blocks in a structured loop in a spv.loop op.
class LoopStructurizer {
public:
  /// Structurizes the loop at the given `headerBlock`.
  ///
  /// This method will create an spv.loop op in the `mergeBlock` and move all
  /// blocks in the structured loop into the spv.loop's region. All branches to
  /// the `headerBlock` will be redirected to the `mergeBlock`.
  static LogicalResult structurize(Location loc, Block *headerBlock,
                                   Block *mergeBlock, Block *continueBlock) {
    return LoopStructurizer(loc, headerBlock, mergeBlock, continueBlock)
        .structurizeImpl();
  }

private:
  LoopStructurizer(Location loc, Block *header, Block *merge, Block *cont)
      : location(loc), headerBlock(header), mergeBlock(merge),
        continueBlock(cont) {}

  /// Creates a new spv.loop op at the beginning of the `mergeBlock`.
  spirv::LoopOp createLoopOp();

  /// Collects all blocks reachable from `headerBlock` except `mergeBlock` and
  /// `continueBlock` into `constructBlocks`.
  void collectBlocksInConstruct();

  LogicalResult structurizeImpl();

  Location location;

  Block *headerBlock;
  Block *mergeBlock;
  Block *continueBlock;

  llvm::SetVector<Block *> constructBlocks;
};
} // namespace

spirv::LoopOp LoopStructurizer::createLoopOp() {
  // Create a builder and set the insertion point to the beginning of the
  // merge block so that the newly created LoopOp will be inserted there.
  OpBuilder builder(&mergeBlock->front());

  auto control = builder.getI32IntegerAttr(
      static_cast<uint32_t>(spirv::LoopControl::None));
  auto loopOp = builder.create<spirv::LoopOp>(location, control);
  loopOp.addEntryAndMergeBlock();

  return loopOp;
}

void LoopStructurizer::collectBlocksInConstruct() {
  assert(constructBlocks.empty() && "expected empty constructBlocks");

  // Put the header block in the work list first.
  constructBlocks.insert(headerBlock);

  // For each item in the work list, add its successors under conditions.
  for (unsigned i = 0; i < constructBlocks.size(); ++i) {
    for (auto *successor : constructBlocks[i]->getSuccessors())
      if (successor != mergeBlock && successor != continueBlock &&
          constructBlocks.count(successor) == 0) {
        constructBlocks.insert(successor);
      }
  }
}

LogicalResult LoopStructurizer::structurizeImpl() {
  auto loopOp = createLoopOp();
  if (!loopOp)
    return failure();

  BlockAndValueMapping mapper;
  // All references to the old merge block should be directed to the loop
  // merge block in the LoopOp's region.
  mapper.map(mergeBlock, &loopOp.body().back());

  collectBlocksInConstruct();
  // Add the loop continue block at the last so it's the second to last block
  // in LoopOp's region.
  constructBlocks.insert(continueBlock);

  // We've identified all blocks belonging to the loop's region. Now need to
  // "move" them into the loop. Instead of really moving the blocks, in the
  // following we copy them and remap all values and branches. This is because:
  // * Inserting a block into a region requires the block not in any region
  //   before. But loops can nest so we can create loop ops in a nested manner,
  //   which means some blocks may already be in a loop region when to be moved
  //   again.
  // * It's much trickier to fix up the branches into and out of the loop's
  //   region: we need to treat not-moved blocks and moved blocks differently:
  //   Not-moved blocks jumping to the loop header block need to jump to the
  //   merge point containing the new loop op but not the loop continue block's
  //   back edge. Moved blocks jumping out of the loop need to jump to the
  //   merge block inside the loop region but not other not-moved blocks.
  //   We cannot use replaceAllUsesWith clearly and it's harder to follow the
  //   logic.

  // Create a corresponding block in the LoopOp's region for each block in
  // this loop construct.
  OpBuilder loopBuilder(loopOp.body());
  for (auto *block : constructBlocks) {
    assert(block->getNumArguments() == 0 &&
           "block in loop construct should not have arguments");

    // Create an block and insert it before the loop merge block in the
    // LoopOp's region.
    auto *newBlock = loopBuilder.createBlock(&loopOp.body().back());
    mapper.map(block, newBlock);

    for (auto &op : *block)
      newBlock->push_back(op.clone(mapper));
  }

  // Go through all ops and remap the operands.
  auto remapOperands = [&](Operation *op) {
    for (auto &operand : op->getOpOperands())
      if (auto *mappedOp = mapper.lookupOrNull(operand.get()))
        operand.set(mappedOp);
    for (auto &succOp : op->getBlockOperands())
      if (auto *mappedOp = mapper.lookupOrNull(succOp.get()))
        succOp.set(mappedOp);
  };
  for (auto &block : loopOp.body()) {
    block.walk(remapOperands);
  }

  // We have created the LoopOp and "moved" all blocks belonging to the loop
  // construct into its region. Next we need to fix the connections between
  // this new LoopOp with existing blocks.

  // All existing incoming branches should go to the merge block, where the
  // LoopOp resides right now.
  headerBlock->replaceAllUsesWith(mergeBlock);

  // The loop entry block should have a unconditional branch jumping to the
  // loop header block.
  loopBuilder.setInsertionPointToEnd(&loopOp.body().front());
  loopBuilder.create<spirv::BranchOp>(location,
                                      mapper.lookupOrNull(headerBlock));

  // All the blocks cloned into the LoopOp's region can now be deleted.
  for (auto *block : constructBlocks) {
    block->clear();
    block->erase();
  }

  return success();
}

LogicalResult Deserializer::structurizeControlFlow() {
  LLVM_DEBUG(llvm::dbgs() << "[cf] structurizing control flow\n");

  while (!blockMergeInfo.empty()) {
    auto *headerBlock = blockMergeInfo.begin()->first;
    const auto &mergeInfo = blockMergeInfo.begin()->second;

    auto *mergeBlock = mergeInfo.mergeBlock;
    auto *continueBlock = mergeInfo.continueBlock;
    LLVM_DEBUG(llvm::dbgs() << "[cf] header block @ " << headerBlock << "\n");
    assert(mergeBlock && "merge block cannot be nullptr");
    LLVM_DEBUG(llvm::dbgs() << "[cf] merge block @ " << mergeBlock << "\n");
    if (!continueBlock) {
      return emitError(unknownLoc, "structurizing selection unimplemented");
    }
    LLVM_DEBUG(llvm::dbgs()
               << "[cf] continue block @ " << continueBlock << "\n");

    if (failed(LoopStructurizer::structurize(unknownLoc, headerBlock,
                                             mergeBlock, continueBlock))) {
      return failure();
    }

    blockMergeInfo.erase(headerBlock);
  }

  LLVM_DEBUG(llvm::dbgs() << "[cf] completed structurizing control flow\n");
  return success();
}

//===----------------------------------------------------------------------===//
// Instruction
//===----------------------------------------------------------------------===//

Value *Deserializer::getValue(uint32_t id) {
  if (auto constInfo = getConstant(id)) {
    // Materialize a `spv.constant` op at every use site.
    return opBuilder.create<spirv::ConstantOp>(unknownLoc, constInfo->second,
                                               constInfo->first);
  }
  if (auto varOp = getGlobalVariable(id)) {
    auto addressOfOp = opBuilder.create<spirv::AddressOfOp>(
        unknownLoc, varOp.type(),
        opBuilder.getSymbolRefAttr(varOp.getOperation()));
    return addressOfOp.pointer();
  }
  if (auto constOp = getSpecConstant(id)) {
    auto referenceOfOp = opBuilder.create<spirv::ReferenceOfOp>(
        unknownLoc, constOp.default_value().getType(),
        opBuilder.getSymbolRefAttr(constOp.getOperation()));
    return referenceOfOp.reference();
  }
  return valueMap.lookup(id);
}

LogicalResult
Deserializer::sliceInstruction(spirv::Opcode &opcode,
                               ArrayRef<uint32_t> &operands,
                               Optional<spirv::Opcode> expectedOpcode) {
  auto binarySize = binary.size();
  if (curOffset >= binarySize) {
    return emitError(unknownLoc, "expected ")
           << (expectedOpcode ? spirv::stringifyOpcode(*expectedOpcode)
                              : "more")
           << " instruction";
  }

  // For each instruction, get its word count from the first word to slice it
  // from the stream properly, and then dispatch to the instruction handler.

  uint32_t wordCount = binary[curOffset] >> 16;

  if (wordCount == 0)
    return emitError(unknownLoc, "word count cannot be zero");

  uint32_t nextOffset = curOffset + wordCount;
  if (nextOffset > binarySize)
    return emitError(unknownLoc, "insufficient words for the last instruction");

  opcode = extractOpcode(binary[curOffset]);
  operands = binary.slice(curOffset + 1, wordCount - 1);
  curOffset = nextOffset;
  return success();
}

Optional<spirv::Opcode> Deserializer::peekOpcode() {
  if (curOffset >= binary.size())
    return llvm::None;
  return extractOpcode(binary[curOffset]);
}

LogicalResult Deserializer::processInstruction(spirv::Opcode opcode,
                                               ArrayRef<uint32_t> operands,
                                               bool deferInstructions) {
  LLVM_DEBUG(llvm::dbgs() << "[inst] processing instruction "
                          << spirv::stringifyOpcode(opcode) << "\n");

  // First dispatch all the instructions whose opcode does not correspond to
  // those that have a direct mirror in the SPIR-V dialect
  switch (opcode) {
  case spirv::Opcode::OpCapability:
    return processCapability(operands);
  case spirv::Opcode::OpExtension:
    return processExtension(operands);
  case spirv::Opcode::OpExtInst:
    return processExtInst(operands);
  case spirv::Opcode::OpExtInstImport:
    return processExtInstImport(operands);
  case spirv::Opcode::OpMemoryModel:
    return processMemoryModel(operands);
  case spirv::Opcode::OpEntryPoint:
  case spirv::Opcode::OpExecutionMode:
    if (deferInstructions) {
      deferedInstructions.emplace_back(opcode, operands);
      return success();
    }
    break;
  case spirv::Opcode::OpVariable:
    if (isa<spirv::ModuleOp>(opBuilder.getBlock()->getParentOp())) {
      return processGlobalVariable(operands);
    }
    break;
  case spirv::Opcode::OpName:
    return processName(operands);
  case spirv::Opcode::OpTypeVoid:
  case spirv::Opcode::OpTypeBool:
  case spirv::Opcode::OpTypeInt:
  case spirv::Opcode::OpTypeFloat:
  case spirv::Opcode::OpTypeVector:
  case spirv::Opcode::OpTypeArray:
  case spirv::Opcode::OpTypeFunction:
  case spirv::Opcode::OpTypeRuntimeArray:
  case spirv::Opcode::OpTypeStruct:
  case spirv::Opcode::OpTypePointer:
    return processType(opcode, operands);
  case spirv::Opcode::OpConstant:
    return processConstant(operands, /*isSpec=*/false);
  case spirv::Opcode::OpSpecConstant:
    return processConstant(operands, /*isSpec=*/true);
  case spirv::Opcode::OpConstantComposite:
    return processConstantComposite(operands);
  case spirv::Opcode::OpConstantTrue:
    return processConstantBool(/*isTrue=*/true, operands, /*isSpec=*/false);
  case spirv::Opcode::OpSpecConstantTrue:
    return processConstantBool(/*isTrue=*/true, operands, /*isSpec=*/true);
  case spirv::Opcode::OpConstantFalse:
    return processConstantBool(/*isTrue=*/false, operands, /*isSpec=*/false);
  case spirv::Opcode::OpSpecConstantFalse:
    return processConstantBool(/*isTrue=*/false, operands, /*isSpec=*/true);
  case spirv::Opcode::OpConstantNull:
    return processConstantNull(operands);
  case spirv::Opcode::OpDecorate:
    return processDecoration(operands);
  case spirv::Opcode::OpMemberDecorate:
    return processMemberDecoration(operands);
  case spirv::Opcode::OpFunction:
    return processFunction(operands);
  case spirv::Opcode::OpLabel:
    return processLabel(operands);
  case spirv::Opcode::OpBranch:
    return processBranch(operands);
  case spirv::Opcode::OpBranchConditional:
    return processBranchConditional(operands);
  case spirv::Opcode::OpLoopMerge:
    return processLoopMerge(operands);
  default:
    break;
  }
  return dispatchToAutogenDeserialization(opcode, operands);
}

LogicalResult Deserializer::processExtInst(ArrayRef<uint32_t> operands) {
  if (operands.size() < 4) {
    return emitError(unknownLoc,
                     "OpExtInst must have at least 4 operands, result type "
                     "<id>, result <id>, set <id> and instruction opcode");
  }
  if (!extendedInstSets.count(operands[2])) {
    return emitError(unknownLoc, "undefined set <id> in OpExtInst");
  }
  SmallVector<uint32_t, 4> slicedOperands;
  slicedOperands.append(operands.begin(), std::next(operands.begin(), 2));
  slicedOperands.append(std::next(operands.begin(), 4), operands.end());
  return dispatchToExtensionSetAutogenDeserialization(
      extendedInstSets[operands[2]], operands[3], slicedOperands);
}

namespace {

template <>
LogicalResult
Deserializer::processOp<spirv::EntryPointOp>(ArrayRef<uint32_t> words) {
  unsigned wordIndex = 0;
  if (wordIndex >= words.size()) {
    return emitError(unknownLoc,
                     "missing Execution Model specification in OpEntryPoint");
  }
  auto exec_model = opBuilder.getI32IntegerAttr(words[wordIndex++]);
  if (wordIndex >= words.size()) {
    return emitError(unknownLoc, "missing <id> in OpEntryPoint");
  }
  // Get the function <id>
  auto fnID = words[wordIndex++];
  // Get the function name
  auto fnName = decodeStringLiteral(words, wordIndex);
  // Verify that the function <id> matches the fnName
  auto parsedFunc = getFunction(fnID);
  if (!parsedFunc) {
    return emitError(unknownLoc, "no function matching <id> ") << fnID;
  }
  if (parsedFunc.getName() != fnName) {
    return emitError(unknownLoc, "function name mismatch between OpEntryPoint "
                                 "and OpFunction with <id> ")
           << fnID << ": " << fnName << " vs. " << parsedFunc.getName();
  }
  SmallVector<Attribute, 4> interface;
  while (wordIndex < words.size()) {
    auto arg = getGlobalVariable(words[wordIndex]);
    if (!arg) {
      return emitError(unknownLoc, "undefined result <id> ")
             << words[wordIndex] << " while decoding OpEntryPoint";
    }
    interface.push_back(opBuilder.getSymbolRefAttr(arg.getOperation()));
    wordIndex++;
  }
  opBuilder.create<spirv::EntryPointOp>(unknownLoc, exec_model,
                                        opBuilder.getSymbolRefAttr(fnName),
                                        opBuilder.getArrayAttr(interface));
  return success();
}

template <>
LogicalResult
Deserializer::processOp<spirv::ExecutionModeOp>(ArrayRef<uint32_t> words) {
  unsigned wordIndex = 0;
  if (wordIndex >= words.size()) {
    return emitError(unknownLoc,
                     "missing function result <id> in OpExecutionMode");
  }
  // Get the function <id> to get the name of the function
  auto fnID = words[wordIndex++];
  auto fn = getFunction(fnID);
  if (!fn) {
    return emitError(unknownLoc, "no function matching <id> ") << fnID;
  }
  // Get the Execution mode
  if (wordIndex >= words.size()) {
    return emitError(unknownLoc, "missing Execution Mode in OpExecutionMode");
  }
  auto execMode = opBuilder.getI32IntegerAttr(words[wordIndex++]);

  // Get the values
  SmallVector<Attribute, 4> attrListElems;
  while (wordIndex < words.size()) {
    attrListElems.push_back(opBuilder.getI32IntegerAttr(words[wordIndex++]));
  }
  auto values = opBuilder.getArrayAttr(attrListElems);
  opBuilder.create<spirv::ExecutionModeOp>(
      unknownLoc, opBuilder.getSymbolRefAttr(fn.getName()), execMode, values);
  return success();
}

template <>
LogicalResult
Deserializer::processOp<spirv::FunctionCallOp>(ArrayRef<uint32_t> operands) {
  if (operands.size() < 3) {
    return emitError(unknownLoc,
                     "OpFunctionCall must have at least 3 operands");
  }

  Type resultType = getType(operands[0]);
  if (!resultType) {
    return emitError(unknownLoc, "undefined result type from <id> ")
           << operands[0];
  }

  auto resultID = operands[1];
  auto functionID = operands[2];

  auto functionName = getFunctionSymbol(functionID);

  llvm::SmallVector<Value *, 4> arguments;
  for (auto operand : llvm::drop_begin(operands, 3)) {
    auto *value = getValue(operand);
    if (!value) {
      return emitError(unknownLoc, "unknown <id> ")
             << operand << " used by OpFunctionCall";
    }
    arguments.push_back(value);
  }

  SmallVector<Type, 1> resultTypes;
  if (!isVoidType(resultType)) {
    resultTypes.push_back(resultType);
  }

  auto opFunctionCall = opBuilder.create<spirv::FunctionCallOp>(
      unknownLoc, resultTypes, opBuilder.getSymbolRefAttr(functionName),
      arguments);

  if (!resultTypes.empty()) {
    valueMap[resultID] = opFunctionCall.getResult(0);
  }
  return success();
}

// Pull in auto-generated Deserializer::dispatchToAutogenDeserialization() and
// various Deserializer::processOp<...>() specializations.
#define GET_DESERIALIZATION_FNS
#include "mlir/Dialect/SPIRV/SPIRVSerialization.inc"
} // namespace

Optional<spirv::ModuleOp> spirv::deserialize(ArrayRef<uint32_t> binary,
                                             MLIRContext *context) {
  Deserializer deserializer(binary, context);

  if (failed(deserializer.deserialize()))
    return llvm::None;

  return deserializer.collect();
}
