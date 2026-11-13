# An alias takes the visibility of the method it captured, a builtin's
# included, also where a require that is no top-level statement gives the
# load record a second region (the graph load model): the settlement of
# the record's answers asks the same question as the first reading.

def load_lib
  require_relative 'fixtures/alias_vis_region/lib'
end

class Plain
  alias pp2 puts
  alias_method :pr2, :print
end

class Opened
  def tick = 1
  alias tock tick
  alias pp3 puts
end

class Object
  public :p
end

class Later
  alias_method :show, :p
end

load_lib

[Plain, Opened, Later].each do |k|
  p [k, k.instance_methods(false).sort, k.private_instance_methods(false).sort]
end
