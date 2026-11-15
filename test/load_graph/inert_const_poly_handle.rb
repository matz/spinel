# A String argument of a poly call is held across a later argument that
# autoloads a file: the file allocates and rebinds the global, so under GC
# stress the held handle was collected and the call saw another String.
$s = +"a"
$s << "b"
autoload :LRB, File.join(__dir__, "inert_const_lazy_rb_lib")
class A; def f(a, b = 0); p [:A, a, b]; end; end
class B; def f(a, b = 0); p [:B, a, b]; end; end
o = [A.new, B.new][ARGV.size]
o.f($s, LRB)
p $s
