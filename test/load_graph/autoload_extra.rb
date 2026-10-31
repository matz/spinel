# Other constants in the file become present only after the registered read.
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
p defined?(AutoExtra)
p AutoFoo.five
p AutoExtra
p defined?($el4_autoload_marker)
