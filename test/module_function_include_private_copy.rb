# The copy of a `module_function` method that `include` puts in a class is
# a private instance method: an outside call raises NoMethodError and a call
# on self runs. A body with a receiverless call (`raise`, a sibling method)
# gets its own copy per includer, and that copy was public.

module Check
  def present(x)
    raise ArgumentError, "empty" if x.empty?
    x
  end
  module_function :present
end

module Wrap
  module_function
  def wrap(x) = quote(x)
  def quote(x) = "'#{x}'"
end

class Form
  include Check
  include Wrap
  def run = wrap(present("name"))
end

class Open
  include Check
  public :present
end

form = Form.new
p form.run
p form.respond_to?(:present)
p form.respond_to?(:wrap)
p form.respond_to?(:present, true)
begin
  form.present("x")
rescue NoMethodError
  puts "present is private"
end
begin
  form.wrap("x")
rescue NoMethodError
  puts "wrap is private"
end

# The module side stays public.
p Check.present("module")
p Wrap.wrap("module")
begin
  Check.present("")
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end

# An includer's own `public` still wins.
p Open.new.respond_to?(:present)
p Open.new.present("open")
