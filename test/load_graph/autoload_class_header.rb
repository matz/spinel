autoload :Foo, File.join(__dir__, "autoload_class_header_lib")
class Foo
  def bye = "main bye"
end
p Foo.new.bye
p Foo.new.hi
