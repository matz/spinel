# A constant that holds an anonymous class names it by the leaf the program
# wrote, under the module that holds it, also when another module holds a
# constant of the same leaf (#10787: constants keep the names the program
# wrote; their slots go by the load record's keys)
def make = Class.new { def hi = 7 }

module Visible
  K = make
end
module Other
  K = 5
end

p Visible::K
p Visible::K.name
p Other::K
