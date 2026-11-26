#!/usr/bin/env ruby
require_relative "boxed_methods_check"

checks = 0
check = lambda do |label, ok|
  abort "boxed_methods_check_test: FAIL: #{label}" unless ok
  checks += 1
end

ops = <<~C
  { TY_IO, "getc", 0, 127 },
  { TY_IO, "pid", 0, 127 },
  { TY_IO, "__sp_internal", 0, 0 },
  { TY_THREAD, "name", 0, 0 },
C
rt = <<~C
  static const char *const uni[] = { "inspect", NULL };
  static const char *const iom[] = { "getc", NULL };
C

check.("a typed row in a boxed list or a known gap passes",
       BoxedMethodsCheck.problems(ops, rt, { "IO" => %w[pid], "Thread" => %w[name] }).empty?)
found = BoxedMethodsCheck.problems(ops, rt, { "Thread" => %w[name] })
check.("a typed row in neither fails", found == ["IO#pid: a typed row, in no boxed list and not in KNOWN_GAPS"])
found = BoxedMethodsCheck.problems(ops, rt, { "IO" => %w[getc pid], "Thread" => %w[name] })
check.("a known gap the boxed lists hold fails", found == ["IO#getc: in KNOWN_GAPS and in a boxed list; remove it from KNOWN_GAPS"])
found = BoxedMethodsCheck.problems(ops, rt, { "IO" => %w[pid], "Thread" => %w[name], "Mutex" => %w[lock] })
check.("a known gap with no row fails", found == ["Mutex#lock: in KNOWN_GAPS and in no row"])
check.("names starting with __ are skipped", !BoxedMethodsCheck.row_names(ops).fetch("IO").include?("__sp_internal"))
check.("the repository passes", BoxedMethodsCheck.run.zero?)

puts "boxed_methods_check_test: #{checks} checks passed"
