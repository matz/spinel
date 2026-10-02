# `::Array` inside a namespace names the builtin, as does a bare Array (#7075).
module Shapes
  class Points < ::Array
  end
end

p Shapes::Points.new.size
