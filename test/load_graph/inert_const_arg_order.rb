# A parenthesized autoloading constant is built ahead of the call's other
# arguments, since its file allocates; the file also assigns a global an
# earlier argument reads, so that read is bound first: the call sees the
# value the global had before the file ran.
$x = +"old"
autoload :LZ, File.join(__dir__, "inert_const_lazy_arg_lib")
def f(a, b); p [a, b]; end
f($x, (LZ))
p $x
