// ARM32 is a bit special from the linker's viewpoint because ARM
// processors support two different instruction encodings: Thumb and
// ARM (in a narrower sense). Thumb instructions are either 16 bits or
// 32 bits, while ARM instructions are all 32 bits. Feature-wise,
// thumb is a subset of ARM, so not all ARM instructions are
// representable in Thumb.
//
// ARM processors originally supported only ARM instructions. Thumb
// instructions were later added to increase code density.
//
// ARM processors runs in either ARM mode or Thumb mode. The mode can
// be switched using BX (branch and mode exchange)-family instructions.
// We need to use that instructions to, for example, call a function
// encoded in Thumb from a function encoded in ARM. Sometimes, the
// linker even has to emit an interworking thunk code to switch mode.
//
// ARM instructions are aligned to 4 byte boundaries. Thumb are to 2
// byte boundaries.
//
// You can distinguish Thumb functions from ARM functions by looking
// at the least significant bit (LSB) of its "address". If LSB is 0,
// it's ARM; otherwise, Thumb.
//
// For example, if a symbol `foo` is of type STT_FUNC and has value
// 0x2001, `foo` is a function using Thumb instructions whose address
// is 0x2000 (not 0x2001, as Thumb instructions are always 2-byte
// aligned). Likewise, if a function pointer has value 0x2001, it
// refers a Thumb function at 0x2000.
//
// https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst

#include "mold.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/parallel_sort.h>

namespace mold::elf {

using E = ARM32;

static void write_mov_imm(u8 *loc, u32 val) {
  u32 imm12 = bits(val, 11, 0);
  u32 imm4 = bits(val, 15, 12);
  *(ul32 *)loc = (*(ul32 *)loc & 0xfff0f000) | (imm4 << 16) | imm12;
}

static void write_thm_b_imm(u8 *loc, u32 val) {
  // https://developer.arm.com/documentation/ddi0406/cb/Application-Level-Architecture/Instruction-Details/Alphabetical-list-of-instructions/BL--BLX--immediate-
  u32 sign = bit(val, 24);
  u32 I1 = bit(val, 23);
  u32 I2 = bit(val, 22);
  u32 J1 = !I1 ^ sign;
  u32 J2 = !I2 ^ sign;
  u32 imm10 = bits(val, 21, 12);
  u32 imm11 = bits(val, 11, 1);

  ul16 *buf = (ul16 *)loc;
  buf[0] = (buf[0] & 0b1111'1000'0000'0000) | (sign << 10) | imm10;
  buf[1] = (buf[1] & 0b1101'0000'0000'0000) | (J1 << 13) | (J2 << 11) | imm11;
}

static void write_thm_mov_imm(u8 *loc, u32 val) {
  // https://developer.arm.com/documentation/ddi0406/cb/Application-Level-Architecture/Instruction-Details/Alphabetical-list-of-instructions/MOVT
  u32 imm4 = bits(val, 15, 12);
  u32 i = bit(val, 11);
  u32 imm3 = bits(val, 10, 8);
  u32 imm8 = bits(val, 7, 0);

  ul16 *buf = (ul16 *)loc;
  buf[0] = (buf[0] & 0b1111'1011'1111'0000) | (i << 10) | imm4;
  buf[1] = (buf[1] & 0b1000'1111'0000'0000) | (imm3 << 12) | imm8;
}

template <>
void write_plt_header(Context<E> &ctx, u8 *buf) {
  static const ul32 insn[] = {
    0xe52d'e004, // push {lr}
    0xe59f'e004, // ldr lr, 2f
    0xe08f'e00e, // 1: add lr, pc, lr
    0xe5be'f008, // ldr pc, [lr, #8]!
    0x0000'0000, // 2: .word .got.plt - 1b - 8
    0xe320'f000, // nop
    0xe320'f000, // nop
    0xe320'f000, // nop
  };

  memcpy(buf, insn, sizeof(insn));
  *(ul32 *)(buf + 16) = ctx.gotplt->shdr.sh_addr - ctx.plt->shdr.sh_addr - 16;
}

static const ul32 plt_entry[] = {
  0xe59f'c004, // 1: ldr ip, 2f
  0xe08c'c00f, // add ip, ip, pc
  0xe59c'f000, // ldr pc, [ip]
  0x0000'0000, // 2: .word sym@GOT - 1b
};

template <>
void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  memcpy(buf, plt_entry, sizeof(plt_entry));
  *(ul32 *)(buf + 12) = sym.get_gotplt_addr(ctx) - sym.get_plt_addr(ctx) - 12;
}

template <>
void write_pltgot_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  memcpy(buf, plt_entry, sizeof(plt_entry));
  *(ul32 *)(buf + 12) = sym.get_got_addr(ctx) - sym.get_plt_addr(ctx) - 12;
}

// ARM does not use .eh_frame for exception handling. Instead, it uses
// .ARM.exidx and .ARM.extab. So this function is empty.
template <>
void EhFrameSection<E>::apply_reloc(Context<E> &ctx, const ElfRel<E> &rel,
                                    u64 offset, u64 val) {}

// ARM and Thumb branch instructions can jump within ±16 MiB.
static bool is_jump_reachable(i64 val) {
  return sign_extend(val, 24) == val;
}

template <>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  ElfRel<E> *dynrel = nullptr;
  if (ctx.reldyn)
    dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                           file.reldyn_offset + this->reldyn_offset);

  i64 trampoline_idx = 0;

  auto get_trampoline_addr = [&](u64 addr) {
    for (; trampoline_idx < output_section->thunks.size(); trampoline_idx++) {
      RangeExtensionThunk<E> &thunk = *output_section->thunks[trampoline_idx];
      i64 disp = output_section->shdr.sh_addr + thunk.offset - addr;
      if (is_jump_reachable(disp))
        return disp;
    }
    unreachable();
  };

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE || rel.r_type == R_ARM_V4BX)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

#define S   sym.get_addr(ctx)
#define A   this->get_addend(rel)
#define P   (get_addr() + rel.r_offset)
#define T   (sym.get_addr(ctx) & 1)
#define G   (sym.get_got_idx(ctx) * sizeof(Word<E>))
#define GOT ctx.got->shdr.sh_addr

    auto get_thumb_thunk_addr = [&] {
      RangeExtensionRef ref = extra.range_extn[i];
      assert(ref.thunk_idx != -1);
      u64 addr = output_section->thunks[ref.thunk_idx]->get_addr(ref.sym_idx);
      assert(is_jump_reachable(addr + A - P));
      return addr;
    };

    auto get_arm_thunk_addr = [&] {
      return get_thumb_thunk_addr() + 4;
    };

    switch (rel.r_type) {
    case R_ARM_ABS32:
    case R_ARM_TARGET1:
      apply_dyn_absrel(ctx, sym, rel, loc, S, A, P, dynrel);
      break;
    case R_ARM_REL32:
      *(ul32 *)loc = S + A - P;
      break;
    case R_ARM_THM_CALL: {
      // THM_CALL relocation refers either BL or BLX instruction.
      // They are different in only one bit. We need to use BL if
      // the jump target is Thumb. Otherwise, use BLX.
      if (sym.is_remaining_undef_weak()) {
        // On ARM, calling an weak undefined symbol jumps to the
        // next instruction.
        *(ul32 *)loc = 0x8000'f3af; // NOP.W
        break;
      }

      i64 val = S + A - P;
      if (is_jump_reachable(val)) {
        if (T) {
          write_thm_b_imm(loc, val);
          *(ul16 *)(loc + 2) |= 0x1000;  // rewrite to BL
        } else {
          write_thm_b_imm(loc, align_to(val, 4));
          *(ul16 *)(loc + 2) &= ~0x1000; // rewrite to BLX
        }
      } else {
        write_thm_b_imm(loc, align_to(get_arm_thunk_addr() + A - P, 4));
        *(ul16 *)(loc + 2) &= ~0x1000;  // rewrite to BLX
      }
      break;
    }
    case R_ARM_BASE_PREL:
      *(ul32 *)loc = GOT + A - P;
      break;
    case R_ARM_GOT_PREL:
    case R_ARM_TARGET2:
      *(ul32 *)loc = GOT + G + A - P;
      break;
    case R_ARM_GOT_BREL:
      *(ul32 *)loc = G + A;
      break;
    case R_ARM_CALL: {
      // Just like THM_CALL, ARM_CALL relocation refers either BL or
      // BLX instruction. We may need to rewrite BL → BLX or BLX → BL.
      bool is_bl = ((*(ul32 *)loc & 0xff00'0000) == 0xeb00'0000);
      bool is_blx = ((*(ul32 *)loc & 0xfe00'0000) == 0xfa00'0000);
      if (!is_bl && !is_blx)
        Fatal(ctx) << *this << ": R_ARM_CALL refers neither BL nor BLX";

      if (sym.is_remaining_undef_weak()) {
        // On ARM, calling an weak undefined symbol jumps to the
        // next instruction.
        *(ul32 *)loc = 0xe320'f000; // NOP
        break;
      }

      u64 val = S + A - P;
      if (is_jump_reachable(val)) {
        if (T)
          *(ul32 *)loc = 0xfa00'0000 | (bit(val, 1) << 24) | bits(val, 25, 2);
        else
          *(ul32 *)loc = 0xeb00'0000 | bits(val, 25, 2);
      } else {
        *(ul32 *)loc = 0xeb00'0000 | bits(get_arm_thunk_addr() + A - P, 25, 2);
      }
      break;
    }
    case R_ARM_JUMP24:
      if (sym.is_remaining_undef_weak()) {
        *(ul32 *)loc = 0xe320'f000; // NOP
      } else {
        // Unlike BL and BLX, we can't rewrite B to BX because BX doesn't
        // takes an immediate; it takes only a register. So if mode switch
        // is required, we jump to a linker-synthesized thunk which constructs
        // a branch destination in a register and branches to that address.
        u64 val = S + A - P;
        if (!is_jump_reachable(val) || T)
          val = get_arm_thunk_addr() + A - P;
        *(ul32 *)loc = (*(ul32 *)loc & 0xff00'0000) | bits(val, 25, 2);
      }
      break;
    case R_ARM_THM_JUMP11:
      assert(T);
      *(ul16 *)loc = (*(ul16 *)loc & 0xf800) | bits(S + A - P, 11, 1);
      break;
    case R_ARM_THM_JUMP19: {
      i64 val = S + A - P;

      // sign:J2:J1:imm6:imm11:'0'
      u32 sign = bit(val, 20);
      u32 J2 = bit(val, 19);
      u32 J1 = bit(val, 18);
      u32 imm6 = bits(val, 17, 12);
      u32 imm11 = bits(val, 11, 1);

      *(ul16 *)loc &= 0b1111'1011'1100'0000;
      *(ul16 *)loc |= (sign << 10) | imm6;

      *(ul16 *)(loc + 2) &= 0b1101'0000'0000'0000;
      *(ul16 *)(loc + 2) |= (J2 << 13) | (J1 << 11) | imm11;
      break;
    }
    case R_ARM_THM_JUMP24:
      if (sym.is_remaining_undef_weak()) {
        *(ul32 *)loc = 0x8000'f3af; // NOP.W
      } else {
        // Just like R_ARM_JUMP24, we need to jump to a thunk if we need to
        // switch processor mode.
        u64 val = S + A - P;
        if (!is_jump_reachable(val) || !T)
          val = get_thumb_thunk_addr() + A - P;
        write_thm_b_imm(loc, val);
      }
      break;
    case R_ARM_MOVW_PREL_NC:
      write_mov_imm(loc, ((S + A) | T) - P);
      break;
    case R_ARM_MOVW_ABS_NC:
      write_mov_imm(loc, (S + A) | T);
      break;
    case R_ARM_THM_MOVW_PREL_NC:
      write_thm_mov_imm(loc, ((S + A) | T) - P);
      break;
    case R_ARM_PREL31:
      *(ul32 *)loc = (*(ul32 *)loc & 0x8000'0000) | ((S + A - P) & 0x7fff'ffff);
      break;
    case R_ARM_THM_MOVW_ABS_NC:
      write_thm_mov_imm(loc, (S + A) | T);
      break;
    case R_ARM_MOVT_PREL:
      write_mov_imm(loc, (S + A - P) >> 16);
      break;
    case R_ARM_THM_MOVT_PREL:
      write_thm_mov_imm(loc, (S + A - P) >> 16);
      break;
    case R_ARM_MOVT_ABS:
      write_mov_imm(loc, (S + A) >> 16);
      break;
    case R_ARM_THM_MOVT_ABS:
      write_thm_mov_imm(loc, (S + A) >> 16);
      break;
    case R_ARM_TLS_GD32:
      *(ul32 *)loc = sym.get_tlsgd_addr(ctx) + A - P;
      break;
    case R_ARM_TLS_LDM32:
      *(ul32 *)loc = ctx.got->get_tlsld_addr(ctx) + A - P;
      break;
    case R_ARM_TLS_LDO32:
      *(ul32 *)loc = S + A - ctx.tls_begin;
      break;
    case R_ARM_TLS_IE32:
      *(ul32 *)loc = sym.get_gottp_addr(ctx) + A - P;
      break;
    case R_ARM_TLS_LE32:
      *(ul32 *)loc = S + A - ctx.tp_addr;
      break;
    case R_ARM_TLS_GOTDESC:
      if (sym.has_tlsdesc(ctx)) {
        // A is odd if the corresponding TLS_CALL is Thumb.
        if (A & 1)
          *(ul32 *)loc = sym.get_tlsdesc_addr(ctx) - P + A - 6;
        else
          *(ul32 *)loc = sym.get_tlsdesc_addr(ctx) - P + A - 4;
      } else {
        *(ul32 *)loc = S - ctx.tp_addr;
      }
      break;
    case R_ARM_TLS_CALL:
      if (sym.has_tlsdesc(ctx)) {
        // BL <tls_trampoline>
        *(ul32 *)loc = 0xeb00'0000 | bits(get_trampoline_addr(P + 8), 25, 2);
      } else {
        // BL -> NOP
        *(ul32 *)loc = 0xe320'f000;
      }
      break;
    case R_ARM_THM_TLS_CALL:
      if (sym.has_tlsdesc(ctx)) {
        u64 val = align_to(get_trampoline_addr(P + 4), 4);
        write_thm_b_imm(loc, val);
        *(ul16 *)(loc + 2) &= ~0x1000; // rewrite BL with BLX
      } else {
        // BL -> NOP.W
        *(ul32 *)loc = 0x8000'f3af;
      }
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }

#undef S
#undef A
#undef P
#undef T
#undef G
#undef GOT
  }
}

template <>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    if (!sym.file) {
      record_undef_error(ctx, rel);
      continue;
    }

    SectionFragment<E> *frag;
    i64 frag_addend;
    std::tie(frag, frag_addend) = get_fragment(ctx, rel);

#define S (frag ? frag->get_addr(ctx) : sym.get_addr(ctx))
#define A (frag ? frag_addend : this->get_addend(rel))

    switch (rel.r_type) {
    case R_ARM_ABS32:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(ul32 *)loc = *val;
      else
        *(ul32 *)loc = S + A;
      break;
    case R_ARM_TLS_LDO32:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(ul32 *)loc = *val;
      else
        *(ul32 *)loc = S + A - ctx.tls_begin;
      break;
    default:
      Fatal(ctx) << *this << ": invalid relocation for non-allocated sections: "
                 << rel;
      break;
    }

#undef S
#undef A
  }
}

template <>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr().sh_flags & SHF_ALLOC);

  this->reldyn_offset = file.num_dynrel * sizeof(ElfRel<E>);
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  // Scan relocations
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    if (!sym.file) {
      record_undef_error(ctx, rel);
      continue;
    }

    if (sym.is_ifunc())
      sym.flags |= (NEEDS_GOT | NEEDS_PLT);

    switch (rel.r_type) {
    case R_ARM_ABS32:
    case R_ARM_MOVT_ABS:
    case R_ARM_THM_MOVT_ABS:
    case R_ARM_TARGET1:
      scan_rel(ctx, sym, rel, dyn_absrel_table);
      break;
    case R_ARM_THM_CALL:
    case R_ARM_CALL:
    case R_ARM_JUMP24:
    case R_ARM_THM_JUMP24:
      if (sym.is_imported)
        sym.flags |= NEEDS_PLT;
      break;
    case R_ARM_GOT_PREL:
    case R_ARM_GOT_BREL:
    case R_ARM_TARGET2:
      sym.flags |= NEEDS_GOT;
      break;
    case R_ARM_MOVT_PREL:
    case R_ARM_THM_MOVT_PREL:
    case R_ARM_PREL31:
      scan_rel(ctx, sym, rel, pcrel_table);
      break;
    case R_ARM_TLS_GD32:
      sym.flags |= NEEDS_TLSGD;
      break;
    case R_ARM_TLS_LDM32:
      ctx.needs_tlsld = true;
      break;
    case R_ARM_TLS_IE32:
      sym.flags |= NEEDS_GOTTP;
      break;
    case R_ARM_TLS_GOTDESC:
      if (!relax_tlsdesc(ctx, sym))
        sym.flags |= NEEDS_TLSDESC;
      break;
    case R_ARM_REL32:
    case R_ARM_BASE_PREL:
    case R_ARM_THM_JUMP11:
    case R_ARM_THM_JUMP19:
    case R_ARM_MOVW_PREL_NC:
    case R_ARM_MOVW_ABS_NC:
    case R_ARM_THM_MOVW_PREL_NC:
    case R_ARM_THM_MOVW_ABS_NC:
    case R_ARM_TLS_LDO32:
    case R_ARM_TLS_LE32:
    case R_ARM_TLS_CALL:
    case R_ARM_THM_TLS_CALL:
    case R_ARM_V4BX:
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

template <>
void RangeExtensionThunk<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + output_section.shdr.sh_offset + offset;

  // TLS trampoline code. ARM32's TLSDESC is designed so that this
  // common piece of code is factored out from object files to reduce
  // output size. Since no one provide, the linker has to synthesize it.
  static ul32 hdr[] = {
    0xe08e'0000, // add r0, lr, r0
    0xe590'1004, // ldr r1, [r0, #4]
    0xe12f'ff11, // bx  r1
  };

  // This is a range extension and mode switch thunk.
  // It has two entry points: +0 for Thumb and +4 for ARM.
  const u8 entry[] = {
    // .thumb
    0xfc, 0x46,             //    mov  ip, pc
    0x60, 0x47,             //    bx   ip  # jumps to the following `ldr` insn
    // .arm
    0x04, 0xc0, 0x9f, 0xe5, //    ldr  ip, 2f
    0x0f, 0xc0, 0x8c, 0xe0, // 1: add  ip, ip, pc
    0x1c, 0xff, 0x2f, 0xe1, //    bx   ip
    0x00, 0x00, 0x00, 0x00, // 2: .word sym - 1b
  };

  static_assert(E::thunk_hdr_size == sizeof(hdr));
  static_assert(E::thunk_size == sizeof(entry));

  memcpy(buf, hdr, sizeof(hdr));

  for (i64 i = 0; i < symbols.size(); i++) {
    u8 *loc = buf + sizeof(hdr) + i * sizeof(entry);
    memcpy(loc, entry, sizeof(entry));

    u64 S = symbols[i]->get_addr(ctx);
    u64 P = output_section.shdr.sh_addr + offset + sizeof(hdr) + i * sizeof(entry);
    *(ul32 *)(loc + 16) = S - P - 16;
  }
}

template <typename E>
static OutputSection<E> *find_exidx_section(Context<E> &ctx) {
  for (std::unique_ptr<OutputSection<E>> &osec : ctx.output_sections)
    if (osec->shdr.sh_type == SHT_ARM_EXIDX)
      return osec.get();
  return nullptr;
}

// ARM executables use an .ARM.exidx section to look up an exception
// handling record for the current instruction pointer. The table needs
// to be sorted by their addresses.
//
// Other target uses .eh_frame_hdr instead for the same purpose.
// I don't know why only ARM uses the different mechanism, but it's
// likely that it's due to some historical reason.
//
// This function sorts .ARM.exidx records.
void sort_arm_exidx(Context<E> &ctx) {
  Timer t(ctx, "sort_arm_exidx");

  OutputSection<E> *osec = find_exidx_section(ctx);
  if (!osec)
    return;

  // .ARM.exidx records consists of a signed 31-bit relative address
  // and a 32-bit value. The relative address indicates the start
  // address of a function that the record covers. The value is one of
  // the followings:
  //
  // 1. CANTUNWIND indicating that there's no unwinding info for the function,
  // 2. a compact unwinding record encoded into a 32-bit value, or
  // 3. a 31-bit relative address which points to a larger record in
  //    the .ARM.extab section.
  //
  // CANTUNWIND is value 1. The most significant bit is set in (2) but
  // not in (3). So we can distinguished them just by looking at a value.
  const u32 EXIDX_CANTUNWIND = 1;

  struct Entry {
    ul32 addr;
    ul32 val;
  };

  if (osec->shdr.sh_size % sizeof(Entry))
    Fatal(ctx) << "invalid .ARM.exidx section size";

  Entry *ent = (Entry *)(ctx.buf + osec->shdr.sh_offset);
  i64 num_entries = osec->shdr.sh_size / sizeof(Entry);

  // Entry's addresses are relative to themselves. In order to sort
  // records by addresses, we first translate them so that the addresses
  // are relative to the beginning of the section.
  auto is_relative = [](u32 val) {
    return val != EXIDX_CANTUNWIND && !(val & 0x8000'0000);
  };

  tbb::parallel_for((i64)0, num_entries, [&](i64 i) {
    i64 offset = sizeof(Entry) * i;
    ent[i].addr = sign_extend(ent[i].addr, 30) + offset;
    if (is_relative(ent[i].val))
      ent[i].val = 0x7fff'ffff & (ent[i].val + offset);
  });

  tbb::parallel_sort(ent, ent + num_entries, [](const Entry &a, const Entry &b) {
    return a.addr < b.addr;
  });

  // Make addresses relative to themselves.
  tbb::parallel_for((i64)0, num_entries, [&](i64 i) {
    i64 offset = sizeof(Entry) * i;
    ent[i].addr = 0x7fff'ffff & (ent[i].addr - offset);
    if (is_relative(ent[i].val))
      ent[i].val = 0x7fff'ffff & (ent[i].val - offset);
  });
}

} // namespace mold::elf
