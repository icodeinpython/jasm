# jasm — A Minimal AMD64 Assembler in C

**jasm** is a lightweight AMD64 assembler written in C.  
It parses x86-64 assembly instructions and encodes them into machine code.  
The project focuses on learning, simplicity, and correctness — providing a clear look into how real assemblers like `nasm` or `gas` translate human-readable assembly into binary opcodes.

---

## 🧩 Features

- **Register Mapping**
  - Supports 8-bit, 16-bit, 32-bit, and 64-bit general-purpose registers (`%al`, `%ax`, `%eax`, `%rax`, etc.).
- **ModR/M Byte Encoding**
  - Correctly constructs the `ModR/M` byte for register-to-register and (soon) memory operands.
- **Instruction Node System**
  - Uses internal AST-style nodes (`NODE_LABEL`, `NODE_DIRECTIVE`, `NODE_INSTRUCTION`) for future expansion.
- **Scalable Design**
  - Built to easily extend with instruction encoders, SIB byte support, displacement encoding, and relocation handling.

---

## 🧠 Example

```c
uint8_t modrm = encode_modrm_reg("%rax", "%rbx");
// mov rax, rbx  → ModR/M = 0xD8
printf("ModR/M byte: 0x%02x\n", modrm);