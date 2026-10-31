# A module scope does not reach Object's constants: the scoped read raises
# NameError without running the registered file.
module Holder; end
autoload :Scoped, File.join(__dir__, "autoload_scoped_module_lib")
begin
  Holder::Scoped
rescue NameError => e
  puts e.message
end
puts "mid"
p Scoped.v
