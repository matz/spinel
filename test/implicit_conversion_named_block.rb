# spinel: gc-stress
class ComparisonLabel
  def to_str(&callback)
    puts "convert"
    callback ? "no" : "yes"
  end
end
label = ComparisonLabel.new
p "YES".casecmp(label)
p "z".between?(label, "zz")
p "prefix " + label
