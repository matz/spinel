# A define_method block in a method's body that writes a local of that method
# through a regex's named capture is refused where the program calls the name
# it defines: the capture's writer does not know a closure cell, as for a proc.
class K
  def self.mk(n)
    w = "x"
    define_method(:m) { if /(?<w>\d+)/ =~ "ab#{n}"; w; end }
    :ok
  end
end
K.mk(3)
p K.new.m
