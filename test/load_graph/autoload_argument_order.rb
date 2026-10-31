# Earlier arguments run before an autoloading constant argument.
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
p(puts("a"), AutoFoo)
