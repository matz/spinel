# A bare include, extend or prepend statement of a class or module body
# naming a constant the program defines only further down raises NameError
# where the body runs, as CRuby does, and mixes nothing in.
begin
  class C
    include M
  end
rescue NameError => e
  puts e.message
end

begin
  module Tools
    extend Helpers
  end
rescue NameError => e
  puts e.message
end

begin
  class D
    prepend Outer::Inner
  end
rescue NameError => e
  puts e.message
end

module M
  def foo = 42
end
module Helpers
  def help = :help
end
module Outer
  module Inner
    def bar = :bar
  end
end

p C.new.respond_to?(:foo)
p Tools.respond_to?(:help)
p D.new.respond_to?(:bar)

# defined before: mixed in
class E
  include M
end
p E.new.foo
