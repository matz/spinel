p defined?(AutoFoo)
begin
  p AutoFoo
rescue NameError
  puts "not registered"
end
autoload :AutoFoo, File.join(__dir__, "autoload_lib")
p defined?(AutoFoo)
p AutoFoo
