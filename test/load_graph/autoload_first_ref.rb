# An autoload runs its registered file at the first constant reference.
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
p AutoFoo.new.hi
p $el4_autoload_marker
