# A raise of a constant the program gave a value may re-raise the LoadError
# it rescued, whether the constant holds the rescued exception, its class, or
# a path below Object that names LoadError; so may a local that holds it and
# `$!`: the def after the raise, in the same begin body, is not sure to have
# run. A re-raise in a handler that cannot catch a LoadError, and a raise of
# a path to a class of the program's own, are not one. A local counts through
# the locals it was copied from.
def f = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError => e
  ERR = e
end
begin
  raise ERR if ARGV.size == 0
  def f = :new
rescue LoadError
  p :reraised
end
p f

def g = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError => e
  KLASS = e.class
end
begin
  raise KLASS, "x" if ARGV.size == 0
  def g = :new
rescue LoadError
  p :reraised
end
p g

def h = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError
  O = Object
  begin
    raise O::LoadError if ARGV.size == 0
    def h = :new
  rescue LoadError
    p :reraised
  end
end
p h

def i = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError => e
  held = e
end
begin
  raise held if ARGV.size == 0
  def i = :new
rescue LoadError
  p :reraised
end
p i

def j = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError
  begin
    raise $! if ARGV.size == 0
    def j = :new
  rescue LoadError
    p :reraised
  end
end
p j

module Own; class Err < StandardError; end; end
def k = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError
  begin
    raise Own::Err, "x" if ARGV.size == 9
    def k = :new
  rescue Own::Err
    p :own
  end
end
p k

def m = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError
  begin
    begin
      Integer("x")
    rescue ArgumentError
      raise
    end
    def m = :new
  rescue ArgumentError
    p :argument
  end
end
p m

def n = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError => e
  first = e
end
second = first
begin
  raise second if ARGV.size == 0
  def n = :new
rescue LoadError
  p :reraised
end
p n
