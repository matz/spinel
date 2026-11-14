# A reopening under a run-time condition of a program class a top-level
# body opened before: the class is there either way, so a def in the
# reopening answers only once its branch has run (#11819).
class Foo; end
if ENV["SPINEL_NO_SUCH_VARIABLE"]
  class Foo
    def bar = 1
    def inspect = "foo!"
  end
end
class Baz; end
if $stdout
  class Baz
    def bar = 2
  end
end
begin
  p Foo.new.bar
rescue NoMethodError
  puts "nomethod"
end
p Foo.new.inspect == "foo!"
p Baz.new.bar

# a class the condition defines is that class's for good
if $stdout
  class Qux
    def bar = 3
  end
end
p Qux.new.bar
