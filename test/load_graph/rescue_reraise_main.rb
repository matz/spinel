# A raise inside a handler of a failed require may re-raise the LoadError
# it rescued: a bare `raise`, `raise e`, and a raise under a condition, all
# without the program naming LoadError anywhere but in rescue lists. The
# def after it in the nested begin body is not sure to have run.
def f = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError
  begin
    raise
    def f = :new
  rescue LoadError
    p :reraised
  end
end
p f

def g = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError => e
  begin
    raise e
    def g = :new
  rescue LoadError
    p :reraised
  end
end
p g

def h = :old
begin
  require "rescue_reraise_no_such_lib"
rescue LoadError
  begin
    raise if ARGV.size == 0
    undef h
  rescue LoadError
    p :reraised
  end
end
p((h rescue $!.class))
