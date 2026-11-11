/* The methods Kernel defines private (CRuby's Kernel.private_instance_methods),
   one X row each: the one list the compiler's kernel_private_method
   (builtin_names.c) and the runtime's sp_nomethod_kernel_private read, for the
   "private method 'select' called for ..." an explicit receiver gets.
   tools/gen_builtin_arity_spec.rb --check holds every name to CRuby's list. */
#ifndef SP_KERNEL_PRIVATE_H
#define SP_KERNEL_PRIVATE_H

#define SP_KERNEL_PRIVATE(X) \
 X("Array") X("Complex") X("Float") X("Hash") X("Integer") \
 X("Rational") X("String") X("abort") X("at_exit") X("autoload") \
 X("autoload?") X("binding") X("block_given?") X("caller") \
 X("caller_locations") X("catch") X("eval") X("exec") X("exit") \
 X("exit!") X("fail") X("fork") X("format") X("gets") \
 X("global_variables") X("iterator?") X("lambda") X("load") \
 X("local_variables") X("loop") X("open") X("p") X("pp") X("print") \
 X("printf") X("proc") X("putc") X("puts") X("raise") X("rand") \
 X("readline") X("readlines") X("require") X("require_relative") \
 X("respond_to_missing?") X("select") X("set_trace_func") X("sleep") \
 X("spawn") X("sprintf") X("srand") X("syscall") X("system") X("test") \
 X("throw") X("trace_var") X("trap") X("untrace_var") X("warn")

#endif
