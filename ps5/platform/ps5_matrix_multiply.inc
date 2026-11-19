/* Native common path for the lifted 4x4 transform at Xbox VA 0x001E522D.
 * Keep the guest ABI and the x87 stack values left by the lifted loop. */
static int matrix_guest_ram(uint32_t address)
{
    return address >= 0x10000u && address <= 0x04000000u - 64u &&
           !(address & 3u);
}

static int matrix_overlap(uint32_t a, uint32_t b)
{
    return a < b + 64u && b < a + 64u;
}

static int oogie_native_matrix(OogieGuestRegisters *regs)
{
#pragma clang fp contract(off)
    const uint32_t sp = regs->reg_esp;
    if (sp < 0x10050u || sp > 0x04000000u - 16u || (sp & 3u) ||
        !(regs->reg_fp_control_word & 0x300u))
        return 0;

    const uint32_t dst = MEM32(sp + 4);
    const uint32_t left = MEM32(sp + 8);
    const uint32_t right = MEM32(sp + 12);
    if (!matrix_guest_ram(dst) || !matrix_guest_ram(left) || !matrix_guest_ram(right) ||
        (dst == left && dst == right) ||
        (dst != left && matrix_overlap(dst, left)) ||
        (dst != right && matrix_overlap(dst, right)))
        return 0;

    /* Preserve the original path for operands overlapping the saved frame. */
    const uint32_t low = sp - 80u, high = sp + 16u;
    if ((dst < high && dst + 64u > low) ||
        (left < high && left + 64u > low) ||
        (right < high && right + 64u > low))
        return 0;

    float lhs[4][4], rhs[4][4], out[4][4];
    memcpy(lhs, (const void *)XBOX_PTR(left), sizeof lhs);
    memcpy(rhs, (const void *)XBOX_PTR(right), sizeof rhs);

    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            if (!isfinite(lhs[row][col]) || !isfinite(rhs[row][col]))
                return 0;

    MEM32(sp - 4) = regs->reg_ebp;
    MEM32(sp - 72) = regs->reg_ebx;
    MEM32(sp - 76) = regs->reg_esi;
    MEM32(sp - 80) = regs->reg_edi;

    double final_q[4] = {0};
    double final_sum = 0.0, final_pair = 0.0, final_p0 = 0.0, final_p3 = 0.0;
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            double p0 = (double)lhs[row][0] * rhs[0][column];
            double p1 = (double)lhs[row][1] * rhs[1][column];
            double p2 = (double)lhs[row][2] * rhs[2][column];
            double p3 = (double)lhs[row][3] * rhs[3][column];
            double pair = p1 + p3;
            double sum = (p2 + p0) + pair;
            out[row][column] = (float)sum;
            if (row == 3 && column == 3) {
                for (unsigned k = 0; k < 4; ++k)
                    final_q[k] = dst == right ? rhs[k][3] : lhs[3][k];
                final_p0 = p0;
                final_p3 = p3;
                final_pair = pair;
                final_sum = sum;
            }
        }
    }
    memcpy((void *)XBOX_PTR(dst), out, sizeof out);

    unsigned top = (unsigned)regs->reg_fp_top;
    regs->reg_fp_stack[(top + 0u) & 7u] = final_q[3];
    regs->reg_fp_stack[(top + 1u) & 7u] = final_q[2];
    regs->reg_fp_stack[(top + 2u) & 7u] = final_q[1];
    regs->reg_fp_stack[(top + 3u) & 7u] = final_q[0];
    regs->reg_fp_stack[(top + 4u) & 7u] = final_p0;
    regs->reg_fp_stack[(top + 5u) & 7u] = final_p3;
    regs->reg_fp_stack[(top + 6u) & 7u] = final_pair;
    regs->reg_fp_stack[(top + 7u) & 7u] = final_sum;
    regs->reg_eax = dst;
    regs->reg_ecx = dst == right ? left : left + 64u;
    regs->reg_edx = right;
    regs->reg_ebp = regs->reg_seh_ebp = sp - 4u;
    regs->reg_esp = sp + 16u;
    return 1;
}

