# A `return` earlier in a begin that holds a require ends the required file's load before the require, so the file
# that require names defines nothing: a super that looks for its top-level def finds none.
require_relative "super_top_def_begin_return_lib"
class Foo
  def greet = "F" + super
end
begin
  p Foo.new.greet
rescue NoMethodError => e
  p e.class
end
