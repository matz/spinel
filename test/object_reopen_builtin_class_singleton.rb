# A program's reopening of a builtin class keeps the class's own singleton
# methods ahead of an Object instance method of the same name.
class File; end
class Dir; end
class Integer; end
class String; end
class Array; end
class Object
  def basename(x) = "obj"
  def exist?(x) = "obj"
  def sqrt(x) = "obj"
  def try_convert(x) = "obj"
  def only_object = "only"
end
p File.basename("/a/b.rb")
p File.dirname("/a/b.rb")
p Dir.exist?("/")
p Integer.sqrt(17)
p String.try_convert("a")
p String.try_convert(1)
p Array.try_convert([1])
# a name only Object has still reaches it
p File.only_object
p Dir.only_object
# an instance keeps Object's
p "x".basename(1)
p Object.new.try_convert(1)
