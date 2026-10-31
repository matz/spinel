if ARGV.empty?
  autoload :AutoFoo, File.join(__dir__, "autoload_lib")
end
p AutoFoo.five
