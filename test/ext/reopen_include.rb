# A module written inside a class and included in a reopening of it (a
# Rails concern): the class's table has a row of the module's method under
# its name, which a host calls through sp_ext_send (#12224).
class ExtReopenP
  module N
    def b? = true
  end
end

class ExtReopenP
  include ExtReopenP::N
  def k = 3
end

module ExtReopenMain
  # the entry answers a boxed object, as a host holds it
  #: () -> untyped
  def self.make = ExtReopenP.new
end
