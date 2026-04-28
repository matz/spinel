# Issue #49: bare-method dispatch from within a class body (implicit
# self) needs to fill in defaults too. Previously this path called
# compile_call_args without the defaults map.

class Foo
  def initialize
    bar
    bar(99)
  end
  def bar(x = 42)
    puts x
  end
end

Foo.new
# 42
# 99
