# Registration without a reference runs no file or global assignment.
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
puts "no ref"
p defined?(AutoExtra)
