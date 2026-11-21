# A class-method visibility word in a method body that names a method
# nothing defines raises NameError where it runs, naming the singleton
# class, and changes nothing.
class Registry
  def self.entry = :entry
  def self.seal = private_class_method(:missing)
  def self.seal_list = [1, (private_class_method :missing, :entry rescue $!.class), entry]
end

module Tools
  def self.tool = :tool
  def self.seal = public_class_method(:gone)
end

p((Registry.seal rescue [$!.class, $!.message]))
p((Tools.seal rescue [$!.class, $!.message]))
p Registry.seal_list
p [Registry.entry, Tools.tool, Registry.respond_to?(:entry)]
