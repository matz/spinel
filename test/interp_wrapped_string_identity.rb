class InterpolationCell
  attr_accessor :value
end

cell = InterpolationCell.new
s = +"ab"
p "#{s.upcase!.to_s}-#{s << "?"}-#{(cell.value = +"q") << "!"}"
p [s, cell.value]

s = +"ab"
p "#{s.upcase!.to_s}-#{(cell.value = s).concat("c")}-#{s.replace("R")}"
p [s, cell.value]

s = +"ab"
p "#{s.chop!.to_s}-#{(cell.value = s) << "!"}"
p [s, cell.value]
