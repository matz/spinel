# An inline annotation on a module function and an --rbs signature for the
# method an includer gets as its copy (the includer's receiverless call runs
# the copy). When they disagree the compile stops and names both.
module Meter
  module_function
  #: (Integer) -> Integer
  def read(x)
    x
  end
end

class Gauge
  include Meter
  def show = read("a")
end

p Gauge.new.show
