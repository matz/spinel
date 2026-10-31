# The same scoped read inside a method, run twice: the file loads once, at
# the first read, and both reads raise NameError.
class Holder; end
autoload :Scoped, File.join(__dir__, "autoload_scoped_class_method_lib")
def probe
  Holder::Scoped.v
rescue NameError => e
  puts "ne #{e.message}"
end
probe
p Scoped.v
probe
