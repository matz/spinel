# A class body registers a constant in that class.
class AutoHolder
  autoload :Z, File.join(__dir__, "autoload_class_lib")
end
puts "before reference"
p AutoHolder::Z
