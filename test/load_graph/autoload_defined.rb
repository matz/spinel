# A presence query after registration leaves the autoload pending.
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
p defined?(AutoFoo)
puts "before reference"
p AutoFoo
p defined?(AutoFoo)
