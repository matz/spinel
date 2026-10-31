# spinel: exit 1
# The registered file defines ::Nested, not Holder::Nested: the scoped read
# raises NameError (uncaught, so the program exits 1) after the bare read,
# inside the class body's method, has run the file.
class Holder
  autoload :Nested, File.join(__dir__, "autoload_scoped_nested_lib")
  def self.get = Nested.v
end
puts "main"
p Holder.get
p Holder::Nested.v
