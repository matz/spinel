# A call at the top level that names a method the program defines only later is the builtin's (or none): a bare `refine`
# with a block is not Module#refine on main, it raises NoMethodError, and the refinement refusal is for a refine a module
# can run. Before the def, `b =~ 1` on a boolean is the NoMethodError a boolean has; the def, a private method of Object,
# does not answer it after either.
begin
  refine(String) { }
rescue NoMethodError
  p :nm
end
def refine(x) = :mine
p refine(1) { }

b = ARGV.empty?
begin
  b =~ 1
rescue NoMethodError => e
  p e.message
end
def =~(o) = :mine
p((b =~ 1 rescue :nm))
