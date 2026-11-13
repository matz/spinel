# A class-level ivar of a Data class is the class object's: an instance
# neither lists it nor has it defined.
D = Data.define(:a) do
  @made = 0
  def self.made = @made
  def self.count = @made += 1
end
d = D.new(a: 1)
D.count
p d
p D.made
p d.instance_variables
p d.instance_variable_defined?(:@made)
p d == D.new(a: 1)
p d.to_h
