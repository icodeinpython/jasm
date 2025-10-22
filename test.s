	movl %eax, %ebx
	movl %ebx, %ecx
	movl %ecx, %edx
	movl %edx, %esi
	movl %esi, %edi
	movl %edi, %ebp
	movl %ebp, %esp
	movl %esp, %eax

############################################################
# 64-BIT INTER-REGISTER MOVs
############################################################
	movq %rax, %rbx
	movq %rbx, %rcx
	movq %rcx, %rdx
	movq %rdx, %rsi
	movq %rsi, %rdi
	movq %rdi, %rbp
	movq %rbp, %rsp
	movq %rsp, %rax

############################################################
# EXTENDED REGISTERS (r8–r15)
############################################################
	# 8-bit extended
	movb %r8b, %r9b
	movb %r9b, %r10b
	movb %r10b, %r11b
	movb %r11b, %r12b
	movb %r12b, %r13b
	movb %r13b, %r14b
	movb %r14b, %r15b
	movb %r15b, %r8b

	# 16-bit extended
	movw %r8w, %r9w
	movw %r9w, %r10w
	movw %r10w, %r11w
	movw %r11w, %r12w
	movw %r12w, %r13w
	movw %r13w, %r14w
	movw %r14w, %r15w
	movw %r15w, %r8w

	# 32-bit extended
	movl %r8d, %r9d
	movl %r9d, %r10d
	movl %r10d, %r11d
	movl %r11d, %r12d
	movl %r12d, %r13d
	movl %r13d, %r14d
	movl %r14d, %r15d
	movl %r15d, %r8d

	# 64-bit extended
	movq %r8, %r9
	movq %r9, %r10
	movq %r10, %r11
	movq %r11, %r12
	movq %r12, %r13
	movq %r13, %r14
	movq %r14, %r15
	movq %r15, %r8
