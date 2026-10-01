# A constructor given a count its initialize cannot take: CRuby runs the
# arguments, in order, and then raises ArgumentError. The raise sat ahead of
# them, so none ran.

$l = []
def lg(x) = ($l << x; x)

class K; def initialize(a) = nil; end
class K2; def initialize(a, b) = nil; end
class K3; def initialize = nil; end
class K4; def initialize(a, b = 2) = nil; end

[-> { K.new(lg(1), lg(2)) },            # too many
 -> { K2.new(lg(3)) },                  # too few
 -> { K3.new(lg(4)) },                  # none taken
 -> { K4.new(lg(5), lg(6), lg(7)) },    # past the optional
 -> { K.new(lg([8]), lg("s")) },        # other kinds of value
 -> { K2.new }].each do |f|
  begin
    f.call
  rescue ArgumentError => e
    p [$l, e.message]
  end
end

# a count it takes still constructs, its arguments run once
p K4.new(lg(9)).class
p $l.size
