# A class scope reaches Object's constants, so a scoped read of a name
# autoloaded at the top level runs the registered file and then raises
# NameError: the file defines ::Scoped, not Holder::Scoped.
class Holder; end
autoload :Scoped, File.join(__dir__, "autoload_scoped_class_lib")
begin
  Holder::Scoped
rescue NameError => e
  puts e.message
end
puts "mid"
p Scoped.v
