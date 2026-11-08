# spinel: rbs-seed-check
# A signature file that does not parse (test/rbs-seed/broken_sig) stops
# the build: its valid `get: () -> String`, which this program
# contradicts, would otherwise be dropped with it and the build pass
# (#11323).
class BrokenSigBox
  def initialize(v) = @v = v
  def get = @v
end
puts BrokenSigBox.new(41).get + 1
