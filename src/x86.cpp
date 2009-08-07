/* Copyright (c) 2008-2009, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */
   
   
#if (defined __i386__) || (defined __x86_64__)

#include "assembler.h"
#include "vector.h"

#define CAST1(x) reinterpret_cast<UnaryOperationType>(x)
#define CAST2(x) reinterpret_cast<BinaryOperationType>(x)

using namespace vm;

namespace {

enum {
  rax = 0,
  rcx = 1,
  rdx = 2,
  rbx = 3,
  rsp = 4,
  rbp = 5,
  rsi = 6,
  rdi = 7,
  r8 = 8,
  r9 = 9,
  r10 = 10,
  r11 = 11,
  r12 = 12,
  r13 = 13,
  r14 = 14,
  r15 = 15,
};

const unsigned FrameHeaderSize = 2;

const unsigned StackAlignmentInBytes = 16;
const unsigned StackAlignmentInWords = StackAlignmentInBytes / BytesPerWord;

inline bool
isInt8(intptr_t v)
{
  return v == static_cast<int8_t>(v);
}

inline bool
isInt32(intptr_t v)
{
  return v == static_cast<int32_t>(v);
}

class Task;
class AlignmentPadding;

unsigned
padding(AlignmentPadding* p, unsigned index, unsigned offset,
        AlignmentPadding* limit);

class MyBlock: public Assembler::Block {
 public:
  MyBlock(unsigned offset):
    next(0), firstPadding(0), lastPadding(0), offset(offset), start(~0),
    size(0)
  { }

  virtual unsigned resolve(unsigned start, Assembler::Block* next) {
    this->start = start;
    this->next = static_cast<MyBlock*>(next);

    return start + size + padding(firstPadding, start, offset, lastPadding);
  }

  MyBlock* next;
  AlignmentPadding* firstPadding;
  AlignmentPadding* lastPadding;
  unsigned offset;
  unsigned start;
  unsigned size;
};

class Context {
 public:
  Context(System* s, Allocator* a, Zone* zone):
    s(s), zone(zone), client(0), code(s, a, 1024), tasks(0), result(0),
    firstBlock(new (zone->allocate(sizeof(MyBlock))) MyBlock(0)),
    lastBlock(firstBlock)
  { }

  System* s;
  Zone* zone;
  Assembler::Client* client;
  Vector code;
  Task* tasks;
  uint8_t* result;
  MyBlock* firstBlock;
  MyBlock* lastBlock;
};

typedef void (*OperationType)(Context*);

typedef void (*UnaryOperationType)(Context*, unsigned, Assembler::Operand*);

typedef void (*BinaryOperationType)
(Context*, unsigned, Assembler::Operand*, unsigned, Assembler::Operand*);

class ArchitectureContext {
 public:
  ArchitectureContext(System* s): s(s) { }

  System* s;
  OperationType operations[OperationCount];
  UnaryOperationType unaryOperations[UnaryOperationCount
                                     * OperandTypeCount];
  BinaryOperationType binaryOperations
  [(BinaryOperationCount + TernaryOperationCount)
   * OperandTypeCount
   * OperandTypeCount];
};

inline void NO_RETURN
abort(Context* c)
{
  abort(c->s);
}

inline void NO_RETURN
abort(ArchitectureContext* c)
{
  abort(c->s);
}

#ifndef NDEBUG
inline void
assert(Context* c, bool v)
{
  assert(c->s, v);
}

inline void
assert(ArchitectureContext* c, bool v)
{
  assert(c->s, v);
}
#endif // not NDEBUG

inline void
expect(Context* c, bool v)
{
  expect(c->s, v);
}

ResolvedPromise*
resolved(Context* c, int64_t value)
{
  return new (c->zone->allocate(sizeof(ResolvedPromise)))
    ResolvedPromise(value);
}

class CodePromise: public Promise {
 public:
  CodePromise(Context* c, unsigned offset): c(c), offset(offset) { }

  virtual int64_t value() {
    if (resolved()) {
      return reinterpret_cast<intptr_t>(c->result + offset);
    }
    
    abort(c);
  }

  virtual bool resolved() {
    return c->result != 0;
  }

  Context* c;
  unsigned offset;
};

CodePromise*
codePromise(Context* c, unsigned offset)
{
  return new (c->zone->allocate(sizeof(CodePromise))) CodePromise(c, offset);
}

class Offset: public Promise {
 public:
  Offset(Context* c, MyBlock* block, unsigned offset, AlignmentPadding* limit):
    c(c), block(block), offset(offset), limit(limit)
  { }

  virtual bool resolved() {
    return block->start != static_cast<unsigned>(~0);
  }
  
  virtual int64_t value() {
    assert(c, resolved());

    return block->start + (offset - block->offset)
      + padding(block->firstPadding, block->start, block->offset, limit);
  }

  Context* c;
  MyBlock* block;
  unsigned offset;
  AlignmentPadding* limit;
};

Promise*
offset(Context* c)
{
  return new (c->zone->allocate(sizeof(Offset)))
    Offset(c, c->lastBlock, c->code.length(), c->lastBlock->lastPadding);
}

class Task {
 public:
  Task(Task* next): next(next) { }

  virtual void run(Context* c) = 0;

  Task* next;
};

void*
resolveOffset(System* s, uint8_t* instruction, unsigned instructionSize,
              int64_t value)
{
  intptr_t v = reinterpret_cast<uint8_t*>(value)
    - instruction - instructionSize;
    
  expect(s, isInt32(v));
    
  int32_t v4 = v;
  memcpy(instruction + instructionSize - 4, &v4, 4);
  return instruction + instructionSize;
}

class OffsetListener: public Promise::Listener {
 public:
  OffsetListener(System* s, uint8_t* instruction,
                 unsigned instructionSize):
    s(s),
    instruction(instruction),
    instructionSize(instructionSize)
  { }

  virtual bool resolve(int64_t value, void** location) {
    void* p = resolveOffset(s, instruction, instructionSize, value);
    if (location) *location = p;
    return false;
  }

  System* s;
  uint8_t* instruction;
  unsigned instructionSize;
};

class OffsetTask: public Task {
 public:
  OffsetTask(Task* next, Promise* promise, Promise* instructionOffset,
             unsigned instructionSize):
    Task(next),
    promise(promise),
    instructionOffset(instructionOffset),
    instructionSize(instructionSize)
  { }

  virtual void run(Context* c) {
    if (promise->resolved()) {
      resolveOffset
        (c->s, c->result + instructionOffset->value(), instructionSize,
         promise->value());
    } else {
      new (promise->listen(sizeof(OffsetListener)))
        OffsetListener(c->s, c->result + instructionOffset->value(),
                       instructionSize);
    }
  }

  Promise* promise;
  Promise* instructionOffset;
  unsigned instructionSize;
};

void
appendOffsetTask(Context* c, Promise* promise, Promise* instructionOffset,
                 unsigned instructionSize)
{
  c->tasks = new (c->zone->allocate(sizeof(OffsetTask))) OffsetTask
    (c->tasks, promise, instructionOffset, instructionSize);
}

void
copy(System* s, void* dst, int64_t src, unsigned size)
{
  switch (size) {
  case 4: {
    int32_t v = src;
    memcpy(dst, &v, 4);
  } break;

  case 8: {
    int64_t v = src;
    memcpy(dst, &v, 8);
  } break;

  default: abort(s);
  }
}

class ImmediateListener: public Promise::Listener {
 public:
  ImmediateListener(System* s, void* dst, unsigned size, unsigned offset):
    s(s), dst(dst), size(size), offset(offset)
  { }

  virtual bool resolve(int64_t value, void** location) {
    copy(s, dst, value, size);
    if (location) *location = static_cast<uint8_t*>(dst) + offset;
    return offset == 0;
  }

  System* s;
  void* dst;
  unsigned size;
  unsigned offset;
};

class ImmediateTask: public Task {
 public:
  ImmediateTask(Task* next, Promise* promise, Promise* offset, unsigned size,
                unsigned promiseOffset):
    Task(next),
    promise(promise),
    offset(offset),
    size(size),
    promiseOffset(promiseOffset)
  { }

  virtual void run(Context* c) {
    if (promise->resolved()) {
      copy(c->s, c->result + offset->value(), promise->value(), size);
    } else {
      new (promise->listen(sizeof(ImmediateListener))) ImmediateListener
        (c->s, c->result + offset->value(), size, promiseOffset);
    }
  }

  Promise* promise;
  Promise* offset;
  unsigned size;
  unsigned promiseOffset;
};

void
appendImmediateTask(Context* c, Promise* promise, Promise* offset,
                    unsigned size, unsigned promiseOffset = 0)
{
  c->tasks = new (c->zone->allocate(sizeof(ImmediateTask))) ImmediateTask
    (c->tasks, promise, offset, size, promiseOffset);
}

class AlignmentPadding {
 public:
  AlignmentPadding(Context* c): offset(c->code.length()), next(0) {
    if (c->lastBlock->firstPadding) {
      c->lastBlock->lastPadding->next = this;
    } else {
      c->lastBlock->firstPadding = this;
    }
    c->lastBlock->lastPadding = this;
  }

  unsigned offset;
  AlignmentPadding* next;
};

unsigned
padding(AlignmentPadding* p, unsigned start, unsigned offset,
        AlignmentPadding* limit)
{
  unsigned padding = 0;
  if (limit) {
    unsigned index = 0;
    for (; p; p = p->next) {
      index = p->offset - offset;
      while ((start + index + padding + 1) % 4) {
        ++ padding;
      }
      
      if (p == limit) break;
    }
  }
  return padding;
}

#define REX_W 0x48
#define REX_R 0x44
#define REX_X 0x42
#define REX_B 0x41
#define REX_NONE 0x40

void maybeRex(Context* c, unsigned size, int a, int index, int base, bool always) {
  if(BytesPerWord == 8) {
    uint8_t byte;
    if(size == 8) {
      byte = REX_W;
    } else {
      byte = REX_NONE;
    }
    if(a != NoRegister && (a & 8)) byte |= REX_R;
    if(index != NoRegister && (index & 8)) byte |= REX_X;
    if(base != NoRegister && (base & 8)) byte |= REX_B;
    if(always or byte != REX_NONE) c->code.append(byte);
  }
}

inline void maybeRex(Context* c, unsigned size, Assembler::Register* a, 
	Assembler::Register* b) {
  maybeRex(c, size, a->low, NoRegister, b->low, false);
}

inline void alwaysRex(Context* c, unsigned size, Assembler::Register* a, 
	Assembler::Register* b) {
  maybeRex(c, size, a->low, NoRegister, b->low, true);
}

inline void maybeRex(Context* c, unsigned size, Assembler::Register* a) {
  maybeRex(c, size, NoRegister, NoRegister, a->low, false);
}

inline void maybeRex(Context* c, unsigned size, Assembler::Register* a,
	Assembler::Memory* b) {
  maybeRex(c, size, a->low, b->index, b->base, false);
}

inline void maybeRex(Context* c, unsigned size, Assembler::Memory* a) {
  maybeRex(c, size, NoRegister, a->index, a->base, false);
}

inline int regCode(int a) {
  return a & 7;
}

inline int regCode(Assembler::Register* a) {
  return regCode(a->low);
}

inline void modrm(Context* c, uint8_t mod, int a, int b) {
  c->code.append(mod | (regCode(b) << 3) | regCode(a));
}

inline void modrm(Context* c, uint8_t mod, Assembler::Register* a, 
	Assembler::Register* b) {
  modrm(c, mod, a->low, b->low);
}

inline void sib(Context* c, unsigned scale, int index, int base) {
  c->code.append((log(scale) << 6) | (regCode(index) << 3) | regCode(base));
}

inline void modrmSib(Context* c, int width, int a, int scale, int index, int base) {
  if(index == NoRegister) {
    modrm(c, width, base, a);
    if(regCode(base) == rsp) {
      sib(c, 0x00, rsp, rsp);
    }
  } else {
    modrm(c, width, rsp, a);
    sib(c, scale, index, base);
  }
}

inline void modrmSibImm(Context* c, int a, int scale, int index, int base, int offset) {
  if(offset == 0 && regCode(base) != rbp) {
    modrmSib(c, 0x00, a, scale, index, base);
  } else if(isInt8(offset)) {
    modrmSib(c, 0x40, a, scale, index, base);
    c->code.append(offset);
  } else {
    modrmSib(c, 0x80, a, scale, index, base);
    c->code.append4(offset);
  }
}
  

inline void modrmSibImm(Context* c, Assembler::Register* a,
	Assembler::Memory* b) {
  modrmSibImm(c, a->low, b->scale, b->index, b->base, b->offset);
}

inline void opcode(Context* c, uint8_t op) {
  c->code.append(op);
}

inline void opcode(Context* c, uint8_t op1, uint8_t op2) {
  c->code.append(op1);
  c->code.append(op2);
}

void
return_(Context* c)
{
  opcode(c, 0xc3);
}

void
ignore(Context*)
{ }

void
unconditional(Context* c, unsigned jump, Assembler::Constant* a)
{
  appendOffsetTask(c, a->value, offset(c), 5);

  opcode(c, jump);
  c->code.append4(0);
}

void
conditional(Context* c, unsigned condition, Assembler::Constant* a)
{
  appendOffsetTask(c, a->value, offset(c), 6);
  
  opcode(c, 0x0f, condition);
  c->code.append4(0);
}

inline unsigned
index(UnaryOperation operation, OperandType operand)
{
  return operation + (UnaryOperationCount * operand);
}

inline unsigned
index(BinaryOperation operation,
      OperandType operand1,
      OperandType operand2)
{
  return operation
    + ((BinaryOperationCount + TernaryOperationCount) * operand1)
    + ((BinaryOperationCount + TernaryOperationCount)
       * OperandTypeCount * operand2);
}

inline unsigned
index(TernaryOperation operation,
      OperandType operand1,
      OperandType operand2)
{
  return BinaryOperationCount + operation
    + ((BinaryOperationCount + TernaryOperationCount) * operand1)
    + ((BinaryOperationCount + TernaryOperationCount)
       * OperandTypeCount * operand2);
}

void
moveCR(Context* c, unsigned aSize, Assembler::Constant* a,
       unsigned bSize, Assembler::Register* b);

void
moveCR2(Context*, unsigned, Assembler::Constant*, unsigned,
        Assembler::Register*, unsigned);

void
callR(Context*, unsigned, Assembler::Register*);

void
callC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  unconditional(c, 0xe8, a);
}

void
longCallC(Context* c, unsigned size, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  if (BytesPerWord == 8) {
    Assembler::Register r(r10);
    moveCR2(c, size, a, size, &r, 11);
    callR(c, size, &r);
  } else {
    callC(c, size, a);
  }
}

void
jumpR(Context* c, unsigned size UNUSED, Assembler::Register* a)
{
  assert(c, size == BytesPerWord);

  maybeRex(c, 4, a);
  opcode(c, 0xff, 0xe0 + regCode(a));
}

void
jumpC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  unconditional(c, 0xe9, a);
}

void
jumpM(Context* c, unsigned size UNUSED, Assembler::Memory* a)
{
  assert(c, size == BytesPerWord);
  
  maybeRex(c, 4, a);
  opcode(c, 0xff);
  modrmSibImm(c, rsp, a->scale, a->index, a->base, a->offset);
}

void
jumpIfEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x84, a);
}

void
jumpIfNotEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x85, a);
}

void
jumpIfGreaterC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8f, a);
}

void
jumpIfGreaterOrEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8d, a);
}

void
jumpIfLessC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8c, a);
}

void
jumpIfLessOrEqualC(Context* c, unsigned size UNUSED, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  conditional(c, 0x8e, a);
}

void
longJumpC(Context* c, unsigned size, Assembler::Constant* a)
{
  assert(c, size == BytesPerWord);

  if (BytesPerWord == 8) {
    Assembler::Register r(r10);
    moveCR2(c, size, a, size, &r, 11);
    jumpR(c, size, &r);
  } else {
    jumpC(c, size, a);
  }
}

void
callR(Context* c, unsigned size UNUSED, Assembler::Register* a)
{
  assert(c, size == BytesPerWord);

	//maybeRex.W has no meaning here so we disable it
  maybeRex(c, 4, a);
  opcode(c, 0xff, 0xd0 + regCode(a));
}

void
callM(Context* c, unsigned size UNUSED, Assembler::Memory* a)
{
  assert(c, size == BytesPerWord);
  
  maybeRex(c, 4, a);
  opcode(c, 0xff);
  modrmSibImm(c, rdx, a->scale, a->index, a->base, a->offset);
}

void
alignedCallC(Context* c, unsigned size, Assembler::Constant* a)
{
  new (c->zone->allocate(sizeof(AlignmentPadding))) AlignmentPadding(c);
  callC(c, size, a);
}

void
alignedJumpC(Context* c, unsigned size, Assembler::Constant* a)
{
  new (c->zone->allocate(sizeof(AlignmentPadding))) AlignmentPadding(c);
  jumpC(c, size, a);
}

void
pushR(Context* c, unsigned size, Assembler::Register* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);

    pushR(c, 4, &ah);
    pushR(c, 4, a);
  } else {
    maybeRex(c, 4, a);
    opcode(c, 0x50 + regCode(a));
  }
}

void
moveRR(Context* c, unsigned aSize, Assembler::Register* a,
       unsigned bSize, Assembler::Register* b);

void
popR(Context* c, unsigned size, Assembler::Register* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Register ah(a->high);

    popR(c, 4, a);
    popR(c, 4, &ah);
  } else {
    maybeRex(c, 4, a);
    opcode(c, 0x58 + regCode(a));
    if (BytesPerWord == 8 and size == 4) {
      moveRR(c, 4, a, 8, a);
    }
  }
}

void
popM(Context* c, unsigned size, Assembler::Memory* a)
{
  if (BytesPerWord == 4 and size == 8) {
    Assembler::Memory ah(a->base, a->offset + 4, a->index, a->scale);

    popM(c, 4, a);
    popM(c, 4, &ah);
  } else {
    assert(c, BytesPerWord == 4 or size == 8);

    opcode(c, 0x8f);
    modrmSibImm(c, 0, a->scale, a->index, a->base, a->offset);
  }
}

void
addCarryCR(Context* c, unsigned size, Assembler::Constant* a,
           Assembler::Register* b);

void
negateR(Context* c, unsigned size, Assembler::Register* a)
{
  if (BytesPerWord == 4 and size == 8) {
    assert(c, a->low == rax and a->high == rdx);

    ResolvedPromise zeroPromise(0);
    Assembler::Constant zero(&zeroPromise);

    Assembler::Register ah(a->high);

    negateR(c, 4, a);
    addCarryCR(c, 4, &zero, &ah);
    negateR(c, 4, &ah);
  } else {
    maybeRex(c, size, a);
    opcode(c, 0xf7, 0xd8 + regCode(a));
  }
}

void
negateRR(Context* c, unsigned aSize, Assembler::Register* a,
         unsigned bSize UNUSED, Assembler::Register* b UNUSED)
{
  assert(c, aSize == bSize);

  negateR(c, aSize, a);
}

void
moveCR2(Context* c, UNUSED unsigned aSize, Assembler::Constant* a,
        UNUSED unsigned bSize, Assembler::Register* b, unsigned promiseOffset)
{
  if (BytesPerWord == 4 and bSize == 8) {
    int64_t v = a->value->value();

    ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
    Assembler::Constant ah(&high);

    ResolvedPromise low(v & 0xFFFFFFFF);
    Assembler::Constant al(&low);

    Assembler::Register bh(b->high);

    moveCR(c, 4, &al, 4, b);
    moveCR(c, 4, &ah, 4, &bh);
  } else {
    maybeRex(c, BytesPerWord, b);
    opcode(c, 0xb8 + regCode(b));
    if (a->value->resolved()) {
      c->code.appendAddress(a->value->value());
    } else {
      appendImmediateTask
        (c, a->value, offset(c), BytesPerWord, promiseOffset);
      c->code.appendAddress(static_cast<uintptr_t>(0));
    }
  }
}

void
moveCR(Context* c, unsigned aSize, Assembler::Constant* a,
       unsigned bSize, Assembler::Register* b)
{
  moveCR2(c, aSize, a, bSize, b, 0);
}

void
swapRR(Context* c, unsigned aSize UNUSED, Assembler::Register* a,
       unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);
  assert(c, aSize == BytesPerWord);
  
  alwaysRex(c, aSize, a, b);
  opcode(c, 0x87);
  modrm(c, 0xc0, b, a);
}

void
moveRR(Context* c, unsigned aSize, Assembler::Register* a,
       UNUSED unsigned bSize, Assembler::Register* b)
{
	
  if (BytesPerWord == 4 and aSize == 8 and bSize == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    if (a->high == b->low) {
      if (a->low == b->high) {
        swapRR(c, 4, a, 4, b);
      } else {
        moveRR(c, 4, &ah, 4, &bh);
        moveRR(c, 4, a, 4, b);
      }
    } else {
      moveRR(c, 4, a, 4, b);
      moveRR(c, 4, &ah, 4, &bh);
    }
  } else {
    switch (aSize) {
    case 1:
      if (BytesPerWord == 4 and a->low > rbx) {
        assert(c, b->low <= rbx);

        moveRR(c, BytesPerWord, a, BytesPerWord, b);
        moveRR(c, 1, b, BytesPerWord, b);
      } else {
        alwaysRex(c, aSize, b, a);
        opcode(c, 0x0f, 0xbe);
        modrm(c, 0xc0, a, b);
      }
      break;

    case 2:
      alwaysRex(c, aSize, b, a);
      opcode(c, 0x0f, 0xbf);
      modrm(c, 0xc0, a, b);
      break;

    case 4:
      if (bSize == 8) {
      	if (BytesPerWord == 8) {
          alwaysRex(c, bSize, b, a);
          opcode(c, 0x63);
          modrm(c, 0xc0, a, b);
      	} else {
      	  if (a->low == rax and b->low == rax and b->high == rdx) {
      	  	opcode(c, 0x99); //cdq
      	  } else {
            assert(c, b->low == rax and b->high == rdx);

            moveRR(c, 4, a, 4, b);
            moveRR(c, 4, b, 8, b);
          }
        }
      } else {
        if (a->low != b->low) {
          alwaysRex(c, aSize, a, b);
          opcode(c, 0x89);
          modrm(c, 0xc0, b, a);
        }
      }
      break; 
      
    case 8:
      if (a->low != b->low){
        maybeRex(c, aSize, a, b);
        opcode(c, 0x89);
        modrm(c, 0xc0, b, a);
      }
      break;
    }
  }
}

void
moveMR(Context* c, unsigned aSize, Assembler::Memory* a,
       unsigned bSize, Assembler::Register* b)
{
  switch (aSize) {
  case 1:
    maybeRex(c, bSize, b, a);
    opcode(c, 0x0f, 0xbe);
    modrmSibImm(c, b, a);
    break;

  case 2:
    maybeRex(c, bSize, b, a);
    opcode(c, 0x0f, 0xbf);
    modrmSibImm(c, b, a);
    break;

  case 4:
    if (BytesPerWord == 8) {
      maybeRex(c, bSize, b, a);
      opcode(c, 0x63);
      modrmSibImm(c, b, a);
    } else {
      if (bSize == 8) {
        assert(c, b->low == rax and b->high == rdx);
        
        moveMR(c, 4, a, 4, b);
        moveRR(c, 4, b, 8, b);
      } else {
        maybeRex(c, bSize, b, a);
        opcode(c, 0x8b);
        modrmSibImm(c, b, a);
      }
    }
    break;
    
  case 8:
    if (BytesPerWord == 4 and bSize == 8) {
      Assembler::Memory ah(a->base, a->offset + 4, a->index, a->scale);
      Assembler::Register bh(b->high);

      moveMR(c, 4, a, 4, b);    
      moveMR(c, 4, &ah, 4, &bh);
    } else {
      maybeRex(c, bSize, b, a);
      opcode(c, 0x8b);
      modrmSibImm(c, b, a);
    }
    break;

  default: abort(c);
  }
}

void
moveRM(Context* c, unsigned aSize, Assembler::Register* a,
       unsigned bSize UNUSED, Assembler::Memory* b)
{
  assert(c, aSize == bSize);
  
  switch (aSize) {
  case 1:
    maybeRex(c, bSize, a, b);
    opcode(c, 0x88);
    modrmSibImm(c, a, b);
    break;

  case 2:
    opcode(c, 0x66);
    maybeRex(c, bSize, a, b);
    opcode(c, 0x89);
    modrmSibImm(c, a, b);
    break;

  case 4:
    if (BytesPerWord == 8) {
      maybeRex(c, bSize, a, b);
      opcode(c, 0x89);
      modrmSibImm(c, a, b);
      break;
    } else {
      opcode(c, 0x89);
      modrmSibImm(c, a, b);
    }
    break;
    
  case 8:
    if(BytesPerWord == 8) {
      maybeRex(c, bSize, a, b);
      opcode(c, 0x89);
      modrmSibImm(c, a, b);
    } else {
      Assembler::Register ah(a->high);
      Assembler::Memory bh(b->base, b->offset + 4, b->index, b->scale);

      moveRM(c, 4, a, 4, b);    
      moveRM(c, 4, &ah, 4, &bh);
    }
    break;

  default: abort(c);
  }
}

void
moveAR(Context* c, unsigned aSize, Assembler::Address* a,
       unsigned bSize, Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or (aSize == 4 and bSize == 4));

  Assembler::Constant constant(a->address);
  Assembler::Memory memory(b->low, 0, -1, 0);

  moveCR(c, aSize, &constant, bSize, b);
  moveMR(c, bSize, &memory, bSize, b);
}

ShiftMaskPromise*
shiftMaskPromise(Context* c, Promise* base, unsigned shift, int64_t mask)
{
  return new (c->zone->allocate(sizeof(ShiftMaskPromise)))
    ShiftMaskPromise(base, shift, mask);
}

void
moveCM(Context* c, unsigned aSize UNUSED, Assembler::Constant* a,
       unsigned bSize, Assembler::Memory* b)
{
  switch (bSize) {
  case 1:
    maybeRex(c, bSize, b);
    opcode(c, 0xc6);
    modrmSibImm(c, 0, b->scale, b->index, b->base, b->offset);
    c->code.append(a->value->value());
    break;

  case 2:
    opcode(c, 0x66);
    maybeRex(c, bSize, b);
    opcode(c, 0xc7);
    modrmSibImm(c, 0, b->scale, b->index, b->base, b->offset);
    c->code.append2(a->value->value());
    break;

  case 4:
    maybeRex(c, bSize, b);
    opcode(c, 0xc7);
    modrmSibImm(c, 0, b->scale, b->index, b->base, b->offset);
    if (a->value->resolved()) {
      c->code.append4(a->value->value());
    } else {
      appendImmediateTask(c, a->value, offset(c), 4);
      c->code.append4(0);
    }
    break;

  case 8: {
  	if (BytesPerWord == 8) {
      if(a->value->resolved() and isInt32(a->value->value())) {
        maybeRex(c, bSize, b);
        opcode(c, 0xc7);
        modrmSibImm(c, 0, b->scale, b->index, b->base, b->offset);
        c->code.append4(a->value->value());
      } else {
        Assembler::Register tmp(c->client->acquireTemporary());
        moveCR(c, 8, a, 8, &tmp);
        moveRM(c, 8, &tmp, 8, b);
        c->client->releaseTemporary(tmp.low);
      }
  	} else {
  	  Assembler::Constant ah(shiftMaskPromise(c, a->value, 32, 0xFFFFFFFF));
      Assembler::Constant al(shiftMaskPromise(c, a->value, 0, 0xFFFFFFFF));

      Assembler::Memory bh(b->base, b->offset + 4, b->index, b->scale);

      moveCM(c, 4, &al, 4, b);
      moveCM(c, 4, &ah, 4, &bh);
    }
  } break;

  default: abort(c);
  }
}

void
moveZRR(Context* c, unsigned aSize, Assembler::Register* a,
        unsigned bSize UNUSED, Assembler::Register* b)
{
  switch (aSize) {
  case 2:
    alwaysRex(c, aSize, b, a);
    opcode(c, 0x0f, 0xb7);
    modrm(c, 0xc0, a, b);
    break;

  default: abort(c);
  }
}

void
moveZMR(Context* c, unsigned aSize UNUSED, Assembler::Memory* a,
        unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, bSize == BytesPerWord);
  assert(c, aSize == 2);
  
  maybeRex(c, bSize, b, a);
  opcode(c, 0x0f, 0xb7);
  modrmSibImm(c, b->low, a->scale, a->index, a->base, a->offset);
}

void
addCarryRR(Context* c, unsigned size, Assembler::Register* a,
           Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4);
  
  maybeRex(c, size, a, b);
  opcode(c, 0x11);
  modrm(c, 0xc0, b, a);
}

void
addRR(Context* c, unsigned aSize, Assembler::Register* a,
      unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  if (BytesPerWord == 4 and aSize == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    addRR(c, 4, a, 4, b);
    addCarryRR(c, 4, &ah, &bh);
  } else {
    maybeRex(c, aSize, a, b);
    opcode(c, 0x01);
    modrm(c, 0xc0, b, a);
  }
}

void
addCarryCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
           Assembler::Register* b)
{
  
  int64_t v = a->value->value();
  if (isInt8(v)) {
    maybeRex(c, size, b);
    opcode(c, 0x83, 0xd0 + regCode(b));
    c->code.append(v);
  } else {
    abort(c);
  }
}

void
addCR(Context* c, unsigned aSize, Assembler::Constant* a,
      unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  int64_t v = a->value->value();
  if (v) {
    if (BytesPerWord == 4 and bSize == 8) {
      ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
      Assembler::Constant ah(&high);

      ResolvedPromise low(v & 0xFFFFFFFF);
      Assembler::Constant al(&low);

      Assembler::Register bh(b->high);

      addCR(c, 4, &al, 4, b);
      addCarryCR(c, 4, &ah, &bh);
    } else {
      if (isInt32(v)) {
        maybeRex(c, aSize, b);
        if (isInt8(v)) {
          opcode(c, 0x83, 0xc0 + regCode(b));
          c->code.append(v);
        } else {
          opcode(c, 0x81, 0xc0 + regCode(b));
          c->code.append4(v);
        }
      } else {
        Assembler::Register tmp(c->client->acquireTemporary());
        moveCR(c, aSize, a, aSize, &tmp);
        addRR(c, aSize, &tmp, bSize, b);
        c->client->releaseTemporary(tmp.low);
      }
    }
  }
}

void
subtractBorrowCR(Context* c, unsigned size UNUSED, Assembler::Constant* a,
                 Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4);
  
  int64_t v = a->value->value();
  if (isInt8(v)) {
    opcode(c, 0x83, 0xd8 + regCode(b));
    c->code.append(v);
  } else {
    abort(c);
  }
}

void
subtractRR(Context* c, unsigned aSize, Assembler::Register* a,
           unsigned bSize, Assembler::Register* b);

void
subtractCR(Context* c, unsigned aSize, Assembler::Constant* a,
           unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  int64_t v = a->value->value();
  if (v) {
    if (BytesPerWord == 4 and bSize == 8) {
      ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
      Assembler::Constant ah(&high);

      ResolvedPromise low(v & 0xFFFFFFFF);
      Assembler::Constant al(&low);

      Assembler::Register bh(b->high);

      subtractCR(c, 4, &al, 4, b);
      subtractBorrowCR(c, 4, &ah, &bh);
    } else {
      if (isInt32(v)) {
        maybeRex(c, aSize, b);
        if (isInt8(v)) {
          opcode(c, 0x83, 0xe8 + regCode(b));
          c->code.append(v);
        } else {
          opcode(c, 0x81, 0xe8 + regCode(b));
          c->code.append4(v);
        }
      } else {
        Assembler::Register tmp(c->client->acquireTemporary());
        moveCR(c, aSize, a, aSize, &tmp);
        subtractRR(c, aSize, &tmp, bSize, b);
        c->client->releaseTemporary(tmp.low);
      }
    }
  }
}

void
subtractBorrowRR(Context* c, unsigned size, Assembler::Register* a,
                 Assembler::Register* b)
{
  assert(c, BytesPerWord == 8 or size == 4);
  
  maybeRex(c, size, a, b);
  opcode(c, 0x19);
  modrm(c, 0xc0, b, a);
}

void
subtractRR(Context* c, unsigned aSize, Assembler::Register* a,
           unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);
  
  if (BytesPerWord == 4 and aSize == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    subtractRR(c, 4, a, 4, b);
    subtractBorrowRR(c, 4, &ah, &bh);
  } else {
    maybeRex(c, aSize, a, b);
    opcode(c, 0x29);
    modrm(c, 0xc0, b, a);
  }
}

void
andRR(Context* c, unsigned aSize, Assembler::Register* a,
      unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);


  if (BytesPerWord == 4 and aSize == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    andRR(c, 4, a, 4, b);
    andRR(c, 4, &ah, 4, &bh);
  } else {
    maybeRex(c, aSize, a, b);
    opcode(c, 0x21);
    modrm(c, 0xc0, b, a);
  }
}

void
andCR(Context* c, unsigned aSize, Assembler::Constant* a,
      unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  int64_t v = a->value->value();

  if (BytesPerWord == 4 and bSize == 8) {
    ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
    Assembler::Constant ah(&high);

    ResolvedPromise low(v & 0xFFFFFFFF);
    Assembler::Constant al(&low);

    Assembler::Register bh(b->high);

    andCR(c, 4, &al, 4, b);
    andCR(c, 4, &ah, 4, &bh);
  } else {
    if (isInt32(v)) {
      maybeRex(c, aSize, b);
      if (isInt8(v)) {
        opcode(c, 0x83, 0xe0 + regCode(b));
        c->code.append(v);
      } else {
        opcode(c, 0x81, 0xe0 + regCode(b));
        c->code.append4(v);
      }
    } else {
      Assembler::Register tmp(c->client->acquireTemporary());
      moveCR(c, aSize, a, aSize, &tmp);
      andRR(c, aSize, &tmp, bSize, b);
      c->client->releaseTemporary(tmp.low);
    }
  }
}

void
orRR(Context* c, unsigned aSize, Assembler::Register* a,
     unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  if (BytesPerWord == 4 and aSize == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    orRR(c, 4, a, 4, b);
    orRR(c, 4, &ah, 4, &bh);
  } else {
    maybeRex(c, aSize, a, b);
    opcode(c, 0x09);
    modrm(c, 0xc0, b, a);
  }
}

void
orCR(Context* c, unsigned aSize, Assembler::Constant* a,
     unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  int64_t v = a->value->value();
  if (v) {
    if (BytesPerWord == 4 and bSize == 8) {
      ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
      Assembler::Constant ah(&high);

      ResolvedPromise low(v & 0xFFFFFFFF);
      Assembler::Constant al(&low);

      Assembler::Register bh(b->high);

      orCR(c, 4, &al, 4, b);
      orCR(c, 4, &ah, 4, &bh);
    } else {
      if (isInt32(v)) {
        maybeRex(c, aSize, b);
        if (isInt8(v)) {
          opcode(c, 0x83, 0xc8 + regCode(b));
          c->code.append(v);
        } else {
          opcode(c, 0x81, 0xc8 + regCode(b));
          c->code.append4(v);        
        }
      } else {
        Assembler::Register tmp(c->client->acquireTemporary());
        moveCR(c, aSize, a, aSize, &tmp);
        orRR(c, aSize, &tmp, bSize, b);
        c->client->releaseTemporary(tmp.low);
      }
    }
  }
}

void
xorRR(Context* c, unsigned aSize, Assembler::Register* a,
      unsigned bSize UNUSED, Assembler::Register* b)
{
  if (BytesPerWord == 4 and aSize == 8) {
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    xorRR(c, 4, a, 4, b);
    xorRR(c, 4, &ah, 4, &bh);
  } else {
    maybeRex(c, aSize, a, b);
    opcode(c, 0x31);
    modrm(c, 0xc0, b, a);
  }
}

void
xorCR(Context* c, unsigned aSize, Assembler::Constant* a,
      unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  int64_t v = a->value->value();
  if (v) {
    if (BytesPerWord == 4 and bSize == 8) {
      ResolvedPromise high((v >> 32) & 0xFFFFFFFF);
      Assembler::Constant ah(&high);

      ResolvedPromise low(v & 0xFFFFFFFF);
      Assembler::Constant al(&low);

      Assembler::Register bh(b->high);

      xorCR(c, 4, &al, 4, b);
      xorCR(c, 4, &ah, 4, &bh);
    } else {
      if (isInt32(v)) {
        maybeRex(c, aSize, b);
        if (isInt8(v)) {
          opcode(c, 0x83, 0xf0 + regCode(b));
          c->code.append(v);
        } else {
          opcode(c, 0x81, 0xf0 + regCode(b));
          c->code.append4(v);        
        }
      } else {
        Assembler::Register tmp(c->client->acquireTemporary());
        moveCR(c, aSize, a, aSize, &tmp);
        xorRR(c, aSize, &tmp, bSize, b);
        c->client->releaseTemporary(tmp.low);
      }
    }
  }
}

void
multiplyRR(Context* c, unsigned aSize, Assembler::Register* a,
           unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);


  if (BytesPerWord == 4 and aSize == 8) {
    assert(c, b->high == rdx);
    assert(c, b->low != rax);
    assert(c, a->low != rax);
    assert(c, a->high != rax);

    c->client->save(rax);

    Assembler::Register axdx(rax, rdx);
    Assembler::Register ah(a->high);
    Assembler::Register bh(b->high);

    moveRR(c, 4, b, 4, &axdx);
    multiplyRR(c, 4, &ah, 4, b);
    multiplyRR(c, 4, a, 4, &bh);
    addRR(c, 4, &bh, 4, b);
    
    // mul a->low,%eax%edx
    opcode(c, 0xf7, 0xe0 + a->low);
    
    addRR(c, 4, b, 4, &bh);
    moveRR(c, 4, &axdx, 4, b);
  } else {
    maybeRex(c, aSize, b, a);
    opcode(c, 0x0f, 0xaf);
    modrm(c, 0xc0, a, b);
  }
}

void
compareRR(Context* c, unsigned aSize, Assembler::Register* a,
          unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);


  maybeRex(c, aSize, a, b);
  opcode(c, 0x39);
  modrm(c, 0xc0, b, a);
}

void
compareCR(Context* c, unsigned aSize, Assembler::Constant* a,
          unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);
  assert(c, BytesPerWord == 8 or aSize == 4);
  
  if (a->value->resolved() and isInt32(a->value->value())) {
    int64_t v = a->value->value();
    maybeRex(c, aSize, b);
    if (isInt8(v)) {
      opcode(c, 0x83, 0xf8 + regCode(b));
      c->code.append(v);
    } else {
      opcode(c, 0x81, 0xf8 + regCode(b));
      c->code.append4(v);
    }
  } else {
    Assembler::Register tmp(c->client->acquireTemporary());
    moveCR(c, aSize, a, aSize, &tmp);
    compareRR(c, aSize, &tmp, bSize, b);
    c->client->releaseTemporary(tmp.low);
  }
}

void
multiplyCR(Context* c, unsigned aSize, Assembler::Constant* a,
           unsigned bSize, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  if (BytesPerWord == 4 and aSize == 8) {
    const uint32_t mask = ~((1 << rax) | (1 << rdx));
    Assembler::Register tmp(c->client->acquireTemporary(mask),
                            c->client->acquireTemporary(mask));

    moveCR(c, aSize, a, aSize, &tmp);
    multiplyRR(c, aSize, &tmp, bSize, b);
    c->client->releaseTemporary(tmp.low);
    c->client->releaseTemporary(tmp.high);
  } else {
    int64_t v = a->value->value();
    if (v != 1) {
      if (isInt32(v)) {
        maybeRex(c, bSize, b, b);
        if (isInt8(v)) {
          opcode(c, 0x6b);
          modrm(c, 0xc0, b, b);
          c->code.append(v);
        } else {
          opcode(c, 0x69);
          modrm(c, 0xc0, b, b);
          c->code.append4(v);        
        }
      } else {
        Assembler::Register tmp(c->client->acquireTemporary());
        moveCR(c, aSize, a, aSize, &tmp);
        multiplyRR(c, aSize, &tmp, bSize, b);
        c->client->releaseTemporary(tmp.low);      
      }
    }
  }
}

void
compareRM(Context* c, unsigned aSize, Assembler::Register* a,
          unsigned bSize UNUSED, Assembler::Memory* b)
{
  assert(c, aSize == bSize);
  assert(c, BytesPerWord == 8 or aSize == 4);
  
  if (BytesPerWord == 8 and aSize == 4) {
    moveRR(c, 4, a, 8, a);
  }
  maybeRex(c, bSize, a, b);
  opcode(c, 0x39);
  modrmSibImm(c, a, b);
}

void
compareCM(Context* c, unsigned aSize, Assembler::Constant* a,
          unsigned bSize, Assembler::Memory* b)
{
  assert(c, aSize == bSize);
  assert(c, BytesPerWord == 8 or aSize == 4);
  
  if (a->value->resolved()) { 
    int64_t v = a->value->value();   
    maybeRex(c, aSize, b);
    opcode(c, isInt8(v) ? 0x83 : 0x81);
    modrmSibImm(c, rdi, b->scale, b->index, b->base, b->offset);
    
    if (isInt8(v)) {
      c->code.append(v);
    } else if (isInt32(v)) {
      c->code.append4(v);
    } else {
      abort(c);
    }
  } else {
    Assembler::Register tmp(c->client->acquireTemporary());
    moveCR(c, aSize, a, bSize, &tmp);
    compareRM(c, bSize, &tmp, bSize, b);
    c->client->releaseTemporary(tmp.low);
  }
}

void
longCompare(Context* c, Assembler::Operand* al, UNUSED Assembler::Operand* ah,
            Assembler::Register* bl, UNUSED Assembler::Operand* bh,
            BinaryOperationType compare)
{
  ResolvedPromise negativePromise(-1);
  Assembler::Constant negative(&negativePromise);

  ResolvedPromise zeroPromise(0);
  Assembler::Constant zero(&zeroPromise);

  ResolvedPromise positivePromise(1);
  Assembler::Constant positive(&positivePromise);

  if (BytesPerWord == 8) {
    compare(c, 8, al, 8, bl);
    
    opcode(c, 0x0f, 0x8c); // jl
    unsigned less = c->code.length();
    c->code.append4(0);

    opcode(c, 0x0f, 0x8f); // jg
    unsigned greater = c->code.length();
    c->code.append4(0);

    moveCR(c, 4, &zero, 4, bl);
    
    opcode(c, 0xe9); // jmp
    unsigned nextFirst = c->code.length();
    c->code.append4(0);

    int32_t lessOffset = c->code.length() - less - 4;
    c->code.set(less, &lessOffset, 4);

    moveCR(c, 4, &negative, 4, bl);

    opcode(c, 0xe9); // jmp
    unsigned nextSecond = c->code.length();
    c->code.append4(0);

    int32_t greaterOffset = c->code.length() - greater - 4;
    c->code.set(greater, &greaterOffset, 4);

    moveCR(c, 4, &positive, 4, bl);

    int32_t nextFirstOffset = c->code.length() - nextFirst - 4;
    c->code.set(nextFirst, &nextFirstOffset, 4);

    int32_t nextSecondOffset = c->code.length() - nextSecond - 4;
    c->code.set(nextSecond, &nextSecondOffset, 4);
  } else {
    compare(c, 4, ah, 4, bh);
    
    opcode(c, 0x0f, 0x8c); //jl
    unsigned less = c->code.length();
    c->code.append4(0);

    opcode(c, 0x0f, 0x8f); //jg
    unsigned greater = c->code.length();
    c->code.append4(0);

    compare(c, 4, al, 4, bl);

    opcode(c, 0x0f, 0x82); //ja
    unsigned above = c->code.length();
    c->code.append4(0);

    opcode(c, 0x0f, 0x87); //jb
    unsigned below = c->code.length();
    c->code.append4(0);

    moveCR(c, 4, &zero, 4, bl);
    
    c->code.append(0xe9); // jmp
    unsigned nextFirst = c->code.length();
    c->code.append4(0);

    int32_t lessOffset = c->code.length() - less - 4;
    c->code.set(less, &lessOffset, 4);

    int32_t aboveOffset = c->code.length() - above - 4;
    c->code.set(above, &aboveOffset, 4);

    moveCR(c, 4, &negative, 4, bl);

    opcode(c, 0xe9); // jmp
    unsigned nextSecond = c->code.length();
    c->code.append4(0);

    int32_t greaterOffset = c->code.length() - greater - 4;
    c->code.set(greater, &greaterOffset, 4);

    int32_t belowOffset = c->code.length() - below - 4;
    c->code.set(below, &belowOffset, 4);

    moveCR(c, 4, &positive, 4, bl);

    int32_t nextFirstOffset = c->code.length() - nextFirst - 4;
    c->code.set(nextFirst, &nextFirstOffset, 4);

    int32_t nextSecondOffset = c->code.length() - nextSecond - 4;
    c->code.set(nextSecond, &nextSecondOffset, 4);
  }
}

void
divideRR(Context* c, unsigned aSize, Assembler::Register* a,
         unsigned bSize UNUSED, Assembler::Register* b UNUSED)
{
  assert(c, aSize == bSize);

  assert(c, b->low == rax);
  assert(c, a->low != rdx);

  c->client->save(rdx);
    
  maybeRex(c, aSize, a, b);
  opcode(c, 0x99); // cdq
  maybeRex(c, aSize, b, a);
  opcode(c, 0xf7, 0xf8 + regCode(a));
}

void
remainderRR(Context* c, unsigned aSize, Assembler::Register* a,
            unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == bSize);

  assert(c, b->low == rax);
  assert(c, a->low != rdx);

  c->client->save(rdx);
    
  maybeRex(c, aSize, a, b);
  opcode(c, 0x99); // cdq
  maybeRex(c, aSize, b, a);
  opcode(c, 0xf7, 0xf8 + regCode(a));

  Assembler::Register dx(rdx);
  moveRR(c, BytesPerWord, &dx, BytesPerWord, b);
}

void
longCompareCR(Context* c, unsigned aSize UNUSED, Assembler::Constant* a,
              unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == 8);
  assert(c, bSize == 8);
  
  int64_t v = a->value->value();

  ResolvedPromise low(v & ~static_cast<uintptr_t>(0));
  Assembler::Constant al(&low);
  
  ResolvedPromise high((v >> 32) & ~static_cast<uintptr_t>(0));
  Assembler::Constant ah(&high);
  
  Assembler::Register bh(b->high);
  
  longCompare(c, &al, &ah, b, &bh, CAST2(compareCR));
}

void
longCompareRR(Context* c, unsigned aSize UNUSED, Assembler::Register* a,
              unsigned bSize UNUSED, Assembler::Register* b)
{
  assert(c, aSize == 8);
  assert(c, bSize == 8);
  
  Assembler::Register ah(a->high);
  Assembler::Register bh(b->high);
  
  longCompare(c, a, &ah, b, &bh, CAST2(compareRR));
}

void
doShift(Context* c, UNUSED void (*shift)
        (Context*, unsigned, Assembler::Register*, unsigned,
         Assembler::Register*),
        int type, UNUSED unsigned aSize, Assembler::Constant* a,
        unsigned bSize, Assembler::Register* b)
{
  int64_t v = a->value->value();

  if (BytesPerWord == 4 and bSize == 8) {
    c->client->save(rcx);

    Assembler::Register cx(rcx);
    moveCR(c, 4, a, 4, &cx);
    shift(c, aSize, &cx, bSize, b);
  } else {
    maybeRex(c, bSize, b);
    if (v == 1) {
      opcode(c, 0xd1, type + regCode(b));
    } else if (isInt8(v)) {
      opcode(c, 0xc1, type + regCode(b));
      c->code.append(v);
    } else {
      abort(c);
    }
  }
}

void
shiftLeftRR(Context* c, UNUSED unsigned aSize, Assembler::Register* a,
            unsigned bSize, Assembler::Register* b)
{
  assert(c, a->low == rcx);
  
  if (BytesPerWord == 4 and bSize == 8) {
    // shld
    opcode(c, 0x0f, 0xa5);
    modrm(c, 0xc0, b->high, b->low);

    // shl
    opcode(c, 0xd3, 0xe0 + b->low);

    ResolvedPromise promise(32);
    Assembler::Constant constant(&promise);
    compareCR(c, aSize, &constant, aSize, a);

    opcode(c, 0x0f, 0x8c); //jl
    c->code.append4(2 + 2);

    Assembler::Register bh(b->high);
    moveRR(c, 4, b, 4, &bh); // 2 bytes
    xorRR(c, 4, b, 4, b); // 2 bytes
  } else {
    maybeRex(c, bSize, a, b);
    opcode(c, 0xd3, 0xe0 + regCode(b));
  }
}

void
shiftLeftCR(Context* c, unsigned aSize, Assembler::Constant* a,
            unsigned bSize, Assembler::Register* b)
{
  doShift(c, shiftLeftRR, 0xe0, aSize, a, bSize, b);
}

void
shiftRightRR(Context* c, UNUSED unsigned aSize, Assembler::Register* a,
             unsigned bSize, Assembler::Register* b)
{
  assert(c, a->low == rcx);
  if (BytesPerWord == 4 and bSize == 8) {
    // shrd
    opcode(c, 0x0f, 0xad);
    modrm(c, 0xc0, b->low, b->high);

    // sar
    opcode(c, 0xd3, 0xf8 + b->high);

    ResolvedPromise promise(32);
    Assembler::Constant constant(&promise);
    compareCR(c, aSize, &constant, aSize, a);

    opcode(c, 0x0f, 0x8c); //jl
    c->code.append4(2 + 3);

    Assembler::Register bh(b->high);
    moveRR(c, 4, &bh, 4, b); // 2 bytes

    // sar 31,high
    opcode(c, 0xc1, 0xf8 + b->high);
    c->code.append(31);
  } else {
    maybeRex(c, bSize, a, b);
    opcode(c, 0xd3, 0xf8 + regCode(b));
  }
}

void
shiftRightCR(Context* c, unsigned aSize, Assembler::Constant* a,
             unsigned bSize, Assembler::Register* b)
{
  doShift(c, shiftRightRR, 0xf8, aSize, a, bSize, b);
}

void
unsignedShiftRightRR(Context* c, UNUSED unsigned aSize, Assembler::Register* a,
                     unsigned bSize, Assembler::Register* b)
{
  assert(c, a->low == rcx);

  if (BytesPerWord == 4 and bSize == 8) {
    // shrd
    opcode(c, 0x0f, 0xad);
    modrm(c, 0xc0, b->low, b->high);

    // shr
    opcode(c, 0xd3, 0xe8 + b->high);

    ResolvedPromise promise(32);
    Assembler::Constant constant(&promise);
    compareCR(c, aSize, &constant, aSize, a);

    opcode(c, 0x0f, 0x8c); //jl
    c->code.append4(2 + 2);

    Assembler::Register bh(b->high);
    moveRR(c, 4, &bh, 4, b); // 2 bytes
    xorRR(c, 4, &bh, 4, &bh); // 2 bytes
  } else {
    maybeRex(c, bSize, a, b);
    opcode(c, 0xd3, 0xe8 + regCode(b));
  }
}

void
unsignedShiftRightCR(Context* c, unsigned aSize UNUSED, Assembler::Constant* a,
                     unsigned bSize, Assembler::Register* b)
{
  doShift(c, unsignedShiftRightRR, 0xe8, aSize, a, bSize, b);
}

void
populateTables(ArchitectureContext* c)
{
  const OperandType C = ConstantOperand;
  const OperandType A = AddressOperand;
  const OperandType R = RegisterOperand;
  const OperandType M = MemoryOperand;

  OperationType* zo = c->operations;
  UnaryOperationType* uo = c->unaryOperations;
  BinaryOperationType* bo = c->binaryOperations;

  zo[Return] = return_;
  zo[LoadBarrier] = ignore;
  zo[StoreStoreBarrier] = ignore;
  zo[StoreLoadBarrier] = ignore;

  uo[index(Call, C)] = CAST1(callC);
  uo[index(Call, R)] = CAST1(callR);
  uo[index(Call, M)] = CAST1(callM);

  uo[index(AlignedCall, C)] = CAST1(alignedCallC);

  uo[index(LongCall, C)] = CAST1(longCallC);

  uo[index(Jump, R)] = CAST1(jumpR);
  uo[index(Jump, C)] = CAST1(jumpC);
  uo[index(Jump, M)] = CAST1(jumpM);

  uo[index(AlignedJump, C)] = CAST1(alignedJumpC);

  uo[index(JumpIfEqual, C)] = CAST1(jumpIfEqualC);
  uo[index(JumpIfNotEqual, C)] = CAST1(jumpIfNotEqualC);
  uo[index(JumpIfGreater, C)] = CAST1(jumpIfGreaterC);
  uo[index(JumpIfGreaterOrEqual, C)] = CAST1(jumpIfGreaterOrEqualC);
  uo[index(JumpIfLess, C)] = CAST1(jumpIfLessC);
  uo[index(JumpIfLessOrEqual, C)] = CAST1(jumpIfLessOrEqualC);

  uo[index(LongJump, C)] = CAST1(longJumpC);

  bo[index(Negate, R, R)] = CAST2(negateRR);

  bo[index(Move, R, R)] = CAST2(moveRR);
  bo[index(Move, C, R)] = CAST2(moveCR);
  bo[index(Move, M, R)] = CAST2(moveMR);
  bo[index(Move, R, M)] = CAST2(moveRM);
  bo[index(Move, C, M)] = CAST2(moveCM);
  bo[index(Move, A, R)] = CAST2(moveAR);

  bo[index(MoveZ, R, R)] = CAST2(moveZRR);
  bo[index(MoveZ, M, R)] = CAST2(moveZMR);

  bo[index(Compare, R, R)] = CAST2(compareRR);
  bo[index(Compare, C, R)] = CAST2(compareCR);
  bo[index(Compare, C, M)] = CAST2(compareCM);
  bo[index(Compare, R, M)] = CAST2(compareRM);

  bo[index(Add, R, R)] = CAST2(addRR);
  bo[index(Add, C, R)] = CAST2(addCR);

  bo[index(Subtract, C, R)] = CAST2(subtractCR);
  bo[index(Subtract, R, R)] = CAST2(subtractRR);

  bo[index(And, R, R)] = CAST2(andRR);
  bo[index(And, C, R)] = CAST2(andCR);

  bo[index(Or, R, R)] = CAST2(orRR);
  bo[index(Or, C, R)] = CAST2(orCR);

  bo[index(Xor, R, R)] = CAST2(xorRR);
  bo[index(Xor, C, R)] = CAST2(xorCR);

  bo[index(Multiply, R, R)] = CAST2(multiplyRR);
  bo[index(Multiply, C, R)] = CAST2(multiplyCR);

  bo[index(Divide, R, R)] = CAST2(divideRR);

  bo[index(Remainder, R, R)] = CAST2(remainderRR);

  bo[index(LongCompare, C, R)] = CAST2(longCompareCR);
  bo[index(LongCompare, R, R)] = CAST2(longCompareRR);

  bo[index(ShiftLeft, R, R)] = CAST2(shiftLeftRR);
  bo[index(ShiftLeft, C, R)] = CAST2(shiftLeftCR);

  bo[index(ShiftRight, R, R)] = CAST2(shiftRightRR);
  bo[index(ShiftRight, C, R)] = CAST2(shiftRightCR);

  bo[index(UnsignedShiftRight, R, R)] = CAST2(unsignedShiftRightRR);
  bo[index(UnsignedShiftRight, C, R)] = CAST2(unsignedShiftRightCR);
}

class MyArchitecture: public Assembler::Architecture {
 public:
  MyArchitecture(System* system): c(system), referenceCount(0) {
    populateTables(&c);
  }

  virtual unsigned registerCount() {
    return (BytesPerWord == 4 ? 8 : 16);
  }

  virtual int stack() {
    return rsp;
  }

  virtual int thread() {
    return rbx;
  }

  virtual int returnLow() {
    return rax;
  }

  virtual int returnHigh() {
    return (BytesPerWord == 4 ? rdx : NoRegister);
  }

  virtual int virtualCallTarget() {
    return rax;
  }

  virtual int virtualCallIndex() {
    return rdx;
  }

  virtual bool condensedAddressing() {
    return true;
  }

  virtual bool bigEndian() {
    return false;
  }

  virtual bool reserved(int register_) {
    switch (register_) {
    case rbp:
    case rsp:
    case rbx:
      return true;

    default:
      return false;
    }
  }

  virtual unsigned frameFootprint(unsigned footprint) {
#ifdef __MINGW32__
    return max(footprint, StackAlignmentInWords);
#else
    return max(footprint > argumentRegisterCount() ?
               footprint - argumentRegisterCount() : 0,
               StackAlignmentInWords);
#endif
  }

  virtual unsigned argumentFootprint(unsigned footprint) {
    return max(pad(footprint, StackAlignmentInWords), StackAlignmentInWords);
  }

  virtual unsigned argumentRegisterCount() {
#ifdef __MINGW32__
    if (BytesPerWord == 8) return 4; else
#else
    if (BytesPerWord == 8) return 6; else
#endif
    return 0;
  }

  virtual int argumentRegister(unsigned index) {
    assert(&c, BytesPerWord == 8);
    switch (index) {
#ifdef __MINGW32__
    case 0:
      return rcx;
    case 1:
      return rdx;
    case 2:
      return r8;
    case 3:
      return r9;
#else
    case 0:
      return rdi;
    case 1:
      return rsi;
    case 2:
      return rdx;
    case 3:
      return rcx;
    case 4:
      return r8;
    case 5:
      return r9;
#endif
    default:
      abort(&c);
    }
  }

  virtual unsigned stackAlignmentInWords() {
    return StackAlignmentInWords;
  }

  virtual bool matchCall(void* returnAddress, void* target) {
    uint8_t* instruction = static_cast<uint8_t*>(returnAddress) - 5;
    int32_t actualOffset; memcpy(&actualOffset, instruction + 1, 4);
    void* actualTarget = static_cast<uint8_t*>(returnAddress) + actualOffset;

    return *instruction == 0xE8 and actualTarget == target;
  }

  virtual void updateCall(UnaryOperation op, bool assertAlignment UNUSED,
                          void* returnAddress, void* newTarget)
  {
    if (BytesPerWord == 4 or op == Call or op == Jump) {
      uint8_t* instruction = static_cast<uint8_t*>(returnAddress) - 5;
      
      assert(&c, ((op == Call or op == LongCall) and *instruction == 0xE8)
             or ((op == Jump or op == LongJump) and *instruction == 0xE9));

      assert(&c, (not assertAlignment)
             or reinterpret_cast<uintptr_t>(instruction + 1) % 4 == 0);
      
      int32_t v = static_cast<uint8_t*>(newTarget)
        - static_cast<uint8_t*>(returnAddress);
      memcpy(instruction + 1, &v, 4);
    } else {
      uint8_t* instruction = static_cast<uint8_t*>(returnAddress) - 13;

      assert(&c, instruction[0] == 0x49 and instruction[1] == 0xBA);
      assert(&c, instruction[10] == 0x41 and instruction[11] == 0xFF);
      assert(&c, (op == LongCall and instruction[12] == 0xD2)
             or (op == LongJump and instruction[12] == 0xE2));

      assert(&c, (not assertAlignment)
             or reinterpret_cast<uintptr_t>(instruction + 2) % 8 == 0);
      
      memcpy(instruction + 2, &newTarget, 8);
    }
  }

  virtual uintptr_t getConstant(const void* src) {
    uintptr_t v;
    memcpy(&v, src, BytesPerWord);
    return v;
  }

  virtual void setConstant(void* dst, uintptr_t constant) {
    memcpy(dst, &constant, BytesPerWord);
  }

  virtual unsigned alignFrameSize(unsigned sizeInWords) {
    return pad(sizeInWords + FrameHeaderSize, StackAlignmentInWords)
      - FrameHeaderSize;
  }

  virtual void* frameIp(void* stack) {
    return stack ? *static_cast<void**>(stack) : 0;
  }

  virtual unsigned frameHeaderSize() {
    return FrameHeaderSize;
  }

  virtual unsigned frameReturnAddressSize() {
    return 1;
  }

  virtual unsigned frameFooterSize() {
    return 0;
  }

  virtual int returnAddressOffset() {
    return 0;
  }

  virtual int framePointerOffset() {
    return -1;
  }

  virtual void nextFrame(void** stack, void** base) {
    assert(&c, *static_cast<void**>(*base) != *base);

    *stack = static_cast<void**>(*base) + 1;
    *base = *static_cast<void**>(*base);
  }

  virtual void plan
  (UnaryOperation,
   unsigned, uint8_t* aTypeMask, uint64_t* aRegisterMask,
   bool* thunk)
  {
    *aTypeMask = (1 << RegisterOperand) | (1 << MemoryOperand)
      | (1 << ConstantOperand);
    *aRegisterMask = ~static_cast<uint64_t>(0);
    *thunk = false;
  }

  virtual void plan
  (BinaryOperation op,
   unsigned aSize, uint8_t* aTypeMask, uint64_t* aRegisterMask,
   unsigned bSize, uint8_t* bTypeMask, uint64_t* bRegisterMask,
   bool* thunk)
  {
    *aTypeMask = ~0;
    *aRegisterMask = ~static_cast<uint64_t>(0);

    *bTypeMask = (1 << RegisterOperand) | (1 << MemoryOperand);
    *bRegisterMask = ~static_cast<uint64_t>(0);

    *thunk = false;

    switch (op) {
    case Compare:
      *aTypeMask = (1 << RegisterOperand) | (1 << ConstantOperand);
      *bTypeMask = (1 << RegisterOperand);
      break;

    case Negate:
      *aTypeMask = (1 << RegisterOperand);
      *bTypeMask = (1 << RegisterOperand);
      *aRegisterMask = (static_cast<uint64_t>(1) << (rdx + 32))
        | (static_cast<uint64_t>(1) << rax);
      *bRegisterMask = *aRegisterMask;
      break;

    case Move:
      if (BytesPerWord == 4) {
        if (aSize == 4 and bSize == 8) {
          const uint32_t mask = ~((1 << rax) | (1 << rdx));
          *aRegisterMask = (static_cast<uint64_t>(mask) << 32) | mask;
          *bRegisterMask = (static_cast<uint64_t>(1) << (rdx + 32))
            | (static_cast<uint64_t>(1) << rax);        
        } else if (aSize == 1 or bSize == 1) {
          const uint32_t mask
            = (1 << rax) | (1 << rcx) | (1 << rdx) | (1 << rbx);
          *aRegisterMask = (static_cast<uint64_t>(mask) << 32) | mask;
          *bRegisterMask = (static_cast<uint64_t>(mask) << 32) | mask;        
        }
      }
      break;

    default:
      break;
    }
  }

  virtual void plan
  (TernaryOperation op,
   unsigned aSize, uint8_t* aTypeMask, uint64_t* aRegisterMask,
   unsigned, uint8_t* bTypeMask, uint64_t* bRegisterMask,
   unsigned, uint8_t* cTypeMask, uint64_t* cRegisterMask,
   bool* thunk)
  {
    *aTypeMask = (1 << RegisterOperand) | (1 << ConstantOperand);
    *aRegisterMask = ~static_cast<uint64_t>(0);

    *bTypeMask = (1 << RegisterOperand);
    *bRegisterMask = ~static_cast<uint64_t>(0);

    *thunk = false;

    switch (op) {
    case Multiply:
      if (BytesPerWord == 4 and aSize == 8) { 
        const uint32_t mask = ~((1 << rax) | (1 << rdx));
        *aRegisterMask = (static_cast<uint64_t>(mask) << 32) | mask;
        *bRegisterMask = (static_cast<uint64_t>(1) << (rdx + 32)) | mask;
      }
      break;

    case Divide:
      if (BytesPerWord == 4 and aSize == 8) {
        *bTypeMask = ~0;
        *thunk = true;        
      } else {
        *aTypeMask = (1 << RegisterOperand);
        *aRegisterMask = ~((1 << rax) | (1 << rdx));
        *bRegisterMask = 1 << rax;      
      }
      break;

    case Remainder:
      if (BytesPerWord == 4 and aSize == 8) {
        *bTypeMask = ~0;
        *thunk = true;
      } else {
        *aTypeMask = (1 << RegisterOperand);
        *aRegisterMask = ~((1 << rax) | (1 << rdx));
        *bRegisterMask = 1 << rax;      
      }
      break;

    case ShiftLeft:
    case ShiftRight:
    case UnsignedShiftRight: {
      *aRegisterMask = (~static_cast<uint64_t>(0) << 32)
        | (static_cast<uint64_t>(1) << rcx);
      const uint32_t mask = ~(1 << rcx);
      *bRegisterMask = (static_cast<uint64_t>(mask) << 32) | mask;
    } break;

    default:
      break;
    }

    *cTypeMask = *bTypeMask;
    *cRegisterMask = *bRegisterMask;
  }

  virtual void acquire() {
    ++ referenceCount;
  }

  virtual void release() {
    if (-- referenceCount == 0) {
      c.s->free(this);
    }
  }

  ArchitectureContext c;
  unsigned referenceCount;
};

class MyAssembler: public Assembler {
 public:
  MyAssembler(System* s, Allocator* a, Zone* zone, MyArchitecture* arch):
    c(s, a, zone), arch_(arch)
  { }

  virtual void setClient(Client* client) {
    assert(&c, c.client == 0);
    c.client = client;
  }

  virtual Architecture* arch() {
    return arch_;
  }

  virtual void saveFrame(unsigned stackOffset, unsigned baseOffset) {
    Register stack(rsp);
    Memory stackDst(rbx, stackOffset);
    apply(Move, BytesPerWord, RegisterOperand, &stack,
          BytesPerWord, MemoryOperand, &stackDst);

    Register base(rbp);
    Memory baseDst(rbx, baseOffset);
    apply(Move, BytesPerWord, RegisterOperand, &base,
          BytesPerWord, MemoryOperand, &baseDst);
  }

  virtual void pushFrame(unsigned argumentCount, ...) {
    struct {
      unsigned size;
      OperandType type;
      Operand* operand;
    } arguments[argumentCount];
    va_list a; va_start(a, argumentCount);
    unsigned footprint = 0;
    for (unsigned i = 0; i < argumentCount; ++i) {
      arguments[i].size = va_arg(a, unsigned);
      arguments[i].type = static_cast<OperandType>(va_arg(a, int));
      arguments[i].operand = va_arg(a, Operand*);
      footprint += ceiling(arguments[i].size, BytesPerWord);
    }
    va_end(a);

    allocateFrame(arch_->alignFrameSize(footprint));
    
    unsigned offset = 0;
    for (unsigned i = 0; i < argumentCount; ++i) {
      if (i < arch_->argumentRegisterCount()) {
        Register dst(arch_->argumentRegister(i));
        apply(Move,
              arguments[i].size, arguments[i].type, arguments[i].operand,
              pad(arguments[i].size), RegisterOperand, &dst);
      } else {
        Memory dst(rsp, offset * BytesPerWord);
        apply(Move,
              arguments[i].size, arguments[i].type, arguments[i].operand,
              pad(arguments[i].size), MemoryOperand, &dst);
        offset += ceiling(arguments[i].size, BytesPerWord);
      }
    }
  }

  virtual void allocateFrame(unsigned footprint) {
    Register base(rbp);
    pushR(&c, BytesPerWord, &base);

    Register stack(rsp);
    apply(Move, BytesPerWord, RegisterOperand, &stack,
          BytesPerWord, RegisterOperand, &base);

    Constant footprintConstant(resolved(&c, footprint * BytesPerWord));
    apply(Subtract, BytesPerWord, ConstantOperand, &footprintConstant,
          BytesPerWord, RegisterOperand, &stack,
          BytesPerWord, RegisterOperand, &stack);
  }

  virtual void adjustFrame(unsigned footprint) {
    Register stack(rsp);
    Constant footprintConstant(resolved(&c, footprint * BytesPerWord));
    apply(Subtract, BytesPerWord, ConstantOperand, &footprintConstant,
          BytesPerWord, RegisterOperand, &stack,
          BytesPerWord, RegisterOperand, &stack);
  }

  virtual void popFrame() {
    Register base(rbp);
    Register stack(rsp);
    apply(Move, BytesPerWord, RegisterOperand, &base,
          BytesPerWord, RegisterOperand, &stack);

    popR(&c, BytesPerWord, &base);
  }

  virtual void popFrameForTailCall(unsigned footprint,
                                   int offset,
                                   int returnAddressSurrogate,
                                   int framePointerSurrogate)
  {
    if (TailCalls) {
      if (offset) {
        Register tmp(c.client->acquireTemporary());
      
        Memory returnAddressSrc(rsp, (footprint + 1) * BytesPerWord);
        moveMR(&c, BytesPerWord, &returnAddressSrc, BytesPerWord, &tmp);
    
        Memory returnAddressDst(rsp, (footprint - offset + 1) * BytesPerWord);
        moveRM(&c, BytesPerWord, &tmp, BytesPerWord, &returnAddressDst);

        c.client->releaseTemporary(tmp.low);

        Memory baseSrc(rsp, footprint * BytesPerWord);
        Register base(rbp);
        moveMR(&c, BytesPerWord, &baseSrc, BytesPerWord, &base);

        Register stack(rsp);
        Constant footprintConstant
          (resolved(&c, (footprint - offset + 1) * BytesPerWord));
        addCR(&c, BytesPerWord, &footprintConstant, BytesPerWord, &stack);

        if (returnAddressSurrogate != NoRegister) {
          assert(&c, offset > 0);

          Register ras(returnAddressSurrogate);
          Memory dst(rsp, offset * BytesPerWord);
          moveRM(&c, BytesPerWord, &ras, BytesPerWord, &dst);
        }

        if (framePointerSurrogate != NoRegister) {
          assert(&c, offset > 0);

          Register fps(framePointerSurrogate);
          Memory dst(rsp, (offset - 1) * BytesPerWord);
          moveRM(&c, BytesPerWord, &fps, BytesPerWord, &dst);
        }
      } else {
        popFrame();
      }
    } else {
      abort(&c);
    }
  }

  virtual void popFrameAndPopArgumentsAndReturn(unsigned argumentFootprint) {
    popFrame();

    assert(&c, argumentFootprint >= StackAlignmentInWords);
    assert(&c, (argumentFootprint % StackAlignmentInWords) == 0);

    if (TailCalls and argumentFootprint > StackAlignmentInWords) {
      Register returnAddress(rcx);
      popR(&c, BytesPerWord, &returnAddress);

      Register stack(rsp);
      Constant adjustment
        (resolved(&c, (argumentFootprint - StackAlignmentInWords)
                  * BytesPerWord));
      addCR(&c, BytesPerWord, &adjustment, BytesPerWord, &stack);

      jumpR(&c, BytesPerWord, &returnAddress);
    } else {
      return_(&c);
    }
  }

  virtual void popFrameAndUpdateStackAndReturn(unsigned stackOffsetFromThread)
  {
    popFrame();

    Register returnAddress(rcx);
    popR(&c, BytesPerWord, &returnAddress);

    Register stack(rsp);
    Memory stackSrc(rbx, stackOffsetFromThread);
    moveMR(&c, BytesPerWord, &stackSrc, BytesPerWord, &stack);

    jumpR(&c, BytesPerWord, &returnAddress);
  }

  virtual void apply(Operation op) {
    arch_->c.operations[op](&c);
  }

  virtual void apply(UnaryOperation op,
                     unsigned aSize, OperandType aType, Operand* aOperand)
  {
    arch_->c.unaryOperations[index(op, aType)](&c, aSize, aOperand);
  }

  virtual void apply(BinaryOperation op,
                     unsigned aSize, OperandType aType, Operand* aOperand,
                     unsigned bSize, OperandType bType, Operand* bOperand)
  {
    arch_->c.binaryOperations[index(op, aType, bType)]
      (&c, aSize, aOperand, bSize, bOperand);
  }

  virtual void apply(TernaryOperation op,
                     unsigned aSize, OperandType aType, Operand* aOperand,
                     unsigned bSize, OperandType bType, Operand* bOperand,
                     unsigned cSize UNUSED, OperandType cType UNUSED,
                     Operand*)
  {
    assert(&c, bSize == cSize);
    assert(&c, bType == cType);

    arch_->c.binaryOperations[index(op, aType, bType)]
      (&c, aSize, aOperand, bSize, bOperand);
  }

  virtual void writeTo(uint8_t* dst) {
    c.result = dst;
    
    for (MyBlock* b = c.firstBlock; b; b = b->next) {
      unsigned index = 0;
      unsigned padding = 0;
      for (AlignmentPadding* p = b->firstPadding; p; p = p->next) {
        unsigned size = p->offset - b->offset - index;

        memcpy(dst + b->start + index + padding,
               c.code.data + b->offset + index,
               size);

        index += size;

        while ((b->start + index + padding + 1) % 4) {
          *(dst + b->start + index + padding) = 0x90;
          ++ padding;
        }
      }

      memcpy(dst + b->start + index + padding,
             c.code.data + b->offset + index,
             b->size - index);
    }
    
    for (Task* t = c.tasks; t; t = t->next) {
      t->run(&c);
    }
  }

  virtual Promise* offset() {
    return ::offset(&c);
  }

  virtual Block* endBlock(bool startNew) {
    MyBlock* b = c.lastBlock;
    b->size = c.code.length() - b->offset;
    if (startNew) {
      c.lastBlock = new (c.zone->allocate(sizeof(MyBlock)))
        MyBlock(c.code.length());
    } else {
      c.lastBlock = 0;
    }
    return b;
  }

  virtual unsigned length() {
    return c.code.length();
  }

  virtual void dispose() {
    c.code.dispose();
  }

  Context c;
  MyArchitecture* arch_;
};

} // namespace

namespace vm {

Assembler::Architecture*
makeArchitecture(System* system)
{
  return new (allocate(system, sizeof(MyArchitecture))) MyArchitecture(system);
}

Assembler*
makeAssembler(System* system, Allocator* allocator, Zone* zone,
              Assembler::Architecture* architecture)
{
  return new (zone->allocate(sizeof(MyAssembler)))
    MyAssembler(system, allocator, zone,
                static_cast<MyArchitecture*>(architecture));
}


} // namespace vm

#endif //(defined __i386__) || (defined __x86_64__)
