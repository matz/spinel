# A class rooted in BasicObject has no Object among its ancestors: the scoped
# read raises NameError without running the registered file.
class Holder < BasicObject; end
autoload :Scoped, File.join(__dir__, "autoload_scoped_basic_object_lib")
begin
  Holder::Scoped
rescue NameError => e
  puts e.message
end
puts "mid"
p Scoped.v
